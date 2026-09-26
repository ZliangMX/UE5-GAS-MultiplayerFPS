#include "BlasterGameMode.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/Pickup/UltOrb.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/Weapon/WeaponKillIconSet.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/GameInstance/BlasterGameInstance.h"
#include "Blaster/Barrier/BlastBarrier.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"
#include "TimerManager.h"
#include "EngineUtils.h"	// TActorIterator

namespace MatchState
{
	const FName Cooldown = FName(TEXT("Cooldown"));
	const FName BuyPhase = FName(TEXT("BuyPhase"));
	const FName PreRound = FName(TEXT("PreRound"));   // legacy alias
	const FName PostRound = FName(TEXT("PostRound"));
	const FName GameOver = FName(TEXT("GameOver"));
}

namespace
{
	/*
	 * 这个 PlayerStart 算不算"打了某个出生点 tag"。
	 *
	 * Lotus 里用的是 PlayerStartTag（细节面板的 "Player Start Tag"）——
	 * 已 headless 核实：10 个 PlayerStart 的 Actor Tags 全是空的，tag 都在 PlayerStartTag 上。
	 * 顺手也认一下 Actor 的 Tags 数组：以后改用标签面板打 tag 的话不用回来改代码，
	 * 代价只是一次很短的数组遍历。
	 */
	bool MatchesSpawnTag(const APlayerStart* Start, const FName WantedTag)
	{
		if (!Start || WantedTag.IsNone()) return false;

		if (Start->PlayerStartTag == WantedTag) return true;

		for (const FName& Tag : Start->Tags)
		{
			if (Tag == WantedTag) return true;
		}
		return false;
	}

	/*
	 * 从候选里挑一个**没被别的角色占着**的出生点。
	 *
	 * 为什么要挑：Lotus 的防守方 5 个点彼此只差 130cm 左右，纯随机的话两个人经常叠在一起出生
	 *（胶囊互相挤出去，表现是"我出生在别人身上、被弹到墙角"）。
	 *
	 * 全占着（人比点多）就退回纯随机 —— 宁可重叠也别不生成。
	 */
	APlayerStart* PickUnoccupiedStart(UWorld* World, const TArray<APlayerStart*>& Candidates, float MinSeparation)
	{
		if (Candidates.Num() == 0) return nullptr;
		if (!World) return Candidates[0];

		const float MinSepSq = FMath::Square(MinSeparation);

		TArray<APlayerStart*> Free;
		for (APlayerStart* Start : Candidates)
		{
			bool bOccupied = false;
			for (TActorIterator<ABlasterCharacter> It(World); It; ++It)
			{
				ABlasterCharacter* Other = *It;

				// IsValid 会把"这一帧正在被 Destroy 的上回合角色"过滤掉 ——
				// RespawnAllPlayers 是一边销毁旧角色一边选新点的，那些旧角色还在 actor 表里。
				if (!IsValid(Other)) continue;

				if (FVector::DistSquared2D(Other->GetActorLocation(), Start->GetActorLocation()) < MinSepSq)
				{
					bOccupied = true;
					break;
				}
			}

			if (!bOccupied) Free.Add(Start);
		}

		const TArray<APlayerStart*>& Pool = (Free.Num() > 0) ? Free : Candidates;
		return Pool[FMath::RandRange(0, Pool.Num() - 1)];
	}

	/*
	 * 击杀者**手上那把枪**配的击杀图标集 —— 击杀确认标记画哪套图由这里决定。
	 *
	 * 在服务器这边取（而不是让客户端自己去读手上的枪）有两个好处：
	 *   · 服务器才是"这一枪是谁用什么打的"的权威，客户端不用猜；
	 *   · 客户端收到 RPC 后 HUD 直接拿这套图，中途换枪/丢枪都不会把已经弹出来的图标换掉。
	 * 取不到角色 / 手上没枪 / 枪上没配资产都返回 nullptr，HUD 会回落到 C++ 的矢量造型，
	 * 所以这个函数永远不需要报错。
	 */
	UWeaponKillIconSet* GetKillerKillIconSet(AController* KillerController)
	{
		if (!KillerController) return nullptr;

		ABlasterCharacter* KillerChar = Cast<ABlasterCharacter>(KillerController->GetPawn());
		if (!KillerChar) return nullptr;

		AWeapon* KillerWeapon = KillerChar->GetEquippedWeapon();
		return KillerWeapon ? KillerWeapon->KillIconSet : nullptr;
	}
}

ABlasterGameMode::ABlasterGameMode()
{
	bDelayedStart = true;
	GameStateClass = ABlasterGameState::StaticClass();

	// 出生/兜底手枪：默认用项目现成的 BP_Pistol，可在 GameMode 蓝图里覆盖
	static ConstructorHelpers::FClassFinder<AWeapon> PistolFinder(TEXT("/Game/Blueprints/Weapon/BP_Pistol"));
	if (PistolFinder.Succeeded())
	{
		DefaultPistolClass = PistolFinder.Class;
	}

	// 出生自带的近战武器（3 号槽的刀）：每次重生都发一把，蒙太奇挂在它的蓝图里
	static ConstructorHelpers::FClassFinder<AWeapon> MeleeFinder(TEXT("/Game/Blueprints/Weapon/BP_Melee"));
	if (MeleeFinder.Succeeded())
	{
		DefaultMeleeClass = MeleeFinder.Class;
	}

	// 测试机器人默认用玩家角色蓝图（复用模型/动画/碰撞），可在 GameMode 蓝图里覆盖
	static ConstructorHelpers::FClassFinder<ABlasterCharacter> BotCharacterFinder(TEXT("/Game/Blueprints/Character/BP_BlasterCharacter"));
	if (BotCharacterFinder.Succeeded())
	{
		TestBotCharacterClass = BotCharacterFinder.Class;
	}
}

