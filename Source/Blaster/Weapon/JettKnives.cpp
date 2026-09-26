// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Weapon/JettKnives.h"

// 数身体那条蒙太奇有几个段 / 按序号取段名（PlayThrowPresentation 里"两段来回交替"要用）
#include "Animation/AnimMontage.h"
#include "Components/SphereComponent.h"

#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Character/Agents/JettCharacter.h"
#include "WeaponTypes.h"

AJettKnives::AJettKnives()
{
	/*
	 * 飞刀**占近战槽**，但类型是 EWT_BladeStorm，和普通刀的 EWT_Melee 分开。
	 *
	 * 分开是为了第一人称动画状态机 —— 它按 WeaponType 换状态，飞刀要一套自己的 idle/移动。
	 * 槽位那几条行为不会因此丢掉：收回 MeleeHolsterSocket / 不能丢 / 不能被捡 / 跑得最快
	 * 都挂在 AWeapon::IsMeleeWeapon() 上，它认这两个值。
	 */
	WeaponType = EWeaponType::EWT_BladeStorm;

	// 刀数就是弹匣。默认 5 把（Valorant 刃风暴也是 5 把）。
	// ⚠️ 两个都要设：MagCapacity 是上限，Ammo 是当前值；武器默认 Ammo=0，
	//    不改的话一掏出刀就是"空仓"，扔不出来而且什么都不报。
	MagCapacity = 5;
	Ammo = 5;

	// 右手一把一把往外甩，不是连发
	bAutomatic = false;
	FireDelay = 0.25f;

	// 飞刀伤害：身体 50 / 头 150 —— 也就是 Damage 50 + 爆头倍率 3。
	// 这两个都是基类（AHitScanWeapon::Damage / AWeapon::HeadshotMultiplier）上的属性，
	// BP 面板里照样能改。
	// 以前是本类自己的 KnifeDamage/KnifeHeadshotMultiplier，因为那时候伤害是"生成投射物时灌进去"的
	//（投射物飞在路上时射手可能已经切回枪，读不到射手手上的武器）。改成射线之后判定就在同一帧、
	// 同一个对象里完成，直接走基类的 GetDamageForHit 就行，重复的属性删掉了 ——
	// 免得 BP 上出现两个"伤害"，改了一个另一个不生效。
	Damage = 50.f;
	HeadshotMultiplier = 3.f;

	// 飞刀没有后坐力：镜头不该因为扔刀往上飘
	RecoilPitch = 0.f;
	RecoilYaw = 0.f;
	MaxRecoilPitch = 0.f;

	// 不开镜、也不能瞄准 —— 右键被拿去当"全扔"了
	//（见 ABlasterCharacter::AimStart 里的 TryThrowAllKnives）
	bCanAim = false;
	ZoomedFOV = 90.f;

	// 不开服务器回溯。两条理由：
	//   · 单扔：伤害由 MulticastFire 那一遍 Fire() 在服务器上结算（AHitScanWeapon::ProcessHit
	//     里那条 !bUseServerSideRewind 的门禁），和 AMeleeWeapon 同一条路；
	//   · 全扔：一次好几条射线，而回溯那条路一次只能带**一个**终点，装不下。
	// 相应地 AppliesOwnDamage() 上报 true，让 UCombatComponent::ServerFire 知道这条路有人结算，
	// 不会掉进"bUseServerSideRewind=false → 远端客户端没人结算伤害"那条假警告里。
	bUseServerSideRewind = false;

	/*
	 * 不能被捡。拾取提示和"按 E 捡枪"的入口全挂在 AreaSphere 的 overlap 上
	 *（AWeapon::OnSphereOverlap → Character->SetOverlappingWeapon），球没碰撞就永远不会 overlap。
	 *
	 * IsMeleeWeapon() 已经让 DropEquippedWeapon（丢枪输入）和 EquipWeapon（捡枪）两处直接返回了，
	 * 这里再把球关掉是第二道保险（和 AMeleeWeapon 的构造一模一样）。
	 * 刀永远不进 EWS_Dropped 状态，而只有那个状态会重新打开碰撞。
	 */
	if (USphereComponent* Sphere = GetAreaSphere())
	{
		Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	ShowPickupWidget(false);
}

bool AJettKnives::IsFull()
{
	// 覆盖 AWeapon::IsFull()（Ammo == MagCapacity）。
	// 语义上也对 —— 飞刀靠击杀刷新、靠大招收回，本来就没有"补弹"这个概念，恒真让整条换弹链路
	//（CanReload / Reload / AmountToReload / PlayReloadMontage）自己短路掉，按 R 什么都不发生。
	return true;
}

void AJettKnives::Fire(const FVector& HitTarget)
{
	// 这一次开火是不是"全扔"。读完立刻清掉：它是"这一发"的属性，不是武器的持久状态。
	// 设它的地方有两处，各自只在自己那台机器上有效（见 RequestThrowAll 的注释）：
	//   · 客户端/房主：CombatComponent::Fire() 之前由 TryThrowAllKnives 设
	//   · 服务器：ServerFire_Implementation 收到 bThrowAll=true 时设
	const bool bThrowAll = bThrowAllNextShot;
	bThrowAllNextShot = false;

	if (bThrowAll)
	{
		ThrowAllKnives(HitTarget);
	}
	else
	{
		ThrowOneKnife(HitTarget);
	}
}

void AJettKnives::ThrowOneKnife(const FVector& HitTarget)
{
	/*
	 * 一把刀 = 一条射线。判定的每一环都在基类里，这里一行不抄：
	 *
	 *   AHitScanWeapon::Fire
	 *     → AWeapon::Fire        开火特效（枪口火光/枪声/手模开火蒙太奇）+ 末尾 SpendRound() 扣一把刀
	 *     → WeaponTraceHit       从本机眼位（GetShotOrigin）沿准心方向打一条 ECC_Visibility
	 *     → ProcessHit           伤害 / 爆头倍率 / 击杀确认音 / 命中特效 / 命中标记 + 伤害数字
	 *
	 * 起点是本机眼位、和 UCombatComponent::TraceUnderCrosshairs 那条准星射线同源，
	 * 所以"准星指着谁"和"打中谁"是同一件事（理由见 AHitScanWeapon::GetShotOrigin 的注释）。
	 *
	 * ⚠️ 别改成 "Super::Fire" 之外的写法去自己扣刀 —— AWeapon::Fire 末尾那句 SpendRound()
	 *    同时管着 HUD 上的刀数，绕开它就会出现"手上少了一把、HUD 还写着 5"。
	 */
	Super::Fire(HitTarget);

	// 攻击动画：第几次扔 → 第几段。
	// 顺序必须在 Super::Fire **之后** —— Ammo 是在那里扣的，而"这是第几次扔"正是从
	// 扣完之后的 Ammo 反推的（5 把刀时第一扔扣成 4 → 5-4=1 → 第 1 段）。
	// 放前面的话永远是"第 1 次"，五段动画都跳同一段。
	const int32 ThrowIndex = FMath::Clamp(MagCapacity - Ammo, 1, FMath::Max(MagCapacity, 1));
	PlayThrowPresentation(ThrowIndex);

	// 这一把可能就是最后一把 → 刃风暴到此为止。
	// 不需要像以前那样额外等"天上飞的刀落定"：射线武器的命中判定就在上面那一行里，
	// 打死了谁、有没有触发击杀刷新（Ammo 补回 5）在这一刻全都已经定下来了。
	NotifyIfDepleted();
}

void AJettKnives::ThrowAllKnives(const FVector& HitTarget)
{
	if (IsEmpty()) return;

	/*
	 * 全扔**不走 Super::Fire**：它扔且只扔一把、扣且只扣一把刀。
	 * 这里自己把该做的补齐 —— 开火表现照播（手感），射线和伤害按霰弹枪那套逐把打。
	 *
	 * 不用 AWeapon::Fire 中间那段表现的原因是它末尾带着 SpendRound()，
	 * 调到它就会白扣一把刀（这一路的计数是后面自己管的）。
	 */
	PlayFireEffects();

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	AController* InstigatorController = OwnerPawn ? OwnerPawn->GetController() : nullptr;

	// 起点和单扔走同一个入口（本机 = 第一人称眼位，其他机器 = 看得见的枪口）。
	// 所有刀都从这个点散出去 —— 和准星那条射线共起点，近距离才不会整片打偏。
	const FVector Start = GetShotOrigin();

	const FVector ToTarget = HitTarget - Start;
	const float Range = ToTarget.Size();
	const FRotator BaseRotation = ToTarget.Rotation();

	/*
	 * 先记下要扔几把、然后**立刻清零**。
	 *
	 * 清零必须在伤害结算**之前**：下面每一刀都可能打死人、触发击杀刷新（Ammo 补回 5 把）。
	 * 先清零再结算，刷新才留得住 —— "最后一把刀杀到人了，刀就该回来、大招继续"。
	 * 反过来（先结算再清零）的话，人明明杀死了、刀也确实补回来了，紧接着被这一行抹掉，
	 * 表现就是"杀了人刀还是没了、大招照样收"。
	 */
	const int32 Count = Ammo;
	SetAmmo(0);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		FVector ShotTarget = HitTarget;

		// 绕准心方向抖一个锥角。第一把（Index 0）不偏 —— 保证"准心指着谁，至少有一把扎谁"。
		if (ThrowAllSpreadDegrees > 0.f && Index > 0)
		{
			FRotator ShotRotation = BaseRotation;
			ShotRotation.Pitch += FMath::FRandRange(-ThrowAllSpreadDegrees, ThrowAllSpreadDegrees);
			ShotRotation.Yaw += FMath::FRandRange(-ThrowAllSpreadDegrees, ThrowAllSpreadDegrees);
			ShotTarget = Start + ShotRotation.Vector() * Range;
		}

		FHitResult FireHit;
		WeaponTraceHit(Start, ShotTarget, FireHit);

		/*
		 * 每把刀各自走一遍**完整的**命中结算（伤害 / 爆头 / 命中特效 / 命中标记 / 击杀确认音 /
		 * "打死了就刷新刀数"）—— 直接复用 ProcessHit，不再自己抄一遍。
		 *
		 * 霰弹枪那边是攒一张伤害表一次性结算的，理由是它有十几颗钢珠、一次性结算省事。
		 * 飞刀不照抄：最多 5 把，而且每把都该是独立的一刀（独立算爆头、独立触发击杀刷新），
		 * 逐把走现成那条路最短，也不会漏掉击杀确认音/刷新刀数这些分支。
		 */
		if (InstigatorController)
		{
			ProcessHit(FireHit, InstigatorController, /*bRewindConfirmation=*/false);
		}
	}

	/*
	 * 手上的刀跟着消失。
	 *
	 * 单扔时这一步是**动画通知**（UAnimNotify_KnifeConsumed，挂在刀骨骼那条 KnifeAttack 上）
	 * 在"刀脱手"那一帧去角色那边同步的 —— 右键这条路**没有**那条动画了
	 *（用户："右键刀是没有动画的，只有 fp 和 tp"），通知不会来，所以必须在这里手动推一次。
	 *
	 * 少了这一句的表现：刀一把把飞出画面、伤害也结算了，**但 5 把小刀还全挂在身上**——
	 * 直到下一次 Ammo 复制（OnRep_Ammo）才突然一起消失。而且只在"还剩刀"时看得出来，
	 * 最后一把扔完本来就要收招、整套隐藏，很容易被当成"没问题"。
	 *
	 * 位置必须在循环**之后**：循环里每一刀都可能触发击杀刷新（Ammo 补回 5），
	 * 而 SyncKnivesToAmmo 读的是当前 Ammo —— 放前面的话，补回来的刀会被这次同步抹掉。
	 */
	if (AJettCharacter* Jett = Cast<AJettCharacter>(GetOwner()))
	{
		Jett->SyncKnivesToAmmo();
	}

	// 表现：手上一次全空（或者刚才杀到人、已经补回来了 —— SyncKnivesToAmmo 读的就是 Ammo，
	// 两种情况它都算得对）。
	PlayThrowAllPresentation();

	// 一把不剩（且没被击杀刷新）→ 刃风暴结束
	NotifyIfDepleted();
}

