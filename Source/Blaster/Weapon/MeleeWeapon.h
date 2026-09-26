// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Weapon/Weapon.h"
#include "MeleeWeapon.generated.h"

/*
 * 一次挥刀的全部参数。轻击的每一段、重击，都是一个这个结构 ——
 * **要填的蒙太奇就挂在这里**（BP_Melee 的详情面板 → Melee → LightCombo / HeavyAttack）。
 */
USTRUCT(BlueprintType)
struct FMeleeAttackData
{
	GENERATED_BODY()

	/*
	 * 这一刀在**手模**（FPArmsMesh / ABP_WushuFP）上播的蒙太奇 —— 也就是玩家自己看到的挥刀。
	 * 必须是 Montage，不能是裸的 AnimSequence：手模靠 ABP 里的 Slot 节点接住它。
	 * 留空 = 这一刀不出挥刀动作（判定/伤害/音效照常，只是手上看不见动作）。
	 * 三段轻击按 0/1/2 顺序取，重击取 HeavyAttack 的那一条。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Melee|Animation")
	TObjectPtr<UAnimMontage> Montage = nullptr;

	/*
	 * 这一刀在**第三人称身体**（GetMesh()）上播的动画 —— 别人屏幕上看你挥的那一刀。
	 * 这条**所有机器都播**（和枪的 FireAnimation 同一个定位），和上面那条"只在本机播"的分工别搞混。
	 *
	 * 类型是 UAnimationAsset：填 AnimSequence（我们导进来的那些就是）走单节点播放、零配置；
	 * 填 Montage 也能用（走身体动画机的 Slot）。
	 * 留空 = 别人看不到挥刀动作（比播步枪后坐好看，所以近战把 FireWeaponMontage 那条路挡掉
	 * 了，见 AWeapon::ShouldPlayBodyFireMontage）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Melee|Animation")
	TObjectPtr<UAnimationAsset> ThirdPersonAnimation = nullptr;

	// 这一刀的伤害（爆头再 × AWeapon::HeadshotMultiplier）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Melee|Damage")
	float Damage = 50.f;

	// 这一刀的射程（厘米）：从眼位沿准心方向最多打这么远。
	// 刀的默认 200cm ≈ 两米，比人物碰撞体（半径 34）长得多，正常交战距离够用。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Melee|Damage")
	float Range = 200.f;

	/*
	 * 判定用的球半径（厘米）。0 = 纯线检测（默认，严格按准心，不误伤旁边的人）。
	 *
	 * 想要"手感宽松一点"就填个 20~40：射线变成从眼位扫出去的球，
	 * 准心旁边一点点的目标也能砍到（近战游戏常见的做法）。
	 * 注意球是从眼睛起算的，半径开太大（比如 100）会连身后/身侧的东西一起罩进来。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Melee|Damage")
	float Radius = 0.f;
};

/*
 * 近战武器「刀」—— 3 号槽（ESlot_Melee），角色天然自带。
 *
 * 三条硬性行为（用户要求 + 引擎里对应的拦截点）：
 *   · **不能丢弃**：UCombatComponent::DropEquippedWeapon 看到 IsMeleeWeapon() 直接返回，
 *     DropAllWeapons（阵亡掉枪）本来就只有主副武器两个槽 → 两条路都碰不到它。
 *   · **不能被捡**：构造里把 AreaSphere 的碰撞整个关掉（拾取提示 widget 靠它的 overlap 触发），
 *     而它永远不会进 EWS_Dropped 状态（那个状态才会重新打开碰撞）→ 地上不会有这么个东西。
 *     另外 UCombatComponent::EquipWeapon（捡枪）也加了 IsMeleeWeapon 的守卫，双保险。
 *   · **每次重生都发一把**：ABlasterGameMode::RestorePlayerWeapons 里发（和默认手枪同一处）。
 *
 * 左右键的分工：
 *   · 左键（轻击）→ 走正常的开火链路（UCombatComponent::Fire → ServerFire → MulticastFire），
 *     三段连击：每次挥刀把 ComboIndex 往后推一段，超过 ComboResetWindow 没续上就回到第一段。
 *   · 右键（重击）→ ABlasterCharacter::AimStart 里先问 TryMeleeHeavyAttack()，
 *     是近战就吃掉这次右键（不进瞄准）并让**下一刀**走重击分支。走的是**同一条开火链路**，
 *     所以射速/阵亡/回合禁用那套服务器校验一个不少（绕开自己发 RPC 等于开一个无校验后门）。
 *
 * 伤害只在服务器结算（和 AHitScanWeapon::ProcessHit 同一套写法：ApplyDamage + 击杀确认音 +
 * 命中标记/伤害数字）。近战**不开服务器回溯**（bUseServerSideRewind=false）：回溯是补偿 ping
 * 造成的目标位移，两米内的近身战那个位移量没有意义，而且回溯那条路要在
 * ProcessServerRewindHit 里单独结算伤害，近战按段取伤害会更绕。
 *
 * 要填的属性全在 BP_Melee 的 Melee 分类下：LightCombo（3 段）/ HeavyAttack / ComboResetWindow，
 * 加上从 AWeapon 继承来的 FireDelay（两刀之间的最短间隔，三段轻击和重击共用）。
 */
UCLASS()
class BLASTER_API AMeleeWeapon : public AWeapon
{
	GENERATED_BODY()

public:
	AMeleeWeapon();

