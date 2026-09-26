#include "CombatComponent.h"
#include "LagCompensationComponent.h"
#include "DrawDebugHelpers.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/Weapon/HitScanWeapon.h"
#include "Blaster/Weapon/ProjectileWeapon.h"
#include "Blaster/Weapon/JettKnives.h"
#include "Blaster/Weapon/MeleeWeapon.h"
#include "Components/SphereComponent.h"
#include "Engine/SkeletalMeshSocket.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Camera/CameraComponent.h"
#include "TimerManager.h"
#include "Sound/SoundCue.h"
#include "Blaster/Character/BlasterAnimInstance.h"


UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	BaseWalkSpeed = 600.f;

	// 每个武器类型的兜底跑动上限。数值是"重量感"的排序：刀最快、狙击最慢。
	// 真正生效的优先顺序是 AWeapon::MaxWalkSpeed > 这张表 > BaseWalkSpeed。
	PistolWalkSpeed = 650.f;
	ShotgunWalkSpeed = 575.f;
	SniperWalkSpeed = 550.f;
	MeleeWalkSpeed = 700.f;

	// 0.75 × 600 = 450、0.5833 × 600 = 350 —— 正好是拆成系数之前那两个绝对值，
	// 所以"没配过任何武器蓝图"的行为和以前逐帧一致。
	AimSpeedRatio = 0.75f;
	ScopedSpeedRatio = 0.5833f;
}

float UCombatComponent::GetDefaultWalkSpeedForWeaponType(EWeaponType Type) const
{
	switch (Type)
	{
	case EWeaponType::EWT_Pistol:       return PistolWalkSpeed;
	case EWeaponType::EWT_Shotgun:      return ShotgunWalkSpeed;
	case EWeaponType::EWT_SniperRifle:  return SniperWalkSpeed;
	// 飞刀和普通刀一样跑得最快 —— 它占的是同一个槽（IsMeleeWeapon 认这两个类型），
	// 而"拿着刀跑得快"是槽位的手感，不该因为换了个枚举值就掉回基础速度。
	case EWeaponType::EWT_Melee:        return MeleeWalkSpeed;
	case EWeaponType::EWT_BladeStorm:   return MeleeWalkSpeed;
	case EWeaponType::EWT_AssaultRifle: return BaseWalkSpeed;
	default:                            return BaseWalkSpeed;
	}
}

float UCombatComponent::GetWeaponWalkSpeed() const
{
	const AWeapon* Weapon = Character ? Character->GetEquippedWeapon() : nullptr;
	if (!Weapon) return BaseWalkSpeed;

	// 武器自己配了就只认它（>0）。0 / 负数 = 没配过 → 按类型兜底。
	if (Weapon->MaxWalkSpeed > 0.f) return Weapon->MaxWalkSpeed;

	return GetDefaultWalkSpeedForWeaponType(Weapon->GetWeaponType());
}

void UCombatComponent::ApplyMaxWalkSpeed(bool bIsAiming)
{
	if (!Character) return;
	UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	if (!Move) return;

	// 跑动上限 —— 没拿武器时也走这条（GetWeaponWalkSpeed 会返回 BaseWalkSpeed），
	// 因为收枪/丢枪之后速度必须回到"空手"那一档，不能停在上一个武器上。
	float Speed = GetWeaponWalkSpeed();

	if (bIsAiming)
	{
		// 狙击开镜比普通瞄准更慢。判据和 SetAiming 里那道门一致（CanScope）。
		const AWeapon* Weapon = Character->GetEquippedWeapon();
		Speed *= (Weapon && Weapon->CanScope()) ? ScopedSpeedRatio : AimSpeedRatio;
	}

	Move->MaxWalkSpeed = Speed;
}

void UCombatComponent::BeginPlay()
{
	Super::BeginPlay();


	if (Character)
	{
		UE_LOG(LogTemp, Warning, TEXT("Begin"));
		// 出生时手上还没武器（武器由 GameMode 的 RestorePlayerWeapons 发），
		// 所以这里拿到的就是空手档。真正换成武器那一档是在 SetEquippedWeapon 之后。
		ApplyMaxWalkSpeed(false);

		if (Character->GetFollowCamera())
		{
			DefaultFOV = Character->GetFollowCamera()->FieldOfView;
			CurrentFOV=DefaultFOV;
		}
		if (Character->HasAuthority())
		{
			InitializeCarriedAmmo();
		}
	}
}

void UCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	if (Character && Character->IsLocallyControlled())
	{
		TraceUnderCrosshairs(HitTarget);
		SetHUDCrosshairs(DeltaTime);
		InterpFOV(DeltaTime);
	}
}

void UCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UCombatComponent, PrimaryWeapon);
	DOREPLIFETIME(UCombatComponent, SecondaryWeapon);
	// 近战槽（Jett 飞刀）：也要复制 —— 队友/观战要看到你手上拿着刀
	DOREPLIFETIME(UCombatComponent, MeleeWeapon);
}

void UCombatComponent::FireButtonPressed(bool bPressed)
{
	bFireButtonPressed = bPressed;
	
	
	if (bFireButtonPressed&&Character->GetEquippedWeapon()!=nullptr)
	{
		Fire();
		
	}
}

bool UCombatComponent::TryThrowAllKnives()
{
	if (!Character) return false;

	AJettKnives* Knives = Cast<AJettKnives>(Character->GetEquippedWeapon());
	if (!Knives) return false;

	// 没刀了也算"吃掉了这次右键"：手上拿着飞刀时右键就不该去开镜，
	// 哪怕刀已经扔空 —— 否则"扔完最后一把后右键突然变成开镜"，手感会很怪。
	if (Knives->IsEmpty()) return true;
	if (!CanFire()) return true;

	/*
	 * 让这一次 Fire() 走"全扔"分支。要设**两处**，它们服务和读取的对象都不一样：
	 *
	 *   · Knives->RequestThrowAll() → 设在**武器**上。本机这次本地预测跑的是
	 *     AJettKnives::Fire，它读的是**它自己那份**标志。原来这里只设了组件那份，
	 *     于是本机预测永远只扔 1 把刀，而服务器扔掉 5 把 —— 射手看到的和真正发生的是两回事。
	 *   · bThrowAllNextShot → 设在**组件**上，只用来把意图报给服务器
	 *     （服务器那份武器对象由 ServerFire_Implementation 自己设，见那里的注释）。
	 */
	Knives->RequestThrowAll();
	bThrowAllNextShot = true;
	FireButtonPressed(true);
	FireButtonPressed(false);
	return true;
}

bool UCombatComponent::TryMeleeHeavyAttack()
{
	if (!Character) return false;

	AMeleeWeapon* Melee = Cast<AMeleeWeapon>(Character->GetEquippedWeapon());
	if (!Melee) return false;

	// 硬直中（上一刀还没收完）也算"吃掉了这次右键"：手上拿着刀时右键就不该去开镜，
	// 哪怕这一下砍不出来 —— 否则"连砍期间右键突然变成开镜"，手感会很怪。和 TryThrowAllKnives 一致。
	if (!CanFire()) return true;

	// 让这一次 Fire() 走重击分支。两边各设一次：本机这份给本地预测用，
	// 服务器那份由 ServerFire 的 bMeleeHeavy 参数传达（见它的注释）。
	Melee->RequestHeavyAttack();
	FireButtonPressed(true);
	FireButtonPressed(false);
	return true;
}

void UCombatComponent::ThrowGrenade()
{
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied)return;
	Character->SetCombatState(ECombatState::ECS_ThrowingGrenade);
	if (Character)
	{
		Character->PlayThrowGrenadeMontage();
		ShowAttachGrenade(true);
		AttachActorToLeftHand(Character->GetEquippedWeapon());
	}
	if (!Character->HasAuthority())
	{
		ServerThrowGrenade();
	}
}

void UCombatComponent::ShotgunShellReload()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	/*
	 * 霰弹枪的压弹节奏现在由服务器的循环计时器掌管（StartShotgunShellTimer）。
	 * 这个函数是换代蒙太奇里 Shell 通知的老入口，留着是为了兼容 —— 但计时器在跑的时候
	 * 必须让路：通知和计时器各压一发的话，同样的时间里子弹会翻倍。
	 */
	if (Character->GetWorldTimerManager().IsTimerActive(ShotgunShellTimer)) return;

	UpdateShotgunAmmoValues();
}

void UCombatComponent::ShowAttachGrenade(bool bShowGrenade)
{
	if (Character && Character->GetAttachedGrenade())
	{
		Character->GetAttachedGrenade()->SetVisibility(bShowGrenade);
	}
}

void UCombatComponent::JumpToShotGunEnd()
{
	UAnimInstance* AnimInstance = Character->GetMesh()->GetAnimInstance();
	UE_LOG(LogTemp,Warning,TEXT("JumpToEnd"));
	if (AnimInstance && Character->GetReloadMontage())
	{
		
		AnimInstance->Montage_JumpToSection(FName("ShotgunEnd"));
	}
}

void UCombatComponent::ThrowGrenadeFinished()
{
	if (Character == nullptr) return;

	// 和 FinishReloading 同样的幂等门禁，理由也一样（计时器 + 动画通知两条路）。
	// ECS_ThrowingGrenade 卡住和 ECS_Reloading 卡住是同一个症状：开不了枪、切不了枪。
	if (Character->HasAuthority() && Character->GetCombatState() != ECombatState::ECS_ThrowingGrenade) return;

	Character->GetWorldTimerManager().ClearTimer(ThrowGrenadeTimer);
	Character->SetCombatState(ECombatState::ECS_Unoccupied);
	AttachActorToRightHand(Character->GetEquippedWeapon());
}

void UCombatComponent::ThrowGrenadeTimerFinished()
{
	ThrowGrenadeFinished();
}

