// Valorant 底部 HUD 实现。三块：版面表 + 数值刷新 + 烤图工具。
// 版面坐标全部照抄 E:\Notion\claude-temp\hud\README.txt 的坐标表（源图 2559x1439 的像素框）。

#include "ValorantBottomHUD.h"

#include "AbilitySystemComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameplayAbilitySpec.h"
#include "Styling/CoreStyle.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Weapon/MeleeWeapon.h"
#include "Blaster/Weapon/Weapon.h"

namespace
{
	// 素材目录。导入时统一加了 T_HUD_ 前缀（资产名不能以数字开头），其余部分就是抠图时的原名。
	const TCHAR* const HUDFolder = TEXT("/Game/Assets/Textures/HUD/Jett/");

	// 贴图加载。不用担心 GC：贴图被 UImage 的 Brush（FSlateBrush::ResourceObject，是个 UPROPERTY）
	// 引用着，控件活着贴图就活着。
	UTexture2D* LoadHUDTexture(const TCHAR* AssetName)
	{
		const FString Path = FString(HUDFolder) + AssetName + TEXT(".") + AssetName;
		return LoadObject<UTexture2D>(nullptr, *Path);
	}

	// ★ 文本的字体必须挂在**字体资产**（UFont）上，不能用 FCoreStyle::GetDefaultFontStyle()。
	//   后者是从 Slate 的样式集里取的 FSlateFontInfo，它的字体是个 FCompositeFont（运行时对象，
	//   TSharedPtr）—— FSlateFontInfo 只存得住这种字体的弱引用，**序列化不进资产**。
	//   于是烤进 WBP 的那些 TextBlock 存盘后字体就丢了（重新打开资产是默认字体，
	//   代码路径每次重建反而"看着正常"，两条路不一致）。用引擎自带的 Roboto 资产就没这问题：
	//   FSlateFontInfo(const UObject*, ...) 会把 UFont 记成可序列化的引用。
	UFont* HUDFont()
	{
		static TWeakObjectPtr<UFont> Cached;
		if (!Cached.IsValid())
		{
			Cached = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
		}
		return Cached.Get();
	}

	// 锚点：坐标表里的 X 是"源图左边"的像素，贴中/贴右要分别减掉半个/一整个设计宽。
	enum class EAnchor : uint8 { Left, Center, Right };

	// 版面的一行：控件名 + 贴图 + 源图 1x 像素框 + 锚点 + 基础透明度。
	// 控件名同时是烤进 WBP 之后的控件名和运行时 GetWidgetFromName 查的名字 —— 两边必须一致。
	struct FLayoutPiece
	{
		const TCHAR* Name;
		const TCHAR* Texture;
		float X, Y, W, H;
		EAnchor Anchor;
		float Alpha;        // 1 = 原样；血线底下那条用 42%（素材里 _gray 那版就是这个值）
	};

	// —— 会动的那几件，尺寸单独起名字：刷新时要按比例改 slot 尺寸 ——
	constexpr float HealthLineX = 714.f;
	constexpr float HealthLineY = 1392.f;
	constexpr float HealthLineW = 167.f;
	constexpr float HealthLineH = 17.f;

	// 大招充能点：7 格。步长 10.6 是从 F_pips_X_strip_white.png 上量出来的（不是均分 ——
	// 整条 74 宽 / 7 格 ≈ 10.6）。README 坐标表里给的是重新定框后的整条框，格子得自己排。
	// ★ PipX/PipY 是**用户在 UMG 设计器里拖过的值**（原来 1440/1372，右移 27.3、下移 2.1）：
	//   拖完是"整排与 X 图标（Icon_X 1479.6 宽 49 → 中心 1504.1）左右居中"，
	//   PipX = 1504.1 - (6*10.6+10)/2 = 1467.3。别再按 README 表挪回去。
	constexpr int32 NumUltPips = 7;
	constexpr float PipX = 1467.4f;
	constexpr float PipY = 1374.1f;
	constexpr float PipW = 10.f;
	constexpr float PipH = 12.f;
	// 10.65 = 手调后 Pip_1→Pip_7 实测差 8.0 画布单位 / LayoutScale（原本按素材量的是 10.6，
	// 差 0.05 画布/格：7 格累计 0.4 —— 说得过去但没必要留着）。
	constexpr float PipPitch = 10.65f;

	// 充能条的颜色（源图实测色，见 README「大招充能（语义）」）：有充能 = 青蓝 62,240,205。
	// （没充能那版的灰 118,118,126 现在用不上了：空格子直接不画 —— 见 ApplySkillSlot。）
	const FLinearColor ChargeColor(62.f / 255.f, 240.f / 255.f, 205.f / 255.f, 1.f);

	// 充能条素材有**两种格数**，格数是画在贴图里的（101x18，扫一遍像素列就知道）：
	//   T_HUD_6_barfill_C_white / _Q_white —— 中间有一段空列（C 在 x=49..52、Q 在 x=48..51）→ **2 格**
	//   T_HUD_6_barfill_E_white            —— 一条通到底、没有断口 → **1 格**
	// 这是照着 Jett 画的（逐风云/腾空各 2 发、逐风冲刺 1 发），所以"按槽位字母取图"一直没露馅。
	// ★ 换英雄就错：火男的闪光 2 发落在 E 槽、火球 1 发落在 Q 槽 —— 选图那段的说明见 ApplySkillSlot。
	constexpr const TCHAR* BarTextureOneCell = TEXT("T_HUD_6_barfill_E_white");
	constexpr const TCHAR* BarTextureTwoCell = TEXT("T_HUD_6_barfill_C_white");

	// 两种素材里最多的格数。只当 MaxCharges 的上限用：技能的充能层数超过它时，多出来的层只能整条画满。
	constexpr int32 MaxBarCells = 2;

	// 没充能时图标压到的透明度 = 素材里 _gray 那版（42%），和 USkillBarWidget 用的是同一个值。
	constexpr float DimmedAlpha = 0.42f;

