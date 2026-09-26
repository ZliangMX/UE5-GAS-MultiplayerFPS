#include "LobbyOverlay.h"
#include "LobbyPlayerSlot.h"
#include "AgentSelectStrip.h"
#include "Blaster/Lobby/LobbyAgentShowcase.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/GameMode/LobbyGameMode.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"   // FInputModeGameAndUI / FInputModeGameOnly
#include "Kismet/GameplayStatics.h"

void ULobbyOverlay::NativeConstruct()
{
	Super::NativeConstruct();

	if (ReadyButton) ReadyButton->OnClicked.AddDynamic(this, &ULobbyOverlay::OnReadyButtonClicked);
	if (StartGameButton) StartGameButton->OnClicked.AddDynamic(this, &ULobbyOverlay::OnStartGameButtonClicked);

	CreateSlots();
	// 面板挪位必须在 CreateSlots 之后（槽是那时候塞进去的）
	LayoutTeamPanels();
	CreateAgentSelectStrip();
	EnsureShowcase();
	ApplyLobbyViewSetup();
	RefreshSlots();
	UpdateButtonStates();
}

void ULobbyOverlay::NativeDestruct()
{
	RestoreGameViewSetup();
	DestroyShowcase();
	Super::NativeDestruct();
}

void ULobbyOverlay::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 每帧走一遍（内部各自 early-out）——本地 Pawn 可能比 overlay 晚 spawn，
	// 藏到了就不再重复设。
	ApplyLobbyViewSetup();

	RefreshTimer += InDeltaTime;
	if (RefreshTimer >= RefreshInterval)
	{
		RefreshTimer = 0.f;
		RefreshSlots();
	}
}

void ULobbyOverlay::CreateSlots()
{
	if (!PlayerSlotClass) return;

	TeamASlots.Empty(5);
	TeamBSlots.Empty(5);

	for (int32 i = 0; i < SlotsPerTeam; i++)
	{
		ULobbyPlayerSlot* SlotA = CreateWidget<ULobbyPlayerSlot>(this, PlayerSlotClass);
		if (SlotA)
		{
			SlotA->SlotIndex = i;
			SlotA->SlotTeam = ETeam::ET_TeamA;
			SlotA->OnClickedDelegate.BindUObject(this, &ULobbyOverlay::OnSlotClicked);
			TeamASlots.Add(SlotA);
			if (TeamAPanel) TeamAPanel->AddChildToVerticalBox(SlotA);
		}

		ULobbyPlayerSlot* SlotB = CreateWidget<ULobbyPlayerSlot>(this, PlayerSlotClass);
		if (SlotB)
		{
			SlotB->SlotIndex = i;
			SlotB->SlotTeam = ETeam::ET_TeamB;
			SlotB->OnClickedDelegate.BindUObject(this, &ULobbyOverlay::OnSlotClicked);
			TeamBSlots.Add(SlotB);
			if (TeamBPanel) TeamBPanel->AddChildToVerticalBox(SlotB);
		}
	}
}

void ULobbyOverlay::LayoutTeamPanels()
{
	// 两个队伍面板原本是由 WBP 摆在屏幕中部的，正好挡住角色展示位。这里把它们改到
	// 左右两侧竖排，中间整块腾出来。改的是面板自身的 CanvasPanelSlot（锚点 + 固定尺寸），
	// **不动 WBP 资产** —— 所以随时可以在编辑器里再手调，C++ 这步只负责给个合理默认。
	auto PlacePanel = [](UWidget* Panel, bool bRightSide, const TCHAR* Label)
	{
		if (!Panel) return;

		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Panel->Slot);
		if (!CanvasSlot)
		{
			// 面板的父容器不是 CanvasPanel（比如 WBP 根是 VerticalBox）→ 没有锚点可改。
			// 这种情况自动摆位做不了，只能去 WBP 里手动拖，这里只报一声。
			UE_LOG(LogTemp, Warning,
				TEXT("LobbyOverlay: %s 的 slot 是 %s，不是 CanvasPanelSlot —— 队伍面板没法自动挪到侧边，请在 WBP_LobbyOverlay 里手动摆位。"),
				Label, Panel->Slot ? *Panel->Slot->GetClass()->GetName() : TEXT("null"));
			return;
		}

		CanvasSlot->SetAnchors(bRightSide ? FAnchors(1.f, 0.5f) : FAnchors(0.f, 0.5f));
		CanvasSlot->SetAlignment(bRightSide ? FVector2D(1.f, 0.5f) : FVector2D(0.f, 0.5f));
		CanvasSlot->SetAutoSize(false);
		CanvasSlot->SetSize(FVector2D(TeamPanelWidth, TeamPanelHeight));
		// 锚点在左侧时 X 向右为正，锚点在右侧时 X 向右仍为正 → 右侧要用负值才往屏内收
		CanvasSlot->SetPosition(FVector2D(bRightSide ? -TeamPanelMargin : TeamPanelMargin, 0.f));
	};

	PlacePanel(TeamAPanel, /*bRightSide=*/false, TEXT("TeamAPanel"));
	PlacePanel(TeamBPanel, /*bRightSide=*/true, TEXT("TeamBPanel"));
}