void UCombatComponent::LaunchGrenade()
{
	
}

void UCombatComponent::ServerFire_Implementation(const FVector_NetQuantize& TraceHitTarget, float ClientHitTime, bool bThrowAll, bool bMeleeHeavy)
{
	// 服务器自己判一发能不能开：本地那套 bCanFire/FireTimer 完全在客户端跑，服务器不能信。
	// 校验不过就直接丢掉这一发（不广播、不扣弹、不结算伤害）。
	if (!ValidateServerFire())
	{
		// 这一条以前是静默 return —— 「RPC 根本没到」和「到了但被射速/弹药校验毙了」在日志上长得一样。
		// 弹药和令牌都打出来：弹药=0/满载 是弹药门禁，令牌不够是射速门禁，一眼分得开。
		AWeapon* Voided = Character ? Character->GetEquippedWeapon() : nullptr;
		UE_LOG(LogTemp, Warning,
			TEXT("[LagComp|开火被拒] %s 服务器射速/弹药校验没过（武器=%s 弹药=%d/%d 本机控制=%d 阵亡=%d 禁用操作=%d 令牌=%.3f/%.3f）"),
			Character ? *Character->GetName() : TEXT("无角色"),
			Voided ? *Voided->GetName() : TEXT("无武器"),
			Voided ? Voided->GetAmmo() : -1,
			Voided ? Voided->GetMagCapacity() : -1,
			Character ? Character->IsLocallyControlled() : -1,
			Character ? Character->IsElimmed() : -1,
			Character ? Character->GetDisableGameplay() : -1,
			ServerFireRateCredit,
			Voided ? Voided->FireDelay * ServerFireRateTolerance : -1.f);
		return;
	}

	// 回溯命中判定：用「客户端开火那一刻」的位置重算这一枪，命中才结算伤害。
	// 所以 MulticastFire 只负责表现 —— 开了回溯的武器在服务器上不再由 Fire() 扣血（见 AHitScanWeapon::Fire），
	// 否则同一次开枪会扣两次。
	AWeapon* Weapon = Character ? Character->GetEquippedWeapon() : nullptr;
	if (Weapon == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LagComp|开火被拒] %s 服务器上没有已装备武器（EquippedWeapon=null）"),
			*Character->GetName());
	}
	else if (Weapon->AppliesOwnDamage())
	{
		/*
		 *这把武器的伤害自己结算，服务器这边不需要回溯 —— 这条路上什么都不用做。
		 *目前有三种（见 AWeapon::AppliesOwnDamage 的注释）：投射物武器、近战刀、Jett 飞刀。
		 *
		 *以前这里是两个 Cast（AProjectileWeapon / AMeleeWeapon），每加一种自结算的武器就得
		 *在这里再补一个分支；而且漏补的后果很隐蔽 —— 会掉进下面那条
		 *"bUseServerSideRewind=false → 远端客户端这条路上没人结算伤害" 的警告里，
		 *把完全正常的情况报成异常（AProjectileWeapon 的构造里本来就是 false，
		 *曾经每开一枪刷一条）。现在改用武器自己申报，加武器不用再动这里。
		 */
	}
	else if (!Weapon->bUseServerSideRewind)
	{
		// 武器没开回溯 → 服务器这边整个跳过判定；而 Fire() 只在射手本地跑、HasAuthority()=false，
		// 所以远端客户端的伤害就**没有任何来源**。这是「打人不掉血」的一个隐蔽成因。
		UE_LOG(LogTemp, Warning,
			TEXT("[LagComp|开火被拒] %s 武器 %s 的 bUseServerSideRewind=false —— 远端客户端这条路上没人结算伤害"),
			*Character->GetName(), *Weapon->GetName());
	}
	else if (ULagCompensationComponent* LagComp = Character->GetLagCompensation())
	{
		FHitResult RewindHit;
		if (LagComp->ServerSideRewind(Character, TraceHitTarget, ClientHitTime, RewindHit))
		{
			Weapon->ProcessServerRewindHit(RewindHit);
		}
		// 没命中就不结算：客户端只是本地预测显示了一下，伤害以服务器回溯结果为准
		// （未命中的原因由 ServerSideRewind 里的 [LagComp|未命中] 打出来）
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[LagComp|开火被拒] %s 没有 LagCompensationComponent，回溯无从发起"),
			*Character->GetName());
	}

	// Jett 飞刀「全扔」：客户端报了 bThrowAll → 在**服务器自己那份**武器上设标志。
	// 必须在这里设而不是让客户端设：下面 MulticastFire 里跑的 Weapon->Fire() 读的是服务器这份对象，
	// 客户端本地设的标志复制不上来（标志本身不复制，它只是"这一发"的属性）。
	//
	// ⚠ 只给**不是本机控制**的角色设（远端客户端）。本机控制的（listen server 房主 / 单机）在
	//    Fire() 里已经本地预测过一遍"全扔"（那一下带着 authority，投射物就是那时候生成的），
	//    武器标志也被 AJettKnives::Fire 读完清掉了；而 MulticastFire 对他是直接 return 的 ——
	//    这里再设一次没人清，下一次左键就会莫名其妙变成全扔。
	if (bThrowAll && Character && !Character->IsLocallyControlled())
	{
		if (AJettKnives* Knives = Cast<AJettKnives>(Character->GetEquippedWeapon()))
		{
			Knives->RequestThrowAll();
		}
	}

	// bMeleeHeavy 不在服务器这侧设标志 —— 它是随多播走到**跑 Fire() 的那台机器**上的，
	// 由 MulticastFire_Implementation 设（见那里的注释：服务器自己的这一遍、以及其他客户端
	// 的那一遍都是从多播里进去的，本机控制的那台已经本地预测过、绝不能再设）。
	MulticastFire(TraceHitTarget, bMeleeHeavy);
}

bool UCombatComponent::ValidateServerFire()
{
	if (Character == nullptr) return false;

	AWeapon* Weapon = Character->GetEquippedWeapon();
	if (Weapon == nullptr) return false;

	// 阵亡 / 回合结算或购买阶段收了操作权 → 不许开火
	if (Character->IsElimmed() || Character->GetDisableGameplay()) return false;

	/*
	 *空仓不许开火（弹药数在服务器上是权威的，客户端只能通过 ServerReload 补）。
	 *
	 *⚠ 但**本机控制**的角色（listen server 的房主 / 单机）必须跳过这一条：
	 *
	 *那一台上 UCombatComponent::Fire() 的顺序是「先本地扣弹、再上报 ServerFire」——
	 *	EquippedWeapon->Fire()  → AWeapon::Fire() → SpendRound()（Ammo--）
	 *	ServerFire()           ← 在上面之后
	 *而服务器自己的 MulticastFire 对本地控制的角色是直接 return 的（不重复扣），
	 *所以校验跑到这里时，Ammo **已经**是扣完这一发的值：打得只剩 1 发时它就是 0。
	 *于是每个弹匣的最后一发都被自己判成「空仓」毙掉 ——
	 *症状正是「准星有命中反馈、跳了伤害数字，但服务器不结算伤害（不掉血）」：
	 *反馈来自本地预测（Fire() 里那一遍 ProcessHit），伤害只来自回溯结算，而回溯根本没被调用。
	 *
	 *跳过它是安全的、也不是在放松防作弊：
	 *	1) 这一枪能走到这里，说明它刚通过 CanFire()，而 CanFire() 里已经有 !IsEmpty()
	 *	   （那句读的是扣弹**之前**的 Ammo，所以"真的空仓"根本开不出枪）；
	 *	2) 本机控制的角色，开枪的和判定的在同一个进程里，弹药值本身就是权威的，没有要防的客户端。
	 *远端客户端的 ServerFire 照旧走这条判定 —— 那才是这条门禁存在的意义。
	 */
	const bool bOwnLocalPrediction = Character->IsLocallyControlled();
	if (!bOwnLocalPrediction && Weapon->IsEmpty()) return false;

	const float Now = GetWorld()->GetTimeSeconds();
	const float Interval = FMath::Max(Weapon->FireDelay, 0.01f);

	if (!bServerFireCreditInit)
	{
		// 第一发：桶是满的，直接放行
		bServerFireCreditInit = true;
		ServerFireRateCredit = Interval;
		ServerLastFireTime = Now;
	}
	else
	{
		// 按真实经过时间补令牌，上限一发
		ServerFireRateCredit = FMath::Min(ServerFireRateCredit + (Now - ServerLastFireTime), Interval);
		ServerLastFireTime = Now;
	}

	if (ServerFireRateCredit < Interval * ServerFireRateTolerance) return false;

	ServerFireRateCredit -= Interval;
	return true;
}

