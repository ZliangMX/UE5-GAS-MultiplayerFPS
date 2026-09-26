// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Weapon/MeleeWeapon.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"

#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

AMeleeWeapon::AMeleeWeapon()
{
	// 3 号槽的武器。它决定了三件事（和 AJettKnives 完全一样的一串副作用）：
	//   · GetHolsterSocketForWeapon 会把它收回 MeleeHolsterSocket
	//   · DropAllWeapons（阵亡掉枪）不含近战槽 → 死一次不会把它掉在地上
	//   · EquipSlotWeapon 时 UpdateCarriedAmmo 找不到 EWT_Melee → 备用弹显示 0
	WeaponType = EWeaponType::EWT_Melee;

	// 刀没有"弹匣"，但 IsEmpty()/IsFull() 被 CanFire() 和整条换弹链路读着，
	// 两个都在头文件里覆盖成常量了。这里给个 1 只是让构造函数里有个自洽的值。
	MagCapacity = 1;
	Ammo = 1;

	// 轻击是"点一下砍一刀"，不是按住自动连砍；两刀之间的最短间隔（三段轻击和重击共用）。
	// ⚠ 这个值同时被服务器的射速校验用（UCombatComponent::ValidateServerFire 的令牌桶），
	//    所以**别在 Fire() 里按段改它** —— 段与段之间时长不一样时，
	//    服务器会拿上一段的间隔来判断这一刀，短的那一段会被判成"射速违规"直接毙掉（砍了不掉血）。
	//    要分段的节奏感，等真需要了再说，别在这里改 FireDelay。
	bAutomatic = false;
	FireDelay = 0.4f;

	// 刀没有后坐力：镜头不该因为挥刀往上飘
	RecoilPitch = 0.f;
	RecoilYaw = 0.f;
	MaxRecoilPitch = 0.f;

	// 不能右键瞄准 —— 右键被拿去当重击了（见 ABlasterCharacter::AimStart）
	bCanAim = false;
	ZoomedFOV = 90.f;

	// 不开服务器回溯：理由见头文件（两米内不需要补偿 ping，而且回溯那条路
	// 要在 ProcessServerRewindHit 里单独结算伤害，按段取伤害会更绕）。
	// 伤害由 MulticastFire 那一遍 Fire() 在服务器上结算（HasAuthority 门禁）。
	bUseServerSideRewind = false;

	// 刀不做爆头倍率（贴脸砍，命中哪根骨头很随机，翻倍会变成运气游戏）。
	// 想开就在 BP 里把倍率改回 2 并把 HeadBoneName 留着。
	HeadshotMultiplier = 1.f;

	// 三段轻击的默认条目。蒙太奇留空（要填的在 BP_Melee 里），伤害/射程先给一套能用的值 ——
	// 这样"先把 BP 建出来、动画回头再挂"也能真的砍死人，而不是一点反应都没有。
	LightCombo.SetNum(3);
	LightCombo[0].Damage = 50.f;
	LightCombo[1].Damage = 50.f;
	LightCombo[2].Damage = 75.f;   // 第三段收招重一点
	HeavyAttack.Damage = 100.f;
	HeavyAttack.Range = 220.f;

	/*
	 *不能被捡 —— 这一条从**构造**就开始拦。
	 *
	 *拾取提示（PickupWidget）和"按 E 捡枪"的入口全挂在 AreaSphere 的 overlap 上
	 *（AWeapon::OnSphereOverlap → Character->SetOverlappingWeapon）。球没碰撞 → 永远不 overlap
	 *→ 既不会有提示，也不会有可捡的引用。
	 *
	 *SetWeaponState(EWS_Dropped) 会在服务器上把球重新打开，所以必须**保证它永远不进 Dropped**：
	 *   · DropAllWeapons（阵亡）只掉主副武器
	 *   · DropEquippedWeapon（丢枪输入）看到 IsMeleeWeapon() 直接返回
	 *   · 另外 OnSphereOverlap 也空实现了，两道保险
	 */
	if (USphereComponent* Sphere = GetAreaSphere())
	{
		Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	ShowPickupWidget(false);
}

void AMeleeWeapon::SetHUDAmmo()
{
	// 负数 = HUD 上那个弹药数字不显示（见 ABlasterPlayerController::SetHUDWeaponAmmo）。
	// 基类那份逻辑是"把 Ammo 送下去"，对刀没有意义，所以整个覆盖掉而不是调 Super。
	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(OwnerCharacter->GetController()))
	{
		PC->SetHUDWeaponAmmo(-1);
	}
}