void ABlasterGameMode::BeginPlay()
{
	Super::BeginPlay();
	LevelStartingTime = GetWorld()->GetTimeSeconds();

	// Find spike on the map
	TArray<AActor*> Spikes;
	UGameplayStatics::GetAllActorsOfClass(this, ASpike::StaticClass(), Spikes);
	if (Spikes.Num() > 0)
	{
		CurrentSpike = Cast<ASpike>(Spikes[0]);
	}

	CacheInitialWeaponSpawns();

	// 开局是热身/等待阶段（bDelayedStart=true → MatchState 停在 WaitingToStart，
	// OnMatchStateSet 不会被调到），场上不该有出生屏障。这里显式收一次，
	// 免得关卡里摆好的墙在热身阶段就立着。
	SetBarrierWallsActive(false);
}

void ABlasterGameMode::OnPostLogin(AController* NewPlayer)
{
	Super::OnPostLogin(NewPlayer);

	ABlasterPlayerState* PS = NewPlayer->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// Lobby → 对局：英雄和队伍都按「登录顺序」从 GameInstance 恢复（跟 Lobby 同一份
	// PlayerOrder/NextOrder key）。PIE 里玩家身份/PlayerState 重建顺序不稳定，
	// 单靠 SeamlessTravelTo 逐人带 Agent 会串线；登录顺序跟分队一样才稳定。
	UBlasterGameInstance* GI = Cast<UBlasterGameInstance>(GetGameInstance());
	const int32 Order = NextOrder++; // 每个登录消费一个序号，与 Lobby 分配的登录序号对齐
	const EBlasterAgent SavedAgent = GI ? GI->GetAgentByOrder(Order) : EBlasterAgent::None;
	const ETeam SavedTeam = GI ? GI->GetTeamByOrder(Order) : ETeam::ET_None;

	// 英雄：Lobby 选过/全就绪随机补过就带过来；
	// 没记录（没走 Lobby 直接开对局 / None=随机）→ 出生前随机兜底，保证按英雄出角色
	PS->SetAgent(BlasterAgent::IsPlayable(SavedAgent) ? SavedAgent : BlasterAgent::GetRandomAgent());

	// 队伍：Lobby 选过直接恢复；没保存过 → 按人数平衡分配
	if (SavedTeam != ETeam::ET_None)
	{
		PS->SetTeam(SavedTeam);
		ABlasterGameState* GS = GetBlasterGameState();
		if (GS) GS->UpdateTeamCounts();
		return;
	}

	// 没保存过 → 按人数平衡分配
	int32 CountA = 0, CountB = 0;
	ABlasterGameState* GS = GetBlasterGameState();
	if (GS)
	{
		for (APlayerState* OtherPS : GS->PlayerArray)
		{
			ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(OtherPS);
			if (BPS && BPS != PS)
			{
				if (BPS->Team == ETeam::ET_TeamA) CountA++;
				else if (BPS->Team == ETeam::ET_TeamB) CountB++;
			}
		}
	}

	PS->SetTeam(CountA <= CountB ? ETeam::ET_TeamA : ETeam::ET_TeamB);

	if (GS)
	{
		GS->UpdateTeamCounts();
	}
}

void ABlasterGameMode::Logout(AController* Exiting)
{
	ABlasterGameState* GS = GetBlasterGameState();
	if (GS)
	{
		GS->UpdateTeamCounts();
		GS->UpdateAliveCounts();
	}

	Super::Logout(Exiting);
	CheckRoundEnd();
}

UClass* ABlasterGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	if (InController)
	{
		if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(InController))
		{
			switch (PC->GetAgent())
			{
			case EBlasterAgent::Sage:
				if (SageCharacterClass)
				{
					return SageCharacterClass.Get();
				}
				break;
			case EBlasterAgent::Phoenix:
				if (PhoenixCharacterClass)
				{
					return PhoenixCharacterClass.Get();
				}
				break;
			case EBlasterAgent::Clove:
				if (CloveCharacterClass)
				{
					return CloveCharacterClass.Get();
				}
				break;
			case EBlasterAgent::Jett:
				if (JettCharacterClass)
				{
					return JettCharacterClass.Get();
				}
				break;
			default:
				break; // 没填的英雄 / 未选英雄 → 默认角色类（DefaultPawnClass）
			}
		}
	}

	// 走到这里 = 这个英雄没配角色类（或还没选英雄）→ 落到默认角色类上。
	// 默认类（GM 上的 DefaultPawnClass）是不带任何英雄技能的基类实例，表现是
	// 「选了英雄，出生了，但一个技能都放不出来」—— 而且什么都不报，极难查。
	// 这里显式吼一声：要么去 GM 上把对应的 XxxCharacterClass 填上，要么这就是故意的。
	if (const ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(InController))
	{
		const EBlasterAgent Agent = PC->GetAgent();
		if (BlasterAgent::IsPlayable(Agent))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[英雄选择] %s 选了 %s，但 GM 上对应的 XxxCharacterClass 没配 → 落到默认角色类（没有英雄技能）"),
				*PC->GetName(), *BlasterAgent::ToDisplayName(Agent));
		}
	}

	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

