#include "AgentSelectStrip.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Styling/CoreStyle.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/GameMode/LobbyGameMode.h"
#include "Kismet/GameplayStatics.h"

bool UAgentSelectStrip::Initialize()
{
	if (!Super::Initialize())
	{
		return false;
	}

	if (!bTreeBuilt)
	{
		BuildTree();
	}
	return true;
}

void UAgentSelectStrip::BuildTree()
{
	bTreeBuilt = true;

	UWidgetTree* Tree = WidgetTree;
	if (!Tree)
	{
		return;
	}

	// 根：标题 + 一行按钮（AutoSize 挂进 overlay 底部中央）
	UVerticalBox* Root = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AgentSelectRoot"));
	Tree->RootWidget = Root;

	UTextBlock* Caption = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AgentSelectCaption"));
	Caption->SetText(FText::FromString(TEXT("Select your hero  ·  unselected = Random")));
	Caption->SetJustification(ETextJustify::Center);
	Caption->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.95f)));
	Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 14));
	if (UVerticalBoxSlot* CapSlot = Cast<UVerticalBoxSlot>(Root->AddChild(Caption)))
	{
		CapSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("AgentSelectRow"));
	if (UVerticalBoxSlot* RowSlot = Cast<UVerticalBoxSlot>(Root->AddChild(Row)))
	{
		RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 0.f));
	}

	for (int32 i = 0; i < NumCards; i++)
	{
		const EBlasterAgent Agent = CardAgents[i];

		UButton* Btn = Tree->ConstructWidget<UButton>(UButton::StaticClass(), *FString::Printf(TEXT("AgentCard_%d"), i));
		UTextBlock* Lbl = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *FString::Printf(TEXT("AgentCardLabel_%d"), i));
		Lbl->SetText(FText::FromString(BlasterAgent::ToDisplayName(Agent)));
		Lbl->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 16));
		Lbl->SetJustification(ETextJustify::Center);
		Lbl->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.95f)));
		Btn->AddChild(Lbl);

		if (UHorizontalBoxSlot* BtnSlot = Cast<UHorizontalBoxSlot>(Row->AddChild(Btn)))
		{
			BtnSlot->SetPadding(FMargin(5.f, 0.f, 5.f, 0.f));
			BtnSlot->SetVerticalAlignment(VAlign_Center);
		}

		switch (Agent)
		{
		case EBlasterAgent::Jett:
			Btn->OnClicked.AddDynamic(this, &UAgentSelectStrip::HandleJettClicked);
			break;
		case EBlasterAgent::Sage:
			Btn->OnClicked.AddDynamic(this, &UAgentSelectStrip::HandleSageClicked);
			break;
		case EBlasterAgent::Phoenix:
			Btn->OnClicked.AddDynamic(this, &UAgentSelectStrip::HandlePhoenixClicked);
			break;
		case EBlasterAgent::None:
		default:
			Btn->OnClicked.AddDynamic(this, &UAgentSelectStrip::HandleRandomClicked);
			break;
		}

		CardButtons[i] = Btn;
		CardLabels[i] = Lbl;
	}

	// 初始：未选(CurrentAgent=None) → 高亮 Random 卡
	ApplySelectionVisuals();
}

void UAgentSelectStrip::SetCurrentAgent(EBlasterAgent Agent)
{
	if (CurrentAgent == Agent)
	{
		return;
	}
	CurrentAgent = Agent;
	ApplySelectionVisuals();
}

void UAgentSelectStrip::ApplySelectionVisuals()
{
	for (int32 i = 0; i < NumCards; i++)
	{
		if (!CardLabels[i])
		{
			continue;
		}

		const bool bSelected = (CardAgents[i] == CurrentAgent);
		const FString Name = BlasterAgent::ToDisplayName(CardAgents[i]);
		const FString Text = bSelected ? FString::Printf(TEXT("\x25B6  %s"), *Name) : Name;

		CardLabels[i]->SetText(FText::FromString(Text));
		CardLabels[i]->SetColorAndOpacity(FSlateColor(bSelected
			? FLinearColor(1.f, 0.86f, 0.25f, 1.f)
			: FLinearColor(1.f, 1.f, 1.f, 0.95f)));
	}
}

void UAgentSelectStrip::HandleJettClicked()
{
	RequestSelect(EBlasterAgent::Jett);
}

void UAgentSelectStrip::HandleSageClicked()
{
	RequestSelect(EBlasterAgent::Sage);
}

void UAgentSelectStrip::HandlePhoenixClicked()
{
	RequestSelect(EBlasterAgent::Phoenix);
}

void UAgentSelectStrip::HandleRandomClicked()
{
	RequestSelect(EBlasterAgent::None);
}

void UAgentSelectStrip::RequestSelect(EBlasterAgent Agent)
{
	ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetOwningPlayer());
	if (!PC)
	{
		return;
	}

	if (PC->HasAuthority())
	{
		// Listen server host：直接走 GameMode 权威落 PS
		if (ALobbyGameMode* GM = Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this)))
		{
			GM->SelectPlayerAgent(PC, Agent);
		}
	}
	else
	{
		PC->ServerSelectAgent(Agent);
	}
}
