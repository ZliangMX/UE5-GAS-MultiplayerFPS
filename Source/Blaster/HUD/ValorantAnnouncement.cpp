// Valorant 局内公告面板实现。三块：版面表 + 文本加工/显隐 + 烤图工具。
// 版面坐标来自 E:\Notion\claude-temp\ann_draw.py 的实测值（源图 2559x1439 的像素框）。

#include "ValorantAnnouncement.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "UObject/UnrealType.h"

namespace
{
	// 素材目录（和其它 HUD 的目录分开，别混）
	const TCHAR* const AnnFolder = TEXT("/Game/Assets/Textures/HUD/Announce/");

	UTexture2D* LoadAnnTexture(const TCHAR* AssetName)
	{
		const FString Path = FString(AnnFolder) + AssetName + TEXT(".") + AssetName;
		return LoadObject<UTexture2D>(nullptr, *Path);
	}

	// ---- 版面实测值（源图像素；面板本体在 x1003..1558, y179.5..373）----
	constexpr float PanelX = 1003.f, PanelY = 179.f, PanelW = 556.f, PanelH = 195.f;

	// 中间大字那一格。实测 "BUY PHASE" cap 高 77px、宽 435px —— 是**窄体**，引擎自带 Roboto Bold
	// 没有窄体，所以做不到两样都对。这里按"字高对到 96 号（cap≈68）"，再用横向 render transform
	// 压到 0.88 把宽度对回来（96 号 * "BUY PHASE" 的 5.17em ≈ 496，压完 ≈ 437，实测 435）。
	// ★ 字号**故意**没给到 108（那样 cap 才是实测的 77）：108 号不压缩宽 558，刚好顶爆面板宽 556，
	//   万一 render transform 在某个平台不生效就会溢出去。
	constexpr int32 MainFontSize = 96;
	constexpr float MainTextScaleX = 0.88f;

	// 小字（表头 / 提示 / 回合结果）。实测字高：表头 cap≈14、提示 cap≈13 → 19 号 Roboto 的 cap≈13.5。
	constexpr int32 SmallFontSize = 19;

	// 行高 1.171em、cap 顶离行顶 0.216em（Roboto 的 ascent 0.927 / capHeight 0.711）。
	// 要让 cap 顶落在实测的 y=230，框顶就得放 230 - 0.216*96 ≈ 209，框高给整行 1.171*96 ≈ 113。
	constexpr float MainX = 1001.f, MainY = 209.f, MainW = 558.f, MainH = 113.f;

	// 两行文字各自的中线 y（都是 Center 锚 + AutoSize，宽高由内容定 —— 文字是活的，
	// 写死宽度的话 "GAME OVER" 会偏）
	constexpr float ExtraCY  = 325.f;    // 大字底(306) 和提示顶(346) 之间的空档
	constexpr float HintCY   = 352.5f;   // 提示行：B 键框实测 y337..367，中线 352

	// 提示行 B 键框贴图 50x39（实测覆盖 x1249..1299）
	constexpr float KeyBoxW = 50.f, KeyBoxH = 39.f;

	// 行内间距（实测：PRESS 尾 1235 → 键框起 1249 → TO BUY 起 1312）
	constexpr float PadAfterPress = 14.f;
	constexpr float PadAfterKeyBox = 13.f;
	constexpr float PadBetweenExtra = 28.f;

	// ---- 面板底色三态。反解方法 / 每档的原始读数见 fps_project_progress（六十三）----
	// BUY PHASE 那档是锚点：从旧参考图反解出 C=(194,196,200)@0.30，和新这批图上同一套
	// "面板内中位色 / 面板外中位色" 反解出来的 (194,194,194)@0.30 对得上，所以 0.30 是可信的。
	// WON / LOST 在 0.30 下解不出来（LOST 的 R 会算出 290，超 255）—— 结果面板确实更厚，取 0.50；
	// 色相是实测的，0.50 这个不透明度是这两档唯一没被约束死的量，将来嫌厚/嫌薄就改它。
	const FLinearColor PanelToneNeutral = FLinearColor(0.540f, 0.540f, 0.540f, 0.30f);  // 屏幕 (194,194,194)
	const FLinearColor PanelToneWon     = FLinearColor(0.141f, 0.423f, 0.361f, 0.50f);  // 屏幕 (105,174,162)
	const FLinearColor PanelToneLost    = FLinearColor(0.473f, 0.091f, 0.107f, 0.50f);  // 屏幕 (183, 85, 92)

	const FLinearColor& ToneToColor(EValorantAnnPanelTone Tone)
	{
		switch (Tone)
		{
		case EValorantAnnPanelTone::Won:  return PanelToneWon;
		case EValorantAnnPanelTone::Lost: return PanelToneLost;
		default:                          return PanelToneNeutral;
		}
	}

	// ---- 安包/拆包进度条实测值（源图 2550x1433，按中心对齐归一化到 2559x1439）----
	// 整组 = 上面标签框（DEFUSING）+ 下面那根分两半的条，水平居中，标签框底正好贴着条的顶。
	// 竖直实测：标签框 y174..230（高 56），条 y231..270（高 40）→ 整组 y174..270，中线 222。
	//
	// ★ 横向实测是 x1104.5..1446.5（宽 342），但源图整体比设计分辨率窄 9px，测出来所有元素都
	//   偏左 4px（同一张图里顶栏那个 V 的顶点也在 1277 而不是 1280）。宽高不受影响，位置按**居中**放。
	constexpr float SpikeBarW = 342.f, SpikeBarH = 40.f;
	constexpr float SpikeTrackInsetTop = 7.f, SpikeTrackInsetBottom = 8.f;   // 实测槽 y238..262
	constexpr float SpikeTrackH = SpikeBarH - SpikeTrackInsetTop - SpikeTrackInsetBottom;  // 25
	constexpr float SpikeDividerW = 3.f;                                     // 中缝实测 2-3px，正好在条中心
	constexpr float SpikeLabelH = 56.f, SpikeLabelMinW = 124.f;
	constexpr float SpikeLabelPadX = 18.f, SpikeLabelPadY = 17.f;
	constexpr float SpikeLabelLineH = 2.f;   // 上下白细线的高度
	constexpr float SpikeCX = UValorantAnnouncement::DesignWidth * 0.5f;
	constexpr float SpikeCY = 222.f;
	constexpr int32 SpikeFontSize = 19;