void ABlasterGameMode::OnMatchStateSet()
{
	Super::OnMatchStateSet();

	// Reset timer baseline for every phase transition
	LevelStartingTime = GetWorld()->GetTimeSeconds();

	ABlasterGameState* GS = GetBlasterGameState();

	for (FConstControllerIterator Iterator = GetWorld()->GetControllerIterator(); Iterator; ++Iterator)
	{
		ABlasterPlayerController* BlasterPlayer = Cast<ABlasterPlayerController>(*Iterator);
		if (!BlasterPlayer) continue;

		BlasterPlayer->OnMatchStateSet(MatchState);

		if (MatchState == MatchState::BuyPhase)
		{
			if (!BlasterPlayer->GetPawn())
			{
				RestartPlayer(BlasterPlayer);
				// 中途加入/尚未重生的玩家：发一把默认手枪（有继承武器则恢复）
				RestorePlayerWeapons(BlasterPlayer);
			}
			else
			{
				BlasterPlayer->ResetIgnoreInputFlags();
				BlasterPlayer->UnFreeze();
			}

			if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(BlasterPlayer->GetPawn()))
			{
				Character->bIsInvulnerable = true;
			}
		}
		else if (MatchState == MatchState::InProgress)
		{
			if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(BlasterPlayer->GetPawn()))
			{
				Character->bIsInvulnerable = false;
			}
		}
	}

	// Sync the new LevelStartingTime to all clients so their countdown is accurate
	SyncPhaseTimeToClients();

	if (MatchState == MatchState::BuyPhase)
	{
		StartNewRound();
		if (GS) GS->OnRoundPhaseChanged.Broadcast(MatchState::BuyPhase);

		// 购买阶段：出生屏障升起，把双方关在各自的出生区里选枪/买枪。
		// 放在 StartNewRound 之后 —— 那里面刚把所有玩家重生回出生点，墙这时候立起来正好合围。
		SetBarrierWallsActive(true);
	}
	else if (MatchState == MatchState::InProgress)
	{
		if (GS) GS->OnRoundPhaseChanged.Broadcast(MatchState::InProgress);

		// 购买阶段一结束，屏障立刻消失（用户的时序要求：不是"几秒后"，是这一拍就落）
		SetBarrierWallsActive(false);
	}
	else if (MatchState == MatchState::PostRound)
	{
		// 回合结束不再对 spike 做任何动作（跟正常游戏时间一样，保留原位）
		if (GS) GS->OnRoundPhaseChanged.Broadcast(MatchState::PostRound);

		// 兜底：正常路径下进 PostRound 时屏障早在 InProgress 就落了，
		// 但 EndRound 也可能被直接调到（拆包/全灭 → CheckRoundEnd），这里不留下"墙还立着"的可能。
		SetBarrierWallsActive(false);
	}
	else if (MatchState == MatchState::GameOver)
	{
		ETeam Winner = ETeam::ET_None;
		if (GS)
		{
			Winner = GS->TeamAScore >= GS->TotalRoundsToWin ? ETeam::ET_TeamA : ETeam::ET_TeamB;
			GS->OnMatchOver.Broadcast(Winner);
		}

		SetBarrierWallsActive(false);
	}
}

void ABlasterGameMode::SetBarrierWallsActive(bool bActive)
{
	/*
	 * 这个函数只在服务器上跑（GameMode 只存在于服务器），所以"墙开不开"的判断天然是权威的。
	 * 真正的显隐**不在**这里做 —— 这里只是把服务器上的 bool 翻过去，
	 * 每台机器（包括 listen server 自己）由 ABlastBarrier::ApplyState 各自落地
	 *（见那个类头文件里"为什么状态要复制"）。
	 */
	int32 Count = 0;
	for (TActorIterator<ABlastBarrier> It(GetWorld()); It; ++It)
	{
		It->SetBarrierActive(bActive);
		Count++;
	}

	if (Count == 0 && bActive)
	{
		// 没摆墙不算错（别的关卡本来就没有），但"以为摆了其实是空的"很难看出来，吼一声
		UE_LOG(LogTemp, Warning,
			TEXT("[出生屏障] 场上一个 ABlastBarrier 都没有 —— 购买阶段不会有限制行动区域的墙。"));
	}
}

AActor* ABlasterGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	/*
	 * 按队伍把出生点分开。
	 *
	 * TeamA = 进攻、TeamB = 防守（和 AssignSpikeToRandomAttacker 里"进攻方在 TeamA"、
	 * 以及 Tick 里"时间耗尽判 TeamB 赢"这两处一致）。
	 * 队伍还没分配（ET_None）时 WantedTag 保持 None → 直接走引擎默认，不硬塞进攻方出生点。
	 */
	FName WantedTag = NAME_None;
	if (Player)
	{
		const ABlasterPlayerState* PS = Player->GetPlayerState<ABlasterPlayerState>();
		if (PS)
		{
			if (PS->Team == ETeam::ET_TeamA)
			{
				WantedTag = AttackSpawnTag;
			}
			else if (PS->Team == ETeam::ET_TeamB)
			{
				WantedTag = DefendSpawnTag;
			}
		}
	}

	if (!WantedTag.IsNone())
	{
		TArray<APlayerStart*> Candidates;
		for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
		{
			APlayerStart* Start = *It;
			if (IsValid(Start) && MatchesSpawnTag(Start, WantedTag))
			{
				Candidates.Add(Start);
			}
		}

		if (APlayerStart* Chosen = PickUnoccupiedStart(GetWorld(), Candidates, MinSpawnSeparation))
		{
			return Chosen;
		}

		// 一张地图可能只有一张打了 tag；另一队就会走到这里。别静默 ——
		// 表现是"这一队被随机扔到全场任意出生点"，不报错、很难查。
		UE_LOG(LogTemp, Warning,
			TEXT("[出生点] 找不到打了 tag '%s' 的 PlayerStart → 这一队退回引擎默认的随机出生点。"),
			*WantedTag.ToString());
	}

	return Super::ChoosePlayerStart_Implementation(Player);
}

