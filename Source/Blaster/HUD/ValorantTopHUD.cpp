// Valorant 顶部比分栏实现。三块：版面表 + 数值刷新 + 烤图工具。
// 版面坐标来自 E:\Notion\claude-temp\top_draw.py 的实测值（源图 2559x1437 的像素框）。

#include "ValorantTopHUD.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Styling/CoreStyle.h"
#include "UObject/UnrealType.h"

#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/HUD/ValorantAgentPortrait.h"

namespace
{
	// 素材目录。8 张重绘件都在这里（和底部 HUD 的 Jett 目录分开，别混）。
	const TCHAR* const HUDTopFolder = TEXT("/Game/Assets/Textures/HUD/Top/");

	UTexture2D* LoadHUDTopTexture(const TCHAR* AssetName)
	{
		const FString Path = FString(HUDTopFolder) + AssetName + TEXT(".") + AssetName;
		return LoadObject<UTexture2D>(nullptr, *Path);
	}

	// 这一屏的东西都在中间那条带子里，锚点统一用 Center。
	// ★ X 仍然是**设计分辨率里的绝对像素**（PlaceTopOnCanvas 里会减掉半个设计宽），
	//   和底部 HUD 的语义完全一样。
	enum class ETopAnchor : uint8 { Left, Center, Right };

	struct FTopLayoutPiece
	{
		const TCHAR* Name;
		const TCHAR* Texture;
		float X, Y, W, H;
		ETopAnchor Anchor;
		float Alpha;
	};

	// 实测几何（源图像素）：
	//   条带 y 36..92（高 56，中线 y=64）
	//   青条 左尖 536、右尖 1052；红条 右尖 2028、左尖 1500
	//   顶部白线 y 20..21，x 525..2034
	//   V 形线：左臂 (1127,22)->(1280,168)，右臂 (1433,22)->(1280,168)
	//   数字底板：左 chevron 尖端朝右 (1130,56)，右 chevron 尖端朝左 (1430,56)，y 35..77
	//   两端图标：青 (555,64)、红 (2005,64)，半径约 12
	//
	// ★ 顺序 = 绘制顺序（后面的盖在前面上）：装饰线在下，条带压上去，图标/白线在最上。
	const FTopLayoutPiece GTopLayoutPieces[] =
	{
		{ TEXT("Top_VOrn"),    TEXT("T_HUDTOP_v_orn"),    1108.f, 14.f, 344.f, 166.f, ETopAnchor::Center, 1.f },
		{ TEXT("Bar_Ally"),    TEXT("T_HUDTOP_ally_bar"),  530.f, 32.f, 534.f,  64.f, ETopAnchor::Center, 1.f },
		{ TEXT("Bar_Enemy"),   TEXT("T_HUDTOP_enemy_bar"),1496.f, 32.f, 534.f,  64.f, ETopAnchor::Center, 1.f },
		// 头像框：长条水平槽。压在队伍条上，里面那层 HorizontalBox 由 BuildLayout 单独建
		//（局内往它 AddChild 塞角色头像 widget，见 AddTeamPortrait）。
		{ TEXT("Frame_Ally"),  TEXT("T_HUDTOP_frame_ally"),  588.f, 44.f, 452.f,  40.f, ETopAnchor::Center, 1.f },
		{ TEXT("Frame_Enemy"), TEXT("T_HUDTOP_frame_enemy"),1520.f, 44.f, 452.f,  40.f, ETopAnchor::Center, 1.f },
		{ TEXT("Plate_Ally"),  TEXT("T_HUDTOP_plate_l"),  1046.f, 28.f,  96.f,  56.f, ETopAnchor::Center, 1.f },
		{ TEXT("Plate_Enemy"), TEXT("T_HUDTOP_plate_r"),  1418.f, 28.f,  96.f,  56.f, ETopAnchor::Center, 1.f },
		{ TEXT("Icon_Ally"),   TEXT("T_HUDTOP_icon_ally"),  533.f, 42.f,  44.f,  44.f, ETopAnchor::Center, 1.f },
		{ TEXT("Icon_Enemy"),  TEXT("T_HUDTOP_icon_enemy"),1983.f, 42.f,  44.f,  44.f, ETopAnchor::Center, 1.f },
		{ TEXT("Top_Line"),    TEXT("T_HUDTOP_top_line"),   515.f, 14.f,1530.f,  14.f, ETopAnchor::Center, 1.f },
	};