void AMeleeWeapon::OnSphereOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	// 空实现：刀不能被捡。
	// 正常路径下这个函数根本不会被调到（构造里把 AreaSphere 的碰撞关了），
	// 留着是防止将来有人给近战武器的球重新打开碰撞 —— 那时也不会冒出一个"按 E 捡刀"的提示。
}

void AMeleeWeapon::Fire(const FVector& HitTarget)
{
	// 这一次挥刀是不是重击。读完立刻清掉 —— 它是"这一刀"的属性，不是武器的持久状态。
	// 设它的地方有三处，各自只在自己那台机器上有效（见 RequestHeavyAttack 的注释）：
	//   · 本机（客户端 / listen server 房主）：CombatComponent::TryMeleeHeavyAttack
	//   · 服务器：ServerFire_Implementation 收到 bMeleeHeavy=true 时设
	//   · 其他客户端：MulticastFire_Implementation 的 bMeleeHeavy 参数
	const bool bHeavy = bHeavyAttackNextShot;
	bHeavyAttackNextShot = false;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	if (bHeavy)
	{
		// 重击单独一段，不出连击链；挥完之后连击也从第一段重新开始
		ComboIndex = 0;
		LastLightAttackTime = Now;
		PerformAttack(HeavyAttack, HitTarget);
		return;
	}

	// 断连击：上一刀之后隔太久（ComboResetWindow）→ 回到第 1 段
	if (Now - LastLightAttackTime > ComboResetWindow)
	{
		ComboIndex = 0;
	}

	// 复制一份再挥：LightCombo 是蓝图可改的数组，取引用万一半路被改会读到脏数据。
	// 数组被清空时就是一个默认段（伤害 50 / 射程 200），不会崩、也不会挥空。
	FMeleeAttackData AttackData;
	if (LightCombo.IsValidIndex(ComboIndex))
	{
		AttackData = LightCombo[ComboIndex];
	}

	PerformAttack(AttackData, HitTarget);

	// 三段砍完回到第一段：继续按左键就是从头再砍一轮（不是"卡在第三段"）
	const int32 StageCount = FMath::Max(1, LightCombo.Num());
	ComboIndex = (ComboIndex + 1) % StageCount;
	LastLightAttackTime = Now;
}

void AMeleeWeapon::PerformAttack(const FMeleeAttackData& AttackData, const FVector& HitTarget)
{
	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter)
	{
		/*
		 *① 手模挥刀 —— 玩家自己看到的动作。
		 *   走角色那边的 PlayFPArmsMontage（和武器的 PlayFPXxxMontage 最终是同一个入口）：
		 *   它内部判空，并**只在本机播**（远端角色的手模既不渲染也不 tick）。
		 *   蒙太奇必须是 ABP_WushuFP 那个 Slot 节点接得住的 Montage。
		 */
		OwnerCharacter->PlayFPArmsMontage(AttackData.Montage);

		/*
		 *② 第三人称身体挥刀 —— 别人屏幕上看到的动作。
		 *   和枪的 FireAnimation 同一个定位：**所有机器都播**，所以不能像手模那样只在本机播。
		 *   ⚠ 这里**不能**直接调 AWeapon::PlayAnimAssetOnMesh —— 那条路对序列走的是
		 *     USkeletalMeshComponent::PlayAnimation，会把身体的动画蓝图顶掉而且不还原。
		 *     理由和做法见 PlayBodyAttackAnimation。
		 *   （身体那条通用的 FireWeaponMontage 已经被 ShouldPlayBodyFireMontage() 挡掉了，
		 *   否则两条动画会抢同一个槽位、挥刀被步枪后坐顶掉。）
		 */
		PlayBodyAttackAnimation(AttackData.ThirdPersonAnimation);
	}

	/*
	 *③ 挥刀的声音（走基类的 FireSound）。
	 *   只调 PlayFireEffects 这一半：它本来是"枪口火光 + 枪声"，刀的 MuzzleFlash 留空
	 *   （武器上没有枪口 socket，也不会有人填），所以实际效果就是"出声不出火光"。
	 *   基类 AWeapon::Fire 里那一半（枪自己的开火动画 / 抛壳 / SpendRound）对刀没有意义，
	 *   所以这里整个不走 Super::Fire。
	 *
	 *   不会重复播：CombatComponent 的 MulticastFire 在射手自己的机器上是直接 return 的
	 *   （本地预测那一遍已经播过），所以每台机器最多走到这里一次。
	 */
	PlayFireEffects();

	/*
	 *④ 命中判定 + 结算。
	 *   每台机器都跑一遍：服务器那一遍真的扣血（HasAuthority 门禁），
	 *   射手本机那一遍出命中标记/伤害数字（本地预测，不等 RTT）。
	 */
	FHitResult Hit;
	if (MeleeTraceHit(AttackData, HitTarget, Hit))
	{
		ApplyMeleeHit(AttackData, Hit);
	}
}

