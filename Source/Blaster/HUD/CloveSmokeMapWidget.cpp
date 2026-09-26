// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/HUD/CloveSmokeMapWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

#include "Blaster/BlasterComponent/MinimapComponent.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"

// 画圆/圆的边框用的分段数。96 段在 1080p 下已经看不出棱角，再多纯属浪费顶点。
static constexpr int32 DiscSegments = 96;

bool UCloveSmokeMapWidget::Initialize()
{
	if (!Super::Initialize())
	{
		return false;
	}

	// 根节点：一整块半透明黑。
	//
	// 它有两个作用，缺一不可：
	//  ① 压暗背后的世界，让地图醒目（参考图里也是暗的）
	//  ② 让整块 widget 变成**可命中**区域 —— 没有根控件的话这个 UUserWidget 除自绘之外
	//     没有任何 Slate 内容，鼠标事件根本不会路由进来，NativeOnMouseButtonDown 永远不触发。
	//
	// ⚠️ 画图顺序：SCompoundWidget::OnPaint（也就是这个 Border）先画，NativePaint 拿到的
	//    是它之后的 LayerId，所以下面自绘的地图**盖在**这块黑底之上。
	//    （引擎源码：SObjectWidget.cpp:136-141）
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("SmokeMapRoot"));
		if (Root)
		{
			// SetBrushColor 只改色调；不显式给个画刷资源的话某些情况下什么都不画
			if (GEngine && GEngine->DefaultTexture)
			{
				Root->SetBrushFromTexture(GEngine->DefaultTexture);
			}
			Root->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
			WidgetTree->RootWidget = Root;
		}
	}

	return true;
}

void UCloveSmokeMapWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 和小地图同一张底图（俯拍截图的中央正方形）。加载失败也不影响功能，只是底图变纯色。
	BackgroundTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/BG_Minimap"));
}

void UCloveSmokeMapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 纯自绘 widget 默认不会每帧重绘，而鼠标光圈每帧都在动 —— 必须手动强制。
	// （和 UMinimapWidget 同样的处理）
	Invalidate(EInvalidateWidgetReason::Paint);
}

ABlasterPlayerController* UCloveSmokeMapWidget::GetBlasterPC() const
{
	return Cast<ABlasterPlayerController>(GetOwningPlayer());
}

UMinimapComponent* UCloveSmokeMapWidget::GetMinimapComponent() const
{
	if (ABlasterPlayerController* PC = GetBlasterPC())
	{
		return PC->GetMinimapComponent();
	}
	return nullptr;
}

bool UCloveSmokeMapWidget::ComputeDisc(const FGeometry& Geometry, FVector2D& OutCenter, float& OutRadiusPx, float& OutMapSizePx) const
{
	const UMinimapComponent* Minimap = GetMinimapComponent();
	if (!Minimap) return false;

	OutMapSizePx = Minimap->GetMapSizePx();
	if (OutMapSizePx <= KINDA_SMALL_NUMBER) return false;

	const FVector2D ViewportSize = Geometry.GetLocalSize();
	if (ViewportSize.X <= KINDA_SMALL_NUMBER || ViewportSize.Y <= KINDA_SMALL_NUMBER) return false;

	OutCenter = ViewportSize * 0.5f;
	OutRadiusPx = FMath::Min(ViewportSize.X, ViewportSize.Y) * 0.5f * MapScreenRatio;
	return OutRadiusPx > KINDA_SMALL_NUMBER;
}

bool UCloveSmokeMapWidget::ScreenToMapPixel(const FGeometry& Geometry, const FVector2D& ScreenPos, FVector2D& OutMapPixel) const
{
	FVector2D Center;
	float RadiusPx = 0.f;
	float MapSizePx = 0.f;
	if (!ComputeDisc(Geometry, Center, RadiusPx, MapSizePx)) return false;

	// 屏幕 → 归一化 [-1, 1]（相对圆心、以半径为单位）
	const FVector2D Local = Geometry.AbsoluteToLocal(ScreenPos);
	const FVector2D Normalized = (Local - Center) / RadiusPx;

	// 圆外：不接受。这里**不能** clamp —— clamp 会把「点到地图外」悄悄变成「贴边封烟」。
	if (Normalized.SizeSquared() > 1.f) return false;

	// 归一化 → 底图像素（0..MapSizePx）。这一步是 ComputeDisc 里那套比例的严格逆运算。
	OutMapPixel = (Normalized + FVector2D(1.f, 1.f)) * 0.5f * MapSizePx;
	return true;
}

FVector2D UCloveSmokeMapWidget::MapPixelToScreen(const FVector2D& MapPixel, float MapSizePx, const FVector2D& Center, float RadiusPx)
{
	const FVector2D Normalized = MapPixel / MapSizePx * 2.f - FVector2D(1.f, 1.f);
	return Center + Normalized * RadiusPx;
}

