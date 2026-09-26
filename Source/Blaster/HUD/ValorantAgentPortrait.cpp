#include "ValorantAgentPortrait.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Engine/Texture2D.h"

namespace
{
	// 深色底：英雄 PNG 四周是透明的，不垫一层就能看见背后的游戏画面
	const FLinearColor GPortraitBackColor(0.02f, 0.025f, 0.03f, 0.88f);
}

void UValorantAgentPortrait::SetAgent(EBlasterAgent NewAgent)
{
	if (Agent == NewAgent) return;
	Agent = NewAgent;
	ApplyAgent();
}

void UValorantAgentPortrait::ApplyAgent()
{
	if (FrameImage)
	{
		FrameImage->SetColorAndOpacity(BlasterAgent::GetAccentColor(Agent));
	}

	if (PortraitImage)
	{
		// 软路径 -> 对象。贴图没导进来只是这一格空着（露出深色底），不影响 HUD 其它部分。
		const FString Path = BlasterAgent::GetPortraitPath(Agent);
		UTexture2D* Texture = Path.IsEmpty() ? nullptr : LoadObject<UTexture2D>(nullptr, *Path);
		PortraitImage->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
	}
}

TSharedRef<SWidget> UValorantAgentPortrait::RebuildWidget()
{
	// Initialize() 才是建 WidgetTree 的地方（和 UValorantTopHUD 同一个理由）。
	Initialize();

	// 纯 C++ 的 UUserWidget 类不保证 Initialize 会给建 WidgetTree，兜一下。
	if (WidgetTree == nullptr)
	{
		WidgetTree = NewObject<UWidgetTree>(this, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transient);
	}

	if (!bBuilt && WidgetTree && WidgetTree->RootWidget == nullptr)
	{
		// 根：定死 36x36。HorizontalBox 的槽是 Automatic，格子多大就吃多大。
		USizeBox* Root = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("Size_Root"));
		if (Root)
		{
			Root->SetWidthOverride(TileSize);
			Root->SetHeightOverride(TileSize);

			UOverlay* Stack = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("Overlay_Root"));
			Root->AddChild(Stack);
			WidgetTree->RootWidget = Root;

			if (Stack)
			{
				// 三层用同一套 Fill 对齐，靠 padding 0/1/2 叠出同心方 ——
				// 外层留 1px 就是描边环，中层留 1px 就是深色边。
				// OutImage 收 TObjectPtr 引用（成员就是 TObjectPtr，收裸指针引用接不上）
				auto AddLayer = [&](TObjectPtr<UImage>& OutImage, const TCHAR* Name, float Inset) -> void
				{
					OutImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), FName(Name));
					if (OutImage == nullptr) return;
					OutImage->SetVisibility(ESlateVisibility::HitTestInvisible);

					UOverlaySlot* BoxSlot = Stack->AddChildToOverlay(OutImage);
					if (BoxSlot == nullptr) return;
					BoxSlot->SetHorizontalAlignment(HAlign_Fill);
					BoxSlot->SetVerticalAlignment(VAlign_Fill);
					BoxSlot->SetPadding(FMargin(Inset));
				};

				AddLayer(FrameImage,    TEXT("Image_Frame"),    0.f);
				AddLayer(BackImage,     TEXT("Image_Back"),     1.f);
				AddLayer(PortraitImage, TEXT("Image_Portrait"), 2.f);

				if (BackImage)
				{
					BackImage->SetColorAndOpacity(GPortraitBackColor);
				}
			}
		}
	}

	bBuilt = true;
	ApplyAgent();

	return Super::RebuildWidget();
}