	// 屏幕实测色 -> FLinearColor。（不能拿屏幕读数直接当 FLinearColor 填：那是 sRGB，
	// Slate 收到的是线性值。下面每一条都是按"目标屏幕色 / 假设 alpha / 该处背景亮度"反解出来的，
	// 括号里写的是它在取色器里显示成什么。）
	const FLinearColor SpikePlateColor   = FLinearColor(0.218f, 0.292f, 0.350f, 0.70f);  // 屏幕 (113,132,147)
	const FLinearColor SpikeTrackColor   = FLinearColor(0.021f, 0.028f, 0.035f, 0.88f);  // 屏幕 ( 42, 53, 63)
	const FLinearColor SpikeFillColor    = FLinearColor(0.815f, 0.847f, 0.445f, 1.00f);  // 屏幕 (233,237,178) 淡黄
	const FLinearColor SpikeDividerColor = FLinearColor(0.730f, 0.784f, 0.831f, 1.00f);  // 屏幕 (222,229,235)
	const FLinearColor SpikeLabelFaceColor = FLinearColor(0.081f, 0.129f, 0.212f, 0.75f); // 屏幕 ( 75, 95,122)
	const FLinearColor SpikeLabelLineColor = FLinearColor(0.680f, 0.716f, 0.791f, 1.00f); // 屏幕 (215,220,230)

	// 引擎自带 Roboto 没有字间距（letter-spacing）设置，而实测表头/提示每个字约 18px、
	// Roboto 19 号只有约 12px —— 差的 6px 用**字间插空格**补（Roboto 空格 0.25em ≈ 4.75px）。
	// ★ 这个操作**不幂等**（往已经插过的串里再插一遍会越插越宽），所以只能在数据源 -> 显示块
	//   这一步做一次，绝不能原地回写数据源。见 NormalizeAndLayout。
	FString SpaceOut(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() * 2);
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			if (Index > 0) { Out.AppendChar(TEXT(' ')); }
			Out.AppendChar(In[Index]);
		}
		return Out;
	}

	// 空串原样返回 —— 调用方要靠 IsEmpty() 判断"这一行要不要显示"。
	FString Decorate(const FString& In, bool bSpaceOut)
	{
		if (In.IsEmpty()) { return In; }
		return bSpaceOut ? SpaceOut(In.ToUpper()) : In.ToUpper();
	}

	// 把控件摆到 Canvas 上（固定宽高）。换算和 UValorantTopHUD::PlaceTopOnCanvas 一样：
	// 锚在 (正中, 底)，(X, Y) 是框左上角 → 纵向补回高度。
	void PlaceOnCanvas(UCanvasPanel* Root, UWidget* Widget, float X, float Y, float W, float H)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;
		CanvasSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetPosition(FVector2D(X - UValorantAnnouncement::DesignWidth * 0.5f, (Y + H) - UValorantAnnouncement::DesignHeight));
		CanvasSlot->SetSize(FVector2D(W, H));
		CanvasSlot->SetAutoSize(false);
		CanvasSlot->SetZOrder(0);
	}

	// 同上，但**自动宽高**：给的是控件中心点。
	void PlaceCentered(UCanvasPanel* Root, UWidget* Widget, float CX, float CY)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;
		CanvasSlot->SetAnchors(FAnchors(0.5f, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CanvasSlot->SetPosition(FVector2D(CX - UValorantAnnouncement::DesignWidth * 0.5f, CY - UValorantAnnouncement::DesignHeight));
		CanvasSlot->SetAutoSize(true);
		CanvasSlot->SetZOrder(0);
	}

	// 进度条内部那个小 Canvas 用的摆放（锚在左上角，给的就是左上角坐标）。
	// 和 PlaceOnCanvas 不同：那个是设计分辨率下的绝对坐标，这个是容器内的局部坐标。
	void PlaceLocal(UCanvasPanel* Root, UWidget* Widget, float X, float Y, float W, float H, bool bAutoSize = false)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;
		CanvasSlot->SetAnchors(FAnchors(0.f, 0.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 0.f));
		CanvasSlot->SetPosition(FVector2D(X, Y));
		CanvasSlot->SetSize(FVector2D(W, H));
		CanvasSlot->SetAutoSize(bAutoSize);
	}

	// 同上，按容器中心摆（给的是控件自己的尺寸）
	void PlaceLocalCenter(UCanvasPanel* Root, UWidget* Widget, float W, float H)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;
		CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CanvasSlot->SetPosition(FVector2D::ZeroVector);
		CanvasSlot->SetSize(FVector2D(W, H));
		CanvasSlot->SetAutoSize(false);
	}

	UTextBlock* MakeText(UWidgetTree* Tree, const TCHAR* Name, int32 FontSize, const TCHAR* Style, const FLinearColor& Color)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
		if (Text == nullptr) return nullptr;
		Text->SetFont(FCoreStyle::GetDefaultFontStyle(FName(Style), FontSize));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetAutoWrapText(false);
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		// ★ 必须 true：GetWidgetFromName 只认 bIsVariable 的控件，烤进 WBP 之后也靠它绑定
		Text->bIsVariable = true;
		return Text;
	}

	UTextBlock* MakeSmallText(UWidgetTree* Tree, const TCHAR* Name, float Alpha)
	{
		return MakeText(Tree, Name, SmallFontSize, TEXT("Bold"), FLinearColor(1.f, 1.f, 1.f, Alpha));
	}

	UImage* MakeImage(UWidgetTree* Tree, const TCHAR* Name, const TCHAR* TextureName, float Alpha)
	{
		UImage* Image = Tree->ConstructWidget<UImage>(UImage::StaticClass(), FName(Name));
		if (Image == nullptr) return nullptr;
		if (UTexture2D* Texture = LoadAnnTexture(TextureName))
		{
			// bMatchSize = false：尺寸由外面的 SizeBox 说了算（贴图是 4x 的）
			Image->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
		}
		Image->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, Alpha));
		Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		Image->bIsVariable = true;
		return Image;
	}

	// 给水平容器塞一个子控件，顺带设成上下居中 + 右侧间距
	void AddBoxChild(UHorizontalBox* Box, UWidget* Child, float RightPadding)
	{
		if (Box == nullptr || Child == nullptr) return;
		UHorizontalBoxSlot* Slot = Box->AddChildToHorizontalBox(Child);
		if (Slot == nullptr) return;
		Slot->SetHorizontalAlignment(HAlign_Left);
		Slot->SetVerticalAlignment(VAlign_Center);
		Slot->SetPadding(FMargin(0.f, 0.f, RightPadding, 0.f));
	}

	UHorizontalBox* MakeBox(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name, float CY)
	{
		UHorizontalBox* Box = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), FName(Name));
		if (Box == nullptr) return nullptr;
		Box->SetVisibility(ESlateVisibility::HitTestInvisible);
		Box->bIsVariable = true;
		PlaceCentered(Root, Box, UValorantAnnouncement::DesignWidth * 0.5f, CY);
		return Box;
	}

	// 纯色矩形。用 UBorder 而不是 UImage：UImage 不给画刷资源时行为不定，UBorder 的背景就是
	// 一块纯色（FSlateColorBrush），尺寸由外层槽位说了算 —— 正好是我们要的。
	// （同一个项目里 CloveSmokeMapWidget 也是这么干的。）
	UBorder* MakeSolid(UWidgetTree* Tree, const TCHAR* Name, const FLinearColor& Color)
	{
		UBorder* Solid = Tree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(Name));
		if (Solid == nullptr) return nullptr;
		if (GEngine && GEngine->DefaultTexture)
		{
			Solid->SetBrushFromTexture(GEngine->DefaultTexture);
		}
		Solid->SetBrushColor(Color);
		Solid->SetVisibility(ESlateVisibility::HitTestInvisible);
		Solid->bIsVariable = true;
		return Solid;
	}

	// 定尺寸的格子（贴图是 4x 的，不套 SizeBox 的话会按贴图原始像素撑开）
	USizeBox* MakeCell(UWidgetTree* Tree, const TCHAR* Name, float W, float H)
	{
		USizeBox* Cell = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), FName(Name));
		if (Cell == nullptr) return nullptr;
		Cell->SetWidthOverride(W);
		Cell->SetHeightOverride(H);
		Cell->SetVisibility(ESlateVisibility::HitTestInvisible);
		Cell->bIsVariable = true;
		return Cell;
	}
}

