// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Weapon/HitScanWeapon.h"
#include "JettKnives.generated.h"

class ABlasterCharacter;

/*
 * Jett 大招「刃风暴 / Blade Storm」手里的那把飞刀。
 *
 * 它是**一把真武器**，占近战槽，但类型是 EWeaponType::EWT_BladeStorm
 *（和普通刀的 EWT_Melee 分开 —— 第一人称动画状态机要按 WeaponType 换状态）。
 * 占槽位换来的三件事：
 *   ① 左键扔刀直接走现成的开火链路 —— 本地预测表现、ServerFire 的射速/弹药/阵亡/回合禁用校验、
 *      切枪动画机状态，全都白拿；
 *   ② "拿刀时按 1/2 能切回枪" 变成一句"再掏一遍主武器"，因为刀本来就挂在槽里；
 *   ③ 死亡时敌人捡不到它 —— DropAllWeapons 只掉主副武器。
 *
 * 刀数 = 弹匣（MagCapacity / Ammo）。"扔一把" = SpendRound()、"还剩几把" = GetAmmo()，
 * 和枪共用同一套 HUD 显示（Valorant 的刃风暴也是显示剩余刀数）。
 *
 * ——— 判定：射线，不是投射物 ———
 * 继承 AHitScanWeapon（用户要求"改为射线检测，不要用 projectile"），并且**不开服务器回溯**
 *（bUseServerSideRewind = false）：
 *   · 单扔：伤害由 MulticastFire 那一遍 Fire() 在服务器上结算（AHitScanWeapon::ProcessHit 里
 *     那条 `!bUseServerSideRewind` 的门禁），和 AMeleeWeapon 走同一条路；
 *   · 右键全扔：一次好几条射线，而回溯那条路一次只能带**一个**终点，装不下。
 * 所以这里上报 AppliesOwnDamage() = true —— UCombatComponent::ServerFire 靠它知道
 * "这条路有人结算伤害"，不会掉进那条 bUseServerSideRewind=false 的假警告里。
 *
 * ——— 刀的显隐不在这个类里 ———
 * 那套视觉（一个骨骼网格 + 5 个挂在 Knife1Socket..Knife5Socket 上的静态网格）归 **AJettCharacter**
 * 管：它在所有机器上都存在、也比武器活得久（大招没开时也得在，只是不可见），而武器是大招期间
 * 临时生成、收招就销毁的。这里只负责两件事：
 *   · 每次扔刀播攻击蒙太奇（跳到第 N 段），"哪一把该消失"由动画通知
 *     （UAnimNotify_KnifeConsumed）在动画播到那一帧时去角色那边同步
 *     —— ⚠ 这条只对**单扔**成立。右键全扔不播刀骨骼那条（用户要求："右键刀是没有动画的"），
 *     通知自然也不会来，所以那边在 ThrowAllKnives 末尾**手动**推一次 SyncKnivesToAmmo()
 *   · 刀数变化时推一把角色（击杀刷新 / Ammo 复制到客户端）
 *
 * ——— 收招 ———
 * **刀扔空 = 刃风暴结束**（用户要求："飞刀用完大招充能开始重新算"）。
 * 收招动作在角色侧（ABlasterCharacter::ServerRequestBladeStormEnd）—— 这里只上报"我空了"。
 * 改射线之后"扔空"就没有歧义了：以前用投射物时最后一刀还在天上飞，得等它落定才知道扎没扎死人
 *（扎死了要刷新刀数、大招继续），所以那时要额外数一个 KnivesInFlight；现在命中判定和扣刀
 *发生在**同一次 Fire() 里**、击杀刷新也是同步完成的，Ammo 归零就是真归零，这个计数没用了。
 *
 * 参数全在 BP_JettKnives 里调：
 *   MagCapacity         —— 刀数（默认 5）
 *   FireDelay           —— 两次扔刀的最短间隔
 *   Damage              —— 单刀伤害（在 AHitScanWeapon 上，默认 50）
 *   HeadshotMultiplier  —— 爆头倍率（在 AWeapon 上，默认 3）
 *   HeadBoneName        —— 爆头判定的骨骼名（在 AWeapon 上，默认 "head"）
 *   ThrowAllSpreadDegrees —— 右键全扔时的散布
 *   FPFireMontage / FPEquipMontage —— 第一人称手模那两条动画（见下面 PlayThrowPresentation）
 *   ThirdPersonFireMontage —— 身体那 5 段（和 FPFireMontage 同一条约定：段名 "1".."5"），见 ShouldPlayBodyFireMontage
 *   FPRightClickMontage（手模）/ ThirdPersonRightClickMontage（身体）
 *                               —— 右键"一次全扔"的两条动画，各自留空都能跑（见 PlayThrowAllPresentation）
 * ⚠️ 别在 Ammo 里填 0：刀数读的是 Ammo，填 0 就是"一把刀都没有"，扔不出来。
 *    构造里已经默认 Ammo = MagCapacity = 5，但蓝图里手滑改过就要注意。
 */