void UCombatComponent::MulticastFire_Implementation(const FVector_NetQuantize& TraceHitTarget, bool bMeleeHeavy)
{
	if (Character == nullptr || Character->GetEquippedWeapon() == nullptr) return;

	// 开枪的那台机器在 Fire() 里已经本地预测放过一遍表现了，多播这份只管其他人 —— 否则同一次开火
	// 会放两遍（两发弹壳、两下枪声）。注意状态机照走：霰弹枪边换弹边开枪要落回 Unoccupied，
	// 漏掉这一步会卡在换弹状态里。
	if (Character->IsLocallyControlled())
	{
		if (Character->GetCombatState() == ECombatState::ECS_Reloading && Character->GetEquippedWeapon()->GetWeaponType() == EWeaponType::EWT_Shotgun)
		{
			Character->SetCombatState(ECombatState::ECS_Unoccupied);
		}
		return;
	}

	/*
	 * 近战重击：在**这台机器**的武器对象上补设标志 —— 下面那个 Fire() 读的就是它。
	 * 服务器自己那一遍（远端客户端的开火）和其他客户端那一遍都从这里进去，一个入口覆盖两边。
	 *
	 * ⚠ 必须放在上面本机控制的提前 return **之后**：射手自己那台上，标志已经被本地预测
	 *    Fire() 读完清掉了，这里再设一次没人清 → 下一次左键会莫名其妙变成重击。
	 */
	if (bMeleeHeavy)
	{
		if (AMeleeWeapon* Melee = Cast<AMeleeWeapon>(Character->GetEquippedWeapon()))
		{
			Melee->RequestHeavyAttack();
		}
	}

	if (Character->GetCombatState() == ECombatState::ECS_Reloading && Character->GetEquippedWeapon()->GetWeaponType() == EWeaponType::EWT_Shotgun)
	{
		if (Character->GetEquippedWeapon()->ShouldPlayBodyFireMontage()) Character->PlayFireMontage();
		Character->GetEquippedWeapon()->Fire(TraceHitTarget);
		Character->SetCombatState(ECombatState::ECS_Unoccupied);
		return;
	}
	if (Character->GetCombatState()==ECombatState::ECS_Unoccupied)
	{
		if (Character->GetEquippedWeapon()->ShouldPlayBodyFireMontage()) Character->PlayFireMontage();
		Character->GetEquippedWeapon()->Fire(TraceHitTarget);
	}
}

void UCombatComponent::Fire()
{
	if (CanFire())
	{
		bCanFire=false;

		// 这一次开火是不是 Jett 飞刀的"全扔"（右键）。**读完立刻清** —— 它是"这一发"的属性，
		// 留着的话之后每一枪都会拿着 true 去调 ServerFire，而 ServerFire 会据此在武器上设标志
		//（房主尤其致命：下面的 MulticastFire 对他是直接 return 的，没人清那把武器的标志，
		// 于是从第二次左键开始**每次左键都全扔**）。
		// 注意清的是**组件这份**（只是"要不要报给服务器"）；武器那份由 AJettKnives::Fire 自己清，
		// 下面这次 EquippedWeapon->Fire() 就会读它 —— 所以这个顺序不能改：先读、再清、再 Fire。
		const bool bThrowAll = bThrowAllNextShot;
		bThrowAllNextShot = false;

		AWeapon* EquippedWeapon = Character->GetEquippedWeapon();

		// 这一枪的终点只在这里算一次：散布武器（霰弹枪）由射手摇散布，
		// 本地预测、上报服务器、多播给其他机器用的都是同一个终点。
		// 起点不再上报了 —— 服务器回溯用的是它自己记下的射手眼位（见 ServerSideRewind），
		// 客户端只交出方向（这个终点）。
		FVector TraceEnd = HitTarget.ImpactPoint;
		if (AHitScanWeapon* HitScanWeapon = Cast<AHitScanWeapon>(EquippedWeapon))
		{
			TraceEnd = HitScanWeapon->ComputeTraceEnd(HitTarget.ImpactPoint);
		}

		// 这一次是不是近战重击（右键）。读**本机这份武器**上的标志 —— 由 TryMeleeHeavyAttack 设，
		// 下面 EquippedWeapon->Fire() 读完会自己清掉（见 AMeleeWeapon::Fire），所以只能在这里取。
		// 它会随 ServerFire 的参数传到服务器和其他机器上，保证每台机器跑的是同一个分支。
		const AMeleeWeapon* Melee = Cast<AMeleeWeapon>(EquippedWeapon);
		const bool bMeleeHeavy = Melee && Melee->IsHeavyAttackNextShot();

		// 本地预测开火：蒙太奇/枪口火光/枪声/弹壳/命中特效/命中标记立刻出，不等服务器多播回来（那要等一个 RTT）。
		// 伤害仍然只在服务器上结算（HasAuthority 门禁）；弹药本地先扣，服务器的权威值复制回来自然纠偏。
		// 服务器多播那一份会跳过「开枪的这台机器」，所以不会重复播一遍（见 MulticastFire_Implementation）。
		if (EquippedWeapon)
		{
			// 身体那条通用开火蒙太奇（步枪后坐）。近战覆盖成 false —— 它自己按段播挥刀动画。
			if (EquippedWeapon->ShouldPlayBodyFireMontage()) Character->PlayFireMontage();
			EquippedWeapon->Fire(TraceEnd);
		}

		// 时间戳：把「扣下扳机那一刻的服务器时间」一起报给服务器，它拿这个把目标还原到开枪瞬间
		// （客户端/服务器时钟对齐由 PC 的 CheckTimeSync 负责，GetServerTime() 已经算好偏移）
		float ClientHitTime = GetWorld()->GetTimeSeconds();
		if (ABlasterPlayerController* BlasterPC = Cast<ABlasterPlayerController>(Character->GetController()))
		{
			ClientHitTime = BlasterPC->GetServerTime();
		}

		ServerFire(TraceEnd, ClientHitTime, bThrowAll, bMeleeHeavy);

		if (EquippedWeapon)
		{
			CrosshairShootFactor=.75f;
			// 后坐力：Fire() 只在开枪的本地客户端执行，所以每把枪的后坐只作用于自己。
			// 数值全部来自当前武器。若武器配了弹道序列（RecoilPitchPattern/RecoilYawPattern），
			// 按本次连发的第 N 发取序列值 → 弹道固定可压枪；留空则退回单值+随机水平。
			// 间隔过久没开火（换弹/切枪/松手点射）→ 弹道回到第 0 发。
			if (GetWorld()->GetTimeSeconds() - LastRecoilShotTime > RecoilPatternResetDelay)
			{
				RecoilShotCount = 0;
			}
			float Pitch = EquippedWeapon->GetRecoilPitch();
			float Yaw = FMath::FRandRange(-EquippedWeapon->GetRecoilYaw(), EquippedWeapon->GetRecoilYaw());
			const TArray<float>& PitchPattern = EquippedWeapon->GetRecoilPitchPattern();
			const TArray<float>& YawPattern = EquippedWeapon->GetRecoilYawPattern();
			if (PitchPattern.Num() > 0)
			{
				Pitch = PitchPattern[FMath::Min(RecoilShotCount, PitchPattern.Num() - 1)];
			}
			if (YawPattern.Num() > 0)
			{
				Yaw = YawPattern[FMath::Min(RecoilShotCount, YawPattern.Num() - 1)];
			}
			Character->AddRecoil(Pitch, Yaw, EquippedWeapon->GetMaxRecoilPitch());
			RecoilShotCount++;
			LastRecoilShotTime = GetWorld()->GetTimeSeconds();
		}
		StartFireTimer();
	}

}

void UCombatComponent::AttachActorToRightHand(AActor* ActorToAttach)
{
	if (Character == nullptr||Character->GetMesh() == nullptr||ActorToAttach == nullptr)return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(RightHandSocket);
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach,Character->GetMesh());
	}
}

void UCombatComponent::AttachActorToLeftHand(AActor* ActorToAttach)
{
	if (Character == nullptr||Character->GetMesh() == nullptr||ActorToAttach == nullptr|| Character->GetEquippedWeapon() == nullptr)return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(LeftHandSocket);
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach,Character->GetMesh());
	}
}

void UCombatComponent::StartFireTimer()
{
	// Character 必须先判：原来写成 `Character->GetEquippedWeapon()==nullptr || Character==nullptr`，
	// 空指针那条短路判断自己就先解引用了 Character。
	if (Character == nullptr) return;

	AWeapon* Weapon = Character->GetEquippedWeapon();
	if (Weapon == nullptr)
	{
		/*
		 *枪已经不在手上了 —— 没有 FireDelay 可等。
		 *★ 这里必须**立刻**把开火令牌还回去，不能只是 return：
		 *  全工程只有两个地方把 bCanFire 置回 true（这里和霰弹枪换弹），而 Fire() 一进来就把它置 false。
		 *  原来是直接 return（连计时器都不建），于是"开枪后 FireDelay 内枪离手"会让这把枪**本回合永远开不出火**：
		 *  安包 / 取消安包 / 持闪光 / 选治疗 / 吃大招球 / 阵亡重生 都会把手上的枪收走，
		 *  之后左键没有任何反应 —— 不扣子弹、不播蒙太奇、不调 ServerFire，日志里一个字都没有。
		 */
		bCanFire = true;
		return;
	}

	Character->GetWorldTimerManager().SetTimer(
		FireTimer,
		this,
		&UCombatComponent::FireTimerFinished,
		Weapon->FireDelay
	);

}

void UCombatComponent::FireTimerFinished()
{
	// 先复位令牌，再处理"这一步要不要续发" —— 顺过来才能保证令牌任何情况下都还得回去
	// （原来武器为空时在 bCanFire=true 之前就早退了，令牌就永远留在 false 上）。
	bCanFire = true;
	if (Character == nullptr) return;

	AWeapon* Weapon = Character->GetEquippedWeapon();
	if (Weapon == nullptr) return;

	if (bFireButtonPressed && Weapon->bAutomatic)
	{
		Fire();
	}
	ReloadEmptyWeapon();
}

bool UCombatComponent::CanFire()
{
	if (Character->GetEquippedWeapon()==nullptr) return false;
	if (!Character->GetEquippedWeapon()->IsEmpty() && bCanFire && Character->GetCombatState() == ECombatState::ECS_Reloading && Character->GetEquippedWeapon()->GetWeaponType() == EWeaponType::EWT_Shotgun)return  true;
	// 只认 ECS_Unoccupied：掏枪中（ECS_Equip）打不出子弹 —— 枪还在往上抬，这一条是设计不是遗漏。
	return !Character->GetEquippedWeapon()->IsEmpty() && bCanFire && Character->GetCombatState()==ECombatState::ECS_Unoccupied;
}

bool UCombatComponent::CanChangeWeapon()
{
	if (Character == nullptr) return false;

	const ECombatState State = Character->GetCombatState();
	return State == ECombatState::ECS_Unoccupied || State == ECombatState::ECS_Equip;
}