void ABlasterGameMode::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (MatchState == MatchState::WaitingToStart)
	{
		CountDownTime = WarmupDuration - GetWorld()->GetTimeSeconds() + LevelStartingTime;
		if (CountDownTime <= 0.f)
		{
			SetMatchState(MatchState::BuyPhase);
		}
	}
	else if (MatchState == MatchState::BuyPhase)
	{
		CountDownTime = BuyPhaseTime - GetWorld()->GetTimeSeconds() + LevelStartingTime;
		if (CountDownTime <= 0.f)
		{
			SetMatchState(MatchState::InProgress);
		}
	}
	else if (MatchState == MatchState::InProgress)
	{
		// 每帧兜底检查淘汰胜利（PlayerEliminated 里也会立即检查；
		// 这里防止个别击杀路径漏掉 PlayerEliminated 导致全灭后回合不结束）
		CheckRoundEnd();

		// 安包后旧回合倒计时失效，由 spike 爆炸/拆包决定回合何时结束
		//（爆炸倒计时结束才进入 PostRound，不会因回合时间耗尽提前结束）
		ABlasterGameState* GS = GetBlasterGameState();
		if (!GS || !GS->bSpikePlanted)
		{
			CountDownTime = RoundTime - GetWorld()->GetTimeSeconds() + LevelStartingTime;
			if (CountDownTime <= 0.f)
			{
				EndRound(ETeam::ET_TeamB);
			}
		}
	}
	else if (MatchState == MatchState::PostRound)
	{
		CountDownTime = PostRoundTime - GetWorld()->GetTimeSeconds() + LevelStartingTime;
		if (CountDownTime <= 0.f)
		{
			ABlasterGameState* GS = GetBlasterGameState();
			if (GS && GS->IsMatchOver())
			{
				SetMatchState(MatchState::GameOver);
			}
			else
			{
				if (GS && GS->IsSwapRound())
				{
					SwapTeams();
				}
				SetMatchState(MatchState::BuyPhase);
			}
		}
	}
	else if (MatchState == MatchState::GameOver)
	{
		CountDownTime = GameOverTime - GetWorld()->GetTimeSeconds() + LevelStartingTime;
		if (CountDownTime <= 0.f)
		{
			// 13 分打满、胜负已分 → 回菜单图，而不是原地再开一局
			//（RestartGame 是 ServerTravel("?Restart")，只在同一张图里重开）
			TravelToMenuMap();
		}
	}
}

void ABlasterGameMode::TravelToMenuMap()
{
	/*
	 * 一方先到 13 分（GameState::TotalRoundsToWin）→ GameOver 倒计时走完 → 服务器带所有人回菜单图。
	 *
	 * 为什么是 ServerTravel 而不是"回主菜单"那套（AGameSession::ReturnToMainMenuHost /
	 * GEngine->HandleDisconnect）：那两条路是"拆会话 + 各端自己 ClientTravel 回默认图"，
	 * 中间全靠 OnlineSession 的回调驱动，什么时候真的换图不好控。
	 * ServerTravel 在 LobbyGameMode::StartGame（Lobby → 对局图）上已经验证过
	 * listen server 带着客户端一起走，这里照抄同一套。
	 *
	 * 只有服务器会走到这里（GameMode 只存在于服务器），不用做角色判断。
	 */
	UWorld* World = GetWorld();
	if (!World) return;

	// 只发一次：GameOver 倒计时走完后 CountDownTime 一直是负数，Tick 每帧都会进来
	if (bMatchEndTravelStarted) return;
	bMatchEndTravelStarted = true;

	const FString Target = MenuMapPath.TrimStartAndEnd();
	if (Target.IsEmpty())
	{
		// 没配路径 → 退回旧行为：同一张图再开一局（方便反复测对局，不用每次都过菜单）
		RestartGame();
		return;
	}

	// 路径自带 ?xxx 就别再拼（有人可能直接写成 /Game/Maps/GameStartupMap?listen）
	FString TravelURL = Target;
	if (!TravelURL.Contains(TEXT("?")))
	{
		TravelURL += TEXT("?listen");
	}

	// 硬旅行：回菜单＝这局翻篇，世界整体重建，分数/回合/队伍/spike 全归零。
	// 无缝的话 PlayerState 会一路带进菜单图（那张图用的是引擎默认 GameMode，没打算处理这些残留）。
	// 玩家 ID 不受影响 —— 它存在 BlasterGameInstance 里，GameInstance 跨旅行不重建。
	bUseSeamlessTravel = bSeamlessTravelToMenu;

	UE_LOG(LogTemp, Log, TEXT("[关卡流程] 对局结束，服务器回菜单图 %s（无缝旅行=%s）"),
		*TravelURL, bSeamlessTravelToMenu ? TEXT("是") : TEXT("否"));

	World->ServerTravel(TravelURL);
}