// ---------------------------------------------------------------- 版面

void UValorantAnnouncement::BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root)
{
	if (Tree == nullptr || Root == nullptr) return;

	// 【加进 Canvas 的顺序 = 绘制顺序】底板最先加，后面的压在上面。

	// 1) 底板，两层：
	//    Border_PanelFill —— 半透明**纯色**（三态换的就是它）
	//    Panel_Back       —— 只有框的贴图（顶部 3 行渐隐高光 + 四角角标 + 小方块），中间透明
	// ★ 顺序要紧：CanvasPanel 按加入顺序绘制，填充必须先加，否则会把角标盖住。
	if (UBorder* Fill = MakeSolid(Tree, TEXT("Border_PanelFill"), PanelToneNeutral))
	{
		PlaceOnCanvas(Root, Fill, PanelX, PanelY, PanelW, PanelH);
	}
	if (UImage* Panel = MakeImage(Tree, TEXT("Panel_Back"), TEXT("T_AnnPanel"), 1.f))
	{
		PlaceOnCanvas(Root, Panel, PanelX, PanelY, PanelW, PanelH);
	}

	// 2) 中间大字。内容是 AnnouncementText 或 WarmupTime 加工来的，见 NormalizeAndLayout。
	UTextBlock* Main = MakeText(Tree, TEXT("Text_Main"), MainFontSize, TEXT("Bold"), FLinearColor::White);
	if (Main)
	{
		Main->SetJustification(ETextJustify::Center);
		// 横向压窄（pivot 在正中 → 压缩后仍然居中，不会跑偏）
		Main->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
		Main->SetRenderTransform(FWidgetTransform(FVector2D::ZeroVector, FVector2D(MainTextScaleX, 1.f),
			FVector2D::ZeroVector, 0.f));
		PlaceOnCanvas(Root, Main, MainX, MainY, MainW, MainH);
	}

	// 3) 五个**数据源**：名字必须和 UAnnouncement 的属性名一字不差（GetWidgetFromName 靠它绑定，
	//    将来把版面烤进 WBP 时 BindWidget 校验也靠它）。位置/字号无所谓 —— 它们永远是 Collapsed，
	//    只当 BlasterPlayerController 的落点。
	static const TCHAR* const SourceNames[] =
	{
		TEXT("AnnouncementText"), TEXT("WarmupTime"), TEXT("InfoText"),
		TEXT("RoundResultText"), TEXT("TeamSwapText")
	};
	for (const TCHAR* SourceName : SourceNames)
	{
		if (UTextBlock* Source = MakeText(Tree, SourceName, SmallFontSize, TEXT("Regular"), FLinearColor::White))
		{
			Source->SetVisibility(ESlateVisibility::Collapsed);
			PlaceOnCanvas(Root, Source, 0.f, 0.f, 1.f, 1.f);
		}
	}

	// （原来的第 4 步"ROUND n ////// 阵营名"表头行已删：对过三张参考图，只有 BUY PHASE 那张
	//   有表头，WON / LOST 都没有 —— 是那个版式特有的，不是通用件。T_AnnSlash 贴图还留在盘上。）

	// 5) 大字下面那行：回合结果 / 换边提示
	UHorizontalBox* Extra = MakeBox(Tree, Root, TEXT("Box_Extra"), ExtraCY);
	{
		AddBoxChild(Extra, MakeSmallText(Tree, TEXT("Text_ExtraRound"), 0.92f), PadBetweenExtra);
		AddBoxChild(Extra, MakeSmallText(Tree, TEXT("Text_ExtraSwap"), 0.92f), 0.f);
	}

	// 6) 底部提示行：PRESS [B] TO BUY
	UHorizontalBox* Hint = MakeBox(Tree, Root, TEXT("Box_Hint"), HintCY);
	{
		AddBoxChild(Hint, MakeSmallText(Tree, TEXT("Text_HintPress"), 0.92f), PadAfterPress);

		// 键帽那格：SizeBox 定尺寸 → Overlay 让键框贴图和键名叠在一起
		if (USizeBox* KeyCell = MakeCell(Tree, TEXT("Size_Key"), KeyBoxW, KeyBoxH))
		{
			UOverlay* KeyOverlay = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("Overlay_Key"));
			if (KeyOverlay)
			{
				KeyOverlay->SetVisibility(ESlateVisibility::HitTestInvisible);
				KeyOverlay->bIsVariable = true;
				if (UImage* KeyBox = MakeImage(Tree, TEXT("Img_KeyBox"), TEXT("T_AnnKeyBox"), 1.f))
				{
					if (UOverlaySlot* BoxSlot = KeyOverlay->AddChildToOverlay(KeyBox))
					{
						BoxSlot->SetHorizontalAlignment(HAlign_Fill);
						BoxSlot->SetVerticalAlignment(VAlign_Fill);
					}
				}
				UTextBlock* KeyText = MakeText(Tree, TEXT("Text_Key"), 22, TEXT("Bold"), FLinearColor::White);
				if (KeyText)
				{
					KeyText->SetJustification(ETextJustify::Center);
					if (UOverlaySlot* TextSlot = KeyOverlay->AddChildToOverlay(KeyText))
					{
						TextSlot->SetHorizontalAlignment(HAlign_Center);
						TextSlot->SetVerticalAlignment(VAlign_Center);
					}
				}
				KeyCell->AddChild(KeyOverlay);
			}
			AddBoxChild(Hint, KeyCell, PadAfterKeyBox);
		}

		AddBoxChild(Hint, MakeSmallText(Tree, TEXT("Text_InfoDisplay"), 0.92f), 0.f);
	}

	// 7) 安包/拆包进度条。和上面那块公告**共用同一片位置**，NormalizeAndLayout 里二选一显示。
	BuildSpikeBar(Tree, Root);
}