void UCombatComponent::InitializeCarriedAmmo()
{
	CarriedAmmoMap.Emplace(EWeaponType::EWT_AssaultRifle,StartingARAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_Pistol,StartingPistolAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_Shotgun,StartingShotgunAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_SniperRifle,StartingSniperAmmo);
}


void UCombatComponent::OnRep_PrimaryWeapon(AWeapon* LastPrimaryWeapon)
{
	if (PrimaryWeapon && Character && PrimaryWeapon != Character->GetEquippedWeapon())
	{
		PrimaryWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
		AttachActorToSocket(PrimaryWeapon, PrimaryHolsterSocket);
	}
}

void UCombatComponent::OnRep_SecondaryWeapon(AWeapon* LastSecondaryWeapon)
{
	if (SecondaryWeapon && Character && SecondaryWeapon != Character->GetEquippedWeapon())
	{
		SecondaryWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
		AttachActorToSocket(SecondaryWeapon, SecondaryHolsterSocket);
	}
}

void UCombatComponent::OnRep_MeleeWeapon(AWeapon* LastMeleeWeapon)
{
	if (MeleeWeapon && Character && MeleeWeapon != Character->GetEquippedWeapon())
	{
		// 近战武器也收在挂点上（拿刀时按 1/2 切枪就是把它收回 MeleeHolsterSocket）
		MeleeWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
		AttachActorToSocket(MeleeWeapon, MeleeHolsterSocket);
	}
}

void UCombatComponent::EquipMeleeWeapon(AWeapon* WeaponToEquip)
{
	if (!Character || !Character->HasAuthority()) return;
	if (!WeaponToEquip) return;

	/*
	 * ⚠ 这个门禁必须在**改任何东西之前** —— 整个函数要做的是"记账 + 掏出来"两件事，
	 * 而掏那一步（EquipSlotWeapon）自己也有 CanChangeWeapon() 门禁。少了这一句，
	 * 换弹中调进来（CanChangeWeapon 挡 ECS_Reloading）就会出现**记了账但没掏**：
	 * MeleeWeapon 已经指向新刀、旧刀已经进了 StashedMeleeWeapon，
	 * 而人手上还拿着原来那把枪 —— 状态自相矛盾，而且下次大招连"顶替旧刀"那一段都会走歪。
	 *
	 * 这个 bug 的实际表现（2026-09-18 报）：手枪换弹时按 X 开大招，手枪不换、后续开枪还是手枪。
	 * 现在的调用方 ServerStartBladeStorm 会先 CancelReload() 把状态放回 Unoccupied，
	 * 所以正常路径走得到这儿；这一句是兜底 —— 让本函数**要么全做、要么不做**。
	 */
	if (!CanChangeWeapon())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[近战槽] %s 进 EquipMeleeWeapon 时状态是 %d（CanChangeWeapon 只放行 Unoccupied/Equip）→ ")
			TEXT("这一把不掏，槽位也不动。换弹中的话调用方应该先 CancelReload()。"),
			*WeaponToEquip->GetName(), (int32)Character->GetCombatState());
		return;
	}

	// 槽里原有的那把（角色自带的刀）收进 StashedMeleeWeapon，**不销毁** —— 收招时放回来。
	// 以前这里是直接 Destroy()，那是按"槽里只可能有大招的刀"写的；自带的刀进来之后，
	// Destroy 会把它永久弄丢（大招结束时按 3 就再也掏不出刀了）。
	if (MeleeWeapon && MeleeWeapon != WeaponToEquip)
	{
		// 上一个 stash 还留着 = 状态不对（同时有两把被顶掉的刀）。销毁旧的，留条日志。
		if (StashedMeleeWeapon)
		{
			UE_LOG(LogTemp, Warning, TEXT("[近战槽] %s 顶替时发现还收着一把 %s，那把直接销毁"),
				*WeaponToEquip->GetName(), *StashedMeleeWeapon->GetName());
			StashedMeleeWeapon->Destroy();
		}
		StashedMeleeWeapon = MeleeWeapon;
	}

	// 必须先记进槽位再掏 —— EquipSlotWeapon 里收回上一把时要靠 GetHolsterSocketForWeapon
	// 查挂点，槽位没记的话刀会被挂到主武器挂点上（而且 OnRep 也会找错）。
	MeleeWeapon = WeaponToEquip;
	EquipSlotWeapon(WeaponToEquip);
}

void UCombatComponent::GrantMeleeWeapon(AWeapon* WeaponToGrant)
{
	if (!Character || !Character->HasAuthority()) return;
	if (!WeaponToGrant) return;

	// 槽里已经有一把（正常不会发生：角色是新的，RestorePlayerWeapons 每次重生只跑一次）
	// → 保持现状，把这一把销毁掉，别制造第二把没人管的刀。
	if (MeleeWeapon)
	{
		WeaponToGrant->Destroy();
		return;
	}

	// 收进槽位 + 挂到挂点上，但**不 SetEquippedWeapon** —— 手上该拿着枪，刀等玩家按 3。
	// 和 OnRep_MeleeWeapon 在客户端做的是同一件事（那边是被复制驱动，这边是服务器主动发放）。
	MeleeWeapon = WeaponToGrant;
	WeaponToGrant->SetWeaponState(EWeaponState::EWS_Holstered);
	AttachActorToSocket(WeaponToGrant, MeleeHolsterSocket);
}

void UCombatComponent::DestroyMeleeWeapon(bool bRestoreStashed)
{
	if (!Character || !Character->HasAuthority()) return;

	// ——销毁当前槽里那把（大招的刀）——
	// 销毁而不是 Dropped()：敌人不该捡到对手大招的刀
	if (MeleeWeapon)
	{
		AWeapon* Doomed = MeleeWeapon;
		MeleeWeapon = nullptr;

		// 手里正拿着它 → 先清空手上武器，否则角色会一直指着一个已经销毁的 actor
		if (Character->GetEquippedWeapon() == Doomed)
		{
			Character->SetEquippedWeapon(nullptr);
			// 手上空了，HUD 弹药位也要跟着空 —— 不然会一直停着最后那个刀数（比如 0）不变
			if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(Character->GetController()))
			{
				PC->SetHUDWeaponAmmo(0);
			}
		}

		Doomed->Destroy();
	}

	// ——被顶掉的那把常驻刀，两条去路——
	if (StashedMeleeWeapon)
	{
		AWeapon* Stashed = StashedMeleeWeapon;
		StashedMeleeWeapon = nullptr;

		if (bRestoreStashed)
		{
			// 收招：放回槽位，收在挂点上（不掏出来 —— 玩家按 3 才切，和"收招时不该抢走手上的枪"一致）。
			// 槽位是复制属性，客户端那边 OnRep_MeleeWeapon 会把它重新挂到 MeleeHolsterSocket 上；
			// 服务器自己不跑 OnRep，所以这里手工挂一次。
			MeleeWeapon = Stashed;

			/*
			 * ⚠ 它可能**正握在玩家手上** —— 大招生效期间按 3 掏的（见 SwitchWeapon 的 ESlot_Melee 分支）。
			 * 那种情况下不能再走"挂回挂点 + Holstered"：那会把这把刀从手里挪到腰上、
			 * 而且 Holstered 是隐藏状态，表现是"大招结束时手里的刀凭空没了"。
			 *
			 * 客户端同样不用特殊处理：OnRep_MeleeWeapon 只在 `MeleeWeapon != 手上那把` 时才重挂，
			 * 这里的条件正好反过来。
			 */
			if (Character->GetEquippedWeapon() == Stashed)
			{
				Stashed->SetWeaponState(EWeaponState::EWS_Equipped);
			}
			else
			{
				Stashed->SetWeaponState(EWeaponState::EWS_Holstered);
				AttachActorToSocket(Stashed, MeleeHolsterSocket);
			}
		}
		else
		{
			// 角色被销毁：放回去没人接管，会变成一个挂在已销毁角色上的孤儿 actor（每回合漏一个），
			// 所以这里也销毁 —— 下一回合 RestorePlayerWeapons 会重新发一把。
			Stashed->Destroy();
		}
	}
}

void UCombatComponent::DropEquippedWeapon()
{
	AWeapon* Weapon = Character ? Character->GetEquippedWeapon() : nullptr;
	if (!Weapon) return;

	// 近战（3 号槽的刀）丢不掉 —— 角色自带、没有"掉在地上"这个状态（用户要求）。
	// 拦在这里而不是拦 AWeapon::Dropped()：丢枪这条路上还有一串副作用
	//（清槽位引用、SetEquippedWeapon(nullptr)、HUD 归零），只挡武器那一半会留下
	// "槽里空了、刀却还握在手上"的半截状态。
	if (Weapon->IsMeleeWeapon()) return;

	if (Weapon == PrimaryWeapon)   PrimaryWeapon = nullptr;
	if (Weapon == SecondaryWeapon) SecondaryWeapon = nullptr;
	Character->SetEquippedWeapon(nullptr);
	Weapon->Dropped();
}

// 死亡时两把武器都掉到地上（主+副），Combat 槽位清空 → 下一回合不再继承
void UCombatComponent::DropAllWeapons()
{
	if (!Character) return;

	AWeapon* Primary = PrimaryWeapon;
	AWeapon* Secondary = SecondaryWeapon;

	if (Primary) DropWeaponFromSlot(Primary);
	if (Secondary) DropWeaponFromSlot(Secondary);
}

