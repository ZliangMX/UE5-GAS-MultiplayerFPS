#include "LobbyGameMode.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/GameInstance/BlasterGameInstance.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"

ALobbyGameMode::ALobbyGameMode()
{
	HUDClass = ABlasterHUD::StaticClass();
}

void ALobbyGameMode::SaveSelectionsToGameInstance(ABlasterPlayerState* PS)
{
	if (!PS) return;

	// 分配/查找该玩家的登录序号（同一 PS 多次调用只占一个序号）
	int32 Order = 0;
	if (int32* Found = PlayerOrder.Find(PS))
	{
		Order = *Found;
	}
	else
	{
		Order = NextOrder++;
		PlayerOrder.Add(PS, Order);
	}

	if (UBlasterGameInstance* GI = Cast<UBlasterGameInstance>(GetGameInstance()))
	{
		GI->SetTeamByOrder(Order, PS->Team);
		GI->SetAgentByOrder(Order, PS->GetAgent());
	}
}

void ALobbyGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	ABlasterPlayerState* PS = NewPlayer->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	PS->SetReady(false);

	// Auto-assign to team with fewer players
	PS->SetTeam(GetTeamWithFewerPlayers());
	OnPlayerTeamChanged.Broadcast(PS, PS->Team);

	SaveSelectionsToGameInstance(PS);
}

/*
 * 大厅里一个玩家 Pawn 都不要 —— 每个玩家看的是展示位(ALobbyAgentShowcase)上自己选的英雄，
 * 大厅里再站一个角色没有任何用处（相机锁在展示位，玩家也看不到自己在动）。
 *
 * 为什么必须拦这一下（不拦会怎样）：
 *   GM_LobbyGameMode 蓝图的 DefaultPawnClass 是 BP_BlasterCharacter，
 *   而 AGameMode::StartPlay 里 bDelayedStart 默认 false → ReadyToStartMatch() 直接为真
 *   → StartMatch() → MatchState 变 InProgress。此后每来一个玩家，PostLogin 都会走到
 *   AGameMode::HandleStartingNewPlayer（它只在 IsMatchInProgress() 时才 RestartPlayer）
 *   → 生成一个 BP_BlasterCharacter 摆在大厅里。
 *   ULobbyOverlay 里只藏得掉**本机自己的**那个（别人的 Pawn 在服务器上，所有客户端都看得到），
 *   所以表现就是"自己玩没事，一有客户端加入就多出来一个默认角色"。
 *
 * 为什么用虚函数而不是 bStartPlayersAsSpectators / bDelayedStart 那类属性：
 *   那些是 BlueprintReadWrite，GM_LobbyGameMode 蓝图 CDO 里的快照会把 C++ 构造函数的赋值盖回去
 *   （改完没效果、还得去动资产）。C++ 虚函数蓝图不经手，这里设了就是设了。
 *
 * 不调 Super 是故意的：Super 在这个状态下唯一会做的事就是 RestartPlayer。
 */
void ALobbyGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	UE_LOG(LogTemp, Log, TEXT("[Lobby] %s 以无 Pawn 状态进入大厅（大厅不生成玩家角色）"), *GetNameSafe(NewPlayer));
}

void ALobbyGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	ABlasterPlayerState* PS = Exiting->GetPlayerState<ABlasterPlayerState>();
	if (PS)
	{
		PS->SetReady(false);
		OnPlayerReadyChanged.Broadcast(PS);
	}
}

void ALobbyGameMode::TogglePlayerReady(APlayerController* PC)
{
	if (!PC) return;

	ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	if (PC->IsLocalController() && PC->HasAuthority()) return;

	PS->SetReady(!PS->bIsReady);
	OnPlayerReadyChanged.Broadcast(PS);

	if (IsAllClientsReady())
	{
		// 全就绪：给还没选英雄的人随机补位并展示（含 host 自己），
		// 之后点 Start 出发时每个玩家的 Agent 已是真实英雄
		AssignRandomAgentsToUnselected();
		OnAllClientsReady.Broadcast();
	}
}