void ABlasterGameMode::PlayerEliminated(ABlasterCharacter* ElimmedCharacter,
	ABlasterPlayerController* VictimController, ABlasterPlayerController* AttackerController)
{
	// 测试机器人：无 VictimController（没被玩家控制），走独立测试分支
	//（击杀计到玩家头上 + 弹击杀标记 + 自动复活），不碰正式计分/回合逻辑。
	if (!VictimController)
	{
		HandleTestBotEliminated(ElimmedCharacter, AttackerController);
		return;
	}

	ABlasterPlayerState* AttackerPS = AttackerController ? Cast<ABlasterPlayerState>(AttackerController->PlayerState) : nullptr;
	ABlasterPlayerState* VictimPS = Cast<ABlasterPlayerState>(VictimController->PlayerState);

	if (!VictimPS) return;

	// Award score if attacker != victim and both have teams
	if (AttackerPS && VictimPS != AttackerPS && AttackerPS->Team != VictimPS->Team)
	{
		AttackerPS->AddToScore(1.f);
		AttackerPS->AddCredits(KillReward);

		// 大招点：击杀 +1（Valorant 同款；死也涨，见下面 VictimPS 那行）
		AttackerPS->AddUltPoints(1);

		// 击杀确认标记：本回合击杀数 +1，并通知击杀者客户端按档位弹出
		AttackerPS->AddRoundKill();
		UWeaponKillIconSet* KillIcons = GetKillerKillIconSet(AttackerController);
		UE_LOG(LogTemp, Warning, TEXT("[KillMarker] GameMode 击杀: tier=%d, 走%s, 图标集=%s"),
			AttackerPS->GetRoundKills(), AttackerController->IsLocalController() ? TEXT("本地直调") : TEXT("Client RPC"),
			KillIcons ? *KillIcons->GetName() : TEXT("无（用矢量造型）"));
		if (AttackerController->IsLocalController())
		{
			AttackerController->ShowKillMarker(AttackerPS->GetRoundKills(), KillIcons);
		}
		else
		{
			AttackerController->ClientShowKillMarker(AttackerPS->GetRoundKills(), KillIcons);
		}
	}

	VictimPS->AddToDefeats(1);
	// 大招点：阵亡 +1（Valorant 的"死也攒大招"，让落后方不至于完全没节奏）
	VictimPS->AddUltPoints(1);

	ABlasterGameState* GS = GetBlasterGameState();
	if (GS)
	{
		GS->OnPlayerKilled(VictimPS->Team);
		// 击杀信息：广播给所有客户端 → HUD 右上角 Kill Feed
		GS->MulticastKillFeed(
			AttackerPS ? AttackerPS->GetPlayerName() : FString(TEXT("???")),
			VictimPS->GetPlayerName()
		);
	}

	if (ElimmedCharacter)
	{
		ElimmedCharacter->Elim();
	}

	CheckRoundEnd();
}

void ABlasterGameMode::HandleTestBotEliminated(ABlasterCharacter* Bot, ABlasterPlayerController* AttackerController)
{
	// 机器人已经死亡（重复伤害/死亡前又命中）就不重复计分
	if (!Bot || Bot->IsElimmed()) return;

	// 任意一方击杀都算成玩家自己的击杀（测试击杀图标用）：
	// 有攻击者用攻击者；没攻击者（炸/摔等）就把击杀记到场上第一个真人头上
	ABlasterPlayerController* Killer = AttackerController;
	if (!Killer)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(*It);
			if (PC && PC->PlayerState)
			{
				Killer = PC;
				break;
			}
		}
	}

	ABlasterPlayerState* KillerPS = Killer ? Cast<ABlasterPlayerState>(Killer->PlayerState) : nullptr;
	if (KillerPS)
	{
		KillerPS->AddToScore(1.f);
		KillerPS->AddCredits(KillReward);
		KillerPS->AddUltPoints(1);
		KillerPS->AddRoundKill();

		UWeaponKillIconSet* BotKillIcons = GetKillerKillIconSet(Killer);
		UE_LOG(LogTemp, Warning, TEXT("[KillMarker] 测试机器人击杀: tier=%d, 走%s, 图标集=%s"),
			KillerPS->GetRoundKills(), Killer->IsLocalController() ? TEXT("本地直调") : TEXT("Client RPC"),
			BotKillIcons ? *BotKillIcons->GetName() : TEXT("无（用矢量造型）"));
		if (Killer->IsLocalController())
		{
			Killer->ShowKillMarker(KillerPS->GetRoundKills(), BotKillIcons);
		}
		else
		{
			Killer->ClientShowKillMarker(KillerPS->GetRoundKills(), BotKillIcons);
		}
	}

	ABlasterGameState* GS = GetBlasterGameState();
	if (GS)
	{
		GS->MulticastKillFeed(
			KillerPS ? KillerPS->GetPlayerName() : FString(TEXT("???")),
			FString(TEXT("Test Bot"))
		);
	}

	// 死亡视觉（溶解/倒地），尸体留着等复活时清理
	Bot->Elim();

	// 找到对应格子，安排延迟复活
	for (int32 i = 0; i < TestBotSlots.Num(); i++)
	{
		if (TestBotSlots[i].Bot == Bot)
		{
			TestBotSlots[i].Bot = nullptr;
			TestBotSlots[i].DeadBody = Bot;
			GetWorldTimerManager().SetTimer(
				TestBotSlots[i].RespawnTimer,
				FTimerDelegate::CreateUObject(this, &ABlasterGameMode::RespawnTestBot, i),
				TestBotRespawnDelay,
				false
			);
			return;
		}
	}
}

void ABlasterGameMode::SpawnTestBots()
{
	if (bTestBotsSpawned || NumTestBots <= 0) return;
	bTestBotsSpawned = true;

	TestBotSlots.Reset();

	// 中场开阔点位：出生点簇（X=-2744，Y=-1715..-2175）与安装区（X=-1658）之间，
	// 朝向出生点（yaw=180 = 面向 -X），玩家出生后往前看就能看到它们站在场地里
	static const FVector BotSpots[] = {
		FVector(-2550.f, -2075.f, 500.f),
		FVector(-2450.f, -2225.f, 500.f),
		FVector(-2350.f, -1925.f, 500.f),
		FVector(-2250.f, -2075.f, 500.f),
	};

	const int32 Count = FMath::Min(NumTestBots, (int32)UE_ARRAY_COUNT(BotSpots));
	for (int32 i = 0; i < Count; i++)
	{
		FTestBotSlot Slot;

		// 从高处向下打线求真实地面高度（防止悬空/埋地里），失败就用原始点让重力自然下落
		FVector Start = BotSpots[i];
		FHitResult Hit;
		FCollisionQueryParams QueryParams(FName(TEXT("TestBotGround")), false, this);
		if (GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, 10000.f), ECC_Visibility, QueryParams) && Hit.bBlockingHit)
		{
			Slot.SpawnLocation = Hit.ImpactPoint + FVector(0.f, 0.f, 110.f);
		}
		else
		{
			Slot.SpawnLocation = Start;
		}

		TestBotSlots.Add(Slot);
		SpawnTestBot(i);
	}
}

