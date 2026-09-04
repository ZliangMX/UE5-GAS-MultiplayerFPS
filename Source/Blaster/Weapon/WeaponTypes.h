#pragma once
#include "CoreTypes.h"

#define TRACE_LENGTH 80000.f

#define CUSTOM_DEPTH_PURPLE 250
#define CUSTOM_DEPTH_BLUE 251
#define CUSTOM_DEPTH_TAN 252

// 武器类型已 Valorant 化：仅保留 4 种（Vandal/Classic/Operator/Judge）。
// 显式赋值保持底层值不变（Pistol=2、Shotgun=4、SniperRifle=5），
// 这样已配置的武器蓝图 WeaponType 不会因删除中间枚举项而错位。
UENUM(BlueprintType)
enum class EWeaponType : uint8
{
	EWT_AssaultRifle = 0 UMETA(DisplayName="Assault Rifle"),
	EWT_Pistol = 2 UMETA(DisplayName="Pistol"),
	EWT_Shotgun = 4 UMETA(DisplayName="Shotgun"),
	EWT_SniperRifle = 5 UMETA(DisplayName="Sniper Rifle"),

	EWT_MAX UMETA(DisplayName = "DefaultMAX")
};