void ALobbyGameMode::SelectPlayerAgent(APlayerController* PC, EBlasterAgent Agent)
{
	if (!PC) return;

	// 只接受 None(随机) 或真实可玩英雄
	if (Agent != EBlasterAgent::None && !BlasterAgent::IsPlayable(Agent)) return;

	ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	PS->SetAgent(Agent);
	// 落一份到 GameInstance（按登录顺序），对局 GameMode 据此恢复英雄
	SaveSelectionsToGameInstance(PS);
	// UI 每 0.3s 从 PlayerArray 轮询 PlayerState，无需额外广播
}

void ALobbyGameMode::AssignRandomAgentsToUnselected()
{
	if (!GameState) return;

	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (BPS && !BPS->HasSelectedAgent())
		{
			BPS->SetAgent(BlasterAgent::GetRandomAgent());
			// 随机补位结果也要按登录顺序落 GameInstance，出发后对局按序恢复一致
			SaveSelectionsToGameInstance(BPS);
		}
	}
}

void ALobbyGameMode::SwitchPlayerTeam(APlayerController* PC, ETeam TargetTeam)
{
	if (!PC || TargetTeam == ETeam::ET_None) return;

	ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->Team == TargetTeam) return;

	// Don't allow switching if the target team is full (max 5)
	if (CountPlayersOnTeam(TargetTeam) >= 5) return;

	ETeam OldTeam = PS->Team;
	PS->SetTeam(TargetTeam);
	PS->SetReady(false);

	SaveSelectionsToGameInstance(PS);

	OnPlayerTeamChanged.Broadcast(PS, TargetTeam);
	OnPlayerReadyChanged.Broadcast(PS);
}

bool ALobbyGameMode::IsAllClientsReady() const
{
	if (!GameState) return false;

	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS) continue;

		APlayerController* PC = Cast<APlayerController>(BPS->GetOwner());
		if (PC && PC->IsLocalController() && PC->HasAuthority()) continue;

		if (!BPS->bIsReady) return false;
	}

	return GameState->PlayerArray.Num() > 1;
}

void ALobbyGameMode::StartGame()
{
	if (!IsAllClientsReady()) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// 落点由 MatchMapPath 决定（GM_LobbyGameMode 的 Class Defaults 里改），不再是写死的 BlasterMap。
	FString TravelURL = MatchMapPath.TrimStartAndEnd();
	if (TravelURL.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[关卡流程] ALobbyGameMode::MatchMapPath 是空的 —— 出发按钮点下去不会有任何反应。"));
		return;
	}
	if (!TravelURL.Contains(TEXT("?")))
	{
		TravelURL += TEXT("?listen");
	}

	// 大厅 → 对局图走**无缝旅行**：PlayerState 会被带过去（SeamlessTravelTo），
	// 对局 GameMode 另外还按「登录顺序」从 GameInstance 恢复一遍队伍/英雄
	//（见 BlasterGameMode::OnPostLogin —— PIE 里身份重建顺序不稳，登录顺序才靠谱）。
	bUseSeamlessTravel = true;

	UE_LOG(LogTemp, Log, TEXT("[关卡流程] 大厅出发 → %s"), *TravelURL);

	World->ServerTravel(TravelURL);
}

ETeam ALobbyGameMode::GetTeamWithFewerPlayers() const
{
	int32 CountA = CountPlayersOnTeam(ETeam::ET_TeamA);
	int32 CountB = CountPlayersOnTeam(ETeam::ET_TeamB);
	return CountA <= CountB ? ETeam::ET_TeamA : ETeam::ET_TeamB;
}

int32 ALobbyGameMode::CountPlayersOnTeam(ETeam Team) const
{
	if (!GameState) return 0;

	int32 Count = 0;
	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (BPS && BPS->Team == Team) Count++;
	}
	return Count;
}