FReply UCloveSmokeMapWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	ABlasterPlayerController* PC = GetBlasterPC();
	if (!PC) return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);

	const FKey Button = InMouseEvent.GetEffectingButton();

	if (Button == EKeys::LeftMouseButton)
	{
		FVector2D MapPixel;
		UMinimapComponent* Minimap = GetMinimapComponent();
		if (Minimap && ScreenToMapPixel(InGeometry, InMouseEvent.GetScreenSpacePosition(), MapPixel))
		{
			// 像素 → 世界：交给小地图组件，和小地图画敌人/队友用的是同一套标定。
			// Z 这里还不是最终高度，真正落在哪一层由服务器向下打射线决定。
			PC->SetSmokePendingLocation(Minimap->MapToWorld(MapPixel));
		}
		// 点在圆外 = 无效点击，既不改选中状态也不算"点过"。
		// 无论如何都返回 Handled，把事件吃掉 —— 否则会漏给 viewport 变成开火。
		return FReply::Handled();
	}

	if (Button == EKeys::RightMouseButton)
	{
		// 右键 = 直接封烟（不是"取消选点"）。没点过的时候 SmokeMapDeployPressed
		// 自己会忽略，界面照旧开着。
		// 同样无论如何都吃掉事件 —— 右键本来是 ADS，漏下去会变成开镜。
		PC->SmokeMapDeployPressed();
		return FReply::Handled();
	}

	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UCloveSmokeMapWidget::DrawDisc(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, UTexture2D* Texture, const FVector2D& Center, float RadiusPx, const FLinearColor& Color) const
{
	if (!Texture) return;

	// 用资源句柄拿默认 UI 材质，再把顶点/索引喂给 MakeCustomVerts ——
	// 和 MinimapWidget::DrawZonePolygon 同一套路（那边是画多边形填充）。
	FSlateBrush Brush;
	Brush.SetResourceObject(Texture);
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.ImageSize = FVector2f(Texture->GetSizeX(), Texture->GetSizeY());

	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(Brush);
	const FSlateRenderTransform& RT = Geometry.GetAccumulatedRenderTransform();
	const FColor VertColor = Color.ToFColor(true);

	TArray<FSlateVertex> Verts;
	TArray<SlateIndex> Indices;
	Verts.Reserve(DiscSegments + 2);
	Indices.Reserve(DiscSegments * 3);

	// 圆心顶点：UV 取底图正中
	Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RT, FVector2f(Center), FVector2f(0.5f, 0.5f), VertColor));

	// 边缘一圈：位置在圆周上，UV 同步落在底图的**内切圆**上。
	// 这一步就是「圆形裁剪」的全部 —— 底图四角因为 UV 走不到而天然被丢掉。
	for (int32 i = 0; i <= DiscSegments; ++i)
	{
		const float Angle = 2.f * PI * static_cast<float>(i) / static_cast<float>(DiscSegments);
		const float CosA = FMath::Cos(Angle);
		const float SinA = FMath::Sin(Angle);

		Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
			RT,
			FVector2f(Center + FVector2D(CosA, SinA) * RadiusPx),
			FVector2f(0.5f + 0.5f * CosA, 0.5f + 0.5f * SinA),
			VertColor));
	}

	for (int32 i = 1; i <= DiscSegments; ++i)
	{
		Indices.Add(0);
		Indices.Add(i);
		Indices.Add(i + 1);
	}

	FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None);
	++LayerId;
}

void UCloveSmokeMapWidget::DrawRing(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float RadiusPx, const FLinearColor& Color, float Thickness) const
{
	const FPaintGeometry PaintGeom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));

	TArray<FVector2f> Points;
	Points.Reserve(DiscSegments + 1);
	for (int32 i = 0; i <= DiscSegments; ++i)
	{
		const float Angle = 2.f * PI * static_cast<float>(i) / static_cast<float>(DiscSegments);
		Points.Add(FVector2f(Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * RadiusPx));
	}

	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, PaintGeom, MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	++LayerId;
}

void UCloveSmokeMapWidget::DrawText(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FString& Text, const FVector2D& Center, const FLinearColor& Color, int32 FontSize) const
{
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", FontSize);

	// 量一下文字多宽才能居中。拿不到测量服务就退化成左上角对齐（不影响功能）。
	FVector2D TextSize(Text.Len() * FontSize * 0.5f, FontSize);
	if (FSlateApplication::IsInitialized())
	{
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		TextSize = Measure->Measure(Text, Font);
	}

	const FPaintGeometry PaintGeom = Geometry.ToPaintGeometry(
		FVector2f(TextSize),
		FSlateLayoutTransform(FVector2f(Center - TextSize * 0.5f)));

	FSlateDrawElement::MakeText(OutDrawElements, LayerId, PaintGeom, Text, Font, ESlateDrawEffect::None, Color);
	++LayerId;
}