	// 把控件摆到 Canvas 上。和 UValorantBottomHUD::PlaceOnCanvas 同一套换算（注释见那边）：
	// (X, Y) 是框的左上角，Canvas 锚在左下角 → 纵向补回高度。
	void PlaceTopOnCanvas(UCanvasPanel* Root, UWidget* Widget, float X, float Y, float W, float H, ETopAnchor Anchor)
	{
		UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(Widget);
		if (CanvasSlot == nullptr) return;

		float AnchorX = 0.f;
		float AnchorFraction = 0.f;
		if (Anchor == ETopAnchor::Center) { AnchorX = UValorantTopHUD::DesignWidth * 0.5f; AnchorFraction = 0.5f; }
		else if (Anchor == ETopAnchor::Right) { AnchorX = UValorantTopHUD::DesignWidth; AnchorFraction = 1.f; }

		CanvasSlot->SetAnchors(FAnchors(AnchorFraction, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetPosition(FVector2D(X - AnchorX, (Y + H) - UValorantTopHUD::DesignHeight));
		CanvasSlot->SetSize(FVector2D(W, H));
		CanvasSlot->SetAutoSize(false);
		CanvasSlot->SetZOrder(0);
	}

	UImage* MakeTopImage(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name, const TCHAR* TextureName,
		float X, float Y, float W, float H, ETopAnchor Anchor, float Alpha)
	{
		UImage* Image = Tree->ConstructWidget<UImage>(UImage::StaticClass(), FName(Name));
		if (Image == nullptr) return nullptr;

		if (UTexture2D* Texture = LoadHUDTopTexture(TextureName))
		{
			// bMatchSize = false：尺寸由 slot 说了算（贴图是 4x 的，缩到 1x 显示）
			Image->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
		}
		Image->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, Alpha));
		Image->SetVisibility(ESlateVisibility::HitTestInvisible);
		Image->bIsVariable = true;
		PlaceTopOnCanvas(Root, Image, X, Y, W, H, Anchor);
		return Image;
	}

	UTextBlock* MakeTopText(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name,
		float X, float Y, float W, float H, ETopAnchor Anchor, ETextJustify::Type Justify, int32 FontSize)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
		if (Text == nullptr) return nullptr;

		Text->SetFont(FCoreStyle::GetDefaultFontStyle(FName(TEXT("Bold")), FontSize));
		Text->SetJustification(Justify);
		Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		Text->bIsVariable = true;
		PlaceTopOnCanvas(Root, Text, X, Y, W, H, Anchor);
		return Text;
	}

	// 头像框里那层容器。宽 448 = 框贴图 452 减两侧各 2px 内缩；高 36 同理（框是 40）。
	UHorizontalBox* MakePortraitBox(UWidgetTree* Tree, UCanvasPanel* Root, const TCHAR* Name, float X)
	{
		UHorizontalBox* Box = Tree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), FName(Name));
		if (Box == nullptr) return nullptr;

		Box->bIsVariable = true;
		// 容器本身不吃点击：HUD 上的东西一律透传
		Box->SetVisibility(ESlateVisibility::HitTestInvisible);
		PlaceTopOnCanvas(Root, Box, X, 46.f, 448.f, 36.f, ETopAnchor::Center);
		return Box;
	}
}

// ---------------------------------------------------------------- 版面

