#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Team.h"
#include "LobbyPlayerSlot.generated.h"

DECLARE_DELEGATE_TwoParams(FOnLobbySlotClicked, int32 /*SlotIndex*/, ETeam /*TargetTeam*/);

UCLASS()
class BLASTER_API ULobbyPlayerSlot : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	class UButton* SlotButton;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* PlayerNameText;

	UPROPERTY(meta = (BindWidget))
	class UImage* ReadyCheckImage;

	UPROPERTY(meta = (BindWidget))
	class UBorder* SlotBorder;

	int32 SlotIndex = -1;
	ETeam SlotTeam = ETeam::ET_None;

	FOnLobbySlotClicked OnClickedDelegate;

	// 占位：显示玩家名 + 所选英雄（AgentName 为空/“Random” 时显示为随机占位）
	void SetOccupied(const FString& PlayerName, bool bReady, const FString& AgentName = FString());
	void SetEmpty();
	void SetHighlighted(bool bHighlighted);

protected:
	virtual void NativeConstruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	UFUNCTION()
	void HandleSlotClicked();

	void FireClick();
};