void AJettKnives::PlayThrowAllPresentation()
{
	/*
	 * 右键"一次全扔"的动画：两条链各播各的，各有各的字段（都带"留空就退回老行为"）：
	 *
	 *   手模（FP）    AWeapon::FPRightClickMontage          没配 → FPFireMontage 从头播
	 *   第三人称身体  AWeapon::ThirdPersonRightClickMontage 没配 → 身体什么都不播
	 *
	 * **没有刀骨骼（AB）那条** —— 用户 2026-09-18 明确"右键刀是没有动画的，只有 fp 和 tp"。
	 * 曾经有过（AJettCharacter::KnifeThrowAll，没配就退回 KnifeAttack 从头播），已经删掉：
	 * 既然右键**永远**不该出刀骨骼动画，"没配就退回老行为"这个退路本身就是错的 ——
	 * 任何一次漏配都会让右键无端播一段单扔的挥刀。少一条链，少一处能配错的地方。
	 *
	 * 为什么不像单扔那样共用"第 N 段"那一条：一次出去好几把，本来就没有"第 N 次"这个说法；
	 * 而且瓦的资产里右键是**单独一条**动作（手模和身体两套资产都叫 RightClickAttack），
	 * 拿第 1 段顶着只是没配时的退路，不是它该有的样子。
	 *
	 * 两条互相之间不会抢槽 —— 它们打在两块不同的网格上（手模骨架 / 身体骨架）。
	 *
	 * ⚠ 身体那条有个前提：本类现在 ShouldPlayBodyFireMontage() 返回
	 *   "配了 ThirdPersonFireMontage 没有"（见 .h），所以单扔和全扔**都会**先由
	 *   UCombatComponent::Fire 播一遍通用开火蒙太奇。全扔这一路上，下面这句会在同一帧里
	 *   把它顶掉（PlayAnimAssetOnInstance 走的是 bStopAllMontages=true）—— 通用那条实际
	 *   只播了 0 帧，看不出闪，但日志/动画通知要是对不上号，记得这里播过两条。
	 */
	PlayFPRightClickMontage();
	PlayThirdPersonRightClickMontage();
}