void ABlasterGameMode::SpawnTestBot(int32 Index)
{
	if (!TestBotSlots.IsValidIndex(Index)) return;

	TSubclassOf<ABlasterCharacter> BotClass = TestBotCharacterClass;
	if (!BotClass)
	{
		// 兜底：运行时装默认角色蓝图（防止 GameMode 蓝图没配置 TestBotCharacterClass）
		BotClass = LoadClass<ABlasterCharacter>(nullptr, TEXT("/Game/Blueprints/Character/BP_BlasterCharacter.BP_BlasterCharacter_C"));
	}
	if (!BotClass) return;

	FTestBotSlot& Slot = TestBotSlots[Index];
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	Slot.Bot = GetWorld()->SpawnActor<ABlasterCharacter>(BotClass, Slot.SpawnLocation, FRotator(0.f, 180.f, 0.f), SpawnParams);
	if (Slot.Bot)
	{
		Slot.Bot->SetTestBot(true);
		// 靶子不做任何事：无控制器，站着不动（重力落地后即站立）
	}
}

void ABlasterGameMode::RespawnTestBot(int32 Index)
{
	if (!TestBotSlots.IsValidIndex(Index)) return;

	FTestBotSlot& Slot = TestBotSlots[Index];
	if (Slot.DeadBody)
	{
		Slot.DeadBody->Destroy();
		Slot.DeadBody = nullptr;
	}
	SpawnTestBot(Index);
}

void ABlasterGameMode::RequestRespawn(ACharacter* ElimmedCharacter, AController* ElimmedController)
{
	// In round-based mode, no respawn. Do nothing — the character stays as spectator
	// until next round via StartNewRound() -> RespawnAllPlayers().
}

void ABlasterGameMode::StartNewRound()
{
	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	GS->RoundNumber++;
	GS->bSpikePlanted = false;

	AssignTeams();
	RespawnAllPlayers();
	RespawnMapWeapons();
	ResetAllUltOrbs();
	if (CurrentSpike) CurrentSpike->ResetSpike();
	AssignSpikeToRandomAttacker();

	GS->UpdateTeamCounts();
	GS->UpdateAliveCounts();

	RoundWinnerTeam = ETeam::ET_None;
	GS->RoundWinnerTeam = ETeam::ET_None;   // 复制的那份一起清，别让新回合读到上回合的赢家

	// 本回合击杀计数归零（击杀确认标记新回合重新从 1 杀档位开始）
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS))
		{
			BPS->ResetRoundKills();
		}
	}

	// 首个回合摆测试机器人（之后每回合跳过，被击杀的机器人走自己的复活定时器）
	SpawnTestBots();
}

void ABlasterGameMode::EndRound(ETeam WinningTeam)
{
	if (RoundWinnerTeam != ETeam::ET_None) return;

	RoundWinnerTeam = WinningTeam;
	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	if (WinningTeam == ETeam::ET_TeamA)
	{
		GS->TeamAScore++;
	}
	else if (WinningTeam == ETeam::ET_TeamB)
	{
		GS->TeamBScore++;
	}

	// 复制给客户端：HUD 要靠它判"我这边赢没赢"来决定公告栏出 WON 还是 LOST（见 BlasterGameState.h）
	GS->RoundWinnerTeam = WinningTeam;

	GS->OnRoundEnded.Broadcast(WinningTeam, GS->TeamAScore, GS->TeamBScore);

	AwardRoundCredits(WinningTeam);

	SetMatchState(MatchState::PostRound);
}

void ABlasterGameMode::AwardRoundCredits(ETeam WinningTeam)
{
	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None) continue;

		BPS->AddCredits(BPS->Team == WinningTeam ? WinRoundReward : LossRoundReward);
	}
}

void ABlasterGameMode::AssignTeams()
{
	// Reassign on each round start — teams are persistent, just ensure all assigned
	// （OnPostLogin 已按登录顺序从 GameInstance 恢复队伍，这里只对没恢复成功的做兜底）
	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	int32 CountA = 0, CountB = 0;
	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS) continue;
		if (BPS->Team == ETeam::ET_TeamA) CountA++;
		else if (BPS->Team == ETeam::ET_TeamB) CountB++;
	}

	// 仍全部未分配才按顺序兜底分配
	if (CountA == 0 && CountB == 0)
	{
		for (int32 i = 0; i < GS->PlayerArray.Num(); i++)
		{
			ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(GS->PlayerArray[i]);
			if (BPS)
			{
				BPS->SetTeam(i % 2 == 0 ? ETeam::ET_TeamA : ETeam::ET_TeamB);
			}
		}
	}
}

void ABlasterGameMode::AssignSpikeToRandomAttacker()
{
	if (!CurrentSpike) return;

	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	TArray<ABlasterPlayerState*> Attackers;
	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (BPS && BPS->Team == ETeam::ET_TeamA && BPS->GetPawn())
		{
			Attackers.Add(BPS);
		}
	}

	if (Attackers.Num() > 0)
	{
		int32 Index = FMath::RandRange(0, Attackers.Num() - 1);
		ABlasterCharacter* Attacker = Cast<ABlasterCharacter>(Attackers[Index]->GetPawn());
		if (Attacker)
		{
			CurrentSpike->PickUp(Attacker);
		}
	}
}