void ULobbyOverlay::EnsureShowcase()
{
	if (Showcase) return;

	ABlasterPlayerController* PC = GetBlasterPlayerController();
	// 只有本机玩家才需要看自己选的英雄 —— 服务器上的其它 PC 不要跟着 spawn
	if (!PC || !PC->IsLocalController()) return;

	UWorld* World = GetWorld();
	if (!World) return;

	// 注意别写成三目直取：TSubclassOf<T> 和 UClass* 类型不同，混在三目里编不过
	UClass* Class = ShowcaseClass ? ShowcaseClass.Get() : ALobbyAgentShowcase::StaticClass();

	FActorSpawnParameters Params;
	Params.Owner = PC;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;   // 纯运行时摆件，别被存进任何关卡

	// 原点就是地图地板（Lobby.umap 的 Floor 在 z=0），角色正好站在大厅中央
	Showcase = World->SpawnActor<ALobbyAgentShowcase>(Class, FTransform::Identity, Params);
	if (!Showcase) return;

	// 固定机位：把视角锁到展示位上。玩家照样能操作自己的 Pawn，但屏幕上只有选人画面。
	PC->SetViewTarget(Showcase);

	if (ABlasterPlayerState* LocalPS = GetLocalPlayerState())
	{
		Showcase->SetAgent(LocalPS->GetAgent());
	}
}

void ULobbyOverlay::DestroyShowcase()
{
	if (!Showcase) return;

	// 先把视角还给人 —— 否则 ViewTarget 会短暂指向一个正在被销毁的 actor。
	// 正常离开 Lobby 是 ServerTravel 换地图，引擎那边本来也会重设，这里只是兜底。
	if (ABlasterPlayerController* PC = GetBlasterPlayerController())
	{
		if (PC->GetViewTarget() == Showcase)
		{
			PC->SetViewTarget(PC->GetPawn());
		}
	}

	Showcase->Destroy();
	Showcase = nullptr;
}