void UCombatComponent::UpdateCarriedAmmo()
{
	if (Character->GetEquippedWeapon() == nullptr)return;

	// 备用弹药表里没有这个类型（近战飞刀）→ 显示 0。
	// 以前是"没有就什么都不做"，于是拿飞刀时 HUD 会一直挂着上一把枪的备用弹（比如 90），
	// 看着像"手里有把枪"。飞刀本来就没有备用弹药，0 才是对的。
	const EWeaponType Type = Character->GetEquippedWeapon()->GetWeaponType();
	Character->SetCarriedAmmo(CarriedAmmoMap.Contains(Type) ? CarriedAmmoMap[Type] : 0);

	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
}

void UCombatComponent::PlayEquipWeaponSound()
{
	if (Character)
	{
		if (Character->GetEquippedWeapon())
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				Character->GetEquippedWeapon()->EquipSound,
				Character->GetActorLocation()
			);
		}
	}
}

void UCombatComponent::ReloadEmptyWeapon()
{
	if (Character->GetEquippedWeapon())
	{
		if (Character->GetEquippedWeapon()->IsEmpty())
		{
			Reload();
		}
	}
}

void UCombatComponent::AttachActorToSocket(AActor* ActorToAttach, FName SocketName)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr) return;

	const USkeletalMeshSocket* Socket = Character->GetMesh()->GetSocketByName(SocketName);
	if (Socket)
	{
		Socket->AttachActor(ActorToAttach, Character->GetMesh());
		return;
	}

	// 找不到 socket —— 以前是静默什么都不做，于是武器**留在世界上原地不动**
	//（画面上就是"收枪没收起来，枪飘在半空"），而且一句日志都没有。
	// 现在退到副武器挂点，至少让东西跟着角色走，同时报出来好去骨架上加 socket。
	// 最常踩的正是 MeleeHolsterSocket：新加的挂点，骨架资产上还没有。
	if (SocketName == SecondaryHolsterSocket) return;   // 备胎自己也不在，无可退

	if (const USkeletalMeshSocket* Fallback = Character->GetMesh()->GetSocketByName(SecondaryHolsterSocket))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[挂点] 角色骨骼上找不到 socket「%s」，%s 暂挂到「%s」。去骨架资产上加这个 socket。"),
			*SocketName.ToString(), *ActorToAttach->GetName(), *SecondaryHolsterSocket.ToString());
		Fallback->AttachActor(ActorToAttach, Character->GetMesh());
	}
}

FName UCombatComponent::GetHolsterSocketForWeapon(AWeapon* Weapon) const
{
	// 近战武器最先判：飞刀收在腰后/大腿的 MeleeHolsterSocket 上（主武器挂点会被长枪占满）
	if (Weapon == MeleeWeapon) return MeleeHolsterSocket;

	/*
	 * 被顶掉的那把自带刀也算近战。
	 *
	 * 大招生效期间玩家可以按 3 把自带刀握在手上（见 SwitchWeapon），这时候它不在任何槽位里 ——
	 * 少了这一句它就会掉到最后的 else，被挂到**主武器挂点**上：表现是"按 1/2 切枪时那把刀
	 * 瞬移到了背后长枪的位置"（枪是隐藏的，所以看得到的就是刀贴错了地方）。
	 *
	 * 和 MeleeWeapon 共用同一个 socket 是可以的：同一时刻两者最多只有一个挂在上面
	 *（另一个要么握在手上、要么正在被换下来），而且 EWS_Holstered 的武器本来就是收起来不显示的。
	 */
	if (Weapon == StashedMeleeWeapon) return MeleeHolsterSocket;

	return (Weapon == SecondaryWeapon) ? SecondaryHolsterSocket : PrimaryHolsterSocket;
}

void UCombatComponent::ResetRecoilPattern()
{
	RecoilShotCount = 0;
	LastRecoilShotTime = 0.f;
}

void UCombatComponent::EquipSlotWeapon(AWeapon* WeaponToEquip)
{
	if (!Character || !WeaponToEquip) return;
	if (WeaponToEquip == Character->GetEquippedWeapon()) return;
	// ECS_Equip 也放行 —— 掏枪动画播一半再按 2 就是把新枪换上来（见 CanChangeWeapon）
	if (!CanChangeWeapon()) return;

	// 切枪后弹道从第 0 发重新开始（每把枪各跳各的序列）
	ResetRecoilPattern();

	if (Character->IsSpikeDrawn()) HolsterSpike();

	AWeapon* Previous = Character->GetEquippedWeapon();
	if (Previous)
	{
		// EWS_Holstered 而不是 EWS_Equipped：这把枪从"手上"变成"挂在背上"，
		// 而 Holstered 会把它整个藏起来（瓦里背后不显示枪）。
		// 挂点照旧挂着 —— 藏起来的枪不需要"挂在哪好看"，但保持附着能省掉
		// 一条绝对坐标的复制流，而且哪天想把背后的枪显示出来位置就是对的。
		Previous->SetWeaponState(EWeaponState::EWS_Holstered);
		AttachActorToSocket(Previous, GetHolsterSocketForWeapon(Previous));
	}

	WeaponToEquip->SetWeaponState(EWeaponState::EWS_Equipped);
	WeaponToEquip->SetOwner(Character);
	// 挂点由**武器自己**说了算（AWeapon::ThirdPersonAttachSocket，默认 "RightHandSocket"），
	// 这样每把武器可以有不同的握位：在角色骨骼上加个 socket、把名字填到武器蓝图里即可，
	// 不用动 CombatComponent（以前这里是写死的 RightHandSocket，所有枪一个握点）。
	AttachActorToSocket(WeaponToEquip, WeaponToEquip->GetThirdPersonAttachSocket());
	// SetEquippedWeapon 放在 SetOwner 之后：它内部会刷第一人称那份枪械副本（UpdateFPWeaponMesh），
	// 而枪身上一堆表现逻辑（GetViewMesh / PlayGunAnimation / 播蒙太奇）都是靠 GetOwner() 找到角色的。
	// 以前它是第一句，那一刻武器还没有 Owner —— 反推拿不到角色，瞄准镜/弹匣就会留在真枪上。
	Character->SetEquippedWeapon(WeaponToEquip);
	WeaponToEquip->SetHUDAmmo();

	UpdateCarriedAmmo();
	PlayEquipWeaponSound();
	// ★ 下面两条动画只覆盖**服务器这台机器**（本函数只有服务器会跑 —— 客户端按 1/2/3
	//   走的是 ABlasterCharacter::ServerEquipSlot 这个 RPC）。
	//   其他机器那两条腿在 ABlasterCharacter::OnRep_EquipWeapon 里：EquippedWeapon 复制过去的
	//   那一刻才播（本机手模 + 别人看得见的第三人称身体）。
	//   两边都要留 —— 服务器不跑 OnRep、客户端不跑这里，删掉任何一边都会有一半机器没动画。
	//
	// 第一人称手模的切枪动画（各武器蓝图自己挂 FPEquipMontage）。
	// 这里其实只在"房主自己切枪"时播得出来：远端玩家那份 pawn 在服务器上不是本地控制，
	// PlayFPArmsMontage 开头就把 IsLocallyControlled() 挡掉了（正确 —— 别人第一人称不可见）。
	// 必须等 SetOwner(Character) 之后再调 —— 武器是靠 GetOwner() 找到角色的。
	WeaponToEquip->PlayFPEquipMontage();
	// 枪自己那条掏枪动画（EquipAnimation）。这条**所有机器都播**：远端玩家也看得见你的枪在动。
	WeaponToEquip->PlayEquipAnimation();
	// 第三人称身体的掏枪蒙太奇（各武器蓝图自己挂 ThirdPersonEquipMontage）。
	// 这是**服务器看别人切枪**的那一条：房主眼前，客户端切枪的身体动作就靠它。
	// 没配就什么都不播，和改动前完全一样 —— 所以可以一把武器一把武器慢慢补，
	// 不会一次改动全崩。
	Character->PlayEquipMontage();

	/*
	 * 动画都踢出去了 → 进「掏枪中」。开火（CanFire）和换弹（Reload）都只认 ECS_Unoccupied，
	 * 所以这一段里这两件事自动被挡掉；切枪/掏尖刺走 CanChangeWeapon()，**照常能用**。
	 *
	 * 空仓自动换弹**不在这里做了**：上面那条 Reload 也是只认 ECS_Unoccupied 的，这个位置调
	 * 只会被自己挡掉。挪到 EquipFinish 里 —— 枪真正到手上了再开始换弹，本来也更对。
	 */
	Character->SetCombatState(ECombatState::ECS_Equip);
	StartEquipTimer();

	Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = true;
}

void UCombatComponent::SwitchWeapon(EWeaponSlot Slot)
{
	if (!Character || !Character->HasAuthority()) return;
	if (!CanChangeWeapon()) return;

	switch (Slot)
	{
	case EWeaponSlot::ESlot_Primary:
		EquipSlotWeapon(PrimaryWeapon);
		break;
	case EWeaponSlot::ESlot_Secondary:
		EquipSlotWeapon(SecondaryWeapon);
		break;
	case EWeaponSlot::ESlot_Melee:
		/*
		 * 近战槽里现在是 Jett 的飞刀（大招生效期间）→ 按 3 要的是**角色自带的那把刀**，
		 * 不是飞刀（用户 2026-09-18："开大期间按3就是正常切刀"）。
		 *
		 * 大招生效期间槽位是这样分的：MeleeWeapon = 飞刀（大招临时生成的、被顶进了槽位），
		 * StashedMeleeWeapon = 角色自带的那把刀（被顶掉时收着、不销毁）。所以这里要做的是
		 * 把"槽位"这层记账绕过去，直接掏出那把被顶掉的刀 —— 它**不在任何槽位里**，
		 * 但完全可以被握着（EquippedWeapon 和"在哪个槽"本来就是两件事，
		 * SetEquippedWeapon 只认前者；收枪时的挂点由 GetHolsterSocketForWeapon 兜住）。
		 *
		 * 为什么不在 StashedMeleeWeapon 存在时**换槽**（把飞刀塞进 stash、把自带刀放回槽）：
		 * 那样收招时 DestroyMeleeWeapon(bRestoreStashed=true) 会找错对象 —— 它认的是
		 * "槽里那把 = 大招的刀"，换槽之后槽里是自带刀、它会去销毁飞刀再放回自己，
		 * 等于把整套收招逻辑的隐含前提掀了。现在这个写法只碰"手上拿什么"，槽位记账一个字不动。
		 *
		 * 其余情况（没开大 / 没有自带刀）就是原来的行为：有刀掏刀、没刀什么都不做
		 *（EquipSlotWeapon 会因为 nullptr 直接返回，按 3 不会把手上的枪收走）。
		 */
		if (Cast<AJettKnives>(MeleeWeapon) != nullptr && StashedMeleeWeapon != nullptr)
		{
			EquipSlotWeapon(StashedMeleeWeapon);
			break;
		}
		EquipSlotWeapon(MeleeWeapon);
		break;
	case EWeaponSlot::ESlot_Spike:
		DrawSpike();
		break;
	default:
		break;
	}
}