void UValorantTopHUD::BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root)
{
	if (Tree == nullptr || Root == nullptr) return;

	for (const FTopLayoutPiece& Piece : GTopLayoutPieces)
	{
		MakeTopImage(Tree, Root, Piece.Name, Piece.Texture, Piece.X, Piece.Y, Piece.W, Piece.H, Piece.Anchor, Piece.Alpha);
	}

	// 数字全部用文本画（和底部 HUD 一样：素材里不抠数字，字体清晰得多）：
	//   回合数：左右底板正中。底板 y 35..77、中线 56，但数字实测基线略高（glyph y 49..69，中线 59）
	//     —— 所以文本框按中线 59 摆（Y=41、H=36）。
	//   倒计时：两块底板之间那段空档（x 1130..1430），居中，字号给大一点。
	MakeTopText(Tree, Root, TEXT("Text_Timer"),      1130.f, 32.f, 300.f, 56.f, ETopAnchor::Center, ETextJustify::Center, 40);
	MakeTopText(Tree, Root, TEXT("Text_ScoreAlly"),  1067.f, 41.f,  60.f, 36.f, ETopAnchor::Center, ETextJustify::Center, 28);
	MakeTopText(Tree, Root, TEXT("Text_ScoreEnemy"), 1433.f, 41.f,  60.f, 36.f, ETopAnchor::Center, ETextJustify::Center, 28);

	// 头像框里的容器：往里面 AddChild 头像 widget，从左往右自动排（见 AddTeamPortrait）。
	// 位置 = 框贴图内缩 2px（左 588+2、右 1520+2），宽度同理减 4，高度 36 留出框的净空。
	// ★ 这两行是最后加进 Canvas 的 → 绘制在框贴图之上，头像是"装在框里"而不是被框盖住。
	MakePortraitBox(Tree, Root, TEXT("Box_AllyPortraits"),   590.f);
	MakePortraitBox(Tree, Root, TEXT("Box_EnemyPortraits"), 1522.f);
}

TSharedRef<SWidget> UValorantTopHUD::RebuildWidget()
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
			UE_LOG(LogTemp, Warning, TEXT("[ValorantTopHUD] WBP 的根不是 CanvasPanel（当前 %s）：直接用，不再建版面"),
				*WidgetTree->RootWidget->GetClass()->GetName());
		}

		bLayoutBuilt = true;
		ResolveWidgets();
	}

	return Super::RebuildWidget();
}

void UValorantTopHUD::ResolveWidgets()
{
	// 代码建的树和烤进 WBP 的树用同一套名字 → GetWidgetFromName 两条路都通。
	ScoreAllyText = Cast<UTextBlock>(GetWidgetFromName(TEXT("Text_ScoreAlly")));
	ScoreEnemyText = Cast<UTextBlock>(GetWidgetFromName(TEXT("Text_ScoreEnemy")));
	TimerText = Cast<UTextBlock>(GetWidgetFromName(TEXT("Text_Timer")));

	AllyPortraitBox = Cast<UHorizontalBox>(GetWidgetFromName(TEXT("Box_AllyPortraits")));
	EnemyPortraitBox = Cast<UHorizontalBox>(GetWidgetFromName(TEXT("Box_EnemyPortraits")));
}

// ---------------------------------------------------------------- 头像框

UHorizontalBox* UValorantTopHUD::GetTeamPortraitBox(bool bAlly) const
{
	return bAlly ? AllyPortraitBox : EnemyPortraitBox;
}

void UValorantTopHUD::AddTeamPortrait(bool bAlly, UUserWidget* Portrait, float SlotPadding)
{
	UHorizontalBox* Box = GetTeamPortraitBox(bAlly);
	if (Box == nullptr || Portrait == nullptr) return;

	UHorizontalBoxSlot* BoxSlot = Box->AddChildToHorizontalBox(Portrait);
	if (BoxSlot == nullptr) return;

	BoxSlot->SetPadding(FMargin(SlotPadding, 0.f));
	BoxSlot->SetHorizontalAlignment(HAlign_Center);
	BoxSlot->SetVerticalAlignment(VAlign_Center);
	// 槽按头像 widget 自己的期望尺寸走（Automatic = 不拉伸也不压缩）——
	// 头像做多大由它自己的 SizeBox/期望尺寸定，这里不替它决定。
	BoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
}

