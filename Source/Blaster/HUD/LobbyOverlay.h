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

	// --- 中间的角色展示位（本地 spawn，不复制）---
	// 留空 = 用 C++ 的 ALobbyAgentShowcase；想换造型/相机就派生一个 BP 再填这里。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby")
	TSubclassOf<class ALobbyAgentShowcase> ShowcaseClass;

	UPROPERTY(Transient)
	TObjectPtr<class ALobbyAgentShowcase> Showcase;

	static constexpr int32 SlotsPerTeam = 5;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	TArray<TWeakObjectPtr<class ULobbyPlayerSlot>> TeamASlots;
	TArray<TWeakObjectPtr<class ULobbyPlayerSlot>> TeamBSlots;

	float RefreshTimer = 0.f;
	static constexpr float RefreshInterval = 0.3f;

	// 队伍面板挪到左右两侧后的尺寸/边距（设计分辨率 1920x1080 下的像素）
	static constexpr float TeamPanelWidth = 260.f;
	static constexpr float TeamPanelHeight = 520.f;
	static constexpr float TeamPanelMargin = 32.f;

	void CreateSlots();
	void RefreshSlots();
	void UpdateButtonStates();
	void CreateAgentSelectStrip();

	// 把 TeamAPanel / TeamBPanel 从屏幕中间挪到左右两侧 —— 中间腾给角色展示位。
	// 改的是面板自己的 CanvasPanelSlot（锚点+固定尺寸），不动 WBP 资产。
	void LayoutTeamPanels();

	void EnsureShowcase();
	void DestroyShowcase();

	// 大厅的本机视角/输入：光标可见 + 藏掉本地 Pawn（它 spawn 在 PlayerStart，
	// 正好落在展示位和相机中间会挡住角色）。每帧调，内部各自 early-out。
	void ApplyLobbyViewSetup();
	// 离开大厅时还原成"游戏输入 + 无光标"，否则进了对局鼠标还悬着、点击先被 UI 吃掉
	void RestoreGameViewSetup();

	// 已经藏过的那个 Pawn（换 Pawn 时重新藏一次；也用来在还原时把它放出来）
	TWeakObjectPtr<class APawn> HiddenPawn;

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
