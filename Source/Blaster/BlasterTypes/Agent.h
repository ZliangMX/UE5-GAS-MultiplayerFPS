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
	// 暮蝶。新英雄一律**追加在 MAX 之前**，不要插在中间 —— 枚举值会被
	// ABlasterPlayerState 复制、也会存进 UBlasterGameInstance 的选人顺序表，
	// 插在中间等于把已有英雄的编号整体挪位。
	Clove		UMETA(DisplayName = "Clove"),

	MAX			UMETA(Hidden)
};

namespace BlasterAgent
{
	// 第一个/最后一个可玩的真实英雄（None 与 MAX 不算）
	// ⚠️ IsPlayable / GetRandomAgent 都从这两个常量推导。加了新英雄却忘了改 LastPlayable，
	//    表现是「大厅里选不中、随机也永远 roll 不到」—— 而且不报任何错。
	constexpr EBlasterAgent FirstPlayable = EBlasterAgent::Jett;
	constexpr EBlasterAgent LastPlayable = EBlasterAgent::Clove;

	// None → "Random"，其余返回英雄名
	BLASTER_API FString ToDisplayName(EBlasterAgent Agent);

	// 是否为玩家可选的真实英雄
	BLASTER_API bool IsPlayable(EBlasterAgent Agent);

	// 从 FirstPlayable..LastPlayable 之间随机一个（当前 = Jett/Sage/Phoenix/Clove）
	BLASTER_API EBlasterAgent GetRandomAgent();

	// 卡牌/标签强调色（None → 灰，用作"随机"卡）
	BLASTER_API FLinearColor GetAccentColor(EBlasterAgent Agent);

	// 头像贴图的对象路径（顶部比分栏 UValorantAgentPortrait 用）。
	// None 返回空串 = 不画头像。贴图没导进来时 LoadObject 会返回 null，那一格空着。
	BLASTER_API FString GetPortraitPath(EBlasterAgent Agent);

	// 该英雄大招要攒多少点（Valorant：Jett 7 / Phoenix 6 / Sage 8）。
	// 点数的来源与消耗见 ABlasterPlayerState::AddUltPoints。
	BLASTER_API int32 GetUltPointsRequired(EBlasterAgent Agent);
}
