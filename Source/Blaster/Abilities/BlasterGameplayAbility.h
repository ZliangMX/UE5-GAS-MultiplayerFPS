// Valorant 技能的 GAS 基类。首版实现「顺风冲刺」这类自我增益技能的通路：
// 冷却（非堆叠 Duration GE 计层 = 充能）+ 服务器权威冲刺位移。
// 子类/蓝图资产只需配置 CooldownEffectClass / MaxCharges / DashSpeed / DashDuration。

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "BlasterGameplayAbility.generated.h"

class UAbilitySystemComponent;
class ABlasterCharacter;

// 技能类型：HUD 技能条按它画矢量图标（零纹理资产，纯自绘）。
// 新技能接入时给资产配一个类型 + 槽位即可自动出现在技能条上。
UENUM(BlueprintType)
enum class EBlasterSkillType : uint8
{
	// 顺风冲刺（→ 箭头）
	Dash		UMETA(DisplayName = "Dash"),
	// 治疗（十字）
	Heal		UMETA(DisplayName = "Heal"),
	// 墙体/屏障（横线）
	Wall		UMETA(DisplayName = "Wall"),
	// 侦察/闪光（圆环+点）
	Recon		UMETA(DisplayName = "Recon"),
	// 烟雾/遮挡（云）
	Smoke		UMETA(DisplayName = "Smoke"),
	// 闪光（星芒）
	Flash		UMETA(DisplayName = "Flash"),
	// 未归类（方块）
	Custom		UMETA(DisplayName = "Custom")
};

// 一次查询返回的充能/冷却状态，供 HUD Widget 每帧读取。
// Widget 侧拿到 ASC 后调 UBlasterGameplayAbility::GetCooldownInfo(ASC)，无需能力实例。
USTRUCT(BlueprintType)
struct FBlasterAbilityCooldownInfo
{
	GENERATED_BODY()
public:
	// 当前可用充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 Charges = 0;

	// 最大充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 MaxCharges = 0;

	// 距下一层充能恢复的剩余秒数（无冷却中 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float TimeUntilNextCharge = 0.f;

	// 单层冷却时长（秒）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float CooldownDuration = 0.f;

	// 数据是否有效（找到 ASC 且配了冷却 GE）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	bool bValid = false;
};

// 服务器生成的"技能条显示快照"，按 SkillSlotIndex 排序后放进 ABlasterCharacter::ReplicatedSkills。
// 观战者（阵亡看队友）读不到队友 ASC 的能力（GAS ActivatableAbilities 只复制给拥有者），
// 所以服务器每帧基于自己的权威 ASC 重算这份快照、只复制到所有客户端画远程技能条。
// 时间字段一律用"绝对服务器时间"（客户端 PC->GetServerTime() 换算剩余秒数）；0 = 无该状态。
USTRUCT(BlueprintType)
struct FBlasterReplicatedSkill
{
	GENERATED_BODY()
public:
	// 槽位是否有效（false = 空位，客户端跳过）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bValid = 0;

	// 技能类型（HUD 按它画矢量图标）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	EBlasterSkillType SkillType = EBlasterSkillType::Custom;

	// 最大充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 MaxCharges = 0;

	// 当前满充能层数（服务器权威）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 Charges = 0;

	// 下一层充能恢复的绝对服务器时间（无冷却中 / 永不回复(如 Sage 每回合一次) = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float NextChargeServerEndTime = 0.f;

	// 单层冷却时长（恢复进度分母；非时长型冷却 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float CooldownDuration = 0.f;

	// 冷却数据有效（有 ASC 且配了冷却 GE）——镜像本地 GetCooldownInfo().bValid
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bCooldownValid = 0;

	// 武装（逐风）窗口进行中
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bArmed = 0;

	// 武装窗口绝对结束服务器时间（未武装 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float ArmedServerEndTime = 0.f;

	// 武装窗口总时长（窗口进度分母）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float ArmedWindowDuration = 0.f;
};