	// 左键轻击 / 右键重击都从这里进（重击由 bHeavyAttackNextShot 区分）
	virtual void Fire(const FVector& HitTarget) override;

	// 刀没有弹药：恒不空 → CanFire() 的弹药门禁恒过；
	// 恒满 → ReloadEmptyWeapon/Reload 那条链自动短路（否则会进 ECS_Reloading 卡死，
	// 理由和 AJettKnives::IsFull 的注释一模一样）。
	virtual bool IsEmpty() override { return false; }
	virtual bool IsFull() override { return true; }

	// HUD 上那个弹药数字对刀没有意义，清空它（负数 = 不显示，见 ABlasterPlayerController::SetHUDWeaponAmmo）
	virtual void SetHUDAmmo() override;

	// 身体不播通用的 FireWeaponMontage —— 近战自己按段播 ThirdPersonAnimation
	virtual bool ShouldPlayBodyFireMontage() const override { return false; }

	// 伤害由 MulticastFire 那一遍 Fire() 在服务器上结算（ApplyMeleeHit 里的 HasAuthority 门禁），
	// 不开回溯。标出来是为了让 UCombatComponent::ServerFire 知道"这条路有人结算"，
	// 不至于每次挥刀都掉进下面那条 "bUseServerSideRewind=false → 没人结算伤害" 的假警告里。
	virtual bool AppliesOwnDamage() const override { return true; }

	// 刀不能被捡：拾取提示/overlap 的整个入口在这里，空实现（构造里已经把球碰撞关了，这是第二道）
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult
	) override;

	// 让**下一次** Fire() 走重击分支。
	// ⚠ 和 AJettKnives::RequestThrowAll 一样，标志只在**读它的那台机器**上有效：
	//    本机那份由 UCombatComponent::TryMeleeHeavyAttack 设，服务器那份由 ServerFire 的
	//    bMeleeHeavy 参数传达，其他机器由 MulticastFire 的同一个参数补设。
	void RequestHeavyAttack() { bHeavyAttackNextShot = true; }

	// 这一次 Fire() 是不是重击（UCombatComponent::Fire 读它，好把标志报到服务器）
	bool IsHeavyAttackNextShot() const { return bHeavyAttackNextShot; }

protected:
	/*
	 * 三段轻击，索引 0/1/2 = 第 1/2/3 段。
	 * 构造里已经塞了 3 个默认条目（没填蒙太奇也能挥、能造成伤害），别把它清空 ——
	 * 清空了会退回一个内置的默认段（伤害 50 / 射程 200），不会崩，但你在面板上就调不了值了。
	 */
	UPROPERTY(EditAnywhere, Category = "Melee")
	TArray<FMeleeAttackData> LightCombo;

