#include "ScoreboardWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

namespace
{
	// 我方 / 敌方 / 未知 的强调色（只用于队头文字）
	const FLinearColor MyTeamColor(0.38f, 0.78f, 1.f, 1.f);
	const FLinearColor EnemyTeamColor(1.f, 0.45f, 0.45f, 1.f);
	const FLinearColor NeutralColor(0.9f, 0.92f, 0.95f, 1.f);

	FSlateColor TeamRelationColor(ETeam Team, ETeam MyTeam)
	{
		if (MyTeam == ETeam::ET_None) return FSlateColor(NeutralColor);
		return FSlateColor(Team == MyTeam ? MyTeamColor : EnemyTeamColor);
	}

	// 玩家名（角色 id）太长会糊到英雄/K/D 列上，这里按字符数截断加省略号
	FString TruncatePlayerName(const FString& Name, int32 MaxChars)
	{
		if (MaxChars <= 0 || Name.Len() <= MaxChars) return Name;
		return Name.Left(MaxChars) + TEXT("…");
	}
}

void UScoreboardWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// 纯展示叠加层：自己和子件都不吃点击，不打断玩家操作（也不需要切输入模式）
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

TSharedRef<SWidget> UScoreboardWidget::RebuildWidget()
{
	// 只有「没有 WBP 提供布局」时才自绘：WBP 的 WidgetTree 已经有 RootWidget
	if (WidgetTree && !WidgetTree->RootWidget && !TeamAList)
	{
		BuildDefaultLayout();
	}
	return Super::RebuildWidget();
}

void UScoreboardWidget::BuildDefaultLayout()
{
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ScoreboardRoot"));
	if (!Root) return;
	WidgetTree->RootWidget = Root;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ScoreboardPanel"));
	if (!Panel) return;
	RootBorder = Panel;
	Panel->SetPadding(FMargin(18.f, 14.f));
	Panel->SetBrushColor(FLinearColor(0.02f, 0.03f, 0.06f, 0.88f));

	// 居中、按内容自适应大小（AutoSize）——不会被分辨率裁掉
	if (UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(Panel))
	{
		PanelSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		PanelSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		PanelSlot->SetAutoSize(true);
		PanelSlot->SetPosition(FVector2D(0.f, 0.f));
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ScoreboardColumn"));
	if (!Column) return;
	Panel->SetContent(Column);

	// 标题：比分 + 回合
	TitleText = BlasterScoreboardUI::MakeLabel(WidgetTree, Column, FString(), 20);
	if (TitleText)
	{
		TitleText->SetJustification(ETextJustify::Center);
		if (UVerticalBoxSlot* TitleSlot = Cast<UVerticalBoxSlot>(TitleText->Slot))
		{
			TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		}
	}

	// 两队列并排
	UHorizontalBox* Teams = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("TeamsBox"));
	if (!Teams) return;
	Column->AddChildToVerticalBox(Teams);

	BuildTeamColumn(Teams, FString(), FMargin(0.f, 0.f, 16.f, 0.f), TeamAHeader, TeamAList);
	BuildTeamColumn(Teams, FString(), FMargin(0.f), TeamBHeader, TeamBList);
}

