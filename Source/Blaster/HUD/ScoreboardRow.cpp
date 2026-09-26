#include "ScoreboardRow.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"

namespace BlasterScoreboardUI
{
	// 只造文本块，不挂父节点（挂法由调用方决定）
	static UTextBlock* CreateTextBlock(UWidgetTree* Tree, const FString& Text, int32 FontSize)
	{
		if (!Tree) return nullptr;

		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		if (!Block) return nullptr;

		Block->SetText(FText::FromString(Text));
		// UTextBlock 构造时自带一份有效字体（Roboto 24 Bold），这里只改字号
		FSlateFontInfo FontInfo = Block->GetFont();
		FontInfo.Size = FontSize;
		Block->SetFont(FontInfo);
		Block->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		return Block;
	}

	UTextBlock* MakeLabel(UWidgetTree* Tree, UPanelWidget* Parent, const FString& Text, int32 FontSize)
	{
		UTextBlock* Block = CreateTextBlock(Tree, Text, FontSize);
		if (Block && Parent) Parent->AddChild(Block);
		return Block;
	}

	UTextBlock* MakeCell(UWidgetTree* Tree, UHorizontalBox* Parent, const FString& Text, float Width, int32 FontSize)
	{
		UTextBlock* Block = CreateTextBlock(Tree, Text, FontSize);
		if (!Block || !Tree || !Parent) return Block;

		// 定宽 SizeBox 包住文本，保证表头与各行的列左右对齐
		USizeBox* Box = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		if (!Box)
		{
			Parent->AddChild(Block);
			return Block;
		}
		Box->SetWidthOverride(Width);
		Box->SetContent(Block);

		if (UHorizontalBoxSlot* Slot = Parent->AddChildToHorizontalBox(Box))
		{
			Slot->SetVerticalAlignment(VAlign_Center);
			Slot->SetPadding(FMargin(2.f, 1.f));
		}
		return Block;
	}
}

void UScoreboardRow::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// 记分板是纯展示叠加层：自己和子件都不吃点击，别挡住玩家操作
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

TSharedRef<SWidget> UScoreboardRow::RebuildWidget()
{
	// 只有「没有 WBP 提供布局」时才自绘默认行：WBP 的 WidgetTree 已经有 RootWidget
	if (WidgetTree && !WidgetTree->RootWidget && !PlayerNameText)
	{
		BuildDefaultLayout();
	}
	return Super::RebuildWidget();
}

void UScoreboardRow::BuildDefaultLayout()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("RowBox"));
	if (!Row) return;

	MarkerText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::MarkerWidth, 14);
	PlayerNameText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::NameWidth, 15);
	AgentText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::AgentWidth, 15);
	KillsText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::KillsWidth, 15);
	DeathsText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::DeathsWidth, 15);
	CreditsText = BlasterScoreboardUI::MakeCell(WidgetTree, Row, FString(), BlasterScoreboardUI::CreditsWidth, 15);

	if (MarkerText) MarkerText->SetJustification(ETextJustify::Center);
	if (KillsText) KillsText->SetJustification(ETextJustify::Center);
	if (DeathsText) DeathsText->SetJustification(ETextJustify::Center);
	if (CreditsText) CreditsText->SetJustification(ETextJustify::Center);

	// 玩家名限一行 + 裁剪 + 省略号：名字再长也不会换行撑高行、或糊到英雄列上
	if (PlayerNameText)
	{
		PlayerNameText->SetAutoWrapText(false);
		PlayerNameText->SetClipping(EWidgetClipping::ClipToBounds);
		PlayerNameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}
	if (AgentText)
	{
		AgentText->SetAutoWrapText(false);
		AgentText->SetClipping(EWidgetClipping::ClipToBounds);
	}

	// 底色条：行内容塞进 Border（SetRowData 里按「本地玩家 / 阵亡」刷底色）
	RowBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("RowBorder"));
	if (RowBorder)
	{
		RowBorder->SetPadding(FMargin(BlasterScoreboardUI::RowPadding, 2.f));
		RowBorder->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		RowBorder->SetContent(Row);
		WidgetTree->RootWidget = RowBorder;
	}
	else
	{
		WidgetTree->RootWidget = Row;
	}
}

void UScoreboardRow::SetRowData(const FScoreboardEntry& Entry)
{
	// 阵亡整行压暗；本地玩家名字用金色高亮 + 更亮的底色，一眼找到自己
	const float DimAlpha = Entry.bIsAlive ? 1.f : 0.45f;
	const FLinearColor NameColor = Entry.bIsLocalPlayer
		? FLinearColor(1.f, 0.9f, 0.42f, 1.f)
		: FLinearColor(1.f, 1.f, 1.f, DimAlpha);
	const FLinearColor BodyColor = FLinearColor(0.85f, 0.87f, 0.92f, DimAlpha);

	if (PlayerNameText)
	{
		PlayerNameText->SetText(FText::FromString(Entry.PlayerName));
		PlayerNameText->SetColorAndOpacity(FSlateColor(NameColor));
	}
	if (AgentText)
	{
		AgentText->SetText(FText::FromString(Entry.AgentName));
		AgentText->SetColorAndOpacity(FSlateColor(Entry.AccentColor * DimAlpha));
	}
	if (KillsText)
	{
		KillsText->SetText(FText::FromString(FString::Printf(TEXT("%d"), Entry.Kills)));
		KillsText->SetColorAndOpacity(FSlateColor(BodyColor));
	}
	if (DeathsText)
	{
		DeathsText->SetText(FText::FromString(FString::Printf(TEXT("%d"), Entry.Deaths)));
		DeathsText->SetColorAndOpacity(FSlateColor(BodyColor));
	}
	if (CreditsText)
	{
		CreditsText->SetText(FText::FromString(FString::Printf(TEXT("$%d"), Entry.Credits)));
		CreditsText->SetColorAndOpacity(FSlateColor(BodyColor));
	}
	if (MarkerText)
	{
		// 携包优先（带包的人不可能已阵亡），其次阵亡，否则留空
		const FString Marker = Entry.bHasSpike
			? FString(TEXT("SPIKE"))
			: (Entry.bIsAlive ? FString() : FString(TEXT("DEAD")));
		MarkerText->SetText(FText::FromString(Marker));
		MarkerText->SetColorAndOpacity(FSlateColor(Entry.bHasSpike
			? FLinearColor(1.f, 0.6f, 0.1f, 1.f)
			: FLinearColor(0.85f, 0.35f, 0.35f, DimAlpha)));
	}
	if (RowBorder)
	{
		RowBorder->SetBrushColor(Entry.bIsLocalPlayer
			? FLinearColor(0.28f, 0.38f, 0.65f, 0.38f)
			: FLinearColor(1.f, 1.f, 1.f, 0.04f));
	}
}