	// ★ 设计框 → UMG 画布的换算系数。
	//   版面表里的坐标是**参考截图**（2560x1440）框里的像素，而 UMG 画布的 slate 尺寸不由它决定 ——
	//   由工程的 DPI 曲线决定，本工程没改过这条曲线（DefaultEngine.ini 里没有 UserInterfaceSettings 的
	//   UIScaleRule，用的是引擎默认：scale = 最短边 / 1080），于是画布的 slate 尺寸**恒等于 1920x1080**。
	//   也就是说偏离 1.33 倍：贴左/贴右的件按"设计像素"偏移（偏大），贴中间的件按视口中心偏移（不偏），
	//   两组在屏幕上就对不上了 —— 实测 1600x900 下血线量到 (595..734)、C 技能框 (555..663)，直接压在
	//   一起（这就是"整个底栏糊成一坨"）。所以入槽之前一律乘这个系数，把版面表换算到 1080p 基准帧。
	//   换算完 1 设计像素 = 1080/1439 画布单位 ≈ 0.75，和 1920x1080 基准帧的比例完全一致。
	//   ⚠ 要是哪天在工程设置里改了 UIScaleRule / DesignScreenSize，这个系数得跟着改。
	constexpr float LayoutScale = 1080.f / UValorantBottomHUD::DesignHeight;

	// ★ 版面表。顺序 = 绘制顺序（后面的盖在前面上）。
	//   要微调位置改这里（或者烤进 WBP 之后在设计器里拖 —— 那时以 WBP 为准）。
	const FLayoutPiece GLayoutPieces[] =
	{
		// —— 左：血 / 甲。护甲六边形里的 "50" 抠图时被挖掉了（README 已知问题 2），数字用文本画 ——
		{ TEXT("Hex_Armor"),        TEXT("T_HUD_9_armor_hex_white"),     714.f, 1342.f,  49.f, 55.f, EAnchor::Left, 1.f },
		// 血线两层：底（整条 42% 透明）+ 面（按当前血量缩短）。面画在底上面，顺序不能反。
		{ TEXT("Line_HealthTrack"), TEXT("T_HUD_A_line_health_white"), HealthLineX, HealthLineY, HealthLineW, HealthLineH, EAnchor::Left, DimmedAlpha },
		{ TEXT("Line_HealthFill"),  TEXT("T_HUD_A_line_health_white"), HealthLineX, HealthLineY, HealthLineW, HealthLineH, EAnchor::Left, 1.f },

		// —— 中：技能条两侧的飞线（纯装饰，不动）——
		{ TEXT("Swoosh_Left"),      TEXT("T_HUD_B_swoosh_left_white"),   861.f, 1330.f, 134.f, 78.f, EAnchor::Center, 1.f },
		{ TEXT("Swoosh_Right"),     TEXT("T_HUD_E_swoosh_right_white"), 1572.f, 1330.f, 114.f, 68.f, EAnchor::Center, 1.f },

		// —— 中：C（逐风云） / Q（腾空） / E（逐风冲刺）——
		// 每个技能 5 件：框 + 充能条 + 图标 + 键位字母 + "可用"小横杠。
		{ TEXT("Frame_C"), TEXT("T_HUD_5_frame_C_white"),  986.f, 1378.f, 130.f, 38.f, EAnchor::Center, 1.f },
		{ TEXT("Bar_C"),   TEXT("T_HUD_6_barfill_C_white"),1002.f, 1385.f, 101.f, 18.f, EAnchor::Center, 1.f },
		{ TEXT("Icon_C"),  TEXT("T_HUD_1_icon_C_cloudburst_white"), 1028.f, 1309.f, 51.f, 60.f, EAnchor::Center, 1.f },
		{ TEXT("Key_C"),   TEXT("T_HUD_7_key_C_white"),    1048.f, 1406.f,   9.f, 23.f, EAnchor::Center, 1.f },
		{ TEXT("Dash_C"),  TEXT("T_HUD_8_dash_1_white"),   1113.f, 1336.f,  30.f,  6.f, EAnchor::Center, 1.f },

		{ TEXT("Frame_Q"), TEXT("T_HUD_5_frame_Q_white"), 1140.f, 1384.f, 127.f, 28.f, EAnchor::Center, 1.f },
		{ TEXT("Bar_Q"),   TEXT("T_HUD_6_barfill_Q_white"),1153.f, 1385.f, 101.f, 18.f, EAnchor::Center, 1.f },
		{ TEXT("Icon_Q"),  TEXT("T_HUD_2_icon_Q_updraft_white"), 1179.f, 1310.f, 49.f, 58.f, EAnchor::Center, 1.f },
		{ TEXT("Key_Q"),   TEXT("T_HUD_7_key_Q_white"),   1197.f, 1406.f,  12.f, 24.f, EAnchor::Center, 1.f },
		{ TEXT("Dash_Q"),  TEXT("T_HUD_8_dash_2_white"),  1264.f, 1337.f,  30.f,  5.f, EAnchor::Center, 1.f },

		{ TEXT("Frame_E"), TEXT("T_HUD_5_frame_E_white"), 1290.f, 1378.f, 128.f, 34.f, EAnchor::Center, 1.f },
		{ TEXT("Bar_E"),   TEXT("T_HUD_6_barfill_E_white"),1303.f, 1385.f, 101.f, 18.f, EAnchor::Center, 1.f },
		{ TEXT("Icon_E"),  TEXT("T_HUD_3_icon_E_tailwind_white"), 1324.f, 1312.f, 62.f, 52.f, EAnchor::Center, 1.f },
		{ TEXT("Key_E"),   TEXT("T_HUD_7_key_E_white"),   1349.f, 1406.f,   9.f, 23.f, EAnchor::Center, 1.f },
		{ TEXT("Dash_E"),  TEXT("T_HUD_8_dash_3_white"),  1414.f, 1336.f,  30.f,  6.f, EAnchor::Center, 1.f },

		// —— 中：大招（X）：图标 + 框 + 键位；7 格充能点在 BuildLayout 里单独排 ——
		{ TEXT("Frame_X"), TEXT("T_HUD_5_frame_X_white"), 1440.f, 1386.f, 129.f, 30.f, EAnchor::Center, 1.f },
		{ TEXT("Icon_X"),  TEXT("T_HUD_4_icon_X_bladestorm_white"), 1480.f, 1310.f, 49.f, 60.f, EAnchor::Center, 1.f },
		{ TEXT("Key_X"),   TEXT("T_HUD_7_key_X_white"),   1499.f, 1406.f,  10.f, 23.f, EAnchor::Center, 1.f },

		// —— 右：弹药（线和菱形是纯装饰，数字是文本）——
		{ TEXT("Line_Ammo"), TEXT("T_HUD_C_line_ammo_white"), 1694.f, 1401.f, 152.f,  8.f, EAnchor::Right, 1.f },
		{ TEXT("Icon_Ammo"), TEXT("T_HUD_D_ammo_pips_white"), 1775.f, 1355.f,  25.f, 25.f, EAnchor::Right, 1.f },
	};