void ULobbyOverlay::ApplyLobbyViewSetup()
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	if (!PC || !PC->IsLocalController()) return;

	// 1) 光标可见 + GameAndUI —— 大厅里要点英雄卡、队伍槽、Ready/Start 按钮。
	//    用 GameAndUI 而不是 UIOnly：和 BuyMenu / 封烟选点那边同一套，键盘还留给游戏。
	//    （幂等：只在第一次和每次状态被别处改回去时重设）
	if (!PC->bShowMouseCursor)
	{
		PC->bShowMouseCursor = true;
		PC->SetInputMode(FInputModeGameAndUI());
	}

	// 2) 藏掉本地 Pawn。
	//
	//    正常情况这里已经没事可做：ALobbyGameMode::HandleStartingNewPlayer 现在大厅里
	//    根本不生成玩家 Pawn（那才是"有客户端加入就多出一个默认角色"的根因所在）。
	//    留着这段是防御 —— 万一以后大厅又有了 Pawn（比如想在选人时走动），
	//    它至少不会挡在展示位和相机中间（它 spawn 在 PlayerStart(0,0,92)，正落在这条线上）。
	//    记下藏的是哪一个：换了 Pawn 就再藏一次。
	if (APawn* MyPawn = PC->GetPawn())
	{
		if (HiddenPawn.Get() != MyPawn)
		{
			MyPawn->SetActorHiddenInGame(true);
			HiddenPawn = MyPawn;
		}
	}

	// 3) 视角锁在展示位上 —— 而且**每帧重申**，不能只在 EnsureShowcase 里设一次。
	//
	//    客户端 join 时自己的 Pawn 比本 overlay 晚到，引擎在 APawn::PawnClientRestart
	//    （由 AController::ClientRestart 这个 Client RPC 触发，**只有客户端会走**)里会无条件
	//    AutoManageActiveCameraTarget(Pawn)，把 ViewTarget 从展示位抢回自己的 Pawn。
	//    于是视角变成第三人称弹簧臂的"角色背后"，而本地 Pawn 已被上面第 2 步藏掉，
	//    画面上就只剩远处展示角色的后脑勺 —— 这正是"只有客户端、相机在角色背后"的来源。
	//    （Host 的 Pawn 在 overlay 创建之前就有了，PawnClientRestart 早就跑完，抢不走。）
	//
	//    重申是幂等的：ViewTarget 已经是展示位时这里什么都不做，也不会打断相机。
	//    同理也别去改 PC->bAutoManageActiveCameraTarget —— 那是对局里死亡重生自动切视角要用的。
	if (Showcase && PC->GetViewTarget() != Showcase.Get())
	{
		PC->SetViewTarget(Showcase);
	}
}

void ULobbyOverlay::RestoreGameViewSetup()
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	if (!PC || !PC->IsLocalController()) return;

	// 把光标收回去，否则进了对局鼠标还悬着、点击会先被 UI 吃掉。
	// （对局里的 BuyMenu / 封烟地图需要光标时会自己再打开，见 BlasterPlayerController。）
	PC->bShowMouseCursor = false;
	PC->SetInputMode(FInputModeGameOnly());

	// Pawn 的可见性也还原 —— 切地图后这个 Pawn 本来就没了，这行是为了
	// "留在大厅地图里重开 overlay" 这种少见情况不留下一个隐形的自己。
	if (APawn* MyPawn = HiddenPawn.Get())
	{
		MyPawn->SetActorHiddenInGame(false);
	}
	HiddenPawn = nullptr;
}

void ULobbyOverlay::CreateAgentSelectStrip()
{
	if (AgentStrip) return;

	AgentStrip = CreateWidget<UAgentSelectStrip>(this, UAgentSelectStrip::StaticClass());
	if (!AgentStrip) return;

	UWidget* Root = GetRootWidget();

	// 首选：根是 CanvasPanel → 锚定底部中央（Autosize 不撑破布局）
	if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Root))
	{
		if (UCanvasPanelSlot* CanvasSlot = Canvas->AddChildToCanvas(AgentStrip))
		{
			CanvasSlot->SetAnchors(FAnchors(0.5f, 1.f, 0.5f, 1.f));
			CanvasSlot->SetAlignment(FVector2D(0.5f, 1.f));
			CanvasSlot->SetAutoSize(true);
			CanvasSlot->SetZOrder(10);
		}
		return;
	}

	// 兜底：根是 VerticalBox 或其它面板 → 尾部追加、水平居中
	if (UVerticalBox* VBox = Cast<UVerticalBox>(Root))
	{
		if (UVerticalBoxSlot* VSlot = VBox->AddChildToVerticalBox(AgentStrip))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
		}
		return;
	}

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
	{
		Panel->AddChild(AgentStrip);
	}
}

