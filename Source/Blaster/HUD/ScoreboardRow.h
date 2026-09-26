#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ScoreboardRow.generated.h"

class UBorder;
class UHorizontalBox;
class UTextBlock;
class UWidgetTree;

// 记分板一行要显示的数据（UScoreboardWidget 从 PlayerState / GameState 收集后填进来）
USTRUCT(BlueprintType)
struct FScoreboardEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	FString PlayerName;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	FString AgentName;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	int32 Kills = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	int32 Deaths = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	int32 Credits = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	bool bIsLocalPlayer = false;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	bool bIsAlive = true;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	bool bHasSpike = false;

	UPROPERTY(BlueprintReadOnly, Category = "Scoreboard")
	FLinearColor AccentColor = FLinearColor::White;
};

// C++ 自绘回退布局的列宽。表头与数据行共用同一组宽度，保证左右对齐。
// 若以后做了 WBP 记分板并自己想排版，这些值就只是默认值、不影响 WBP。
namespace BlasterScoreboardUI
{
	constexpr float MarkerWidth = 48.f;   // 携包 / 阵亡
	constexpr float NameWidth = 180.f;
	constexpr float AgentWidth = 96.f;
	constexpr float KillsWidth = 48.f;
	constexpr float DeathsWidth = 48.f;
	constexpr float CreditsWidth = 76.f;

	constexpr float ColumnsWidth = MarkerWidth + NameWidth + AgentWidth + KillsWidth + DeathsWidth + CreditsWidth;
	constexpr float CellPadding = 4.f;    // MakeCell 每格左右各 2
	constexpr float RowPadding = 6.f;     // 行 Border 左右内边距
	constexpr float RowTotalWidth = ColumnsWidth + CellPadding * 6 + RowPadding * 2;

	// 造一个定宽文本格挂到 HBox 上（内部套 SizeBox 固定宽度），返回 TextBlock 供后续 SetText
	BLASTER_API UTextBlock* MakeCell(UWidgetTree* Tree, UHorizontalBox* Parent, const FString& Text, float Width, int32 FontSize);

	// 造一个不定宽文本块（标题 / 队头用）
	BLASTER_API UTextBlock* MakeLabel(UWidgetTree* Tree, class UPanelWidget* Parent, const FString& Text, int32 FontSize);
}

/**
 * 记分板的一行：携包/阵亡标记 + 玩家名 + 英雄 + K + D + 经济。
 * 有 WBP 时按名字 BindWidgetOptional 绑定；没有 WBP 时走 C++ 自绘回退布局（见 RebuildWidget）。
 * 只改文字和颜色，不重建控件 —— 记分板显示期间由定时器低频刷数据，不做每帧绘制。
 */
UCLASS()
class BLASTER_API UScoreboardRow : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional))
	UBorder* RowBorder;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* MarkerText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* PlayerNameText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* AgentText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* KillsText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* DeathsText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* CreditsText;

	void SetRowData(const FScoreboardEntry& Entry);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;

private:
	void BuildDefaultLayout();
};
