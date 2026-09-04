#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Team.h"
#include "LobbyOverlay.generated.h"

UCLASS()
class BLASTER_API ULobbyOverlay : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	class UVerticalBox* TeamAPanel;

	UPROPERTY(meta = (BindWidget))
	class UVerticalBox* TeamBPanel;

	UPROPERTY(meta = (BindWidget))
	class UButton* ReadyButton;

	UPROPERTY(meta = (BindWidget))
	class UButton* StartGameButton;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* ReadyButtonText;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* StartGameButtonText;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* TeamATitleText;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* TeamBTitleText;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* LobbyInfoText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby")
	TSubclassOf<class ULobbyPlayerSlot> PlayerSlotClass;

	// 底部共用英雄条（创建时作为 child 挂进根 Canvas/VerticalBox，随 overlay 一起销毁）
	UPROPERTY(Transient)
	TObjectPtr<class UAgentSelectStrip> AgentStrip;

	static constexpr int32 SlotsPerTeam = 5;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	TArray<TWeakObjectPtr<class ULobbyPlayerSlot>> TeamASlots;
	TArray<TWeakObjectPtr<class ULobbyPlayerSlot>> TeamBSlots;

	float RefreshTimer = 0.f;
	static constexpr float RefreshInterval = 0.3f;

	void CreateSlots();
	void RefreshSlots();
	void UpdateButtonStates();
	void CreateAgentSelectStrip();

	void OnSlotClicked(int32 SlotIndex, ETeam TargetTeam);
	void RequestSwitchTeam(ETeam TargetTeam);

	UFUNCTION()
	void OnReadyButtonClicked();

	UFUNCTION()
	void OnStartGameButtonClicked();

	class ABlasterPlayerController* GetBlasterPlayerController() const;
	class ALobbyGameMode* GetLobbyGameMode() const;
	class ABlasterPlayerState* GetLocalPlayerState() const;
};
