#include "LobbyOverlay.h"
#include "LobbyPlayerSlot.h"
#include "AgentSelectStrip.h"
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
#include "Kismet/GameplayStatics.h"

void ULobbyOverlay::NativeConstruct()
{
	Super::NativeConstruct();

	if (ReadyButton) ReadyButton->OnClicked.AddDynamic(this, &ULobbyOverlay::OnReadyButtonClicked);
	if (StartGameButton) StartGameButton->OnClicked.AddDynamic(this, &ULobbyOverlay::OnStartGameButtonClicked);

	CreateSlots();
	CreateAgentSelectStrip();
	RefreshSlots();
	UpdateButtonStates();
}

void ULobbyOverlay::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

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

	// 把本地玩家当前英雄推进底部条（None → 高亮 Random 卡）
	if (AgentStrip)
	{
		ABlasterPlayerState* LocalPS = GetLocalPlayerState();
		if (LocalPS)
		{
			AgentStrip->SetCurrentAgent(LocalPS->GetAgent());
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