	// 把控件摆到 Canvas 上：坐标表给的是"源图里的绝对像素框"，这里换算成"贴某条边的偏移"。
	// ★ (X, Y) 是框的**左上角**（照抄 README 表的 x0/y0），y 向下增长。
	//   而 Canvas 这头用 Alignment=(0,1)，锚点落在控件的**左下角** —— 所以纵向要把高度补回来
	//   （Y + H）。这里曾经漏补过一次，表现是所有件都往上飘了自身高度那么多。
	// 横向让控件左边缘对齐 X：血量线缩短时左端不动、右端往左退，和游戏里一致。
	// ★ 入槽前一律乘 LayoutScale（设计框 → 1080p 画布帧），包括锚点基准宽/高 —— 见 LayoutScale 的注释。
	void PlaceOnCanvas(UCanvasPanel* Root, UWidget* Widget, float X, float Y, float W, float H, EAnchor Anchor)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;

		X *= LayoutScale;
		Y *= LayoutScale;
		W *= LayoutScale;
		H *= LayoutScale;

		// 画布帧的宽/高（换算之后的）：贴中/贴右的偏移量按它算，贴左的就是 0
		const float FrameWidth = UValorantBottomHUD::DesignWidth * LayoutScale;
		const float FrameHeight = UValorantBottomHUD::DesignHeight * LayoutScale;

		float AnchorX = 0.f;
		float AnchorFraction = 0.f;
		if (Anchor == EAnchor::Center) { AnchorX = FrameWidth * 0.5f; AnchorFraction = 0.5f; }
		else if (Anchor == EAnchor::Right) { AnchorX = FrameWidth; AnchorFraction = 1.f; }

