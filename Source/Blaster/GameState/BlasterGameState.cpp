#include "BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Net/UnrealNetwork.h"

void ABlasterGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABlasterGameState, TeamAScore);
	DOREPLIFETIME(ABlasterGameState, TeamBScore);
	DOREPLIFETIME(ABlasterGameState, RoundNumber);
	DOREPLIFETIME(ABlasterGameState, RoundWinnerTeam);
	DOREPLIFETIME(ABlasterGameState, TeamAMemberCount);
	DOREPLIFETIME(ABlasterGameState, TeamBMemberCount);
	DOREPLIFETIME(ABlasterGameState, AliveCountTeamA);
	DOREPLIFETIME(ABlasterGameState, AliveCountTeamB);
	DOREPLIFETIME(ABlasterGameState, SpikeState);
	DOREPLIFETIME(ABlasterGameState, SpikeCarrier);
	DOREPLIFETIME(ABlasterGameState, SpikeTimer);
	DOREPLIFETIME(ABlasterGameState, bSpikePlanted);
}

void ABlasterGameState::UpdateTeamCounts()
{
	int32 CountA = 0;
	int32 CountB = 0;

	for (APlayerState* PS : PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None) continue;

		if (BPS->Team == ETeam::ET_TeamA) CountA++;
		else CountB++;
	}

	TeamAMemberCount = CountA;
	TeamBMemberCount = CountB;
}

void ABlasterGameState::UpdateAliveCounts()
{
	int32 CountA = 0;
	int32 CountB = 0;

	for (APlayerState* PS : PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None) continue;

		APawn* Pawn = BPS->GetPawn();
		if (Pawn && !Pawn->IsPendingKillPending())
		{
			if (BPS->Team == ETeam::ET_TeamA) CountA++;
			else CountB++;
		}
	}

	AliveCountTeamA = CountA;
	AliveCountTeamB = CountB;
}

void ABlasterGameState::OnPlayerKilled(ETeam VictimTeam)
{
	if (VictimTeam == ETeam::ET_TeamA) AliveCountTeamA--;
	else AliveCountTeamB--;
}

void ABlasterGameState::MulticastKillFeed_Implementation(const FString& KillerName, const FString& VictimName)
{
	// 服务器 + 所有客户端各自执行：本机 HUD 监听该委托来画 Kill Feed
	OnKillFeedEntry.Broadcast(KillerName, VictimName);
}