void ABlasterGameMode::RespawnAllPlayers()
{
	for (FConstControllerIterator Iterator = GetWorld()->GetControllerIterator(); Iterator; ++Iterator)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(*Iterator);
		if (!PC) continue;

		// 销毁角色前把身上的武器记到 PlayerState，供下一回合继承
		SavePlayerWeapons(PC);

		if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn()))
		{
			// 回合切换是直接 Destroy 角色，走不到大招的正常收尾出口（ServerEndUltimate）——
			// 手上还开着大的（回合结束时刀还没扔完的 Jett / 还在回程状态的 Phoenix）在这里补收一次，
			// 把那次"结尾扣点"补上。否则那个大招的点数被跳过，下回合白捡一个大招。
			//
			// 收过尾的角色（ActiveUltimate 已清）调它什么也不做，所以死了一整回合的人不受影响。
			// ⚠️ 必须在 SavePlayerWeapons **之后**：那样扣点补收带来的血量/护甲变化
			//    （Phoenix 回程会满血满甲）不会被当成"上回合剩下的甲"存进 PlayerState。
			Character->ServerSettleActiveUltimate();

			Character->Reset();
			Character->Destroy();
		}
		RestartPlayerAtPlayerStart(PC, ChoosePlayerStart(PC));

		// 重生后恢复上回合武器；没有副武器则发一把默认手枪
		RestorePlayerWeapons(PC);
	}
}

// 注意：这个函数现在同时管「武器继承」和「护甲继承」（都是跨回合保留的东西），
// 所以护甲那段必须放在 Combat 的判空**之前** —— Combat 拿不到时武器没法存，
// 但护甲还是该存下来。
void ABlasterGameMode::SavePlayerWeapons(ABlasterPlayerController* PC)
{
	if (!PC) return;

	ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn());
	if (!Character) return;

	ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// 护甲：血量每回合回满，甲不回（Valorant 语义）
	PS->StoredArmor = FMath::RoundToInt(Character->GetArmor());

	UCombatComponent* Combat = Character->GetCombatComponent();
	if (!Combat) return;

	PS->SaveWeaponsForInheritance(Combat->PrimaryWeapon, Combat->SecondaryWeapon);
}

void ABlasterGameMode::RestorePlayerWeapons(ABlasterPlayerController* PC)
{
	if (!PC) return;

	ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn());
	if (!Character) return;

	ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// 护甲：恢复上一回合剩下的甲（血量已由 ResetHealth 回满，甲不回）。
	// 同样放在 Combat 判空之前 —— 甲不依赖武器槽。
	Character->SetArmor(static_cast<float>(PS->StoredArmor));

	UCombatComponent* Combat = Character->GetCombatComponent();
	if (!Combat) return;

	// 主武器：上回合买/捡的主武器
	if (PS->StoredPrimaryWeaponClass)
	{
		if (AWeapon* Primary = SpawnInheritedWeapon(PS->StoredPrimaryWeaponClass, PS->StoredPrimaryAmmo, Character))
		{
			Combat->EquipWeapon(Primary);
		}
	}

	// 副武器：上回合的副武器，否则发默认手枪（-1 = 用武器默认满弹）
	TSubclassOf<AWeapon> SecondaryClass = PS->StoredSecondaryWeaponClass ? PS->StoredSecondaryWeaponClass : DefaultPistolClass;
	if (!SecondaryClass)
	{
		// 兜底：运行时装默认手枪蓝图（防止 GameMode 蓝图没配置 DefaultPistolClass）
		SecondaryClass = LoadClass<AWeapon>(nullptr, TEXT("/Game/Blueprints/Weapon/BP_Pistol.BP_Pistol_C"));
	}
	if (SecondaryClass)
	{
		const int32 SecondaryAmmo = PS->StoredSecondaryWeaponClass ? PS->StoredSecondaryAmmo : -1;
		if (AWeapon* Secondary = SpawnInheritedWeapon(SecondaryClass, SecondaryAmmo, Character))
		{
			Combat->EquipWeapon(Secondary);
		}
	}

	// 近战武器（3 号槽的刀）：**角色天生自带**，所以不继承、也不花 Credits，
	// 每次重生都原地发一把新的（旧的那把随角色一起销毁了，见 ABlasterCharacter::Destroyed）。
	//
	// 放最后、而且是 Grant（不掏）而不是 Equip：上面那句"没有主武器就保持手枪"意味着
	// 出生时手上是手枪；用 Equip 的话刀会把它顶掉，玩家一出生就握着刀 —— 那不是"自带一把刀"，
	// 那是"出生默认拿刀"。玩家按 3 才切出来。
	{
		TSubclassOf<AWeapon> MeleeClass = DefaultMeleeClass;
		if (!MeleeClass)
		{
			// 兜底：运行时装默认的近战武器蓝图（防止 GameMode 蓝图没配置 DefaultMeleeClass）
			MeleeClass = LoadClass<AWeapon>(nullptr, TEXT("/Game/Blueprints/Weapon/BP_Melee.BP_Melee_C"));
		}
		if (MeleeClass)
		{
			if (AWeapon* Melee = SpawnInheritedWeapon(MeleeClass, /*Ammo=*/-1, Character))
			{
				Combat->GrantMeleeWeapon(Melee);
			}
		}
	}

	// 保证出生后主武器在手（没有主武器则保持手枪）
	if (Combat->PrimaryWeapon)
	{
		Combat->SwitchWeapon(EWeaponSlot::ESlot_Primary);
	}
}