void UScoreboardWidget::BuildTeamColumn(UHorizontalBox* TeamsBox, const FString& HeaderText, const FMargin& SlotPadding,
	UTextBlock*& OutHeader, UVerticalBox*& OutList)
{
	if (!TeamsBox) return;

	UVerticalBox* ColumnBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (!ColumnBox) return;

	if (UHorizontalBoxSlot* ColumnSlot = TeamsBox->AddChildToHorizontalBox(ColumnBox))
	{
		ColumnSlot->SetVerticalAlignment(VAlign_Top);
		ColumnSlot->SetPadding(SlotPadding);
	}

	// 队头（队名 + 我方/敌方 + 存活 + 比分）：与数据行同宽，左右对齐
	OutHeader = BlasterScoreboardUI::MakeLabel(WidgetTree, ColumnBox, HeaderText, 17);
	if (OutHeader)
	{
		if (UVerticalBoxSlot* HeaderSlot = Cast<UVerticalBoxSlot>(OutHeader->Slot))
		{
			HeaderSlot->SetPadding(FMargin(BlasterScoreboardUI::RowPadding, 0.f, 0.f, 2.f));
		}
	}

	// 列名行：与数据行共用同一组列宽，保证上下对齐
	UHorizontalBox* Labels = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (Labels)
	{
		USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		if (LabelBox)
		{
			LabelBox->SetWidthOverride(BlasterScoreboardUI::RowTotalWidth);
			LabelBox->SetContent(Labels);
			ColumnBox->AddChildToVerticalBox(LabelBox);
		}

		auto AddLabel = [this, Labels](const FString& Text, float Width)
		{
			if (UTextBlock* Cell = BlasterScoreboardUI::MakeCell(WidgetTree, Labels, Text, Width, 12))
			{
				Cell->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.6f, 0.68f, 1.f)));
			}
		};
		AddLabel(FString(), BlasterScoreboardUI::MarkerWidth);
		AddLabel(TEXT("PLAYER"), BlasterScoreboardUI::NameWidth);
		AddLabel(TEXT("AGENT"), BlasterScoreboardUI::AgentWidth);
		AddLabel(TEXT("K"), BlasterScoreboardUI::KillsWidth);
		AddLabel(TEXT("D"), BlasterScoreboardUI::DeathsWidth);
		AddLabel(TEXT("CREDITS"), BlasterScoreboardUI::CreditsWidth);
	}

	OutList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (OutList) ColumnBox->AddChildToVerticalBox(OutList);
}

void UScoreboardWidget::ShowScoreboard()
{
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	bShown = true;
	RefreshScoreboard();

	// 显示期间才低频刷数据（松手就停），不是每帧
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(RefreshTimerHandle, this, &UScoreboardWidget::RefreshScoreboard,
			FMath::Max(0.05f, RefreshInterval), true);
	}
}

void UScoreboardWidget::HideScoreboard()
{
	bShown = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimerHandle);
	}
	SetVisibility(ESlateVisibility::Collapsed);
}

void UScoreboardWidget::RefreshScoreboard()
{
	UWorld* World = GetWorld();
	ABlasterGameState* GS = World ? World->GetGameState<ABlasterGameState>() : nullptr;
	if (!GS) return;

	const APlayerController* OwningPC = GetOwningPlayer();
	const ABlasterPlayerState* LocalPS = OwningPC ? OwningPC->GetPlayerState<ABlasterPlayerState>() : nullptr;
	const ETeam MyTeam = LocalPS ? LocalPS->Team : ETeam::ET_None;

	if (TitleText)
	{
		TitleText->SetText(FText::FromString(FString::Printf(
			TEXT("SCORE  %d : %d        ROUND %d"), GS->TeamAScore, GS->TeamBScore, GS->RoundNumber)));
	}

	auto MakeHeader = [MyTeam](const TCHAR* TeamName, ETeam Team, int32 Alive, int32 Members, int32 Score) -> FString
	{
		const TCHAR* Relation = (MyTeam == ETeam::ET_None)
			? TEXT("")
			: ((MyTeam == Team) ? TEXT("ALLY") : TEXT("ENEMY"));
		return FString::Printf(TEXT("%s  %s     ALIVE %d/%d     %d PTS"), Relation, TeamName, Alive, Members, Score);
	};

	if (TeamAHeader)
	{
		TeamAHeader->SetText(FText::FromString(MakeHeader(TEXT("TEAM A"), ETeam::ET_TeamA,
			GS->AliveCountTeamA, GS->TeamAMemberCount, GS->TeamAScore)));
		TeamAHeader->SetColorAndOpacity(TeamRelationColor(ETeam::ET_TeamA, MyTeam));
	}
	if (TeamBHeader)
	{
		TeamBHeader->SetText(FText::FromString(MakeHeader(TEXT("TEAM B"), ETeam::ET_TeamB,
			GS->AliveCountTeamB, GS->TeamBMemberCount, GS->TeamBScore)));
		TeamBHeader->SetColorAndOpacity(TeamRelationColor(ETeam::ET_TeamB, MyTeam));
	}

	TArray<FScoreboardEntry> EntriesA;
	TArray<FScoreboardEntry> EntriesB;
	BuildEntries(GS, ETeam::ET_TeamA, LocalPS, EntriesA);
	BuildEntries(GS, ETeam::ET_TeamB, LocalPS, EntriesB);

	ApplyEntries(TeamARows, TeamAList, EntriesA);
	ApplyEntries(TeamBRows, TeamBList, EntriesB);
}