void UValorantTopHUD::ClearTeamPortraits(bool bAlly)
{
	if (UHorizontalBox* Box = GetTeamPortraitBox(bAlly))
	{
		Box->ClearChildren();
	}
}

// ---------------------------------------------------------------- 自动喂头像

void UValorantTopHUD::RefreshTeamPortraits()
{
	UWorld* World = GetWorld();
	ABlasterGameState* GS = World ? World->GetGameState<ABlasterGameState>() : nullptr;
	APlayerController* PC = GetOwningPlayer();
	ABlasterPlayerState* LocalPS = PC ? Cast<ABlasterPlayerState>(PC->PlayerState) : nullptr;

	TArray<EBlasterAgent> Ally;
	TArray<EBlasterAgent> Enemy;

	// 本地玩家自己还没分队（刚连上/观战）时两侧都不画 —— 分不清谁是我方，画出来必然是错的。
	if (GS && LocalPS && (LocalPS->Team == ETeam::ET_TeamA || LocalPS->Team == ETeam::ET_TeamB))
	{
		// 用 GameState 的 PlayerArray 而不是"本地捏一份名单"：它是服务器同步下来的权威名册，
		// 谁进来谁走了自己就反映出来。
		for (APlayerState* Other : GS->PlayerArray)
		{
			ABlasterPlayerState* OtherPS = Cast<ABlasterPlayerState>(Other);
			// 还没 roll 到英雄的人不占格子（大厅里所有人都是 None）。
			// 等 SetAgent 复制过来，下面的比对就会发现变化并自动补上。
			if (OtherPS == nullptr || !OtherPS->HasSelectedAgent()) continue;

			if (OtherPS->Team == LocalPS->Team) { Ally.Add(OtherPS->GetAgent()); }
			else { Enemy.Add(OtherPS->GetAgent()); }
		}
	}

	// ★ 这里是"每帧会被调一次"的入口，先跟上一帧的快照比：
	//   名单没变就什么都不做。存的是英雄列表（不是 widget 指针），
	//   所以换英雄、退人、换队都能被这套比对发现。
	if (bPortraitsBuilt && Ally == LastAllyAgents && Enemy == LastEnemyAgents) return;

	bPortraitsBuilt = true;
	LastAllyAgents = Ally;
	LastEnemyAgents = Enemy;

	RebuildPortraitRow(true, Ally);
	RebuildPortraitRow(false, Enemy);
}

void UValorantTopHUD::RebuildPortraitRow(bool bAlly, const TArray<EBlasterAgent>& Agents)
{
	ClearTeamPortraits(bAlly);

	APlayerController* PC = GetOwningPlayer();
	if (PC == nullptr) return;

	UClass* WidgetClass = PortraitWidgetClass;
	if (WidgetClass == nullptr) { WidgetClass = UValorantAgentPortrait::StaticClass(); }

	for (EBlasterAgent OneAgent : Agents)
	{
		UValorantAgentPortrait* Portrait = CreateWidget<UValorantAgentPortrait>(PC, WidgetClass);
		if (Portrait == nullptr) continue;

		// 先 SetAgent 再进容器：ApplyAgent 里那次刷贴图不必等 Slate 构建
		Portrait->SetAgent(OneAgent);
		AddTeamPortrait(bAlly, Portrait);
	}
}

// ---------------------------------------------------------------- 刷新

