#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CharacterOverlay.generated.h"

UCLASS()
class BLASTER_API UCharacterOverlay : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta=(BindWidget))
	class UProgressBar* HealthBar;

	UPROPERTY(meta=(BindWidget))
	class UTextBlock* HealthText;

	// --- 护甲（可选）---
	// 故意用 BindWidgetOptional：WBP_CharacterOverlay 里**没加**这两个控件也能编译/运行，
	// 只是护甲退回「并进 HealthText 显示 (+50)」。加进 WBP 后自动切到专属显示，不用改代码。
	// 甲为 0 时两个控件都会被 Collapsed（屏上不留一个空条）。
	UPROPERTY(meta=(BindWidgetOptional))
	class UProgressBar* ArmorBar;

	UPROPERTY(meta=(BindWidgetOptional))
	class UTextBlock* ArmorText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* ScoreAmount;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* DefeatsAmount;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* WeaponAmmoAmount;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* CarriedAmmoAmount;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* MatchCountDownText;

	// --- Team UI ---
	UPROPERTY(meta=(BindWidget))
	UTextBlock* TeamText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* TeamScoreText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* AliveCountText;

	UPROPERTY(meta=(BindWidget))
	UTextBlock* SpikeStatusText;

	UPROPERTY(meta=(BindWidget))
	class UProgressBar* SpikeTimerBar;
};