		CanvasSlot->SetAnchors(FAnchors(AnchorFraction, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetPosition(FVector2D(X - AnchorX, (Y + H) - FrameHeight));
		CanvasSlot->SetSize(FVector2D(W, H));
		CanvasSlot->SetAutoSize(false);
		CanvasSlot->SetZOrder(0);
	}

	// 建一个 Image。贴图没加载出来也不跳过 —— 控件留着（之后能补上），版面也不会缺一块。
	UImage* MakeImage(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name, const TCHAR* TextureName,
		float X, float Y, float W, float H, EAnchor Anchor, float Alpha)
	{
		UImage* Image = Tree->ConstructWidget<UImage>(UImage::StaticClass(), FName(Name));
		if (Image == nullptr) return nullptr;

		if (UTexture2D* Texture = LoadHUDTexture(TextureName))
		{
			// bMatchSize = false：尺寸由 slot 说了算（贴图是 4x 的，缩到 1x 显示）
			Image->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
		}
		Image->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, Alpha));
		Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		// 勾上"是变量"：烤进 WBP 之后能在设计器/蓝图里按名字引用（UMG 的 Is Variable 就是这个标志）
		Image->bIsVariable = true;
		PlaceOnCanvas(Root, Image, X, Y, W, H, Anchor);
		return Image;
	}

	// 建一个文本。数字在素材里没抠 —— README 已知问题 3：数字留到 UE 里画文本更好。
	UTextBlock* MakeText(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name,
		float X, float Y, float W, float H, EAnchor Anchor, ETextJustify::Type Justify, int32 FontSize)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
		if (Text == nullptr) return nullptr;

		// 字号也按设计框给的（在 1440p 里量出来的 px）→ 换算到画布帧，不然字比框大一圈。
		// 字体走 UFont 资产 + Bold 字型（原因见 HUDFont()：样式集字体存不进 WBP）。
		const int32 FontPx = FMath::Max(1, FMath::RoundToInt(FontSize * LayoutScale));
		UFont* const Font = HUDFont();
		Text->SetFont(Font != nullptr
			? FSlateFontInfo(Font, (float)FontPx, FName(TEXT("Bold")))
			: FCoreStyle::GetDefaultFontStyle(FName(TEXT("Bold")), FontPx));
		Text->SetJustification(Justify);
		Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		Text->bIsVariable = true;
		PlaceOnCanvas(Root, Text, X, Y, W, H, Anchor);
		return Text;
	}

	// 一个技能槽的显示数据（存活读本地 ASC / 观战读服务器快照，两个来源都先归到这一份）
	struct FSkillSlotData
	{
		// 技能条槽位（0=C、1=Q、2=E）—— 格子按它认，不按 SkillType
		int32 SlotIndex = INDEX_NONE;
		EBlasterSkillType SkillType = EBlasterSkillType::Custom;
		UTexture2D* Icon = nullptr;
		int32 Charges = 0;
		int32 MaxCharges = 0;
	};

	// 把一份技能数据刷到槽上。Data == nullptr = 这个格子没有技能（换了英雄 / 该位是空的）→ 整组收掉。
	void ApplySkillSlot(const FValorantSkillSlotWidgets& Widgets, const FSkillSlotData* Data)
	{
		if (Data == nullptr)
		{
			Widgets.SetVisible(false);
			return;
		}
		Widgets.SetVisible(true);

		// 图标贴图按当前英雄的技能换。不换的话显示的就是 WBP 里烤进去那张
		//（版面表里 C/Q/E 三格烤的是 Jett 的逐风云/腾空/逐风冲刺）—— 玩火男时三格画的全是 Jett 的技能，
		// 这才是"技能栏没配上"的真身。这函数每帧都跑，所以只在贴图真的不一样时才动 brush（换一次会标脏重绘）。
		if (Widgets.Icon && Widgets.Icon->GetBrush().GetResourceObject() != Data->Icon)
		{
			if (Data->Icon)
			{
				// bMatchSize=false：保持版面里那格的尺寸，只换贴图（缩放在 UMG 里手调过，别覆盖）
				Widgets.Icon->SetBrushFromTexture(Data->Icon, /*bMatchSize=*/false);
			}
			else
			{
				// 技能没配图标（老技能可以留空）→ 宁可空着，也不要顶着上一个英雄的图
				Widgets.Icon->SetVisibility(ESlateVisibility::Collapsed);
			}
		}

		const bool bHasCharge = Data->Charges > 0;

		// 图标没充能压到 42%（Valorant 里"用掉的技能是灰的"）；键位字母不压，要认得出按哪个键
		if (Widgets.Icon) Widgets.Icon->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, bHasCharge ? 1.f : DimmedAlpha));

		// 充能条：**按格**显示，不是进度条。
		//   素材这条本身就是分格的（T_HUD_6_barfill_* 中间有格子缝），所以不能只靠改宽度来"缩短" ——
		//   那是把整条压扁：两格用掉一格会变成一条被压成一半的窄条，看着像进度条，语义也不对
		//   （要的是"其中一格不可视"）。做法是槽宽和 UV 一起缩：宽度 ×(剩几格/总格数)，
		//   同时 UV 只取贴图左边那几格 → 每格还是原来的大小和间距，用掉的那几格根本没被采样
		//   （不是画成灰的，是真的没画）。
		if (Widgets.BarFill && Widgets.BarFullWidth > 0.f)
		{
			const int32 MaxCells = FMath::Clamp(Data->MaxCharges, 1, MaxBarCells);
			const int32 Cells = FMath::Clamp(Data->Charges, 0, MaxCells);

			if (Cells <= 0)
			{
				// 一格都没有 → 整条不画
				Widgets.BarFill->SetVisibility(ESlateVisibility::Collapsed);
			}
			else
			{
				/*
				 * ★ 贴图按**技能自己的充能层数**选，不能按槽位字母取（版面表给 Bar_C/Bar_Q/Bar_E 各配了一张）。
				 *   格数是画在贴图里的，而三张图照的是 **Jett 的槽位**：C/Q 两张 2 格、E 那张只有 1 格。
				 *   火男的配置正好和这个错位 —— 闪光 2 发落在 E 槽（图只 1 格：满充能看着像"只有 1 发"，
				 *   用掉一发还被缩成半条），火球 1 发落在 Q 槽（图有 2 格：满充能看着像"还有 2 发"）。
				 *   两边都是 UI 上的数量不对，而技能数据本身是对的，所以只换图。
				 *   两张图都是 101x18、外形同一条（只有缝的位置差 1px），换来换去版面/尺寸都不动。
				 *   只在真的不一样时才 SetBrushFromTexture —— 它标脏重绘，每帧设一次会白刷。
				 */
				const TCHAR* BarTextureName = (MaxCells <= 1) ? BarTextureOneCell : BarTextureTwoCell;
				if (UTexture2D* BarTexture = LoadHUDTexture(BarTextureName))
				{
					if (Widgets.BarFill->GetBrush().GetResourceObject() != BarTexture)
					{
						Widgets.BarFill->SetBrushFromTexture(BarTexture, /*bMatchSize=*/false);
					}
				}

				const float Fraction = (float)Cells / (float)MaxCells;
				Widgets.BarFill->SetVisibility(ESlateVisibility::HitTestInvisible);
				if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widgets.BarFill->Slot))
				{
					Slot->SetSize(FVector2D(Widgets.BarFullWidth * Fraction, Slot->GetSize().Y));
				}
				// UVRegion 是归一化纹理坐标里的子矩形：取左边 Fraction 那一段。
				// 槽宽和 UV 宽同比例缩，格子的像素密度就不变（这正是不做 UV、硬缩宽度的坏处）。
				FSlateBrush Brush = Widgets.BarFill->GetBrush();
				Brush.SetUVRegion(FBox2f(FVector2f(0.f, 0.f), FVector2f(Fraction, 1.f)));
				Widgets.BarFill->SetBrush(Brush);
				Widgets.BarFill->SetColorAndOpacity(ChargeColor);
			}
		}

		// 框角那根小横杠 = "这个技能现在放得出来"。这个判读是从源图几何推的：三根横杠分别在
		// C/Q/E 框右端上方、一件一个 —— 所以它只能表达有/没有，表达不了层数。
		if (Widgets.Dash)
		{
			Widgets.Dash->SetVisibility(bHasCharge ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		}
	}
}

// ---------------------------------------------------------------- 版面

void FValorantSkillSlotWidgets::SetVisible(bool bVisible) const
{
	const ESlateVisibility Visibility = bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (Frame)  Frame->SetVisibility(Visibility);
	if (Icon)   Icon->SetVisibility(Visibility);
	if (BarFill) BarFill->SetVisibility(Visibility);
	if (Key)    Key->SetVisibility(Visibility);
	// Dash 不在这里设：它表达的是"这个技能现在放得出来"，由 ApplySkillSlot 单独控制
}