void UValorantTopHUD::SetHUDVisible(bool bVisible)
{
	if (bShown == bVisible) return;
	bShown = bVisible;

	// ★ 收的是里面的根画布，**不是 widget 自己**（和 UValorantBottomHUD::SetHUDVisible 同一个坑）：
	//   把 UserWidget 自己设成 Collapsed，Slate 会连它的 Tick 一起停掉，而这块 HUD 的可见性判断
	//   就在 NativeTick → UpdateDisplay 里 —— 在 Lobby 里收一次，之后哪怕换了地图回到对局，
	//   这个实例也再没有 Tick 能把它放出来。只收内容，SObjectWidget 那头留着，Tick 就不断。
	UWidget* Root = WidgetTree ? WidgetTree->RootWidget : nullptr;
	if (Root == nullptr)
	{
		SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		return;
	}
	Root->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UValorantTopHUD::UpdateDisplay()
{
	APlayerController* PC = GetOwningPlayer();
	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(PC);

	// Lobby（选人）地图整块不显示。
	if (!PC || !PC->IsLocalController() || (BPC && BPC->IsInLobby()))
	{
		SetHUDVisible(false);
		return;
	}
	SetHUDVisible(true);

	// —— 头像：本地队伍摆左边、另一队摆右边（内部做了快照比对，名单没变不会重建）——
	RefreshTeamPortraits();

	// —— 回合数：GameState 上分的是 TeamA/TeamB，左右要按**本地玩家在哪边**翻 ——
	const ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
	int32 AllyScore = 0;
	int32 EnemyScore = 0;
	if (GS)
	{
		const ABlasterPlayerState* PS = Cast<ABlasterPlayerState>(PC->PlayerState);
		const bool bOnTeamB = PS && PS->Team == ETeam::ET_TeamB;
		AllyScore = bOnTeamB ? GS->TeamBScore : GS->TeamAScore;
		EnemyScore = bOnTeamB ? GS->TeamAScore : GS->TeamBScore;
	}
	if (ScoreAllyText) { ScoreAllyText->SetText(FText::AsNumber(AllyScore)); }
	if (ScoreEnemyText) { ScoreEnemyText->SetText(FText::AsNumber(EnemyScore)); }

	// —— 倒计时：阶段剩余秒数由 PlayerController 算（和它 SetHUDTime 里那套分支同源）——
	// 安包后旧回合倒计时失效，PC 那边会返回 0 → 这里就收掉数字（spike 自己的倒计时另有显示）。
	if (TimerText)
	{
		const float TimeLeft = BPC ? BPC->GetPhaseTimeLeft() : 0.f;
		if (TimeLeft > 0.f)
		{
			const int32 Total = FMath::CeilToInt(TimeLeft);
			TimerText->SetText(FText::FromString(FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60)));
			TimerText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			TimerText->SetText(FText::GetEmpty());
			TimerText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UValorantTopHUD::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bLayoutBuilt) { UpdateDisplay(); }
}

// ---------------------------------------------------------------- 烤图

bool UValorantTopHUD::BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath)
{
#if WITH_EDITOR
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *WidgetBlueprintObjectPath);
	if (Asset == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantTopHUD] 烤图失败：加载不到 %s"), *WidgetBlueprintObjectPath);
		return false;
	}

	// 反射拿 WidgetTree（和 UValorantBottomHUD 一个理由：直接 Cast<UWidgetBlueprint> 会让游戏模块
	// 多一个 UMGEditor 依赖）。只认属性名 → 零依赖。
	FObjectPropertyBase* TreeProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), FName(TEXT("WidgetTree")));
	if (TreeProperty == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ValorantTopHUD] 烤图失败：%s 上没有 WidgetTree 属性（不是 WidgetBlueprint？）"), *WidgetBlueprintObjectPath);
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
		UE_LOG(LogTemp, Warning, TEXT("[ValorantTopHUD] 烤图失败：建不出 RootCanvas"));
		return false;
	}
	Tree->RootWidget = Root;
	BuildLayout(Tree, Root);

	Asset->MarkPackageDirty();
	UE_LOG(LogTemp, Log, TEXT("[ValorantTopHUD] 版面已烤进 %s"), *WidgetBlueprintObjectPath);
	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[ValorantTopHUD] 烤图只能在编辑器里做"));
	return false;
#endif
}