void UCombatComponent::DrawSpike()
{
	if (!Character || !Character->HasAuthority()) return;
	if (!Character->CarriedSpike || Character->IsSpikeDrawn()) return;
	// 掏尖刺包算「换手上的东西」，和切枪同一档：掏枪中也允许（CanChangeWeapon）
	if (!CanChangeWeapon()) return;

	AWeapon* Gun = Character->GetEquippedWeapon();
	if (Gun)
	{
		Gun->SetWeaponState(EWeaponState::EWS_Holstered);
		AttachActorToSocket(Gun, GetHolsterSocketForWeapon(Gun));
		Character->SetEquippedWeapon(nullptr);
	}

	Character->SetSpikeDrawn(true);
	Character->CarriedSpike->Draw(Character);
	Character->bUseControllerRotationYaw = true;
}

void UCombatComponent::HolsterSpike()
{
	if (!Character || !Character->HasAuthority()) return;
	if (!Character->CarriedSpike || !Character->IsSpikeDrawn()) return;

	Character->SetSpikeDrawn(false);
	Character->CarriedSpike->Holster(Character);
}

void UCombatComponent::StartSpikePlant()
{
	if (Character && Character->CarriedSpike && Character->IsSpikeDrawn())
	{
		Character->CarriedSpike->StartPlant(Character);
	}
}

void UCombatComponent::CancelSpikePlant()
{
	if (Character && Character->CarriedSpike)
	{
		Character->CarriedSpike->CancelAction();
	}
}

void UCombatComponent::HolsterEquippedWeapon()
{
	if (!Character || !Character->HasAuthority()) return;

	// 若正掏着 spike，先把 spike 收回去（拆包时不该拿 spike，但兜底）
	if (Character->IsSpikeDrawn()) HolsterSpike();

	AWeapon* Current = Character->GetEquippedWeapon();
	if (!Current) return;

	// 与 EquipSlotWeapon 的“上一把挂回闲置 socket”一致：武器收回挂点、清空手持，
	// 之后 IsWeaponEquipped()==false → 动画机走空手（拆包姿态）
	Current->SetWeaponState(EWeaponState::EWS_Holstered);
	AttachActorToSocket(Current, GetHolsterSocketForWeapon(Current));
	Character->SetEquippedWeapon(nullptr);
}

void UCombatComponent::EquipBestOwnedWeapon()
{
	if (!Character || !Character->HasAuthority()) return;
	// 技能收尾时掏回武器，和切枪同一档：掏枪中允许（CanChangeWeapon）。
	// 同槽重复掏由 EquipSlotWeapon 开头那句 `== GetEquippedWeapon()` 挡掉，不会把动画重播一遍。
	if (!CanChangeWeapon()) return;

	// 主武器优先：有主武器就掏主武器，否则掏副武器
	AWeapon* Best = PrimaryWeapon ? PrimaryWeapon : SecondaryWeapon;

	if (Best) EquipSlotWeapon(Best);
}

void UCombatComponent::DropWeaponFromSlot(AWeapon* Weapon)
{
	if (!Weapon) return;
	if (Weapon == PrimaryWeapon)   PrimaryWeapon = nullptr;
	if (Weapon == SecondaryWeapon) SecondaryWeapon = nullptr;
	if (Weapon == Character->GetEquippedWeapon()) Character->SetEquippedWeapon(nullptr);
	Weapon->Dropped();
}

void UCombatComponent::EquipWeapon(AWeapon* WeaponToEquip)
{
	if (Character == nullptr || WeaponToEquip == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("WeaponToEquip == nullptr."));
		return;
	}
	/*
	 * 放行条件和"切枪"同一档（Unoccupied 或 Equip），**不是**只认 Unoccupied —— 和
	 * EquipSlotWeapon / SwitchWeapon 保持一致。三条理由，都是实测撞出来的：
	 *
	 * 1. 回合开始 ABlasterGameMode::RestorePlayerWeapons 会连着调两次（先主武器、再副武器），
	 *    中间没有任何延迟。第一次调完状态已经是 ECS_Equip（掏枪动画 0.6 秒），这里若只认
	 *    Unoccupied，第二次调用会在**扣完钱、生成完武器之后**被挡掉 —— 副武器槽空着，
	 *    角色手上没枪，那把生成出来的枪变成没人管的孤儿。改之前不卡是因为那时
	 *    EquipSlotWeapon 根本不改状态。
	 * 2. 购买同理：ABlasterPlayerController::ServerBuyWeapon 是**先扣钱再调这里**，
	 *    被挡掉就是"钱花了、枪没了"，而且不退款（只有 SpawnActor 失败才退）。
	 *    连买两把枪的间隔远小于 0.6 秒的掏枪时间，这个门必须开。
	 * 3. 语义上本来就该放行：掏枪中允许换枪是这次改动的核心要求（见 CanChangeWeapon）。
	 *    捡枪走的是同一个函数，掏枪动画播一半捡起地上的枪，行为和"按 2 切枪"没区别。
	 *
	 * 换弹（ECS_Reloading）依然被 CanChangeWeapon 挡在外面 —— 那条限制没松。
	 */
	if (!CanChangeWeapon()) return;

	// 近战不能被捡（3 号槽的刀只走 EquipMeleeWeapon）。正常路径下走不到这里 ——
	// 近战的拾取球在构造里就没碰撞（见 AMeleeWeapon），但下面这行是按 IsSecondaryWeapon()
	// 分主/副槽的：一把近战漏进来会被塞进**主武器槽**，那之后按 1 掏出来的就是刀。所以再挡一道。
	if (WeaponToEquip->IsMeleeWeapon()) return;

	EWeaponSlot Target = WeaponToEquip->IsSecondaryWeapon() ? EWeaponSlot::ESlot_Secondary : EWeaponSlot::ESlot_Primary;
	AWeapon*& SlotRef = (Target == EWeaponSlot::ESlot_Secondary) ? SecondaryWeapon : PrimaryWeapon;
	if (SlotRef) DropWeaponFromSlot(SlotRef);

	SlotRef = WeaponToEquip;
	EquipSlotWeapon(WeaponToEquip);
}

void UCombatComponent::Reload()
{
	if (Character->GetCarriedAmmo()>0 && Character->GetCombatState()==ECombatState::ECS_Unoccupied && Character->GetEquippedWeapon() && !Character->GetEquippedWeapon()->IsFull())
	{
		ServerReload();
	}
}

void UCombatComponent::CancelReload()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	// 幂等门禁，和 FinishReloading 同一个形状：只认"这一次换弹"。
	// 不是 ECS_Reloading 就直接走人（没在换弹、或者已经收过尾了）。
	if (Character->GetCombatState() != ECombatState::ECS_Reloading) return;

	// 两条计时器都要清：普通枪走 ReloadTimer，霰弹枪的"一发一发压"走 ShotgunShellTimer
	//（它还会在没子弹时用 ReloadTimer 排一个收招延时）。漏掉任何一条，那颗计时器到点后
	// 会去跑 FinishReloading —— 那时候状态已经不是 ECS_Reloading，被它自己的门禁挡掉，
	// 所以不会真填弹；但留着一条没用的计时器没必要，而且霰弹枪那条是**循环**的。
	Character->GetWorldTimerManager().ClearTimer(ReloadTimer);
	Character->GetWorldTimerManager().ClearTimer(ShotgunShellTimer);

	// 不回填弹药 —— 这正是"取消"和"完成"的唯一区别（UpdateAmmoValues /
	// UpdateShotgunAmmoValues 都不调）。霰弹枪已经压进去的那几发**留在弹匣里**：
	// 它是真的一发一发压的，压进去的没理由退回去。
	Character->SetCombatState(ECombatState::ECS_Unoccupied);
}

void UCombatComponent::FinishReloading()
{
	if (Character==nullptr)return;

	/*
	 * 幂等门禁：会有两条路走到这里 —— 服务器的换弹计时器，和换弹蒙太奇上的动画通知
	 *（UAnimNotify_ReloadFinished，挂在动画末帧之前）。本工程当前**只有计时器**那条活着
	 *（2026-09-26 已把蒙太奇上的结束通知全摘掉，理由见 StartEquipTimer 的注释），
	 * 但门禁必须留着：将来谁把通知加回蒙太奇，两条路就会**进来两次**，而
	 * UpdateAmmoValues 是"按当前备弹把弹匣填满再扣备弹"—— 进来两次就是填两遍、扣两遍备弹。
	 *
	 * 只认「这一次换弹」：状态不是 ECS_Reloading 就说明已经收过尾了，直接走人。
	 * 客户端不加这道门（它的状态靠复制、时序和服务器不一样，拦了反而可能不填弹）。
	 */
	if (Character->HasAuthority() && Character->GetCombatState() != ECombatState::ECS_Reloading) return;

	if (Character->HasAuthority())
	{
		Character->GetWorldTimerManager().ClearTimer(ReloadTimer);
		Character->GetWorldTimerManager().ClearTimer(ShotgunShellTimer);
		Character->SetCombatState(ECombatState::ECS_Unoccupied);
		UpdateAmmoValues();
	}
	if (bFireButtonPressed)
	{
		Fire();
	}

}