void ULobbyOverlay::RefreshSlots()
{
	UWorld* World = GetWorld();
	if (!World) return;

	AGameStateBase* GS = World->GetGameState();
	if (!GS) return;

	// Reset valid slots to empty
	for (auto& WeakSlot : TeamASlots)
	{
		if (WeakSlot.IsValid()) WeakSlot->SetEmpty();
	}
	for (auto& WeakSlot : TeamBSlots)
	{
		if (WeakSlot.IsValid()) WeakSlot->SetEmpty();
	}

	int32 IndexA = 0, IndexB = 0;
	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS) continue;

		if (BPS->Team == ETeam::ET_TeamA && IndexA < TeamASlots.Num())
		{
			if (TeamASlots[IndexA].IsValid())
			{
				TeamASlots[IndexA]->SetOccupied(BPS->GetPlayerName(), BPS->bIsReady,
					BlasterAgent::ToDisplayName(BPS->GetAgent()));
			}
			IndexA++;
		}
		else if (BPS->Team == ETeam::ET_TeamB && IndexB < TeamBSlots.Num())
		{
			if (TeamBSlots[IndexB].IsValid())
			{
				TeamBSlots[IndexB]->SetOccupied(BPS->GetPlayerName(), BPS->bIsReady,
					BlasterAgent::ToDisplayName(BPS->GetAgent()));
			}
			IndexB++;
		}
	}

	// 本地玩家当前英雄 → 底部条高亮（None → Random 卡）+ 中间展示位换造型
	if (ABlasterPlayerState* LocalPS = GetLocalPlayerState())
	{
		if (AgentStrip)
		{
			AgentStrip->SetCurrentAgent(LocalPS->GetAgent());
		}
		if (Showcase)
		{
			Showcase->SetAgent(LocalPS->GetAgent());
		}
	}

	UpdateButtonStates();
}

void ULobbyOverlay::UpdateButtonStates()
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	ALobbyGameMode* GM = GetLobbyGameMode();
	bool bIsServer = PC && PC->IsLocalController() && PC->HasAuthority();
	bool bAllReady = GM && GM->IsAllClientsReady();

	if (StartGameButton) StartGameButton->SetIsEnabled(bIsServer && bAllReady);
	if (ReadyButton) ReadyButton->SetIsEnabled(!bIsServer);

	ABlasterPlayerState* LocalPS = GetLocalPlayerState();
	if (ReadyButtonText && LocalPS)
	{
		ReadyButtonText->SetText(FText::FromString(LocalPS->bIsReady ? TEXT("Cancel Ready") : TEXT("Ready")));
	}

	if (LobbyInfoText)
	{
		LobbyInfoText->SetText(FText::FromString(
			bAllReady ? TEXT("All players ready -- host can start the game") : TEXT("Waiting for players to ready up...")));
	}
}

void ULobbyOverlay::OnSlotClicked(int32 SlotIndex, ETeam TargetTeam)
{
	ABlasterPlayerState* LocalPS = GetLocalPlayerState();
	if (!LocalPS || LocalPS->Team == TargetTeam) return;

	RequestSwitchTeam(TargetTeam);
}

void ULobbyOverlay::RequestSwitchTeam(ETeam TargetTeam)
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	if (PC && !PC->HasAuthority())
	{
		PC->ServerSwitchTeam(TargetTeam);
	}
	else
	{
		ALobbyGameMode* GM = GetLobbyGameMode();
		if (GM && PC)
		{
			GM->SwitchPlayerTeam(PC, TargetTeam);
		}
	}
}

void ULobbyOverlay::OnReadyButtonClicked()
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	if (PC)
	{
		if (PC->HasAuthority())
		{
			ALobbyGameMode* GM = GetLobbyGameMode();
			if (GM) GM->TogglePlayerReady(PC);
		}
		else
		{
			PC->ServerToggleReady();
		}
	}
	RefreshSlots();
}

void ULobbyOverlay::OnStartGameButtonClicked()
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	if (PC && PC->HasAuthority())
	{
		ALobbyGameMode* GM = GetLobbyGameMode();
		if (GM) GM->StartGame();
	}
	else if (PC)
	{
		PC->ServerStartGame();
	}
}

ABlasterPlayerController* ULobbyOverlay::GetBlasterPlayerController() const
{
	return Cast<ABlasterPlayerController>(GetOwningPlayer());
}

ALobbyGameMode* ULobbyOverlay::GetLobbyGameMode() const
{
	return Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this));
}

ABlasterPlayerState* ULobbyOverlay::GetLocalPlayerState() const
{
	ABlasterPlayerController* PC = GetBlasterPlayerController();
	return PC ? Cast<ABlasterPlayerState>(PC->PlayerState) : nullptr;
}