void UValorantBottomHUD::BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root)
{
	if (Tree == nullptr || Root == nullptr) return;

	// 1) 底板上的全部贴图件（含会动的血线两层、充能条、技能图标、大招图标）
	for (const FLayoutPiece& Piece : GLayoutPieces)
	{
		MakeImage(Tree, Root, Piece.Name, Piece.Texture, Piece.X, Piece.Y, Piece.W, Piece.H, Piece.Anchor, Piece.Alpha);
	}

	// 2) 大招充能点：7 格单格（步长 10.6），点亮/未点亮两张图用同一套坐标
	for (int32 i = 0; i < NumUltPips; ++i)
	{
		const FString Name = FString::Printf(TEXT("Pip_%d"), i + 1);
		MakeImage(Tree, Root, *Name, TEXT("T_HUD_F_pips_X_single_empty"),
			PipX + PipPitch * i, PipY, PipW, PipH, EAnchor::Center, 1.f);
	}

	// 3) 数字（素材里没抠，用文本画）。★ 前三条的坐标是**用户在 UMG 设计器里拖过的值**
	//    （2026-09-19 已同步进这张表，别再按几何推回去）：
	//    血：血线上方（769,1362 → 772.8,1352.3，往右上挪了一点）；
	//    甲：六边形中间那个洞（y 1342 → 1353.3，往下挪了一点）；
	//    弹：挪到弹号线**左端**、菱形图标左边（x 1796 → 1713.6，往左 81.7）——
	//        原来记的"从 1796 起、别压上图标（1774~1799）"已作废；
	//    大招倒计时没动：X 框（底 1416）和屏幕底（1439）之间那条缝。
	MakeText(Tree, Root, TEXT("Text_Health"),   772.8f, 1352.3f, 110.f, 28.f, EAnchor::Left,   ETextJustify::Left,   26);
	MakeText(Tree, Root, TEXT("Text_Armor"),    714.0f, 1353.3f,  49.f, 55.f, EAnchor::Left,   ETextJustify::Center, 18);
	MakeText(Tree, Root, TEXT("Text_Ammo"),    1714.4f, 1346.9f,  50.f, 34.f, EAnchor::Right,  ETextJustify::Right,  26);
	MakeText(Tree, Root, TEXT("Text_UltTimer"),1464.f, 1416.f,  80.f, 18.f, EAnchor::Center, ETextJustify::Center, 13);
}

TSharedRef<SWidget> UValorantBottomHUD::RebuildWidget()
{
	// Initialize() 才是建 WidgetTree 的地方（WBP 的树就是在那一步复制进来的），而
	// Super::RebuildWidget() 只读 WidgetTree->RootWidget —— 所以必须先把 Initialize 跑掉，
	// 再建版面，最后才交给 Super。只建一次：RebuildWidget 会被调多次（改 DPI、重新 TakeWidget）。
	Initialize();

	if (!bLayoutBuilt && WidgetTree)
	{
		// 走了 WBP（烤过版面）：树里已经有根了 → 用 WBP 那份，代码只抓控件不重建。
		// 根是空的两种情况：纯 C++ 类（从没建过树），或 WBP 生成类的 archetype 是旧版（空树）
		// —— 两种都现建一份一样的版面，所以哪怕烤图没进生成类，画出来也是对的。
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
			// 有人在 WBP 里把根换成了别的面板：别动它（硬塞会把他改的东西丢掉），
			// 控件还是按名字全树找 —— 找得到就照常更新，找不到就是静态版面。
			UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] WBP 的根不是 CanvasPanel（当前 %s）：直接用，不再建版面"),
				*WidgetTree->RootWidget->GetClass()->GetName());
		}

		bLayoutBuilt = true;
		ResolveWidgets();
	}

	return Super::RebuildWidget();
}

void UValorantBottomHUD::ResolveWidgets()
{
	// 按名字抓控件：代码建的树和烤进 WBP 的树用同一套名字，所以这里不用 BindWidget ——
	// 纯 C++ 类（没挂 WBP）根本不会跑 BindWidget 那套，只有 GetWidgetFromName 两条路都通。
	auto FindImage = [this](const FName& Name) { return Cast<UImage>(GetWidgetFromName(Name)); };
	auto FindText = [this](const FName& Name) { return Cast<UTextBlock>(GetWidgetFromName(Name)); };

	HexArmor = FindImage(TEXT("Hex_Armor"));
	HealthTrack = FindImage(TEXT("Line_HealthTrack"));
	HealthFill = FindImage(TEXT("Line_HealthFill"));
	ArmorText = FindText(TEXT("Text_Armor"));
	HealthText = FindText(TEXT("Text_Health"));

	AmmoLine = FindImage(TEXT("Line_Ammo"));
	AmmoIcon = FindImage(TEXT("Icon_Ammo"));
	AmmoText = FindText(TEXT("Text_Ammo"));

	FrameX = FindImage(TEXT("Frame_X"));
	IconX = FindImage(TEXT("Icon_X"));
	UltTimerText = FindText(TEXT("Text_UltTimer"));
	PipLitTexture = LoadHUDTexture(TEXT("T_HUD_F_pips_X_single_white"));
	PipEmptyTexture = LoadHUDTexture(TEXT("T_HUD_F_pips_X_single_empty"));

	UltPips.Reset();
	for (int32 i = 0; i < NumUltPips; ++i)
	{
		UltPips.Add(FindImage(*FString::Printf(TEXT("Pip_%d"), i + 1)));
	}

	// 三个普通技能槽
	struct FSlotResolve { const TCHAR* Letter; FValorantSkillSlotWidgets* Widgets; };
	const FSlotResolve SlotResolve[] =
	{
		{ TEXT("C"), &SkillSlotC },
		{ TEXT("Q"), &SkillSlotQ },
		{ TEXT("E"), &SkillSlotE },
	};
	for (const FSlotResolve& Entry : SlotResolve)
	{
		FValorantSkillSlotWidgets& W = *Entry.Widgets;
		W.Frame = FindImage(*FString::Printf(TEXT("Frame_%s"), Entry.Letter));
		W.Icon = FindImage(*FString::Printf(TEXT("Icon_%s"), Entry.Letter));
		W.BarFill = FindImage(*FString::Printf(TEXT("Bar_%s"), Entry.Letter));
		W.Key = FindImage(*FString::Printf(TEXT("Key_%s"), Entry.Letter));
		W.Dash = FindImage(*FString::Printf(TEXT("Dash_%s"), Entry.Letter));

		// 充能条的整条宽度要记下来（刷新时按剩几格缩）。这里读到的就是版面原样尺寸 ——
		// 运行时这一趟紧跟在建树后面，烤图那一趟就是 WBP 里保存的尺寸（在设计器里改过也认）。
		// ★ 取 max、不清零：刷新会把 slot 改窄，要是哪次又走一趟 ResolveWidgets（重建树、
		//   重新 TakeWidget 都会），读回来的就是"当下剩下的那点宽"，再乘比例就一路越缩越短。
		if (W.BarFill)
		{
			if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(W.BarFill->Slot))
			{
				W.BarFullWidth = FMath::Max(W.BarFullWidth, (float)CanvasSlot->GetSize().X);
			}
		}
	}
}