// 安包/拆包这一组：上面一个 "DEFUSING" 标签框，下面一根分两半的进度条，整体水平居中。
// 结构（自下而上看）：
//   Box_Spike (VerticalBox, 自动宽高，中心摆在 (SpikeCX, SpikeCY))
//     ├─ Size_SpikeLabel (USizeBox, 最小宽 124 —— 宽度还是由文字撑)
//     │    Overlay_SpikeLabel
//     │      ├─ Border_LabelFace     半透明蓝灰底
//     │      ├─ Size_LabelLineTop    顶部 2px 白线（VAlign=Top）
//     │      ├─ Size_LabelLineBot    底部 2px 白线（VAlign=Bottom）
//     │      └─ Text_SpikeStatus     白字，四周留白把框撑到实测的 124x56
//     └─ Size_SpikeBar (USizeBox 342x40)
//          Canvas_SpikeBar
//            ├─ Border_BarPlate    浅蓝灰底板（整条 342x40）
//            ├─ Border_BarTrack    深色槽（上下各让出 7/8px，高 25）
//            ├─ Border_BarFill     淡黄填充，宽度 = 342 * Progress（AutoSize，靠 SizeBox 的 WidthOverride）
//            └─ Border_BarDivider  中缝，摆在正中央 —— 就是"分两半"那条
// ★ 左右两侧的实测值来自一张比设计分辨率窄 9px 的截图，整体偏左 4px，所以位置按居中放（见 SpikeBarW 注释）。
void UValorantAnnouncement::BuildSpikeBar(UWidgetTree* Tree, UCanvasPanel* Root)
{
	if (Tree == nullptr || Root == nullptr) return;

	UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box_Spike"));
	if (Box == nullptr) return;
	Box->SetVisibility(ESlateVisibility::Collapsed);   // 默认收起，等 SetSpikeStatus 打开
	Box->bIsVariable = true;
	PlaceCentered(Root, Box, SpikeCX, SpikeCY);

	// --- 标签框 ---
	USizeBox* LabelCell = MakeCell(Tree, TEXT("Size_SpikeLabel"), SpikeLabelMinW, SpikeLabelH);
	if (LabelCell)
	{
		// ★ 宽高都交给文字（+上下左右留白）撑，只是给个**下限** —— 用 SetWidthOverride 的话会把宽度
		//   锁死在 124，状态串一长就被截。实测的 124x56 是 "DEFUSING" 撑出来的结果，不是硬约束。
		LabelCell->SetWidthOverride(0.f);
		LabelCell->SetHeightOverride(0.f);
		LabelCell->SetMinDesiredWidth(SpikeLabelMinW);
		if (UVerticalBoxSlot* CellSlot = Box->AddChildToVerticalBox(LabelCell))
		{
			CellSlot->SetHorizontalAlignment(HAlign_Center);
			CellSlot->SetVerticalAlignment(VAlign_Center);
		}

		UOverlay* LabelOverlay = Tree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("Overlay_SpikeLabel"));
		if (LabelOverlay)
		{
			LabelOverlay->SetVisibility(ESlateVisibility::HitTestInvisible);
			LabelOverlay->bIsVariable = true;

			// 底色（铺满，尺寸由同层那个带留白的文字决定 —— Overlay 取所有子控件 desired 的最大值）
			UBorder* Face = MakeSolid(Tree, TEXT("Border_LabelFace"), SpikeLabelFaceColor);
			if (Face)
			{
				if (UOverlaySlot* FaceSlot = LabelOverlay->AddChildToOverlay(Face))
				{
					FaceSlot->SetHorizontalAlignment(HAlign_Fill);
					FaceSlot->SetVerticalAlignment(VAlign_Fill);
				}
			}

			// 上下各一条白细线（左右没有 —— 实测标签框左右就是直接断掉）
			// SizeBox 只给高度（0 在 USizeBox 里等于"不覆盖"，宽度自然就撑满了）
			struct FLineSpec { const TCHAR* CellName; const TCHAR* LineName; EVerticalAlignment Align; };
			const FLineSpec Lines[] =
			{
				{ TEXT("Size_LabelLineTop"), TEXT("Border_LabelLineTop"), VAlign_Top },
				{ TEXT("Size_LabelLineBot"), TEXT("Border_LabelLineBot"), VAlign_Bottom },
			};
			for (const FLineSpec& Spec : Lines)
			{
				USizeBox* LineCell = MakeCell(Tree, Spec.CellName, 0.f, SpikeLabelLineH);
				if (LineCell == nullptr) continue;
				if (UBorder* Line = MakeSolid(Tree, Spec.LineName, SpikeLabelLineColor))
				{
					LineCell->AddChild(Line);
				}
				if (UOverlaySlot* LineSlot = LabelOverlay->AddChildToOverlay(LineCell))
				{
					LineSlot->SetHorizontalAlignment(HAlign_Fill);
					LineSlot->SetVerticalAlignment(Spec.Align);
				}
			}

			// 文字：四周留白把框撑到实测大小（"DEFUSING" 19 号 Bold 约 90px + 左右 36 = 126 ≈ 实测 124）
			UTextBlock* Status = MakeText(Tree, TEXT("Text_SpikeStatus"), SpikeFontSize, TEXT("Bold"), FLinearColor::White);
			if (Status)
			{
				Status->SetJustification(ETextJustify::Center);
				if (UOverlaySlot* TextSlot = LabelOverlay->AddChildToOverlay(Status))
				{
					TextSlot->SetHorizontalAlignment(HAlign_Center);
					TextSlot->SetVerticalAlignment(VAlign_Center);
					TextSlot->SetPadding(FMargin(SpikeLabelPadX, SpikeLabelPadY, SpikeLabelPadX, SpikeLabelPadY));
				}
			}

			LabelCell->AddChild(LabelOverlay);
		}
	}

	// --- 进度条 ---
	USizeBox* BarCell = MakeCell(Tree, TEXT("Size_SpikeBar"), SpikeBarW, SpikeBarH);
	if (BarCell == nullptr) return;
	if (UVerticalBoxSlot* BarSlot = Box->AddChildToVerticalBox(BarCell))
	{
		BarSlot->SetHorizontalAlignment(HAlign_Center);
		BarSlot->SetVerticalAlignment(VAlign_Center);
	}

	UCanvasPanel* BarCanvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas_SpikeBar"));
	if (BarCanvas == nullptr) return;
	BarCanvas->SetVisibility(ESlateVisibility::HitTestInvisible);
	BarCanvas->bIsVariable = true;

	// 加进 Canvas 的顺序 = 绘制顺序：底板 → 槽 → 填充 → 中缝（中缝压在最上面，填满时也看得见）
	if (UBorder* Plate = MakeSolid(Tree, TEXT("Border_BarPlate"), SpikePlateColor))
	{
		PlaceLocal(BarCanvas, Plate, 0.f, 0.f, SpikeBarW, SpikeBarH);
	}
	if (UBorder* Track = MakeSolid(Tree, TEXT("Border_BarTrack"), SpikeTrackColor))
	{
		PlaceLocal(BarCanvas, Track, 0.f, SpikeTrackInsetTop, SpikeBarW, SpikeTrackH);
	}
	if (USizeBox* FillBox = MakeCell(Tree, TEXT("Size_SpikeFill"), 0.f, SpikeTrackH))
	{
		FillBox->SetWidthOverride(0.f);
		if (UBorder* Fill = MakeSolid(Tree, TEXT("Border_BarFill"), SpikeFillColor))
		{
			FillBox->AddChild(Fill);
		}
		// AutoSize：槽位不写死尺寸，宽度交给里层 SizeBox 的 WidthOverride
		PlaceLocal(BarCanvas, FillBox, 0.f, SpikeTrackInsetTop, 0.f, SpikeTrackH, /*bAutoSize=*/true);
	}
	if (UBorder* Divider = MakeSolid(Tree, TEXT("Border_BarDivider"), SpikeDividerColor))
	{
		PlaceLocalCenter(BarCanvas, Divider, SpikeDividerW, SpikeTrackH);
	}

	BarCell->AddChild(BarCanvas);
}