void AJettKnives::PlayThrowPresentation(int32 ThrowIndex)
{
	// 刀骨骼那条：跳到第 ThrowIndex 段（段名默认就是序号 "1".."5"，见 AJettCharacter 上的注释）。
	// 显隐不在这里动 —— 那是动画通知（UAnimNotify_KnifeConsumed）在"刀脱手"那一帧干的事。
	if (AJettCharacter* Jett = Cast<AJettCharacter>(GetOwner()))
	{
		Jett->PlayKnifeAttackMontage(ThrowIndex);
	}

	/*
	 * 第一人称手模那条（FPFireMontage）也跳到同一段。
	 *
	 * AWeapon::Fire 里已经把它从头播起来了（"连发时每枪从第一帧重播"）——
	 * 手模只有一条挥刀动作时，从头播就是对的（这里跳段找不到同名段，什么都不做）；
	 * 手模也做了 5 段的话，段名起成 "1".."5" 就自动对上了。
	 * 段名走 AJettCharacter::MakeKnifeAttackSectionName 这一个约定，刀骨骼那条用的是同一个 ——
	 * 不这么做就得在 BP 里配两份段名，迟早对不上。
	 *
	 * ⚠ **但段名对齐不代表资产能共用**：FPFireMontage 必须是**手模骨架**
	 *（FP_Wushu_S0_Skeleton）的蒙太奇。上面那条 KnifeAttack 是刀骨架
	 *（AB_Wushu_S0_X_Skeleton）的，两者不能互换 —— 把 AB_Wushu_S0_X_Attack
	 * 填进 FPFireMontage 会让手模的 Skeleton/Root 两根同名骨被改写成刀骨架的值（差 120°），
	 * 而第一人称相机挂在手模的 Camera 骨上，表现就是**整个视角被拧转**（见 Weapon.h 上四条
	 * 手模字段的注释，以及 ABlasterCharacter::IsMontageCompatibleWithMesh 的拦截）。
	 */
	JumpFPFireMontageToSection(AJettCharacter::MakeKnifeAttackSectionName(ThrowIndex));

	/*
	 * 第三人称身体那条（ThirdPersonFireMontage）—— 用户要求"单扔也要播 tp 动画，而且也是 5 段对应"。
	 *
	 * 它和手模那条是同一个段名约定（"1".."5"），但打在两块不同的网格上：
	 * 手模打在 FPArmsMesh 上、身体打在 GetMesh() 上，所以两条不会互相顶掉，同时播是对的。
	 *
	 * 这条蒙太奇**是这里第一次跳段的**：
	 *   · UCombatComponent::Fire 里那次 PlayFireMontage 只是"从头播"（第 1 段）——
	 *     它拿不到"这是第几次扔"，所以那一步只能起头，跳段得在这儿；
	 *   · 跳段必须等扣完刀：ThrowIndex 是从 SpendRound **之后**的 Ammo 反推的（见 ThrowOneKnife）。
	 *   · 本类现在 ShouldPlayBodyFireMontage() 返回 true（见 .h），那个"起头"这一步才会发生；
	 *     万一有人把它改回 false，这里 Montage_IsPlaying 判假 → 这一次跳段静默失效、
	 *     身体**整场都不会动**（不是只跳错段）。要查"身体不播"就先看那个返回值。
	 *
	 * ⚠ 段数按**资产实际情况**取，不按 ThrowIndex 硬跳。
	 *   TP 那套资产只导出了两条攻击片段（TP_Wushu_S0_X_Attack01_UB / Attack02_UB），
	 *   所以 TP_Wushu_S0_X_Attack_UB_Montage **只有 2 段**（段名就是 "1"/"2"）。
	 *   硬跳 "3" 的话第 3/4/5 次扔刀会各刷一条"找不到段"、身体停在第 1 段 —— 看起来像没效果。
	 *   用户 2026-09-18 定的做法是"**两段来回交替**"，所以这里按段数取模循环：
	 *   2 段时就是 1,2,1,2,1。哪天把这条蒙太奇补到 5 段，"取模"自动退化成恒等映射，代码不用动
	 *   （所以**不需要**为了凑五段去改资产）。
	 *   AB / 手模那两条各有 5 段，本来就是 1:1，不走这一下。
	 */
	if (const UAnimMontage* BodyMontage = GetThirdPersonFireMontage())
	{
		const int32 NumSections = BodyMontage->GetNumSections();
		if (NumSections > 0)
		{
			// 段名从蒙太奇上**取**而不是按 "1".."5" 拼：段名万一起成别的也不至于变成静默失败。
			const int32 SectionIndex = (ThrowIndex - 1) % NumSections;
			JumpThirdPersonFireMontageToSection(BodyMontage->GetSectionName(SectionIndex));
		}
	}
}

