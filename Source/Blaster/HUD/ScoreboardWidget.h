#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Team.h"
#include "ScoreboardRow.h"
#include "ScoreboardWidget.generated.h"

class UBorder;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
class ABlasterPlayerState;
class ABlasterGameState;

/**
 * Tab 记分板：敌我两队玩家列表（携包/阵亡标记 + 名字 + 英雄 + K + D + 经济），
 * 作为 UMG Widget 叠加在 HUD 之上（AddToViewport）。
 *
 * 刷新策略：按住显示时用定时器（默认 0.25s）刷一次数据，松手隐藏并停掉定时器；
 * 行控件只在人数不够时新建，之后只 SetText/SetColor —— 没有任何 NativePaint /
 * DrawHUD 的每帧绘制。
 *
 * 有 WBP 时按名字绑定 TeamAList/TeamBList 等；没有 WBP 时走 C++ 自绘默认布局。
 */
UCLASS()
class BLASTER_API UScoreboardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// 有 WBP 时绑这些；没绑上就走 C++ 默认布局
	UPROPERTY(meta = (BindWidgetOptional))
	UBorder* RootBorder;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* TitleText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* TeamAHeader;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* TeamBHeader;

	UPROPERTY(meta = (BindWidgetOptional))
	UVerticalBox* TeamAList;

	UPROPERTY(meta = (BindWidgetOptional))
	UVerticalBox* TeamBList;

	// 行控件类：留空用原生 UScoreboardRow（自绘默认行）
	UPROPERTY(EditAnywhere, Category = "Scoreboard")
	TSubclassOf<UScoreboardRow> RowClass;

	// 显示期间的数据刷新周期（秒）。记分板是「显示时定时刷」，不是每帧刷。
	UPROPERTY(EditAnywhere, Category = "Scoreboard")
	float RefreshInterval = 0.25f;

	// 玩家名（角色 id）显示上限：超长会糊到英雄/KD 列上，这里截断加省略号。
	// <=0 表示不截断（此时靠行内的裁剪/省略号兜底）。
	UPROPERTY(EditAnywhere, Category = "Scoreboard")
	int32 PlayerNameMaxChars = 12;

	// 由 ABlasterPlayerController 的 Tab 输入调用：按住显示、松开隐藏
	void ShowScoreboard();
	void HideScoreboard();
	bool IsScoreboardShown() const { return bShown; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;

private:
	void BuildDefaultLayout();
	void BuildTeamColumn(UHorizontalBox* TeamsBox, const FString& HeaderText, const FMargin& SlotPadding,
		UTextBlock*& OutHeader, UVerticalBox*& OutList);

	void RefreshScoreboard();
	void BuildEntries(const ABlasterGameState* GS, ETeam Team, const ABlasterPlayerState* LocalPS,
		TArray<FScoreboardEntry>& Out) const;
	void ApplyEntries(TArray<TObjectPtr<UScoreboardRow>>& Pool, UVerticalBox* Parent,
		const TArray<FScoreboardEntry>& Entries);
	UScoreboardRow* EnsureRow(TArray<TObjectPtr<UScoreboardRow>>& Pool, UVerticalBox* Parent, int32 Index);

	// 行控件池：按队各留一份，人数变化时只增不减（多出来的行折叠）
	UPROPERTY()
	TArray<TObjectPtr<UScoreboardRow>> TeamARows;

	UPROPERTY()
	TArray<TObjectPtr<UScoreboardRow>> TeamBRows;

	FTimerHandle RefreshTimerHandle;
	bool bShown = false;
};