UCLASS()
class BLASTER_API UBlasterGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UBlasterGameplayAbility();

	// —— 冷却 / 充能 ——
	// 冷却用「非堆叠 Duration GameplayEffect」：每次施法 Apply 一个独立实例、各自到期移除。
	// 活跃实例数 = 已消耗的充能数；CanActivate 检查 活跃实例数 < MaxCharges。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cooldown")
	TSubclassOf<class UGameplayEffect> CooldownEffectClass;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cooldown")
	int32 MaxCharges = 2;

	// 当前剩余充能（0..MaxCharges），供 HUD/激活检查使用
	int32 GetCharges() const;

	// 指定 ASC 视角的充能（CDO 也能用，不依赖实例）
	int32 GetCharges(const UAbilitySystemComponent* ASC) const;

	// 查询充能/冷却信息（供 HUD Widget 每帧读取；ASC 必传，CDO 即可调用）
	UFUNCTION(BlueprintCallable, Category = "Ability|Cooldown")
	FBlasterAbilityCooldownInfo GetCooldownInfo(const UAbilitySystemComponent* ASC) const;

	// —— 技能条（HUD）显示配置 ——
	// 技能类型（HUD 按它画矢量图标；无纹理资产，纯自绘）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	EBlasterSkillType SkillType = EBlasterSkillType::Custom;

	// 是否显示在技能条（被动/纯冷却技能可关掉）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	bool bShowInSkillBar = true;

	// 技能条槽位顺序（越小越靠左；INDEX_NONE 排最后）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	int32 SkillSlotIndex = INDEX_NONE;

	// —— 冲刺（顺风） ——
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Dash")
	float DashSpeed = 3500.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Dash")
	float DashDuration = 0.5f;

	// —— 二段式武装（Valorant Jett E 逐风） ——
	// 开启后：第一次按键 = 武装（消耗 1 层充能 + 起风），ArmedWindowDuration 秒内再按 = 释放冲刺；
	// 窗口内未释放则充能作废。关闭则保持旧行为（按一下直接冲）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|TwoStage")
	bool bUseArmedActivation = false;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|TwoStage")
	float ArmedWindowDuration = 12.f;

	// 当前是否处于武装（逐风）状态。实例成员不复制——客户端靠预测保持一致。
	// 超过武装窗口自动视为未武装（防御：引擎 EndAbility 会清能力定时器，定时器可能失效）。
	bool IsArmed() const { return bArmed && (!GetWorld() || GetWorld()->GetTimeSeconds() < ArmedDeadline); }
	// 原始武装标记（不被窗口期限屏蔽），角色 Tick 兜底清理过期武装态用
	bool IsArmedRaw() const { return bArmed; }

	// 武装窗口到期强制解除（防御：GAS EndAbility 会 ClearAllTimersForObject 清掉能力定时器）
	void ForceExpireArmedWindow();

	// 武装窗口剩余秒数（未武装返回 0），HUD 倒计时用
	float GetArmedTimeRemaining() const;

	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override;
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility) override;

protected:
	void StartDash();
	void StopDash();

	// 一次性技能激活时的"技能动作"（服务器施加冷却后调用）。
	// 基类默认 = StartDash（冲刺类技能）；子类覆写为抛掷闪光等（如 UPhoenixCurveballAbility）。
	// ⚠️ LocalPredicted 客户端预测实例也会跑它：子类若只在服务器上做事（如 SpawnActor），需自行守卫权威。
	virtual void ExecuteSkillAction();

	// 冲刺自然结束 / 撞墙（CMM OnBlasterDashFinished 回调）→ EndAbility
	void HandleDashFinished();

	// 冲刺时长到期兜底 → EndAbility（CMM 通常同时或提前结束；此处只收尾能力生命周期）
	void OnDashTimerExpired();

	// 距最早到期冷却实例的剩余秒数（= 下一层恢复时间；无冷却中返回 0）
	float GetTimeUntilNextCharge(const UAbilitySystemComponent* ASC) const;

	// 施加一层充能冷却：顺序恢复——新一层的时长 = 当前最早到期冷却剩余 + 单层时长，
	// 保证第二层在第一层回满后才开始走自己的完整计时（充能点进度从 0 开始）
	void ApplyChargeCooldown();

	// 武装状态变化钩子：基类实现 = 通知角色显隐风特效（SetSkillArmed）；子类可扩展音效等
	virtual void OnArmedChanged(bool bNowArmed);

	// 武装窗口到期（未释放，充能作废）
	void OnArmedWindowExpired();

	FTimerHandle DashTimer;
	FVector DashDirection = FVector::ForwardVector;

private:
	bool bArmed = false;
	FTimerHandle ArmedTimer;
	double ArmedDeadline = 0.0;
};
