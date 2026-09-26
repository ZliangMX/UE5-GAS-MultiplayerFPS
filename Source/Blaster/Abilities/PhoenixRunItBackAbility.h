// Phoenix 大招「再来一次 / Run It Back」。
// 按 X：在**原地**记下标记点，之后 Duration 秒内"阵亡"不真的死 —— 满血满甲回到标记点继续打。
// 时间到还没死也会被拉回标记点（Valorant 同款：窗口结束 = 自动回标记），两条件共用同一个回程出口。
//
// ⚠️ 扣点时机是这个技能和别的大招最大的不同：**回到标记点之后**才扣，不是按下就扣（用户明确要求）。
//    所以 bSpendUltPointsOnActivate = false，实际扣点在角色侧的
//    ABlasterCharacter::ServerReturnToRunItBack()（阵亡回程 / 到期回程共用的唯一出口）。
//
// 这个类**不需要蓝图子类**：直接把它填进角色 BP 的 DefaultAbilities 就能用。
// （它靠的是"扣光大招点"而不是冷却 GE，所以不用配 CooldownEffectClass；Duration 想调再用子类覆盖。）

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "PhoenixRunItBackAbility.generated.h"

UCLASS()
class BLASTER_API UPhoenixRunItBackAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UPhoenixRunItBackAbility();

	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override;

protected:
	virtual void ExecuteSkillAction() override;

	// 大招窗口时长（秒）。Valorant 的 Run It Back 是 30s，留成属性方便调手感。
	// 只影响"多久后自动拉回"，被击杀时的拉回是随时生效的。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|RunItBack")
	float Duration = 30.f;
};