void UCombatComponent::EquipFinish()
{
	if (Character==nullptr)return;

	/*
	 * 和 FinishReloading 完全同一个形状，理由也一样（那边注释更细）：
	 * 计时器和动画通知两条路都活着时会**进来两次**，而这里的收尾动作是
	 * 「把状态放回去 + 补一发自动换弹」—— 进去两次会多换一次弹（多扣一份备弹）。
	 *
	 * 只认「这一次掏枪」：状态不是 ECS_Equip 就说明已经收过尾了，直接走人。
	 * 客户端不加这道门（它的状态靠复制、时序和服务器不一样）。
	 */
	if (Character->HasAuthority() && Character->GetCombatState() != ECombatState::ECS_Equip) return;

	if (Character->HasAuthority())
	{
		Character->GetWorldTimerManager().ClearTimer(EquipTimer);
		Character->SetCombatState(ECombatState::ECS_Unoccupied);
		// 枪到手了才发现是空仓 → 顺手换弹（这一步原来在 EquipSlotWeapon 末尾，
		// 但那时状态已经是 ECS_Equip，Reload 进不来）。Reload() 自己会判备弹/满仓，不用重复判断。
		ReloadEmptyWeapon();
	}
	// 掏枪期间按着左键不放：枪一抬起来就接着打（和换弹结束那条一样的手感）
	if (bFireButtonPressed)
	{
		Fire();
	}
}

void UCombatComponent::StartEquipTimer()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	AWeapon* Equipped = Character->GetEquippedWeapon();
	if (Equipped == nullptr) return;

	/*
	 * 掏枪什么时候算完：**这条计时器说了算**（和换弹那条一个形状，见 AWeapon::EquipTime 的注释）。
	 *
	 * 2026-09-26 之前这里写的是"通知先到、计时器兜底"，蒙太奇尾部也确实挂着
	 * UAnimNotify_EquipFinished。但逐个量过之后发现那条通知落在**第三人称**掏枪动画末帧之前
	 *（比动画早 0.1~0.5 秒，实测总是它先到），而玩家真正看到的是**第一人称手模**那条 ——
	 * 两条动画长度本来就不一样（例：AK 的第三人称 1.83 秒 / 第一人称 1.46 秒，通知在 1.83 秒那条的
	 * 倒数第二帧上）。于是"掏枪时长"变成了第三人称动画的长度，永远比手上的动作慢半拍。
	 * 现在把那些结束通知全摘了，时长只由这条计时器给（值 = 第一人称蒙太奇的实际长度，
	 * 见 AWeapon::GetEquipDuration），状态和看到的动画就精确对齐了。
	 *
	 * EquipFinish 里的幂等门禁留着 —— 将来谁再把通知加回蒙太奇，也不会双收尾
	 *（那会多换一次弹：EquipFinish 里有一句 ReloadEmptyWeapon）。
	 */
	Character->GetWorldTimerManager().SetTimer(
		EquipTimer,
		this,
		&UCombatComponent::EquipTimerFinished,
		Equipped->GetEquipDuration()
	);
}

void UCombatComponent::EquipTimerFinished()
{
	EquipFinish();
}

void UCombatComponent::ServerReload_Implementation()
{
	if (Character ==  nullptr || Character->GetEquippedWeapon()==nullptr) return;
	// 空手蒙太奇那段不可打断。客户端那条路（Reload() 只认 ECS_Unoccupied）已经挡住了，
	// 这里是服务器侧的兜底：下面那句 SetCombatState 会把空手状态顶掉，而空手的收尾
	//（EmptyHandFinish：掏回最强的武器）会因为"状态已经不是 ECS_EmptyHand"直接走人
	// —— 人就被永久留在空手上了。
	// 备注：这个函数本来就会因为 EquippedWeapon==nullptr 提前返回（空手时枪挂在背上），
	// 但那是副作用不是契约，显式写出来，以后谁改了收枪顺序也不会突然漏。
	if (Character->IsEmptyHandLocked()) return;

	Character->SetCombatState(ECombatState::ECS_Reloading);
	HandleReload();

	// 换弹什么时候算完，由服务器这条计时器说了算 —— 不看动画通知了。
	StartReloadTimer();
}

void UCombatComponent::StartReloadTimer()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	AWeapon* Equipped = Character->GetEquippedWeapon();
	if (Equipped == nullptr) return;

	// 霰弹枪是"一发一发压"，节奏和别的枪不是一回事：走它自己的循环计时器。
	if (Equipped->GetWeaponType() == EWeaponType::EWT_Shotgun)
	{
		StartShotgunShellTimer();
		return;
	}

	// 别的枪一次性换完。时长 = 武器自己配的 ReloadTime，没配就按第一人称换弹蒙太奇的长度。
	Character->GetWorldTimerManager().SetTimer(
		ReloadTimer,
		this,
		&UCombatComponent::ReloadTimerFinished,
		Equipped->GetReloadDuration()
	);
}

void UCombatComponent::ReloadTimerFinished()
{
	FinishReloading();
}

void UCombatComponent::StartShotgunShellTimer()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	AWeapon* Equipped = Character->GetEquippedWeapon();
	if (Equipped == nullptr) return;

	// 循环计时器，每 ShotgunShellTime 压一发，一直压到压满 / 没备弹为止 ——
	// 收尾写在 ShotgunShellTimerFinished 里（那里才知道还有没有下一发）。
	Character->GetWorldTimerManager().SetTimer(
		ShotgunShellTimer,
		this,
		&UCombatComponent::ShotgunShellTimerFinished,
		Equipped->GetShotgunShellTime(),
		true
	);

	// 先立刻压一发，别让玩家干等一个间隔（按 R 之后第一发应该马上进去）。
	ShotgunShellTimerFinished();
}

void UCombatComponent::ShotgunShellTimerFinished()
{
	if (Character == nullptr || !Character->HasAuthority()) return;

	AWeapon* Equipped = Character->GetEquippedWeapon();
	if (Equipped == nullptr)
	{
		Character->GetWorldTimerManager().ClearTimer(ShotgunShellTimer);
		return;
	}

	// 状态已经不是"换弹中"了（阵亡、回合重置、或者别的路已经收过尾）→ 停掉循环，
	// 否则会一直往弹匣里塞子弹。
	if (Character->GetCombatState() != ECombatState::ECS_Reloading)
	{
		Character->GetWorldTimerManager().ClearTimer(ShotgunShellTimer);
		return;
	}

	// 压一发（压满 / 没备弹时它内部会自己 jump 到蒙太奇收尾段）
	UpdateShotgunAmmoValues();

	if (Equipped->IsFull() || Character->GetCarriedAmmo() == 0)
	{
		// 没有下一发了：停掉循环，留一点收招时间再离开"换弹中"（用 ReloadTimer 复用同一条）。
		Character->GetWorldTimerManager().ClearTimer(ShotgunShellTimer);
		Character->GetWorldTimerManager().SetTimer(
			ReloadTimer,
			this,
			&UCombatComponent::ReloadTimerFinished,
			Equipped->GetReloadRecoveryTime()
		);
	}
}

void UCombatComponent::UpdateAmmoValues()
{
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;
	int32 ReloadAmount = AmountToReload();
	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]-= ReloadAmount;
		Character->SetCarriedAmmo(CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]);
	}
	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
	Character->GetEquippedWeapon()->AddAmmo(ReloadAmount);	
}

void UCombatComponent::UpdateShotgunAmmoValues()
{
	UE_LOG(LogTemp,Warning,TEXT("ShotGunReload"))
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;
	if (Character->GetEquippedWeapon()->IsFull() || Character->GetCarriedAmmo() == 0)
	{
		//jump to shotgun section
		JumpToShotGunEnd();
		return;
	}

	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]-= 1;
		Character->SetCarriedAmmo(CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]);
	}
	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
	Character->GetEquippedWeapon()->AddAmmo(1);
	bCanFire = true;
	
	if (Character->GetEquippedWeapon()->IsFull() || Character->GetCarriedAmmo() == 0)
	{
		//jump to shotgun section
		JumpToShotGunEnd();
	}
}


void UCombatComponent::HandleReload()
{
	Character->PlayReloadMontage();

	// 第一人称手模的换弹动画（各武器蓝图自己挂 FPReloadMontage）。
	// 跟第三人称那条同步播：HandleReload 的两条来路（服务器 ServerReload、客户端 OnRep_CombatState）
	// 都正好是"换弹动作该开始"的那一刻，且都在本机跑 —— 不用再单独发 RPC。
	if (AWeapon* Equipped = Character->GetEquippedWeapon())
	{
		Equipped->PlayFPReloadMontage();
		// 枪自己那条换弹动画（ReloadAnimation，弹匣掉落/上膛）。这条**所有机器都播**。
		Equipped->PlayReloadAnimation();
	}
}

int32 UCombatComponent::AmountToReload()
{
	if (Character->GetEquippedWeapon()==nullptr) return 0;
	int RoomInMag =Character->GetEquippedWeapon() ->GetMagCapacity()-Character->GetEquippedWeapon()->GetAmmo();
	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		int AmmoCarried = CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()];
		int least = FMath::Min(RoomInMag, AmmoCarried);
		return FMath::Clamp(RoomInMag,0,least);
	}
	return 0;
}