void UScoreboardWidget::BuildEntries(const ABlasterGameState* GS, ETeam Team, const ABlasterPlayerState* LocalPS,
	TArray<FScoreboardEntry>& Out) const
{
	Out.Reset();
	if (!GS) return;

	TArray<ABlasterPlayerState*> Players;
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS))
		{
			if (BPS->Team == Team)
			{
				Players.Add(BPS);
			}
		}
	}

	// TArray<T*> 的 Sort 会自动解引用，谓词收的是元素本身：击杀多的在前，同击杀死的少的在前
	Players.Sort([](const ABlasterPlayerState& A, const ABlasterPlayerState& B)
	{
		if (A.GetScore() != B.GetScore()) return A.GetScore() > B.GetScore();
		return A.GetDefeats() < B.GetDefeats();
	});

	for (const ABlasterPlayerState* BPS : Players)
	{
		FScoreboardEntry Entry;
		Entry.PlayerName = TruncatePlayerName(BPS->GetPlayerName(), PlayerNameMaxChars);
		Entry.AgentName = BlasterAgent::ToDisplayName(BPS->GetAgent());
		Entry.AccentColor = BlasterAgent::GetAccentColor(BPS->GetAgent());
		Entry.Kills = FMath::RoundToInt(BPS->GetScore());
		Entry.Deaths = BPS->GetDefeats();
		Entry.Credits = BPS->Credits;
		Entry.bIsLocalPlayer = (BPS == LocalPS);
		Entry.bHasSpike = (GS->SpikeCarrier == BPS);

		// 阵亡判定与 GameMode 判胜用的规则一致：pawn 还在且未阵亡才算活着
		const ABlasterCharacter* Character = Cast<ABlasterCharacter>(BPS->GetPawn());
		Entry.bIsAlive = Character && !Character->IsElimmed();

		Out.Add(Entry);
	}
}

void UScoreboardWidget::ApplyEntries(TArray<TObjectPtr<UScoreboardRow>>& Pool, UVerticalBox* Parent,
	const TArray<FScoreboardEntry>& Entries)
{
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (UScoreboardRow* Row = EnsureRow(Pool, Parent, Index))
		{
			Row->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			Row->SetRowData(Entries[Index]);
		}
	}

	// 人变少了：多出来的行折叠收起，不销毁（下次人回来直接复用）
	for (int32 Index = Entries.Num(); Index < Pool.Num(); ++Index)
	{
		if (UScoreboardRow* Row = Pool[Index])
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

UScoreboardRow* UScoreboardWidget::EnsureRow(TArray<TObjectPtr<UScoreboardRow>>& Pool, UVerticalBox* Parent, int32 Index)
{
	if (!Parent || Index < 0) return nullptr;

	while (Pool.Num() <= Index)
	{
		const TSubclassOf<UScoreboardRow> Class = RowClass
			? RowClass
			: TSubclassOf<UScoreboardRow>(UScoreboardRow::StaticClass());

		UScoreboardRow* Row = CreateWidget<UScoreboardRow>(GetOwningPlayer(), Class);
		if (!Row) return nullptr;

		Pool.Add(Row);
		Row->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		Parent->AddChildToVerticalBox(Row);
	}

	return Pool[Index];
}