void AMeleeWeapon::PlayBodyAttackAnimation(UAnimationAsset* Anim)
{
	if (Anim == nullptr) return;

	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	USkeletalMeshComponent* BodyMesh = OwnerCharacter->GetMesh();
	if (BodyMesh == nullptr) return;

	UAnimInstance* AnimInstance = BodyMesh->GetAnimInstance();

	/*
	 *⚠ 这一整段绕的就是引擎里那个坑：
	 *
	 *USkeletalMeshComponent::PlayAnimation（也就是 AWeapon::PlayAnimAssetOnMesh 对序列走的那条）
	 *内部第一步是 SetAnimationMode(EAnimationMode::AnimationSingleNode)。而 SetAnimationMode 在
	 *mode 变化时会 ClearAnimScriptInstance() 再重新建一个 —— 但它是**按新模式**建的：
	 *单节点模式下建出来的是 UAnimSingleNodeInstance（引擎 SkeletalMeshComponent.cpp:1033），
	 *挂在角色身上的 ABP_Blaster 实例就此被销毁。更麻烦的是 AnimationMode 停在单节点上再也回不去了
	 *（NeedToSpawnAnimScriptInstance 要求 mode == AnimationBlueprint 才肯重建蓝图实例）。
	 *
	 *结果不是"这一刀没动画"这么轻 —— 是**第一次挥刀之后这个角色永久僵住**：
	 *走路/跳跃/受击/死亡全都不播，定格在挥刀最后一帧。而且是所有看得到这个角色的机器一起僵。
	 *
	 *所以只要身体挂着动画蓝图，就必须走动画实例的 Slot（和 ABlasterCharacter::PlayFireMontage
	 *播 Mon_FireWeapon 完全同一条路）：混合进出、可被打断、动画蓝图照常跑。
	 */
	if (AnimInstance)
	{
		if (UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
		{
			// 已经是蒙太奇 → 直接播。它自带创建时选的 Slot 名，BodyAttackSlot 对它不生效。
			AnimInstance->Montage_Play(Montage);
		}
		else if (UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Anim))
		{
			/*
			 *裸序列 → 引擎把它临时包成一个"动态蒙太奇"丢进 BodyAttackSlot 那个 Slot 节点，
			 *所以**不用**为第三人称动画专门做蒙太奇资产（三段轻击+重击就是四条）。
			 *
			 *混合时间故意很短（0.05 进 / 0.1 出）：三段轻击最快 0.4 秒一刀，用默认的 0.25/0.25
			 *会让每一刀有一半时间花在混合上，看起来像没挥出去。
			 */
			AnimInstance->PlaySlotAnimationAsDynamicMontage(
				Sequence,
				BodyAttackSlot,
				/*BlendInTime=*/0.05f,
				/*BlendOutTime=*/0.1f,
				/*InPlayRate=*/1.f,
				/*LoopCount=*/1
			);
		}
		return;
	}

	/*
	 *身体网格**没有**动画蓝图（纯单节点网格，比如某些测试用模型）→ 单节点播放是唯一选择，
	 *而且这时候也不存在"动画蓝图被顶掉"的问题。这一条和 AWeapon::PlayAnimAssetOnMesh 等价。
	 */
	BodyMesh->PlayAnimation(Anim, false);
}

bool AMeleeWeapon::MeleeTraceHit(const FMeleeAttackData& AttackData, const FVector& HitTarget, FHitResult& OutHit) const
{
	UWorld* World = GetWorld();
	if (World == nullptr) return false;

	/*
	 * 起点和子弹一样是**眼睛**（理由见 AHitScanWeapon::GetShotOrigin 那段长注释）：
	 * 准星是从相机投出去的，射线从武器/枪口起算的话，近距离会多出一段基线，
	 * 表现就是"准心明明对着人却砍空"。
	 *
	 * 本机（本地预测）和权威机走眼位 —— 权威机上这一遍要给出命中反馈，必须和射手看到的是同一条线；
	 * 旁观者机器这一遍纯粹是表现（没人结算伤害），从武器位置起算就够了。
	 */
	FVector Start = GetActorLocation();
	if (const ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner()))
	{
		if (OwnerCharacter->IsLocallyControlled() || HasAuthority())
		{
			Start = OwnerCharacter->GetFPEyeWorldLocation();
		}
	}

	// HitTarget 是准星那条射线上的一个点（可能在一百米开外），只借它的**方向**，
	// 距离截到这一刀自己的 Range —— 刀就是这么短。
	const FVector End = Start + (HitTarget - Start).GetSafeNormal() * AttackData.Range;

	// 忽略自己 + 手上这把刀：眼位在身体里面，刀就悬在眼前，不忽略的话第一下砍中的是自己
	FCollisionQueryParams Params(TEXT("MeleeTrace"), /*bTraceComplex=*/false, GetOwner());
	Params.AddIgnoredActor(this);

	if (AttackData.Radius > 0.f)
	{
		// Radius > 0 → 球扫，手感宽松一点（准心旁边一点的目标也能砍到）
		return World->SweepSingleByChannel(
			OutHit, Start, End, FQuat::Identity,
			ECollisionChannel::ECC_Visibility,
			FCollisionShape::MakeSphere(AttackData.Radius),
			Params);
	}

	return World->LineTraceSingleByChannel(
		OutHit, Start, End, ECollisionChannel::ECC_Visibility, Params);
}