	// 重击（右键）。伤害/射程比轻击大、硬直也更长（硬直由 AWeapon::FireDelay 统一控制）
	UPROPERTY(EditAnywhere, Category = "Melee")
	FMeleeAttackData HeavyAttack;

	// 断连击的时间窗：上一刀之后超过这么久再按左键，连击回到第 1 段（默认 1.2 秒）
	UPROPERTY(EditAnywhere, Category = "Melee")
	float ComboResetWindow = 1.2f;

	/*
	 * 第三人称那一刀播在**身体动画蓝图**的哪个 Slot 节点上。
	 * **只对 ThirdPersonAnimation 填 AnimSequence 的情况生效** —— 填蒙太奇的话蒙太奇自带 Slot 名。
	 *
	 * 默认 WeaponSlot：ABP_Blaster 里接武器动作的那个 Slot，步枪的开火蒙太奇 Mon_FireWeapon
	 * 走的就是它（近战把身体那条通用 FireWeaponMontage 挡掉了，见 ShouldPlayBodyFireMontage，
	 * 等于接管了同一个通道）。ABP_Blaster 里另外两个 Slot 是 HitReactSlot / ElimSlot，别填那两个。
	 *
	 * 填的 Slot 名在那个 ABP 里不存在时不会报错，只是**什么都不播**（引擎会打一条 warning）。
	 */
	UPROPERTY(EditAnywhere, Category = "Melee")
	FName BodyAttackSlot = TEXT("WeaponSlot");

	// 挥刀音效（走基类那个 FireSound，这里不用再填）。
	// 命中音效 + 命中特效单独给（和 AHitScanWeapon 的 HitSound/ImpactParticles 同一个定位）
	UPROPERTY(EditAnywhere, Category = "Melee")
	USoundCue* HitSound;

	UPROPERTY(EditAnywhere, Category = "Melee")
	class UParticleSystem* HitParticles;

private:
	// 挥这一刀：手模蒙太奇 + 第三人称动画 + 音效 + 命中判定/结算
	void PerformAttack(const FMeleeAttackData& AttackData, const FVector& HitTarget);

	/*
	 * 把这一刀的第三人称动画播到**身体网格**上。
	 *
	 * 单独抽出来是因为这里有个必须绕开的引擎陷阱（详见 .cpp 里的注释）：
	 * USkeletalMeshComponent::PlayAnimation 会把身体的**动画蓝图实例销毁**且不还原，
	 * 所以身体网格挂着动画蓝图时必须走动画实例的 Slot，不能走单节点播放。
	 */
	void PlayBodyAttackAnimation(class UAnimationAsset* Anim);

	// 从眼位沿准心方向打一条 AttackData.Range 长的射线（Radius>0 时改成球扫）
	bool MeleeTraceHit(const FMeleeAttackData& AttackData, const FVector& HitTarget, FHitResult& OutHit) const;

	// 命中结算：服务器扣血 + 击杀确认音，各机器出命中标记/伤害数字/命中特效
	void ApplyMeleeHit(const FMeleeAttackData& AttackData, const FHitResult& Hit);

	// 当前连击段（0/1/2）。只在**跑 Fire() 的那台机器**上推进 —— 各机器每次挥刀都跑一遍，
	// 所以正常情况下是同步的（丢多播才会偏，和飞刀 bThrowAllNextShot 同类的小瑕疵）
	int32 ComboIndex = 0;

	// 上一刀的时刻，用来判"断连击"
	float LastLightAttackTime = -1000.f;

	// 下一次 Fire() 是不是重击。见 RequestHeavyAttack 的警告
	bool bHeavyAttackNextShot = false;
};