void AJettKnives::ProcessHit(const FHitResult& FireHit, AController* InstigatorController, bool bRewindConfirmation)
{
	// 结算**之前**抓一把血量 —— "这一刀打死人了没"只能靠前后对比判断
	//（基类里那段击杀确认音用的也是同一个手法，它就在下面 Super 里）。
	ABlasterCharacter* Victim = Cast<ABlasterCharacter>(FireHit.GetActor());
	const float HealthBefore = Victim ? Victim->GetHealth() : 0.f;

	// 伤害、爆头倍率、击杀确认音、命中特效、命中标记、伤害数字 —— 全在基类那一份里，一行都不抄
	Super::ProcessHit(FireHit, InstigatorController, bRewindConfirmation);

	// 打死了 → 把刀补回 5 把（用户要求"杀了人刷新回 5 个飞镖"）
	RefillIfKilled(Victim, HealthBefore);
}

void AJettKnives::RefillIfKilled(ABlasterCharacter* Victim, float HealthBefore)
{
	if (Victim == nullptr) return;

	// 结算前就没血了 = 打的是一具尸体，不算击杀
	if (HealthBefore <= 0.f) return;
	if (Victim->GetHealth() > 0.f) return;

	// 剩下的门禁（HasAuthority）在 RefillKnives 里 —— 客户端也会走到这里
	//（Victim 的血量是复制过来的，在客户端上"刚好在这一帧看到 0"纯属巧合），
	// 但那边什么都不该改，改的是服务器的权威值。
	RefillKnives();
}

