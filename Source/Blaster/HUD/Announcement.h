#pragma once

#include "CoreMinimal.h"
#include "Runtime/UMG/Public/Blueprint/UserWidget.h"
#include "Announcement.generated.h"

UCLASS()
class BLASTER_API UAnnouncement : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(meta=(BindWidget))
	class UTextBlock* WarmupTime;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* AnnouncementText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* InfoText;

	// --- Round UI ---
	UPROPERTY(meta=(BindWidget))
	UTextBlock* RoundResultText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* TeamSwapText;

	// --- 安包/拆包进度 ---
	// Status 是 Spike.cpp 推来的 "Planting" / "Defusing"（也可能是倒计时串），Progress 是 0-1 的比例；
	// Progress < 0 表示"没人在安/拆，收起来"。两个参数和 ABlasterPlayerController::SetHUDSpikeStatus 一致。
	//
	// ★ 基类**什么都不做**：老那条进度条在 UCharacterOverlay 里（SpikeStatusText / SpikeTimerBar），
	//   由 SetHUDSpikeStatus 自己写。UValorantAnnouncement 覆写这个，把进度画到公告位置上
	//   （bValorantHUDOnly 打开时 CharacterOverlay 根本不创建，老路径是空转的）。
	virtual void SetSpikeStatus(const FString& Status, float Progress) {}

	// 每回合正式开打那一下（MatchState -> InProgress）被 PlayerController 调一次。
	//
	// ★ 基类把整块藏起来 —— 那是给旧 WBP_Announcement 定的行为，别改。
	//   UValorantAnnouncement 必须覆写成"只清字、不藏块"：安包/拆包进度条和爆炸倒计时
	//   都画在这个 widget **里面**，而进 InProgress 是**每回合**都会来的一道 ——
	//   整块一藏，常规阶段就再也看不到那条进度条了。
	//   （面板自己显示不显示由 NormalizeAndLayout 按内容收放，把字清干净它下一帧自己就 Collapsed。）
	virtual void OnRoundStarted() { SetVisibility(ESlateVisibility::Hidden); }
};
