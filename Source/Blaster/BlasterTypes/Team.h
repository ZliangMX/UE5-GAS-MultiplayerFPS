#pragma once

#include "CoreMinimal.h"
#include "Team.generated.h"

UENUM(BlueprintType)
enum class ETeam : uint8
{
	ET_None UMETA(DisplayName = "None"),
	ET_TeamA UMETA(DisplayName = "Attackers"),
	ET_TeamB UMETA(DisplayName = "Defenders"),

	ET_MAX UMETA(DisplayName = "DefaultMAX")
};