FValorantSkillSlotWidgets* UValorantBottomHUD::FindSlotWidgets(int32 SkillSlotIndex)
{
	// ★ 认**槽位号**不认技能类型。原来这里是 switch (SkillType)，只列了 Jett 的三个
	//   （Cloudburst/Updraft/Dash）—— 火男是 WALL/FIREBALL/FLASH、贤者是 WALL/CUSTOM/HEAL，
	//   一个都匹配不上，于是那三个格子**整块不刷**，玩家看到的一直是 WBP 里烤死的 Jett 图标。
	//   槽位号是跨英雄统一的（见 BlasterGameplayAbility.h 的 SkillSlotIndex：0=C、1=Q、2=E），
	//   四个英雄的 GA 资产本来也都按 0/1/2 配好了，按它认就都对了。
	switch (SkillSlotIndex)
	{
	case 0:  return &SkillSlotC;
	case 1:  return &SkillSlotQ;
	case 2:  return &SkillSlotE;
	default: return nullptr;   // INDEX_NONE(-1) 等：大招不进这三格
	}
}

// ---------------------------------------------------------------- 数值

void UValorantBottomHUD::SetHUDVisible(bool bVisible)
{
	// ★ 收的是里面的根画布，**不是 widget 自己**。
	//   把 UserWidget 自己设成 Collapsed 会让 Slate 连 Tick/Paint 一起停掉（Collapsed 的子树不参与
	//   布局、tick、paint），而"这块 HUD 该不该显示"恰恰是在它自己的 NativeTick → UpdateDisplay 里判的
	//   —— 一旦收掉，就再也没有 Tick 来把它放出来，永久不可见。
	//   实测症状：开局头两帧玩家还没 spawn（DisplayChar == nullptr）→ 收起 → 此后整局底栏都是空的，
	//   设计器里却一切正常。所以只收里面的内容，SObjectWidget 那头一直留着（它也就能一直 tick）。
	UWidget* Root = WidgetTree ? WidgetTree->RootWidget : nullptr;
	if (Root == nullptr)
	{
		// 没树（理论上不会走到：Initialize 一定会建一棵）——退回老写法，至少别崩
		SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		return;
	}
	Root->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void UValorantBottomHUD::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 数值全在变（血量/充能/大招点数），每帧刷一次。和 USkillBarWidget 每帧重绘是同一个量级，
	// 都是一堆属性读 + SetText/SetVisibility，不做布局重建。
	UpdateDisplay();
}

