#include "Agent.h"

namespace BlasterAgent
{
	FString ToDisplayName(EBlasterAgent Agent)
	{
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return TEXT("Jett");
		case EBlasterAgent::Sage:		return TEXT("Sage");
		case EBlasterAgent::Phoenix:	return TEXT("Phoenix");
		case EBlasterAgent::None:
		default:						return TEXT("Random");
		}
	}

	bool IsPlayable(EBlasterAgent Agent)
	{
		return Agent >= FirstPlayable && Agent <= LastPlayable;
	}

	EBlasterAgent GetRandomAgent()
	{
		const int32 Index = FMath::RandRange((int32)FirstPlayable, (int32)LastPlayable);
		return (EBlasterAgent)Index;
	}

	FLinearColor GetAccentColor(EBlasterAgent Agent)
	{
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return FLinearColor(0.15f, 0.72f, 0.85f, 1.f); // 青
		case EBlasterAgent::Sage:		return FLinearColor(0.25f, 0.72f, 0.45f, 1.f); // 绿
		case EBlasterAgent::Phoenix:	return FLinearColor(0.95f, 0.45f, 0.15f, 1.f); // 橙
		case EBlasterAgent::None:
		default:						return FLinearColor(0.55f, 0.55f, 0.55f, 1.f); // 灰
		}
	}
}