void AMeleeWeapon::ApplyMeleeHit(const FMeleeAttackData& AttackData, const FHitResult& Hit)
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	AController* InstigatorController = OwnerPawn ? OwnerPawn->GetController() : nullptr;
	if (InstigatorController == nullptr) return;

	// 砍到墙/箱子：不出任何反馈（和子弹打墙不一样，近战打墙没必要出命中音）
	ABlasterCharacter* HitCharacter = Cast<ABlasterCharacter>(Hit.GetActor());
	if (HitCharacter == nullptr) return;

	const float FinalDamage = GetDamageForHit(AttackData.Damage, Hit);

	// ——伤害：只在服务器结算——
	// 客户端那一遍（本地预测）走到这里没有 authority，什么都不做；
	// 服务器那一遍是 MulticastFire 里跑的 Fire()，所以 HasAuthority 门禁就够
	//（近战没开回溯，不需要 ProcessServerRewindHit 那一路）。
	if (HasAuthority())
	{
		const float HealthBefore = HitCharacter->GetHealth();
		UGameplayStatics::ApplyDamage(
			HitCharacter,
			FinalDamage,
			InstigatorController,
			this,
			UDamageType::StaticClass()
		);

		// 击杀确认音效：这一刀把目标砍死（之前血量>0、现在<=0）→ 给击杀者客户端播"叮"
		if (HitCharacter->GetHealth() <= 0.f && HealthBefore > 0.f)
		{
			if (ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(InstigatorController))
			{
				const bool bHeadshot = IsHeadshot(Hit);
				if (KillerPC->IsLocalController())
				{
					KillerPC->PlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
				}
				else
				{
					KillerPC->ClientPlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
				}
			}
		}
	}

	// ——表现：命中特效 + 命中音效——
	if (HitParticles)
	{
		UGameplayStatics::SpawnEmitterAtLocation(
			GetWorld(),
			HitParticles,
			Hit.ImpactPoint,
			Hit.ImpactNormal.Rotation()
		);
	}
	if (HitSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, HitSound, Hit.ImpactPoint);
	}

	// ——命中反馈：准心闪一下 + 命中点飘伤害数字——
	// 只有打到**不同队的敌人**才出（砍队友一直闪会让"有没有砍中敌人"变得没法判断）。
	// 本机（射手客户端）直接画，服务器转发给射手客户端；旁观者机器什么都不做。
	// 本地预测会在射手这边先出一次，所以服务器只给"不是本机"的射手转发（IsLocalController 分支）。
	// 判定写法和 AHitScanWeapon::ProcessHit 完全一致，包括对测试机器人的特例。
	ABlasterPlayerState* ShooterPS = OwnerPawn ? OwnerPawn->GetPlayerState<ABlasterPlayerState>() : nullptr;
	ABlasterPlayerState* HitPS = HitCharacter->GetPlayerState<ABlasterPlayerState>();
	if (ShooterPS && ((HitPS && HitPS != ShooterPS && HitPS->Team != ShooterPS->Team) || HitCharacter->IsTestBot()))
	{
		if (ABlasterPlayerController* ShooterPC = Cast<ABlasterPlayerController>(InstigatorController))
		{
			if (ShooterPC->IsLocalController())
			{
				ShooterPC->ShowHitMarker();
				ShooterPC->ShowDamageNumber(FinalDamage, Hit.ImpactPoint);
			}
			else if (HasAuthority())
			{
				ShooterPC->ClientShowHitMarker();
				ShooterPC->ClientShowDamageNumber(FinalDamage, Hit.ImpactPoint);
			}
		}
	}
}
