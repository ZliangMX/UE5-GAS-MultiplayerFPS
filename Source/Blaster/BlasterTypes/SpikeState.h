#pragma once

#include "CoreMinimal.h"
#include "SpikeState.generated.h"

UENUM(BlueprintType)
enum class ESpikeState : uint8
{
	ESS_Dropped UMETA(DisplayName = "Dropped"),
	ESS_Carried UMETA(DisplayName = "Carried"),
	ESS_Planted UMETA(DisplayName = "Planted"),
	ESS_Defused UMETA(DisplayName = "Defused"),
	ESS_Exploded UMETA(DisplayName = "Exploded"),

	ESS_MAX UMETA(DisplayName = "DefaultMAX")
};
