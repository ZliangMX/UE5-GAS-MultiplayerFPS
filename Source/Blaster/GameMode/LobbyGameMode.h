#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "LobbyGameMode.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerReadyChanged, class ABlasterPlayerState*, PlayerState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAllClientsReady);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerTeamChanged, ABlasterPlayerState*, PlayerState, ETeam, NewTeam);

UCLASS()
class BLASTER_API ALobbyGameMode : public AGameMode
{
	GENERATED_BODY()
public:
	ALobbyGameMode();
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	// 大厅里**不给玩家生成 Pawn**（见 .cpp 里的说明：为什么必须走虚函数而不是属性开关）
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

	void TogglePlayerReady(APlayerController* PC);
	void SwitchPlayerTeam(APlayerController* PC, ETeam TargetTeam);

	// Lobby 选英雄：落到该玩家 PlayerState（复制到所有人）。None = 表示"随机"
	void SelectPlayerAgent(APlayerController* PC, EBlasterAgent Agent);

	void StartGame();

	bool IsAllClientsReady() const;

	// --- 关卡流程：全员就绪后去哪个对局地图 ---
	//
	// 改这张图不用动代码：在 GM_LobbyGameMode 的 Class Defaults 里改这个字段
	//（Lotus 就填 /Game/Maps/Lotus）。
	// **换图后记得那张图的 World Settings 要挂 GM_BlasterGameMode**，否则出生点/回合/计分全不生效。
	// 路径不用带 ?listen，StartGame 自己会补。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Levels")
	FString MatchMapPath = TEXT("/Game/Maps/BlasterMap");

	UPROPERTY(BlueprintAssignable, Category = "Lobby Events")
	FOnPlayerReadyChanged OnPlayerReadyChanged;

	UPROPERTY(BlueprintAssignable, Category = "Lobby Events")
	FOnAllClientsReady OnAllClientsReady;

	UPROPERTY(BlueprintAssignable, Category = "Lobby Events")
	FOnPlayerTeamChanged OnPlayerTeamChanged;

private:
	ETeam GetTeamWithFewerPlayers() const;
	int32 CountPlayersOnTeam(ETeam Team) const;

	// 把该玩家的队伍 + 当前英雄按登录顺序写入 GameInstance
	//（对局 GameMode 同样按登录顺序恢复 → 不受 PIE PlayerId 不稳影响）
	void SaveSelectionsToGameInstance(class ABlasterPlayerState* PS);

	// 全就绪时给还没选英雄(Agent==None)的玩家随机分配一个，UI 离开 Lobby 前即可看到
	void AssignRandomAgentsToUnselected();

	// 玩家 -> 登录序号（PlayerId 跨 ServerTravel 会变，用序号做稳定 key）
	TMap<class ABlasterPlayerState*, int32> PlayerOrder;
	int32 NextOrder = 0;
};
