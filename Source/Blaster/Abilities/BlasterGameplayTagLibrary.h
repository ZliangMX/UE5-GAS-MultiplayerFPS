// 供 Python/蓝图取用 Blaster 技能 GameplayTags 的静态库。
// tag 由 FNativeGameplayTag 宏在模块加载时注册，这里直接打包返回。

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayTagContainer.h"
#include "BlasterGameplayTagLibrary.generated.h"

UCLASS()
class BLASTER_API UBlasterGameplayTagLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Jett 顺风冲刺：激活 tag 容器（Ability.Jett.Dash）
	UFUNCTION(BlueprintPure, Category = "Blaster|Tags")
	static FGameplayTagContainer GetJettDashAbilityTags();

	// Jett 顺风冲刺：冷却 tag 容器（Cooldown.Jett.Dash）
	UFUNCTION(BlueprintPure, Category = "Blaster|Tags")
	static FGameplayTagContainer GetJettDashCooldownTags();

	// 原生设置 GA CDO 的 AbilityTags=Ability.Jett.Dash。
	// Python 反射回写 FGameplayTag 容器时，TagName 是只读字段会被丢弃（写空），
	// 所以必须在 C++ 里直接操作（原生 AddTag 保留 FName）。
	// 供资产生成脚本/编辑器调用：unreal.BlasterGameplayTagLibrary.set_jett_dash_ability_tags(ga_cdo)
	UFUNCTION(BlueprintCallable, Category = "Blaster|Tags")
	static void SetJettDashAbilityTags(class UBlasterGameplayAbility* AbilityCDO);

	// 同上：AbilityTags=Ability.Phoenix.Curveball（曲线球闪光）
	UFUNCTION(BlueprintCallable, Category = "Blaster|Tags")
	static void SetPhoenixCurveballAbilityTags(class UBlasterGameplayAbility* AbilityCDO);
};