void UCombatComponent::ServerThrowGrenade_Implementation()
{
	if (Character == nullptr) return;
	/*
	 * 空手蒙太奇那段不可打断。
	 *
	 * ★ 这条比 ServerReload 那条更**必须**：那边还有一个 EquippedWeapon==nullptr 的提前返回
	 *   兜着（空手时枪挂在背上），这里没有 —— 只要客户端硬发这条 RPC，服务器就会把状态
	 *   推到 ECS_ThrowingGrenade，空手那套收尾（EmptyHandFinish → 掏回最强的武器）
	 *   因为"状态已经不是 ECS_EmptyHand"直接走人，扔雷结束后人**永久空着手**。
	 *   客户端正常路径进不来（ThrowGrenade() 只认 ECS_Unoccupied），所以这条只在
	 *   "客户端和服务器的状态对不上"或改过的客户端上才会被走到。
	 */
	if (Character->IsEmptyHandLocked()) return;

	Character->SetCombatState(ECombatState::ECS_ThrowingGrenade);
	Character->PlayThrowGrenadeMontage();
	ShowAttachGrenade(true);
	AttachActorToLeftHand(Character->GetEquippedWeapon());

	// 扔雷什么时候算完，服务器计时器说了算（理由同换弹：动画通知换个骨架就没了，
	// 卡在 ECS_ThrowingGrenade 一样是开不了枪 + 切不了枪）。
	Character->GetWorldTimerManager().SetTimer(
		ThrowGrenadeTimer,
		this,
		&UCombatComponent::ThrowGrenadeTimerFinished,
		Character->GetThrowGrenadeDuration()
	);
}


void UCombatComponent::SetAiming(bool bIsAiming)
{
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;

	// 不能瞄准的武器（bCanAim=false）右键不进瞄准。
	// 只挡"进瞄准"这一边，松开右键那条路必须照走：否则在瞄准途中手上的枪被换掉/丢掉时
	// 会卡死在瞄准态 —— bAiming 一直是 true，移速一直是瞄准速度，而且再没有第二次
	// SetAiming(false) 的机会（那一路正好也被挡在这里了）。
	if (bIsAiming && !Character->GetEquippedWeapon()->CanAim()) return;

	if (Character->GetLocalRole() == ROLE_Authority)
	{
		Character->SetAiming(bIsAiming);
	}
	else
	{
		ServerSetAiming(bIsAiming);
	}

	// 移速在**两端**都要设，所以放在分支外面。
	// 以前只在 Authority 分支里设 → 客户端自己的 MaxWalkSpeed 永远停在 BeginPlay 那个值：
	// 瞄准时本地按 600 预测、服务器按 450 回放，两边算出来的位置对不上，一路被拉回来。
	// bIsAiming 用参数而不是 Character->IsAiming()：客户端那个值是等复制来的，慢一拍。
	ApplyMaxWalkSpeed(bIsAiming);

	if (Character->IsLocallyControlled() && Character->GetEquippedWeapon()->CanScope())
	{
		Character->ShowSniperScopeWidget(bIsAiming);
	}
}

void UCombatComponent::TraceUnderCrosshairs(FHitResult& TraceHitResult)
{
	FVector2D ViewportSize;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);

	}

	FVector2D CrosshairLocation(ViewportSize.X / 2.f, ViewportSize.Y / 2.f);
	FVector CrosshairWorldPosition;
	FVector CrosshairWorldDirection;
	bool bScreenToWorld = UGameplayStatics::DeprojectScreenToWorld(
		UGameplayStatics::GetPlayerController(this, 0),
		CrosshairLocation,
		CrosshairWorldPosition,
		CrosshairWorldDirection
	);
	
	if (bScreenToWorld)
	{
		FVector Start = CrosshairWorldPosition;

		if (Character)
		{
			float DistanceToCharacter = (Character->GetActorLocation()-Start).Size();
			Start+=CrosshairWorldDirection*(DistanceToCharacter+100.f);
		}
		FVector End = Start + CrosshairWorldDirection * TRACE_LENGTH;
		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(Character);
		GetWorld()->LineTraceSingleByChannel(
			TraceHitResult,
			Start,
			End,
			ECollisionChannel::ECC_Visibility,
			QueryParams
		);
		if (TraceHitResult.GetActor() && TraceHitResult.GetActor()->Implements<UInteractWithCrosshairsInterface>())
		{
			HUDPackage.CrosshairsColor=FLinearColor::Red;
		}
		else
		{
			HUDPackage.CrosshairsColor=FLinearColor::White;	
		}
	}
	if (TraceHitResult.bBlockingHit==false)
	{
		TraceHitResult.ImpactPoint=CrosshairWorldPosition+CrosshairWorldDirection*TRACE_LENGTH;
	}
}

void UCombatComponent::SetHUDCrosshairs(float DeltaTime)
{
	if (Character==nullptr)return;

	// ⚠ 这里原来写的是 `: nullptr`（课上的原版是 `: Controller`）—— 等于每帧把已缓存的 Controller
	// 清掉，于是这个函数隔一帧才真正跑一次（准星扩散/颜色/瞄准框都只按半速更新）。
	Controller = Controller==nullptr ? Cast<ABlasterPlayerController>(Character->GetController()): Controller;
	if (Controller)
	{
		HUD = HUD==nullptr ? Cast<ABlasterHUD>(Controller->GetHUD()):HUD;
		if (HUD)
		{
		
			if (Character->GetEquippedWeapon())
			{
				HUDPackage.CrosshairCenter=Character->GetEquippedWeapon()->CrosshairCenter;
				HUDPackage.CrosshairLeft=Character->GetEquippedWeapon()->CrosshairLeft;
				HUDPackage.CrosshairRight=Character->GetEquippedWeapon()->CrosshairRight;
				HUDPackage.CrosshairTop=Character->GetEquippedWeapon()->CrosshairTop;
				HUDPackage.CrosshairBottom=Character->GetEquippedWeapon()->CrosshairBottom;
			}
			else
			{
				HUDPackage.CrosshairCenter=nullptr;
				HUDPackage.CrosshairLeft=nullptr;
				HUDPackage.CrosshairRight=nullptr;
				HUDPackage.CrosshairTop=nullptr;
				HUDPackage.CrosshairBottom=nullptr;
			}
			// 瞄准镜框（ADS）：瞄准时递给 HUD，由它**叠在准星底下**画在屏幕中央。
			// 没配 AimTexture 的武器就是 nullptr —— HUD 那边什么都不画。
			// 颜色也一起递过去：框有自己的颜色（默认白），不跟准星一起变红。
			HUDPackage.AimTexture=nullptr;
			AWeapon* AimWeapon=Character->GetEquippedWeapon();
			if (Character->IsAiming() && AimWeapon && AimWeapon->GetAimTexture())
			{
				HUDPackage.AimTexture=AimWeapon->GetAimTexture();
				HUDPackage.AimTextureColor=AimWeapon->GetAimTextureColor();
			}

			// 狙击开镜：镜圈即准星，所以这时必须藏掉普通准星（否则屏幕中央两套准星）。
			// ⚠ 只有 CanScope() 这一种情况藏 —— 配了 AimTexture 的武器**不藏**：
			// 那个框是叠加的一层装饰，不是准星的替代品，准星该在还在（该变红还变红）。
			if (Character->IsAiming() && AimWeapon && AimWeapon->CanScope())
			{
				HUDPackage.CrosshairCenter=nullptr;
				HUDPackage.CrosshairLeft=nullptr;
				HUDPackage.CrosshairRight=nullptr;
				HUDPackage.CrosshairTop=nullptr;
				HUDPackage.CrosshairBottom=nullptr;
			}
			//Calculate crosshair spread

			//[0,600] -> [0,1]
			FVector2D WalkSpeedRange(0.f,Character->GetCharacterMovement()->MaxWalkSpeed);
			FVector2D VelocityMultiplierRange(0.f,1.f);
			FVector Velocity=Character->GetVelocity();
			Velocity.Z=0.f;

			CrosshairVelocityFactor = FMath::GetMappedRangeValueClamped(WalkSpeedRange,VelocityMultiplierRange,Velocity.Size());

			if (Character->GetCharacterMovement()->IsFalling())
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor,2.25f,DeltaTime,2.25);
			}
			else
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor,0.f,DeltaTime,30.f);
			}
			if (Character->IsAiming())
			{
				CrosshairAimFactor=FMath::FInterpTo(CrosshairAimFactor,0.58f,DeltaTime,30.f);
			}
			else
			{
				CrosshairAimFactor=FMath::FInterpTo(CrosshairAimFactor,0.f,DeltaTime,30.f);
			}

			CrosshairShootFactor=FMath::FInterpTo(CrosshairShootFactor,0.f,DeltaTime,40.f);
			
			HUDPackage.CrosshairSpread=
				0.5f+
				CrosshairVelocityFactor +
				CrosshairInAirFactor -
				CrosshairAimFactor +
				CrosshairShootFactor;
			
			HUD->SetHUDPackage(HUDPackage);	
			
		}
	}
}



void UCombatComponent::ServerSetAiming_Implementation(bool bIsAiming)
{
	Character->SetAiming(bIsAiming);
	ApplyMaxWalkSpeed(bIsAiming);
}

void UCombatComponent::InterpFOV(float DeltaTime)
{
	if (Character->GetEquippedWeapon()==nullptr) return;
	if (Character->IsAiming())
	{
		CurrentFOV=FMath::FInterpTo(CurrentFOV,Character->GetEquippedWeapon()->GetZoomFOV(),DeltaTime,Character->GetEquippedWeapon()->GetZoomInterpSpeed());
	}
	else
	{
		CurrentFOV=FMath::FInterpTo(CurrentFOV,DefaultFOV,DeltaTime,ZoomInterpSpeed);
	}
	if (Character && Character->GetFollowCamera())
	{
		Character->GetFollowCamera()->SetFieldOfView(CurrentFOV);
	}
}


