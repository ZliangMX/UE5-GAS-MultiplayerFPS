// Valorant 技能系统的 GameplayTags。
// 用 UE 5.4 的 FNativeGameplayTag 宏注册（模块加载时静态注册，早于 bDoneAddingNativeTags，
// 比运行时 AddNativeGameplayTag 干净）。供技能激活（Ability.Jett.Dash）与冷却（Cooldown.Jett.Dash）使用。

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "GameplayTagContainer.h"

namespace BlasterGameplayTags
{
	// Jett 顺风冲刺
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Jett_Dash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Jett_Dash);

	// Phoenix 曲线球（闪光弹）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Phoenix_Curveball);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Phoenix_Curveball);
}
