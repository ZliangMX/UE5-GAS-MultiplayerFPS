#pragma once

#include "CoreMinimal.h"
#include "Agent.generated.h"

// 玩家可选英雄。None = 未选（等价"随机"，全就绪时由服务器 roll 成真实英雄）。
// 原 F9 调试用的 EBlasterAgent 从 PlayerController 挪到这里，成为复制到全端的正式状态。
UENUM(BlueprintType)
enum class EBlasterAgent : uint8
{
	None		UMETA(DisplayName = "None/Random"),
	Jett		UMETA(DisplayName = "Jett"),
	Sage		UMETA(DisplayName = "Sage"),
	Phoenix		UMETA(DisplayName = "Phoenix"),

	MAX			UMETA(Hidden)
};

namespace BlasterAgent
{
	// 第一个/最后一个可玩的真实英雄（None 与 MAX 不算）
	constexpr EBlasterAgent FirstPlayable = EBlasterAgent::Jett;
	constexpr EBlasterAgent LastPlayable = EBlasterAgent::Phoenix;

	// None → "Random"，其余返回英雄名
	BLASTER_API FString ToDisplayName(EBlasterAgent Agent);

	// 是否为玩家可选的真实英雄
	BLASTER_API bool IsPlayable(EBlasterAgent Agent);

	// 从 Jett/Sage/Phoenix 随机一个
	BLASTER_API EBlasterAgent GetRandomAgent();

	// 卡牌/标签强调色（None → 灰，用作"随机"卡）
	BLASTER_API FLinearColor GetAccentColor(EBlasterAgent Agent);
}