int32 UCloveSmokeMapWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const APlayerController* PC = GetOwningPlayer();
	if (!PC || !PC->IsLocalController()) return LayerId;

	const ABlasterPlayerController* BPC = GetBlasterPC();
	if (!BPC) return LayerId;

	FVector2D Center;
	float RadiusPx = 0.f;
	float MapSizePx = 0.f;
	if (!ComputeDisc(AllottedGeometry, Center, RadiusPx, MapSizePx)) return LayerId;

	// --- 底图（圆形裁剪的俯拍图）---
	if (BackgroundTexture)
	{
		DrawDisc(OutDrawElements, LayerId, AllottedGeometry, BackgroundTexture, Center, RadiusPx, FLinearColor::White);
	}
	else
	{
		// 底图没导入：纯色圆占位，选点功能照常可用
		DrawDisc(OutDrawElements, LayerId, AllottedGeometry, GEngine ? GEngine->DefaultTexture : nullptr, Center, RadiusPx, FLinearColor(0.06f, 0.08f, 0.11f, 0.95f));
	}

	// 边缘两圈：外面一圈暗的做描边，里面一圈白色细线定边界
	DrawRing(OutDrawElements, LayerId, AllottedGeometry, Center, RadiusPx + RingThickness, FLinearColor(0.f, 0.f, 0.f, 0.75f), RingThickness * 2.f);
	DrawRing(OutDrawElements, LayerId, AllottedGeometry, Center, RadiusPx, FLinearColor(1.f, 1.f, 1.f, 0.55f), RingThickness);

	// --- 已选中的封烟点位（白环 + 序号，和参考图里那个圈一致）---
	// 是**一圈**而不是一个：2 层充能时可以连点两个球、一次右键齐放。
	// 序号（1/2…）是为了让"到底选了几个、还差几个"一眼看得出来 —— 光看圈数在
	// 两个点靠得很近时是数不清的。
	const TArray<FVector>& Pending = BPC->GetSmokePendingLocations();
	for (int32 i = 0; i < Pending.Num(); ++i)
	{
		const FVector2D MapPixel = GetMinimapComponent()
			? GetMinimapComponent()->WorldToMap(Pending[i])
			: FVector2D::ZeroVector;

		const FVector2D Screen = MapPixelToScreen(MapPixel, MapSizePx, Center, RadiusPx);
		DrawRing(OutDrawElements, LayerId, AllottedGeometry, Screen, 22.f, FLinearColor(1.f, 1.f, 1.f, 0.95f), 3.f);
		// 环心再加一个小点，缩放小的时候也能看清选在哪
		DrawDisc(OutDrawElements, LayerId, AllottedGeometry, GEngine ? GEngine->DefaultTexture : nullptr, Screen, 4.f, FLinearColor(1.f, 1.f, 1.f, 0.95f));

		// 序号画在环**上方**，不去和环心那个点抢位置
		DrawText(OutDrawElements, LayerId, AllottedGeometry,
			FString::Printf(TEXT("%d"), i + 1),
			FVector2D(Screen.X, Screen.Y - 44.f),
			FLinearColor(1.f, 1.f, 1.f, 0.95f), 16);
	}

	// --- 鼠标处的跟随光圈 ---
	// 用 Slate 的全局光标位置而不是 PC->GetMousePosition()：前者和
	// NativeOnMouseButtonDown 里 FPointerEvent 的坐标系是同一个，点选和光圈保证不偏移。
	if (FSlateApplication::IsInitialized())
	{
		const FVector2D Local = AllottedGeometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
		if ((Local - Center).SizeSquared() <= FMath::Square(RadiusPx))
		{
			DrawRing(OutDrawElements, LayerId, AllottedGeometry, Local, 14.f, FLinearColor(1.f, 1.f, 1.f, 0.7f), 2.f);
		}
	}

	// --- 提示文字 ---
	// 文案跟着状态走：没选点时右键是没用的，就不提它，免得玩家反复右键等封烟。
	// 括号里的分母是"这次最多能选几个" = 剩余充能层数，分子是已经选了几个 ——
	// 剩 2 层时看到 (0/2) 就知道可以连着点两个球再一起放。
	const int32 Picked = Pending.Num();
	const FString Hint = (Picked > 0)
		? FString::Printf(TEXT("RMB: deploy %d    E: close"), Picked)
		: FString::Printf(TEXT("LMB: pick a spot  (%d/%d)    E: close"),
			Picked, BPC->GetSmokePendingCapacity());

	DrawText(OutDrawElements, LayerId, AllottedGeometry, Hint,
		FVector2D(Center.X, Center.Y + RadiusPx + 34.f),
		FLinearColor(1.f, 1.f, 1.f, 0.9f), 16);

	return LayerId;
}
