#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameState.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/BlasterTypes/SpikeState.h"
#include "BlasterGameState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRoundPhaseChanged, FName, NewPhase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRoundEnded, ETeam, WinningTeam, int32, ScoreA, int32, ScoreB);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnTeamsSwapped);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchOver, ETeam, WinningTeam);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnKillFeedEntry, const FString&, KillerName, const FString&, VictimName);

UCLASS()
class BLASTER_API ABlasterGameState : public AGameState
{
	GENERATED_BODY()

public:
	void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated)
	int32 TeamAScore = 0;

	UPROPERTY(Replicated)
	int32 TeamBScore = 0;

	UPROPERTY(Replicated)
	int32 RoundNumber = 0;

	// 本回合赢的那一队。GameMode 上的 RoundWinnerTeam 是服务端私有的、不复制，
	// 而客户端要在 PostRound 判"我这边赢没赢"才能决定公告栏出 WON 还是 LOST —— 所以复制一份。
	// 服务端在 ABlasterGameMode::EndRound 里和比分一起写。
	UPROPERTY(Replicated)
	ETeam RoundWinnerTeam = ETeam::ET_None;

	UPROPERTY(Replicated)
	int32 TeamAMemberCount = 0;

	UPROPERTY(Replicated)
	int32 TeamBMemberCount = 0;

	UPROPERTY(Replicated)
	int32 AliveCountTeamA = 0;

	UPROPERTY(Replicated)
	int32 AliveCountTeamB = 0;

	UPROPERTY(Replicated)
	ESpikeState SpikeState = ESpikeState::ESS_Dropped;

	UPROPERTY(Replicated)
	class ABlasterPlayerState* SpikeCarrier = nullptr;

	UPROPERTY(Replicated)
	float SpikeTimer = 0.f;

	UPROPERTY(Replicated)
	bool bSpikePlanted = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match")
	int32 TotalRoundsToWin = 13;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match")
	int32 RoundsPerSide = 3;

	void UpdateTeamCounts();
	void UpdateAliveCounts();
	void OnPlayerKilled(ETeam VictimTeam);

	// 击杀信息：服务器调用，NetMulticast 广播给所有客户端 → OnKillFeedEntry 广播（HUD 监听）
	UFUNCTION(NetMulticast, Reliable)
	void MulticastKillFeed(const FString& KillerName, const FString& VictimName);

	bool IsSwapRound() const { return RoundNumber > 0 && RoundNumber % RoundsPerSide == 0; }
	bool IsMatchOver() const { return TeamAScore >= TotalRoundsToWin || TeamBScore >= TotalRoundsToWin; }

	UPROPERTY(BlueprintAssignable, Category = "Match Events")
	FOnRoundPhaseChanged OnRoundPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Match Events")
	FOnRoundEnded OnRoundEnded;

	UPROPERTY(BlueprintAssignable, Category = "Match Events")
	FOnTeamsSwapped OnTeamsSwapped;

	UPROPERTY(BlueprintAssignable, Category = "Match Events")
	FOnMatchOver OnMatchOver;

	UPROPERTY(BlueprintAssignable, Category = "Match Events")
	FOnKillFeedEntry OnKillFeedEntry;
};