AWeapon* ABlasterGameMode::SpawnInheritedWeapon(TSubclassOf<AWeapon> WeaponClass, int32 Ammo, ABlasterCharacter* Character)
{
	if (!WeaponClass || !Character) return nullptr;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Character;
	AWeapon* Weapon = GetWorld()->SpawnActor<AWeapon>(WeaponClass, SpawnParams);
	if (!Weapon) return nullptr;

	// Ammo < 0 表示用武器蓝图默认值（满弹）
	if (Ammo >= 0)
	{
		Weapon->SetAmmo(Ammo);
	}
	return Weapon;
}

void ABlasterGameMode::SwapTeams()
{
	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS) continue;

		if (BPS->Team == ETeam::ET_TeamA) BPS->SetTeam(ETeam::ET_TeamB);
		else if (BPS->Team == ETeam::ET_TeamB) BPS->SetTeam(ETeam::ET_TeamA);
	}

	GS->UpdateTeamCounts();
	GS->UpdateAliveCounts();

	GS->OnTeamsSwapped.Broadcast();
}

void ABlasterGameMode::CheckRoundEnd()
{
	if (MatchState != MatchState::InProgress) return;
	if (RoundWinnerTeam != ETeam::ET_None) return;

	ABlasterGameState* GS = GetBlasterGameState();
	if (!GS) return;

	if (GS->SpikeState == ESpikeState::ESS_Exploded)
	{
		EndRound(ETeam::ET_TeamA);
		return;
	}
	if (GS->SpikeState == ESpikeState::ESS_Defused)
	{
		EndRound(ETeam::ET_TeamB);
		return;
	}

	// 存活数直接从 PlayerArray + bElimmed 实时重算，不依赖击杀扣减计数。
	// （旧 OnPlayerKilled 扣减在个别击杀路径下会漏数/漂移，导致全灭后不判胜）
	int32 AliveA = 0, AliveB = 0;
	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None) continue;

		ABlasterCharacter* Character = Cast<ABlasterCharacter>(BPS->GetPawn());
		if (!Character || Character->IsElimmed()) continue;

		if (BPS->Team == ETeam::ET_TeamA) AliveA++;
		else AliveB++;
	}
	GS->AliveCountTeamA = AliveA;
	GS->AliveCountTeamB = AliveB;

	// Valorant 团灭规则（TeamA=匪/携包方，TeamB=警/拆包方）：
	// - 未下包：任意一方全灭立即判胜
	// - 已下包：匪杀光警 → 匪立即赢（无人能拆包）；警杀光匪 → 警不立即赢，
	//   必须拆包才算赢（拆成 TeamB 赢 / 爆成 TeamA 赢），spike 计时继续走
	if (AliveA <= 0 && !GS->bSpikePlanted)
	{
		UE_LOG(LogTemp, Warning, TEXT("[RoundEnd] TeamA wiped before plant (aliveA=%d aliveB=%d) -> TeamB wins"), AliveA, AliveB);
		EndRound(ETeam::ET_TeamB);
		return;
	}
	if (AliveB <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[RoundEnd] TeamB wiped (aliveA=%d aliveB=%d) -> TeamA wins"), AliveA, AliveB);
		EndRound(ETeam::ET_TeamA);
		return;
	}
}

void ABlasterGameMode::SyncPhaseTimeToClients()
{
	for (FConstControllerIterator Iterator = GetWorld()->GetControllerIterator(); Iterator; ++Iterator)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(*Iterator);
		if (PC)
		{
			PC->ClientSyncPhaseTime(LevelStartingTime);
		}
	}
}

void ABlasterGameMode::CacheInitialWeaponSpawns()
{
	TArray<AActor*> Weapons;
	UGameplayStatics::GetAllActorsOfClass(this, AWeapon::StaticClass(), Weapons);
	for (AActor* Actor : Weapons)
	{
		AWeapon* Weapon = Cast<AWeapon>(Actor);
		if (!Weapon) continue;

		FInitialWeaponSpawn Spawn;
		Spawn.WeaponClass = Weapon->GetClass();
		Spawn.SpawnTransform = Weapon->GetActorTransform();
		InitialWeaponSpawns.Add(Spawn);
	}
}

void ABlasterGameMode::RespawnMapWeapons()
{
	// 只清理无人持有的掉落武器（上回合掉地上的）；
	// 玩家身上继承的武器有 Owner，必须保留
	TArray<AActor*> Weapons;
	UGameplayStatics::GetAllActorsOfClass(this, AWeapon::StaticClass(), Weapons);
	for (AActor* Actor : Weapons)
	{
		AWeapon* W = Cast<AWeapon>(Actor);
		if (W && W->GetOwner() == nullptr)
		{
			Actor->Destroy();
		}
	}

	// Respawn from cached initial spawns
	for (const FInitialWeaponSpawn& Spawn : InitialWeaponSpawns)
	{
		GetWorld()->SpawnActor<AWeapon>(Spawn.WeaponClass, Spawn.SpawnTransform);
	}
}

void ABlasterGameMode::ResetAllUltOrbs()
{
	// 大招球不像掉落的武器那样要"销毁+重建"（球的位置本来就是关卡设计数据，整局不变），
	// 只要把可用性打回 true 就行 —— 这个值是复制的，客户端会自己把球画回来。
	// 所以这里不需要缓存初始 transform，每回合遍历一次关卡里的球即可（每回合只有一次，开销可忽略）。
	for (TActorIterator<AUltOrb> It(GetWorld()); It; ++It)
	{
		It->SetAvailable(true);
	}
}

ABlasterGameState* ABlasterGameMode::GetBlasterGameState() const
{
	return GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
}

