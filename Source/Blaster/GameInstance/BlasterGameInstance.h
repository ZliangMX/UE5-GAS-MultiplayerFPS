#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "BlasterGameInstance.generated.h"

// 跨关卡持久存储玩家在 Lobby 里的选队 + 选英雄。Lobby 里选定写入这里，
// SeamlessTravel / 硬旅行到对局地图后 GameMode 按「登录顺序」从这里恢复。
// PIE 里 PlayerId / PlayerState 重建顺序不稳定，登录顺序才稳定（跟分队同一套 key）。
UCLASS()
class BLASTER_API UBlasterGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// 按登录顺序记录玩家在 Lobby 里选的队伍（PlayerId 跨 ServerTravel 会变，登录顺序才稳定）
	void SetTeamByOrder(int32 Order, ETeam Team);

	// 按登录顺序查询队伍；没记录过则返回 ET_None
	ETeam GetTeamByOrder(int32 Order) const;

	// 按登录顺序记录玩家在 Lobby 里选的英雄（None=还没定，全就绪随机补位后为真实英雄）
	void SetAgentByOrder(int32 Order, EBlasterAgent Agent);

	// 按登录顺序查询英雄；没记录过则返回 None
	EBlasterAgent GetAgentByOrder(int32 Order) const;

	// 比赛结束后清空，避免下局残留
	void ClearTeamAssignments();

private:
	UPROPERTY()
	TMap<int32, ETeam> TeamByOrder;

	UPROPERTY()
	TMap<int32, EBlasterAgent> AgentByOrder;
};