TSharedRef<SWidget> UValorantAnnouncement::RebuildWidget()
{
	// Initialize() 才是建 WidgetTree 的地方，Super::RebuildWidget() 只读 RootWidget —— 顺序不能反。
	Initialize();

	if (!bLayoutBuilt && WidgetTree)
	{
		if (WidgetTree->RootWidget == nullptr)
		{
			UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			if (Root)
			{
				WidgetTree->RootWidget = Root;
				BuildLayout(WidgetTree, Root);
			}
		}
		else if (Cast<UCanvasPanel>(WidgetTree->RootWidget) == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] WBP 的根不是 CanvasPanel（当前 %s）：直接用，不再建版面"),
				*WidgetTree->RootWidget->GetClass()->GetName());
		}

		bLayoutBuilt = true;
		ResolveWidgets();
	}

	return Super::RebuildWidget();
}

void UValorantAnnouncement::ResolveWidgets()
{
	// 代码建的树和烤进 WBP 的树用同一套名字 → GetWidgetFromName 两条路都通。
	auto FindText = [this](const TCHAR* Name) { return Cast<UTextBlock>(GetWidgetFromName(Name)); };

	PanelFill = Cast<UBorder>(GetWidgetFromName(TEXT("Border_PanelFill")));
	PanelImage = Cast<UImage>(GetWidgetFromName(TEXT("Panel_Back")));
	MainText = FindText(TEXT("Text_Main"));

	ExtraBox = Cast<UHorizontalBox>(GetWidgetFromName(TEXT("Box_Extra")));
	ExtraRoundText = FindText(TEXT("Text_ExtraRound"));
	ExtraSwapText = FindText(TEXT("Text_ExtraSwap"));

	HintBox = Cast<UHorizontalBox>(GetWidgetFromName(TEXT("Box_Hint")));
	HintPressText = FindText(TEXT("Text_HintPress"));
	InfoDisplayText = FindText(TEXT("Text_InfoDisplay"));

	SpikeBox = Cast<UVerticalBox>(GetWidgetFromName(TEXT("Box_Spike")));
	SpikeStatusText = FindText(TEXT("Text_SpikeStatus"));
	SpikeFillBox = Cast<USizeBox>(GetWidgetFromName(TEXT("Size_SpikeFill")));
	SpikeBarCell = Cast<USizeBox>(GetWidgetFromName(TEXT("Size_SpikeBar")));

	// ★ 继承来的五个数据源指针：**必须在这里赋值**，否则 PlayerController 写进来
	//   （BlasterHUD->Announcement->InfoText->SetText(...)）全是写到空指针上、一个字都不显示。
	AnnouncementText = FindText(TEXT("AnnouncementText"));
	WarmupTime = FindText(TEXT("WarmupTime"));
	InfoText = FindText(TEXT("InfoText"));
	RoundResultText = FindText(TEXT("RoundResultText"));
	TeamSwapText = FindText(TEXT("TeamSwapText"));

	if (AnnouncementText == nullptr || InfoText == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] 数据源没绑上（AnnouncementText=%s InfoText=%s）："
			"PlayerController 写的阶段名/提示不会显示"), AnnouncementText ? TEXT("ok") : TEXT("null"),
			InfoText ? TEXT("ok") : TEXT("null"));
	}
}