void UValorantBottomHUD::UpdateDisplay()
{
	APlayerController* PC = GetOwningPlayer();
	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(PC);

	// Lobby（选人）地图整块不显示。**这里就是唯一那一处** —— BlasterHUD 那边不碰底栏
	//（它只收小地图和技能条，见 ABlasterHUD::SetGameplayWidgetsCollapsed 里的说明）。
	// 底栏要按阵亡/观战/有没有显示目标动态开关，交给 BlasterHUD 一起设会互相盖。
	//（widget 跨地图常驻，只懒创建一次 —— 所以每次刷都要重新判一次地图。）
	if (!PC || !PC->IsLocalController() || (BPC && BPC->IsInLobby()))
	{
		SetHUDVisible(false);
		return;
	}

	// 显示谁：观战（阵亡后看队友）→ 被观察的存活队友；存活自己 → 本地角色。
	// 阵亡但还没进观战 → 收掉（不收会在屏幕上留一块"自己尸体"的 HUD）。
	const bool bSpectating = BPC && BPC->IsSpectating();
	ABlasterCharacter* DisplayChar = bSpectating
		? BPC->GetSpectateTargetCharacter()
		: (PC->GetPawn() ? Cast<ABlasterCharacter>(PC->GetPawn()) : nullptr);
	if (DisplayChar == nullptr || DisplayChar->IsElimmed())
	{
		SetHUDVisible(false);
		return;
	}
	SetHUDVisible(true);

	// —— 血量与护甲（角色的复制属性，观战看队友也是同一份）——
	const float Health = DisplayChar->GetHealth();
	const float MaxHealth = DisplayChar->GetMaxHealth();
	const float HealthPercent = MaxHealth > KINDA_SMALL_NUMBER ? FMath::Clamp(Health / MaxHealth, 0.f, 1.f) : 0.f;

	if (HealthText)
	{
		HealthText->SetText(FText::FromString(FString::Printf(TEXT("%d"), FMath::CeilToInt(Health))));
	}
	if (HealthFill)
	{
		// 面的宽度按血量裁。素材是一条等宽的线，横向压扁和裁短在视觉上是一回事，
		// 所以不用再做遮罩/进度条，直接改 slot 宽度。
		// （整条宽度是设计框里的像素，入 slot 要乘 LayoutScale —— 和 PlaceOnCanvas 一个口径，
		//   不然这条线的"满血长度"会和底衬对不上）
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(HealthFill->Slot))
		{
			CanvasSlot->SetSize(FVector2D(HealthLineW * LayoutScale * HealthPercent, CanvasSlot->GetSize().Y));
		}
	}

	// 没甲时整块收掉（屏上不留一个空六边形 + 一个 0），和 CharacterOverlay 的处理一致
	const float Armor = DisplayChar->GetArmor();
	const bool bHasArmor = Armor > 0.f;
	const ESlateVisibility ArmorVisibility = bHasArmor ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (HexArmor) HexArmor->SetVisibility(ArmorVisibility);
	if (ArmorText)
	{
		ArmorText->SetText(FText::FromString(FString::Printf(TEXT("%d"), FMath::CeilToInt(Armor))));
		ArmorText->SetVisibility(ArmorVisibility);
	}

	// —— 弹药 ——
	// 近战（3 号槽那把刀）没有弹药这个概念，数字清空而不是显示 0（照
	// ABlasterPlayerController::SetHUDWeaponAmmo 的约定，但用武器类判断，不跟 -1 那个约定耦合）。
	AWeapon* Weapon = DisplayChar->GetEquippedWeapon();
	const bool bShowAmmo = Weapon != nullptr && !Weapon->IsA<AMeleeWeapon>();
	if (AmmoText)
	{
		AmmoText->SetText(bShowAmmo
			? FText::FromString(FString::Printf(TEXT("%d"), Weapon->GetAmmo()))
			: FText::GetEmpty());
	}

	// —— 技能：和 USkillBarWidget 同源（存活读本地 ASC 预测值，观战读服务器快照）——
	TArray<FSkillSlotData> Slots;
	if (bSpectating)
	{
		for (const FBlasterReplicatedSkill& Entry : DisplayChar->GetReplicatedSkills())
		{
			if (!Entry.bValid) continue;
			FSkillSlotData Data;
			Data.SlotIndex = Entry.SkillSlotIndex;
			Data.SkillType = Entry.SkillType;
			Data.Icon = Entry.SkillIcon;
			Data.MaxCharges = Entry.MaxCharges;
			Data.Charges = Entry.Charges;
			Slots.Add(Data);
		}
	}
	else if (UAbilitySystemComponent* ASC = DisplayChar->GetAbilitySystemComponent())
	{
		// 不看 bShowInSkillBar：这个版面就是固定的 C/Q/E 三个位置（按槽位号找技能），
		// 那个开关是给旧的通用技能条（列所有技能的）用的，拿它当开关会把两边一起关掉。
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			// 优先真实实例（首次激活后创建，含武装状态；配置属性也从 CDO 复制过来），
			// 未激活前实例不存在 → 回退 CDO 读纯配置（SkillType / SkillSlotIndex 这些）。
			const UBlasterGameplayAbility* Ability = Spec.GetPrimaryInstance()
				? Cast<UBlasterGameplayAbility>(Spec.GetPrimaryInstance())
				: Cast<UBlasterGameplayAbility>(Spec.Ability.Get());
			if (Ability == nullptr) continue;
			if (FindSlotWidgets(Ability->SkillSlotIndex) == nullptr) continue;   // 只关心 C/Q/E 那三格

			const FBlasterAbilityCooldownInfo Info = Ability->GetCooldownInfo(ASC);
			FSkillSlotData Data;
			Data.SlotIndex = Ability->SkillSlotIndex;
			Data.SkillType = Ability->SkillType;
			Data.Icon = Ability->SkillIcon;
			Data.MaxCharges = Info.MaxCharges;
			Data.Charges = Info.Charges;
			Slots.Add(Data);
		}
	}

	// 三个格子按**槽位号**对号入座：0=C、1=Q、2=E。图里画的是哪把技能由英雄决定，
	// 底板只固定这三格（键位字母写死在贴图里，所以格子的左右顺序不能跟着数据变）。
	for (int32 SlotIndex = 0; SlotIndex < 3; ++SlotIndex)
	{
		FValorantSkillSlotWidgets* Widgets = FindSlotWidgets(SlotIndex);
		if (Widgets == nullptr) continue;
		const FSkillSlotData* Data = Slots.FindByPredicate(
			[SlotIndex](const FSkillSlotData& Entry) { return Entry.SlotIndex == SlotIndex; });
		ApplySkillSlot(*Widgets, Data);
	}

	// —— 大招（X 位）：点数从 PlayerState 读 ——
	const ABlasterPlayerState* DisplayPlayerState = DisplayChar->GetPlayerState<ABlasterPlayerState>();
	if (DisplayPlayerState == nullptr)
	{
		// 观战看的是远端代理（SimulatedProxy 没有 Controller）→ 它自己的 GetPlayerState() 是空的，
		// 得反查 PlayerArray 里"谁的 Pawn 是这个角色"（和 USkillBarWidget / ScoreboardWidget 同一招）。
		if (const ABlasterGameState* GameState = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr)
		{
			for (APlayerState* Entry : GameState->PlayerArray)
			{
				ABlasterPlayerState* BlasterPlayerState = Cast<ABlasterPlayerState>(Entry);
				if (BlasterPlayerState && BlasterPlayerState->GetPawn() == DisplayChar)
				{
					DisplayPlayerState = BlasterPlayerState;
					break;
				}
			}
		}
	}

	const int32 UltPoints = DisplayPlayerState ? DisplayPlayerState->GetUltPoints() : 0;
	const int32 UltRequired = DisplayPlayerState ? DisplayPlayerState->GetUltPointsRequired() : 0;
	const int32 LitPips = UltRequired > 0
		? FMath::Clamp(FMath::RoundToInt((float)UltPoints / (float)UltRequired * NumUltPips), 0, NumUltPips)
		: 0;

	for (int32 i = 0; i < UltPips.Num(); ++i)
	{
		UImage* Pip = UltPips[i];
		if (Pip == nullptr) continue;
		UTexture2D* Texture = (i < LitPips) ? PipLitTexture : PipEmptyTexture;
		if (Texture) Pip->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
	}

	// 大招图标：攒满点亮，没满压到 42%（和技能图标同一套语汇）
	const bool bUltReady = UltRequired > 0 && UltPoints >= UltRequired;
	if (IconX)
	{
		// 贴图也要按当前英雄换（和 C/Q/E 三格同一个毛病：不换就一直是 WBP 里烤的 Jett 飞刀）。
		// 走 GetUltimateAbilityCDO 而不是 ASC 实例：DefaultAbilities 是**类的默认值**，
		// 任何机器上（包括观战看的远端代理）都能读到，不需要额外的复制（见 SkillBarWidget 同一招）。
		const UBlasterGameplayAbility* UltCDO = DisplayChar->GetUltimateAbilityCDO();
		if (UltCDO && UltCDO->SkillIcon)
		{
			IconX->SetBrushFromTexture(UltCDO->SkillIcon, /*bMatchSize=*/false);
		}
		IconX->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, bUltReady ? 1.f : DimmedAlpha));
	}

	// 大招生效中：X 框下面显示剩余秒数。只看 UltPoints 会一直显示"就绪"，等于骗玩家"你还有一发"，
	// 所以按角色状态盖掉（和 USkillBarWidget 同一个算法：绝对服务器结束时间 - 同步后的服务器时间）。
	const bool bUltActive = DisplayChar->IsUltimateActive();
	const float UltRemaining = (bUltActive && BPC && DisplayChar->UltimateEndTime > 0.f)
		? FMath::Max(0.f, DisplayChar->UltimateEndTime - BPC->GetServerTime())
		: 0.f;

	if (UltTimerText)
	{
		const bool bShowTimer = bUltActive && UltRemaining > 0.f;
		UltTimerText->SetText(bShowTimer
			? FText::FromString(FString::Printf(TEXT("%d"), FMath::CeilToInt(UltRemaining)))
			: FText::GetEmpty());
		UltTimerText->SetVisibility(bShowTimer ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

// ---------------------------------------------------------------- 烤图

bool UValorantBottomHUD::BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath)
{
#if WITH_EDITOR
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *WidgetBlueprintObjectPath);
	if (Asset == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] 烤图失败：加载不到 %s"), *WidgetBlueprintObjectPath);
		return false;
	}

	// 用反射拿 WidgetTree，而不是 Cast<UWidgetBlueprint>：WBP 的树挂在 UBaseWidgetBlueprint
	// （UMGEditor 模块的类）上，直接 include 会让游戏模块多一个编辑器依赖。反射只认属性名 → 零依赖。
	FObjectPropertyBase* TreeProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), FName(TEXT("WidgetTree")));
	if (TreeProperty == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] 烤图失败：%s 上没有 WidgetTree 属性（不是 WidgetBlueprint？）"), *WidgetBlueprintObjectPath);
		return false;
	}

	UWidgetTree* Tree = Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Asset));
	if (Tree == nullptr)
	{
		Tree = NewObject<UWidgetTree>(Asset, UWidgetTree::StaticClass(), TEXT("WidgetTree"), RF_Transactional);
		TreeProperty->SetObjectPropertyValue_InContainer(Asset, Tree);
	}

	// 重烤 = 以这张表为准：先把旧版面整棵摘掉，否则会一层叠一层（同名控件 UMG 还会加 _1 后缀）
	if (Tree->RootWidget)
	{
		Tree->RemoveWidget(Tree->RootWidget);
		Tree->RootWidget = nullptr;
	}

	UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	if (Root == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] 烤图失败：建不出 RootCanvas"));
		return false;
	}
	Tree->RootWidget = Root;
	BuildLayout(Tree, Root);

	Asset->MarkPackageDirty();
	UE_LOG(LogTemp, Log, TEXT("[ValorantHUD] 版面已烤进 %s"), *WidgetBlueprintObjectPath);
	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] 烤图只能在编辑器里做"));
	return false;
