#include "LobbyPlayerSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Input/Reply.h"

void ULobbyPlayerSlot::NativeConstruct()
{
	Super::NativeConstruct();
	if (SlotButton) SlotButton->OnClicked.AddDynamic(this, &ULobbyPlayerSlot::HandleSlotClicked);
	SetEmpty();
}

void ULobbyPlayerSlot::HandleSlotClicked()
{
	FireClick();
}

FReply ULobbyPlayerSlot::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		FireClick();
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void ULobbyPlayerSlot::FireClick()
{
	OnClickedDelegate.ExecuteIfBound(SlotIndex, SlotTeam);
}

void ULobbyPlayerSlot::SetOccupied(const FString& PlayerName, bool bReady, const FString& AgentName)
{
	// 玩家名后追加所选英雄；未选(空/Random) 用 Random 占位，让两侧一眼看出谁还没定
	FString Display = PlayerName;
	if (!AgentName.IsEmpty())
	{
		Display = FString::Printf(TEXT("%s   ·   %s"), *PlayerName, *AgentName);
	}

	if (PlayerNameText) PlayerNameText->SetText(FText::FromString(Display));
	if (PlayerNameText) PlayerNameText->SetVisibility(ESlateVisibility::Visible);
	if (SlotBorder) SlotBorder->SetBrushColor(SlotTeam == ETeam::ET_TeamA
		? FLinearColor(0.6f, 0.1f, 0.1f, 1.f)
		: FLinearColor(0.1f, 0.3f, 0.6f, 1.f));
	if (ReadyCheckImage)
	{
		ReadyCheckImage->SetVisibility(ESlateVisibility::Visible);
		ReadyCheckImage->SetColorAndOpacity(bReady
			? FLinearColor(0.2f, 1.f, 0.2f, 1.f)
			: FLinearColor(0.5f, 0.5f, 0.5f, 1.f));
	}
}

void ULobbyPlayerSlot::SetEmpty()
{
	if (PlayerNameText)
	{
		PlayerNameText->SetText(FText::FromString(TEXT("")));
		PlayerNameText->SetVisibility(ESlateVisibility::Hidden);
	}
	if (SlotBorder) SlotBorder->SetBrushColor(FLinearColor(0.15f, 0.15f, 0.15f, 0.5f));
	if (ReadyCheckImage) ReadyCheckImage->SetVisibility(ESlateVisibility::Hidden);
}

void ULobbyPlayerSlot::SetHighlighted(bool bHighlighted)
{
	if (SlotBorder)
	{
		SlotBorder->SetBrushColor(bHighlighted
			? FLinearColor(0.3f, 0.3f, 0.3f, 0.8f)
			: FLinearColor(0.15f, 0.15f, 0.15f, 0.5f));
	}
}