// ---------------------------------------------------------------- 文本 + 显隐

void UValorantAnnouncement::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bLayoutBuilt)
	{
		NormalizeAndLayout();
	}
}

void UValorantAnnouncement::SetPanelTone(EValorantAnnPanelTone Tone)
{
	// 只记下来，真正的换色交给 NormalizeAndLayout（和文本走同一条路，省得两处各写一遍显隐）
	RequestedTone = Tone;
}

// Auto 时按大字内容判：Valorant 结算那一下大字就是 "WON" / "LOST"。
EValorantAnnPanelTone UValorantAnnouncement::ResolveTone(EValorantAnnPanelTone Requested, const FString& MainValue)
{
	if (Requested != EValorantAnnPanelTone::Auto)
	{
		return Requested;
	}

	// MainValue 进来时已经 ToUpper 过了
	if (MainValue == TEXT("WON"))  { return EValorantAnnPanelTone::Won; }
	if (MainValue == TEXT("LOST")) { return EValorantAnnPanelTone::Lost; }
	return EValorantAnnPanelTone::Neutral;
}

void UValorantAnnouncement::NormalizeAndLayout()
{
	// ★★ 数据源 -> 显示块这一层是必须的（见 SpaceOut 的注释）：加工**不幂等**，不能原地回写。
	//   顺便也解决了"大字既是输入又是输出"的串味问题 —— 准备阶段的倒计时如果写回
	//   AnnouncementText，倒计时结束把 WarmupTime 清空以后那串数字会粘在屏幕上不走。
	auto Source = [](const UTextBlock* Text) -> FString
	{
		return Text ? Text->GetText().ToString() : FString();
	};

	auto ShowText = [](UTextBlock* Text, const FString& Value)
	{
		if (Text == nullptr) { return; }
		Text->SetText(FText::FromString(Value));
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	};
	auto HideText = [](UTextBlock* Text)
	{
		if (Text) { Text->SetVisibility(ESlateVisibility::Collapsed); }
	};

	// --- 中间大字：各阶段是阶段名（AnnouncementText），准备阶段是倒计时数字（WarmupTime）---
	const FString RawPhase = Source(AnnouncementText);
	const FString RawWarmup = Source(WarmupTime);
	const FString MainValue = !RawPhase.IsEmpty() ? RawPhase.ToUpper() : RawWarmup;
	const bool bHasMain = !MainValue.IsEmpty();
	if (bHasMain) { ShowText(MainText, MainValue); } else { HideText(MainText); }

	// --- 大字下面那行：回合结果 / 换边提示 ---
	const FString RawRound = Source(RoundResultText);
	const FString RawSwap = Source(TeamSwapText);
	if (RawRound.IsEmpty()) { HideText(ExtraRoundText); } else { ShowText(ExtraRoundText, Decorate(RawRound, true)); }
	if (RawSwap.IsEmpty())  { HideText(ExtraSwapText); }  else { ShowText(ExtraSwapText,  Decorate(RawSwap,  true)); }
	const bool bHasExtra = !RawRound.IsEmpty() || !RawSwap.IsEmpty();

	// --- 底部提示行："P R E S S" 和键帽是版面写死的，右边的后缀由 InfoText 决定 ---
	//     InfoText 一空（结算那两个阶段会清）整行一起收掉。
	const FString RawInfo = Source(InfoText);
	const bool bHasHint = !RawInfo.IsEmpty();
	if (bHasHint)
	{
		ShowText(HintPressText, Decorate(TEXT("Press"), true));
		ShowText(InfoDisplayText, Decorate(RawInfo, true));
	}
	else
	{
		HideText(HintPressText);
		HideText(InfoDisplayText);
	}

	// --- 整块显隐：一个字都没有就别在屏幕上留个空框 ---
	//     （widget 自己被 PlayerController 的 SetVisibility 管着，这里只管面板内部）
	// ★ 安包/拆包条和公告面板**占同一片位置**，所以二选一：安/拆/爆炸倒计时的时候公告整块让位。
	//   判据用 bShowSpike 而不是 bSpikeActive，理由见下面那条的注释。
	const bool bShowSpike = bSpikeActive || !SpikeStatus.IsEmpty();
	const bool bHasAny = (bHasMain || bHasExtra || bHasHint) && !bShowSpike;
	const ESlateVisibility PanelVis = bHasAny ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (PanelFill)  { PanelFill->SetVisibility(PanelVis); }
	if (PanelImage) { PanelImage->SetVisibility(PanelVis); }
	if (ExtraBox)   { ExtraBox->SetVisibility(bHasExtra && !bShowSpike ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	if (HintBox)    { HintBox->SetVisibility(bHasHint && !bShowSpike ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }

	// --- 底色三态：只有真的换了档才写 SetBrushColor（每帧写会让 Slate 每帧重画一次）---
	const EValorantAnnPanelTone Tone = ResolveTone(RequestedTone, MainValue);
	if (PanelFill && Tone != AppliedTone)
	{
		PanelFill->SetBrushColor(ToneToColor(Tone));
		AppliedTone = Tone;
	}

	// --- 安包/拆包/爆炸倒计时那条 ---
	// ★ 显隐判据是"有没有状态串"，不是 bSpikeActive：spike 爆炸后的倒计时
	//   （Spike.cpp::BroadcastExplodeCountdown）是 Status="00:45" 配 Progress=-1 推过来的，
	//   按 bSpikeActive 判的话那串字永远画不出来（明明 Spike.cpp 那头写着"只显示倒计时文字"）。
	if (SpikeBox) { SpikeBox->SetVisibility(bShowSpike ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	if (bShowSpike)
	{
		// 状态串是 Spike.cpp 推的英文（"Planting" / "Defusing"），和截图里那行一致 → 全大写就完事
		if (SpikeStatusText) { SpikeStatusText->SetText(FText::FromString(SpikeStatus.ToUpper())); }
		// 条只在真有人安/拆的时候出现；纯倒计时（Progress < 0）把那**一行**收掉，别留个空槽
		if (SpikeBarCell) { SpikeBarCell->SetVisibility(bSpikeActive ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
		if (SpikeFillBox) { SpikeFillBox->SetWidthOverride(FMath::Clamp(SpikeProgress, 0.f, 1.f) * SpikeBarW); }
	}
}

void UValorantAnnouncement::SetSpikeStatus(const FString& Status, float Progress)
{
	// 只存状态，显隐/宽度交给 NormalizeAndLayout（每帧跑一次，不差这一帧）
	bSpikeActive = (Progress >= 0.f) && !Status.IsEmpty();
	SpikeProgress = FMath::Max(0.f, Progress);
	SpikeStatus = Status;
}

void UValorantAnnouncement::OnRoundStarted()
{
	// ★★ 这里**不能** SetVisibility(Hidden)（基类就是这么写的，这个子类必须覆写）。
	//   "藏起来"不等于"看不见而已"：Hidden/Collapsed 的 widget 连 NativeTick 一起停，
	//   而 NormalizeAndLayout 是唯一会把 SpikeBox 放出来的地方 —— 整块一藏，常规阶段
	//   安包/拆包那条进度条就永远没机会显示（这就是"回合里没有进度条"那个 bug）。
	//   面板自己不显示靠 NormalizeAndLayout 的 bHasAny 收：把几个数据源清干净，
	//   下一帧它自己就 Collapsed —— 视觉上和以前"整块藏起来"一样。
	SetVisibility(ESlateVisibility::Visible);
	if (AnnouncementText) { AnnouncementText->SetText(FText()); }
	if (InfoText)         { InfoText->SetText(FText()); }
	// 准备阶段的大字倒计时也一起清：进 InProgress 之后 SetHUDAnnouncementCountdown 不再写它，
	// 留着就是个数秒钟不动的冻结数字（大字 = 阶段名，阶段名一空就回落到这个倒计时）。
	if (WarmupTime)       { WarmupTime->SetText(FText()); }
}

// ---------------------------------------------------------------- 烤图

bool UValorantAnnouncement::BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath)
{
#if WITH_EDITOR
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *WidgetBlueprintObjectPath);
	if (Asset == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] 烤图失败：加载不到 %s"), *WidgetBlueprintObjectPath);
		return false;
	}

	// 反射拿 WidgetTree（和另外两个 Valorant HUD 一个理由：直接 Cast<UWidgetBlueprint> 会让游戏模块
	// 多一个 UMGEditor 依赖）。只认属性名 → 零依赖。
	FObjectPropertyBase* TreeProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), FName(TEXT("WidgetTree")));
	if (TreeProperty == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] 烤图失败：%s 上没有 WidgetTree 属性（不是 WidgetBlueprint？）"), *WidgetBlueprintObjectPath);
		return false;
	}

	UWidgetTree* Tree = Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Asset));
	if (Tree == nullptr)
	{
		Tree = NewObject<UWidgetTree>(Asset, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);
		TreeProperty->SetObjectPropertyValue_InContainer(Asset, Tree);
	}

	// 重烤 = 以这张表为准：先摘掉旧版面（否则一层叠一层，同名控件 UMG 还会加 _1 后缀）
	if (Tree->RootWidget)
	{
		Tree->RemoveWidget(Tree->RootWidget);
		Tree->RootWidget = nullptr;
	}

	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	if (Root == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] 烤图失败：建不出 RootCanvas"));
		return false;
	}
	Tree->RootWidget = Root;
	BuildLayout(Tree, Root);

	Asset->MarkPackageDirty();
	UE_LOG(LogTemp, Log, TEXT("[ValorantAnnouncement] 版面已烤进 %s"), *WidgetBlueprintObjectPath);
	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] 烤图只能在编辑器里做"));
	return false;