#endif
}

bool UValorantBottomHUD::DumpLayout(const FString& WidgetBlueprintObjectPath)
{
#if WITH_EDITOR
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *WidgetBlueprintObjectPath);
	if (Asset == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] dump 失败：加载不到 %s"), *WidgetBlueprintObjectPath);
		return false;
	}

	FObjectPropertyBase* TreeProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), FName(TEXT("WidgetTree")));
	UWidgetTree* Tree = TreeProperty ? Cast<UWidgetTree>(TreeProperty->GetObjectPropertyValue_InContainer(Asset)) : nullptr;
	if (Tree == nullptr || Tree->RootWidget == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantHUD] dump 失败：%s 里没有控件树"), *WidgetBlueprintObjectPath);
		return false;
	}

	// 打名字 + 类型 + 画布槽位（锚点/偏移/尺寸）。槽位是必须看的：版面表乘没乘 LayoutScale、
	// 两件有没有叠在一起，光看名字看不出来 —— 而这恰恰是这个 HUD 出过两次问题的地方。
	// 槽位数字是**画布单位**（1920x1080 基准帧），屏幕像素 = 它 × 当前 DPI scale。
	TFunction<void(const UWidget*, int32)> Walk = [&Walk](const UWidget* Widget, int32 Depth)
	{
		if (Widget == nullptr) return;

		FString SlotInfo;
		if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			const FAnchors Anchors = CanvasSlot->GetAnchors();
			const FVector2D Pos = CanvasSlot->GetPosition();
			const FVector2D Size = CanvasSlot->GetSize();
			SlotInfo = FString::Printf(TEXT("  [锚点 %.2f,%.2f 偏移 %.1f,%.1f 尺寸 %.1fx%.1f]"),
				Anchors.Minimum.X, Anchors.Minimum.Y, Pos.X, Pos.Y, Size.X, Size.Y);
		}

		// 文本的字体也打出来：FSlateFontInfo 里存的是 FCompositeFont（运行时对象）的话，
		// **存盘时会丢** —— 建出来看着有字，烤进 WBP 之后 TextBlock 的 Font 就是空的。
		// 判据 = 这里有没有打印出 font=Roboto；打的是 font=<none> 就说明字体没进资产。
		if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
		{
			const FSlateFontInfo FontInfo = TextBlock->GetFont();
			SlotInfo += FString::Printf(TEXT("  [字体 %s / %s / %.1f]"),
				FontInfo.FontObject ? *FontInfo.FontObject->GetName() : TEXT("<none>"),
				FontInfo.TypefaceFontName.IsNone() ? TEXT("-") : *FontInfo.TypefaceFontName.ToString(),
				FontInfo.Size);
		}

		UE_LOG(LogTemp, Display, TEXT("[ValorantHUD] %s%s (%s)%s"),
			*FString::ChrN(Depth * 2, TEXT(' ')), *Widget->GetName(), *Widget->GetClass()->GetName(), *SlotInfo);

		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				Walk(Panel->GetChildAt(i), Depth + 1);
			}
		}
	};

	UE_LOG(LogTemp, Display, TEXT("[ValorantHUD] ===== dump %s ====="), *WidgetBlueprintObjectPath);
	Walk(Tree->RootWidget, 0);
	return true;
#else
	return false;
#endif
}