void AJettKnives::RefillKnives()
{
	if (!HasAuthority()) return;

	// 击杀刷新 = 补满，不是 +1（用户要求）。
	// 用 SetAmmo 而不是直接写 Ammo：它带 Clamp 和 SetHUDAmmo（HUD 刀数立刻更新）。
	SetAmmo(MagCapacity);

	// 权威机改自己的 Ammo **不会**触发 OnRep_Ammo（OnRep 只在复制到达时跑）→ 手动推一把
	// 飞刀的显隐。客户端那边由 OnRep_Ammo 推。
	if (AJettCharacter* Jett = Cast<AJettCharacter>(GetOwner()))
	{
		Jett->SyncKnivesToAmmo();
	}
}

void AJettKnives::OnRep_Ammo()
{
	// 不调 Super。基类那份里唯一有内容的是"满了就 JumpToShotGunEnd()"，那是霰弹枪专用的
	//（一发一发压完，跳到收尾段）—— 而飞刀这里 IsFull() 恒真，于是每次弹药复制都会去跳一遍
	// 身体蒙太奇，白白刷一条 "JumpToEnd" 警告。只有 SetHUDAmmo() 这一句是两种武器都要的，照抄。
	SetHUDAmmo();

	// 刀数变了 → 同步飞刀的显隐。两个场景都会走到这里：
	//   · 击杀刷新（0 → 5）：服务器上直接推了，客户端靠这条复制
	//   · 服务器把客户端本地预测的刀数纠正回来
	// SyncKnivesToAmmo 是幂等的（读的是当前 Ammo 现算），重复调没有副作用。
	if (AJettCharacter* Jett = Cast<AJettCharacter>(GetOwner()))
	{
		Jett->SyncKnivesToAmmo();
	}
}

void AJettKnives::NotifyIfDepleted()
{
	if (Ammo > 0) return;

	// 走角色而不是自己收招：收招要销毁**这把武器自己**（DestroyMeleeWeapon），
	// 而此刻调用栈可能还压在 AJettKnives::Fire() 里面，上层（UCombatComponent::Fire）后面
	// 还要拿这个武器对象去发 ServerFire RPC。所以角色侧把它延到下一帧再真正收
	//（见 ABlasterCharacter::ServerRequestBladeStormEnd），这里只负责上报"我空了"。
	if (ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner()))
	{
		OwnerCharacter->ServerRequestBladeStormEnd();
	}
}