UCLASS()
class BLASTER_API AJettKnives : public AHitScanWeapon
{
	GENERATED_BODY()

public:
	AJettKnives();

	virtual void Fire(const FVector& HitTarget) override;

	// 伤害自己结算（服务器不需要回溯）—— 理由见上面那段
	virtual bool AppliesOwnDamage() const override { return true; }

	/*
	 * 身体那条通用开火蒙太奇（ThirdPersonFireMontage）要不要播 —— 现在**要**，条件是"配了资产"。
	 *
	 * UCombatComponent::Fire 就是在这一句上问的（第 498 行）：true → Character->PlayFireMontage()，
	 * 那是身体唯一一次被播到的地方 —— 单扔的 5 段全靠它起头。
	 *
	 * 以前这里恒返回 false，理由是"扔刀的身体动作是刀骨骼那套，通用步枪后坐会把它顶掉"。
	 * 那个理由现在不成立了：刀骨骼那条（KnifeAttack）打在 KnifeRigMesh 上，
	 * 身体这条打在 GetMesh() 上，**两块网格、两个槽位，谁也顶不掉谁**。
	 *
	 * 两条链的顺序（同一帧内）：
	 *   ① PlayFireMontage()              → 身体蒙太奇从头播（第 1 段，因为通用段名 RifleAim/RifleHip
	 *                                      在这条蒙太奇里不存在，会被 PlayFireMontage 里的
	 *                                      IsValidSectionName 挡掉，不会再刷警告）
	 *   ② EquippedWeapon->Fire()         → AJettKnives::PlayThrowPresentation 再跳到第 N 段
	 * 顺序不能反：跳段必须等 SpendRound 扣完刀才知道是第几次（见 PlayThrowPresentation）。
	 *
	 * 留空就还是恒 false —— 老行为原样保留（BP_JettKnives 不填这一条，身体就什么都不播）。
	 */
	// ⚠ 走 GetThirdPersonFireMontage() 而不是直接读字段：那条字段在 AWeapon 上是 **private**，
	//    派生类也看不到（直接写 ThirdPersonFireMontage 会 error C2248）。
	virtual bool ShouldPlayBodyFireMontage() const override { return GetThirdPersonFireMontage() != nullptr; }

	// 没有换弹概念：恒真 → ACombatComponent 里整条换弹链路（Reload / AmountToReload /
	// PlayReloadMontage）自动短路 —— 按 R 什么都不发生，这是想要的行为。
	//
	// 不覆盖的话：扔剩 2 把后按 R 会进 ECS_Reloading，而 PlayReloadMontage 的 switch 里
	// 没有飞刀这一支（default 直接 return，蒙太奇压根不播），白白罚站到兜底时长（2 秒）。
	virtual bool IsFull() override;

	// 击杀刷新：把刀补满（**补满，不是 +1**，用户要求"杀了人刷新回 5 个"）。
	// 由 ProcessHit / ThrowAllKnives 侦测到"这一刀打死了人"时调，服务器上下文。
	void RefillKnives();