#endif
}

// ---------------------------------------------------------------- 调试 dump

#if WITH_EDITOR
namespace
{
	// UWidget::Slot 的访问级别在引擎各版本里飘，走反射拿最稳
	UPanelSlot* GetWidgetSlot(UWidget* Widget)
	{
		static FObjectPropertyBase* SlotProperty =
			FindFProperty<FObjectPropertyBase>(UWidget::StaticClass(), FName(TEXT("Slot")));
		return (SlotProperty && Widget) ? Cast<UPanelSlot>(SlotProperty->GetObjectPropertyValue_InContainer(Widget)) : nullptr;
	}

	// ⚠ 日志里的格式串一律只用 %s / %d，宽度和左对齐自己在 FString 里拼 ——
	//   UE 的 FormatStringSan 对 "%-22s" 这种带 flag 的说明符会 static_assert 报 C2338。
	void DumpWidget(UWidget* Widget, int32 Depth)
	{
		if (Widget == nullptr) return;
		const FString Pad = FString::ChrN(Depth * 2, TEXT(' '));

		FString Extra;
		if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(GetWidgetSlot(Widget)))
		{
			// ★ LayoutData / bAutoSize 直接读会报 C4996（5.1 起弃用），走 getter
			// ★ 锚点非拉伸时 Offsets 的语义是 (偏移X, 偏移Y, 宽, 高) —— Right/Bottom 本身就是宽高，
			//   不是右/下边缘，别再拿它减 Left/Top。
			const FAnchorData Data = CanvasSlot->GetLayout();
			Extra = FString::Printf(TEXT("  canvas anchor=(%.2f,%.2f) align=(%.2f,%.2f) off=(%.0f,%.0f) size=(%.0f,%.0f) auto=%d"),
				Data.Anchors.Minimum.X, Data.Anchors.Minimum.Y, Data.Alignment.X, Data.Alignment.Y,
				Data.Offsets.Left, Data.Offsets.Top, Data.Offsets.Right, Data.Offsets.Bottom,
				CanvasSlot->GetAutoSize() ? 1 : 0);
		}
		else if (const UPanelSlot* Slot = GetWidgetSlot(Widget))
		{
			Extra = FString::Printf(TEXT("  slot=%s"), *Slot->GetClass()->GetName());
		}

		UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] %s"), *FString::Printf(
			TEXT("%s%s  [%s]  var=%d%s"), *Pad, *Widget->GetName(), *Widget->GetClass()->GetName(),
			Widget->bIsVariable ? 1 : 0, *Extra));

		if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			const FSlateFontInfo& Font = Text->GetFont();
			const FLinearColor Color = Text->GetColorAndOpacity().GetSpecifiedColor();
			UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] %s"), *FString::Printf(
				// ★ FSlateFontInfo::Size 是 float（SlateFontInfo.h:177），用 %d 读会打出 0
				TEXT("%s     font=%s size=%.0f color=(%.2f,%.2f,%.2f,%.2f)"),
				*Pad, *Font.TypefaceFontName.ToString(), Font.Size, Color.R, Color.G, Color.B, Color.A));

			const FWidgetTransform& Transform = Text->GetRenderTransform();
			if (!FMath::IsNearlyEqual(Transform.Scale.X, 1.f) || !FMath::IsNearlyEqual(Transform.Scale.Y, 1.f))
			{
				UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] %s"), *FString::Printf(
					TEXT("%s     * render scale=(%.2f,%.2f) pivot=(%.2f,%.2f)"),
					*Pad, Transform.Scale.X, Transform.Scale.Y,
					Text->GetRenderTransformPivot().X, Text->GetRenderTransformPivot().Y));
			}
		}
		else if (const UImage* Image = Cast<UImage>(Widget))
		{
			const UObject* Resource = Image->GetBrush().GetResourceObject();
			UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] %s"), *FString::Printf(
				TEXT("%s     texture=%s"), *Pad, Resource ? *Resource->GetPathName() : TEXT("<null>")));
		}
		else if (const UBorder* Border = Cast<UBorder>(Widget))
		{
			// 纯色块就看这一个值（三态底色 / 进度条那几块都是）。alpha 为 0 说明颜色没设进去 ——
			// 这种错不看不出来：控件在树上、尺寸也对，就是全透明。
			const FLinearColor Tint = Border->GetBrushColor();
			UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] %s"), *FString::Printf(
				TEXT("%s     brush tint=(%.3f,%.3f,%.3f,%.3f)"), *Pad, Tint.R, Tint.G, Tint.B, Tint.A));
		}

		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				DumpWidget(Panel->GetChildAt(Index), Depth + 1);
			}
		}
	}
}
#endif

bool UValorantAnnouncement::DumpLayout(const FString& WidgetBlueprintObjectPath)
{
#if WITH_EDITOR
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *WidgetBlueprintObjectPath);
	if (Asset == nullptr) { UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] dump 失败：加载不到 %s"), *WidgetBlueprintObjectPath); return false; }

	FObjectPropertyBase* TreeProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), FName(TEXT("WidgetTree")));
	UWidgetTree* Tree = TreeProperty ? Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Asset)) : nullptr;
	if (Tree == nullptr || Tree->RootWidget == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantAnnouncement] dump 失败：%s 里没有控件树"), *WidgetBlueprintObjectPath);
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("[ValorantAnnouncement] ===== dump %s ====="), *WidgetBlueprintObjectPath);
	DumpWidget(Tree->RootWidget, 0);
	return true;
#else
	return false;
#endif
}
