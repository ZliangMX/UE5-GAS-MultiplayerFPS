#pragma once
#include "CoreTypes.h"

UENUM(BlueprintType)
enum class ECombatState : uint8
{
	ECS_Unoccupied UMETA(DisplayName = "Unoccupied"),
	ECS_Reloading UMETA(DisplayName = "Reloading"),
	ECS_ThrowingGrenade UMETA(DisplayName = "ThrowingGrenade"),
	ECS_Planting UMETA(DisplayName = "Planting"),

	ECS_MAX UMETA(DisplayName = "DefaultMax")
};

UENUM(BlueprintType)
enum class EWeaponSlot : uint8
{
	ESlot_Primary UMETA(DisplayName = "Primary"),
	ESlot_Secondary UMETA(DisplayName = "Secondary"),
	ESlot_Melee UMETA(DisplayName = "Melee"),
	ESlot_Spike UMETA(DisplayName = "Spike"),

	ESlot_MAX UMETA(DisplayName = "DefaultMAX")
};