	// 让**下一次** Fire() 走"全扔"分支（右键用）。
	// ⚠️ 只在本机生效，必须配合 ACombatComponent::ServerFire 的 bThrowAll 参数一起用：
	//    服务器上真正跑 Fire() 的那次发生在 MulticastFire 里，读的是**服务器自己那份**
	//    武器对象，本地设的标志传不过去 —— 所以右键那条路要两边各设一次。
	void RequestThrowAll() { bThrowAllNextShot = true; }

protected:
	// 命中结算：调 Super 拿伤害/击杀确认音/命中反馈，再补一个"打死了 → 刷新刀数"。
	// 这里也是**唯一**能侦测击杀的地方 —— 射线武器的伤害和判定就在这条链上。
	virtual void ProcessHit(const FHitResult& FireHit, AController* InstigatorController, bool bRewindConfirmation) override;

	// Ammo 复制到客户端时，把"还剩几把刀"同步给角色的飞刀显隐。
	// 覆盖掉基类那份：基类里的 JumpToShotGunEnd 是霰弹枪专用的，对飞刀是误伤 ——
	// 它的门禁是 IsFull()，而飞刀恒真，于是每次弹药变化都会去跳一遍身体蒙太奇
	//（表现上什么都没发生，只是刷一条 "JumpToEnd" 警告）。
	virtual void OnRep_Ammo() override;

	// 右键全扔时的散布（度）。以准心方向为轴，每把刀在锥内随机偏一点 ——
	// 全砸在同一个点上会看起来像"一把刀"，稍微散开才有"甩出一把刀"的观感。
	// **第一把不偏**：保证"准心指着谁，至少有一把扎谁"。设 0 = 全部沿准心打。
	UPROPERTY(EditAnywhere, Category = "BladeStorm")
	float ThrowAllSpreadDegrees = 3.5f;

	// 单刀伤害和爆头倍率直接复用基类的 Damage / HeadshotMultiplier / HeadBoneName
	//（射线武器本来就有这三个），不再另开一套同名属性 —— 免得 BP 里出现"两个伤害"不知道改哪个。

private:
	// 左键：一条射线 + 攻击动画 + 收招判断
	void ThrowOneKnife(const FVector& HitTarget);

	// 右键：把手上剩下的刀一次全甩出去（霰弹枪那套多条射线）
	void ThrowAllKnives(const FVector& HitTarget);

	// 播这一次扔刀的攻击动画：**三条**都跳到第 ThrowIndex 段 ——
	//   刀骨骼（AB）  KnifeAttack            段名 "1".."5"
	//   手模（FP）    FPFireMontage          段名 "1".."5"
	//   第三人称身体  ThirdPersonFireMontage 段名 "1".."5"
	// 段名走同一个约定（AJettCharacter::MakeKnifeAttackSectionName），三条蒙太奇照这个分段就自动对上。
	// ⚠ 但**段名对齐不代表资产能共用**：三条是**三套骨架**的资产，互相顶替不会报错，
	//   而是把对方骨架的同名骨静默改写成自己的值 —— 详见 PlayThrowPresentation 里的注释
	//   和 ABlasterCharacter::IsMontageCompatibleWithMesh 那道拦截。
	void PlayThrowPresentation(int32 ThrowIndex);

	// 右键"一次全扔"的动画：两条链各播各的（手模 FPRightClickMontage /
	// 第三人称身体 ThirdPersonRightClickMontage），每条都自带"留空退回老行为"。
	// 单扔和全扔**分开**正是这次改动的目的：一次出去好几把，本来就没有"第 N 段"这个说法。
	// ⚠ **没有刀骨骼（AB）那条** —— 用户 2026-09-18 明确："右键刀是没有动画的，只有 fp 和 tp"。
	void PlayThrowAllPresentation();

	// 这一批伤害里有没有把人打死 → 有就刷新刀数。HealthBefore 是**结算前**抓的血量。
	void RefillIfKilled(ABlasterCharacter* Victim, float HealthBefore);

	// 刀扔空了 → 通知角色收招。改刀数/加刀的逻辑不用管它。
	void NotifyIfDepleted();

	// 这一次 Fire() 是不是"全扔"。见 RequestThrowAll 的警告：只在**读它的那台机器**上有效。
	// 不复制、不存档 —— 它是"这一发"的属性，不是武器的持久状态（Fire 里读完立刻清）。
	bool bThrowAllNextShot = false;
};
