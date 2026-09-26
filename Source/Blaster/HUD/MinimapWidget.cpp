// Fill out your copyright notice in the Description page of Project Settings.


#include "MinimapWidget.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/BlasterComponent/MinimapComponent.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"

void UMinimapWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 从 /Game/UI/Minimap/Icons 加载小地图图标纹理（运行时包已可解析）
	SelfArrowTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/Icons/ally_arrow"));
	AllyDotTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/Icons/ally_dot"));
	EnemyArrowTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/Icons/enemy_arrow"));
	DeadMarkerTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/Icons/dead_marker"));
	SpikeTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/Icons/spike"));

	// 底图（俯拍截图中央正方形）；未导入/加载失败时为 nullptr，NativePaint 退纯色占位
	BackgroundTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Minimap/BG_Minimap"));
}

void UMinimapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 纯自绘 widget：默认不会每帧重绘，必须手动强制（数据每帧都在变）
	Invalidate(EInvalidateWidgetReason::Paint);

	// AddToViewport 默认占满全屏：按地图尺寸固定一次 widget 尺寸并摆到左上角（往下让一点）
	ApplyViewportSize();
}

void UMinimapWidget::ApplyViewportSize()
{
	if (bViewportSized) return;

	UMinimapComponent* MinimapComp = GetMinimapComponent();
	if (!MinimapComp) return;

	const float Size = MinimapComp->GetMapSizePx() + 2.f * MinimapCornerMargin;
	if (Size <= 0.f) return;

	SetDesiredSizeInViewport(FVector2D(Size, Size));
	// X 贴左边，Y 往下让 MinimapTopOffset（地图实际左上角 = (MinimapCornerMargin, MinimapTopOffset + MinimapCornerMargin)）
	SetPositionInViewport(FVector2D(0.f, MinimapTopOffset));
	bViewportSized = true;
}

UMinimapComponent* UMinimapWidget::GetMinimapComponent() const
{
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(PC))
		{
			return BPC->GetMinimapComponent();
		}
	}
	return nullptr;
}

FSlateBrush UMinimapWidget::MakeBrush(UTexture2D* Texture)
{
	FSlateBrush Brush;
	if (Texture)
	{
		Brush.SetResourceObject(Texture);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2f(Texture->GetSizeX(), Texture->GetSizeY());
	}
	return Brush;
}

void UMinimapWidget::DrawBox(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FSlateBrush& Brush, const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color) const
{
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(Position)));
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geom, &Brush, ESlateDrawEffect::None, Color);
	++LayerId;
}

void UMinimapWidget::DrawIcon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, UTexture2D* Texture, const FVector2D& Center, float SizePx, const FLinearColor& Color, float RotationDeg) const
{
	if (!Texture) return;

	const FVector2D TopLeft = Center - FVector2D(SizePx * 0.5f, SizePx * 0.5f);
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FVector2f(SizePx, SizePx), FSlateLayoutTransform(FVector2f(TopLeft)));
	const FSlateBrush Brush = MakeBrush(Texture);

	if (RotationDeg == 0.f)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geom, &Brush, ESlateDrawEffect::None, Color);
	}
	else
	{
		// 绕图标中心旋转；正角 = 屏幕顺时针（与 FCanvasTileItem 同约定），直接传原始 Yaw。
		// 注意：MakeRotatedBox 的 Angle 是弧度（内部直接 FQuat2f(Angle)，而 FQuat2f 构造函数参数名
		// RotRadians，TransformCalculus2D.h:307），必须 DegreesToRadians 转换，不能像 Canvas 那样传度。
		const FVector2f Pivot(SizePx * 0.5f, SizePx * 0.5f);
		FSlateDrawElement::MakeRotatedBox(OutDrawElements, LayerId, Geom, &Brush, ESlateDrawEffect::None, FMath::DegreesToRadians(RotationDeg), Pivot, FSlateDrawElement::RelativeToElement, Color);
	}
	++LayerId;
}

void UMinimapWidget::DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const
{
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	TArray<FVector2f> Points;
	Points.Add(FVector2f(A));
	Points.Add(FVector2f(B));
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geom, MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	++LayerId;
}

// 2D 多边形是否凸：所有相邻边叉积同号（共线跳过）
bool UMinimapWidget::IsConvexPolygon(const TArray<FVector2D>& Pts)
{
	const int32 N = Pts.Num();
	if (N < 3) return false;

	float Sign = 0.f;
	for (int32 i = 0; i < N; ++i)
	{
		const FVector2D& A = Pts[i];
		const FVector2D& B = Pts[(i + 1) % N];
		const FVector2D& C = Pts[(i + 2) % N];
		const float Cross = (B.X - A.X) * (C.Y - B.Y) - (B.Y - A.Y) * (C.X - B.X);
		if (FMath::Abs(Cross) < 1.f) continue; // 共线忽略
		const float CurSign = FMath::Sign(Cross);
		if (Sign != 0.f && CurSign != Sign) return false;
		Sign = CurSign;
	}
	return true;
}

void UMinimapWidget::DrawZonePolygon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const TArray<FVector2D>& Pts) const
{
	if (Pts.Num() < 3) return;

	// 轮廓（闭合多段线，任意凸/凹多边形都正确）
	TArray<FVector2f> LinePts;
	LinePts.Reserve(Pts.Num() + 1);
	for (const FVector2D& P : Pts) LinePts.Add(FVector2f(P));
	LinePts.Add(FVector2f(Pts[0]));

	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geom, MoveTemp(LinePts), ESlateDrawEffect::None, FLinearColor(0.5f, 0.8f, 1.f, 0.9f), true, 1.5f);
	++LayerId;

	// 半透明填充：只有凸多边形才能从 Pts[0] 扇形三角剖分（凹多边形画花，跳过填充只留轮廓）
	if (!IsConvexPolygon(Pts)) return;
	if (!GEngine || !GEngine->DefaultTexture) return;

	FSlateBrush WhiteBrush = MakeBrush(GEngine->DefaultTexture);
	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(WhiteBrush);

	const FColor FillColor = FLinearColor(0.3f, 0.6f, 1.f, 0.25f).ToFColor(true);
	const FSlateRenderTransform& RT = Geometry.GetAccumulatedRenderTransform();

	TArray<FSlateVertex> Verts;
	TArray<SlateIndex> Indices;
	Verts.Reserve(Pts.Num());
	Indices.Reserve((Pts.Num() - 2) * 3);
	for (const FVector2D& P : Pts)
	{
		Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RT, FVector2f(P), FVector2f(0.f, 0.f), FillColor));
	}
	for (int32 i = 1; i + 1 < Pts.Num(); ++i)
	{
		Indices.Add(0);
		Indices.Add(i);
		Indices.Add(i + 1);
	}
	FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None);
	++LayerId;
}

int32 UMinimapWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	APlayerController* PC = GetOwningPlayer();
	if (!PC || !PC->IsLocalController()) return LayerId;

	UMinimapComponent* MinimapComp = GetMinimapComponent();
	if (!MinimapComp) return LayerId;

	// 无 pawn（死亡/观战）时沿用旧行为：整块小地图不画
	if (!PC->GetPawn()) return LayerId;

	const float Size = MinimapComp->GetMapSizePx();
	const float Margin = MinimapCornerMargin;
	const FVector2D Origin(Margin, Margin);

	// 图标像素尺寸（自己箭头 > 敌人红叉 > Spike > 敌人箭头 > 队友点）
	const float SelfIconSize = 14.f;
	const float AllyIconSize = 10.f;
	const float EnemyIconSize = 16.f;
	const float DeadIconSize = 22.f;
	const float SpikeIconSize = 18.f;

	// 底图：有底图就铺整张地图（轻微压暗提对比），否则退回纯色占位
	const FSlateBrush WhiteBrush = MakeBrush(GEngine ? GEngine->DefaultTexture : nullptr);
	const FVector2D MapRect(Margin, Margin);
	const FVector2D MapSize(Size, Size);
	if (BackgroundTexture)
	{
		const FSlateBrush BGBrush = MakeBrush(BackgroundTexture);
		DrawBox(OutDrawElements, LayerId, AllottedGeometry, BGBrush, MapRect, MapSize, FLinearColor::White);
		// 半透明黑罩：压暗地图让白色队友点/自己箭头/敌人箭头更醒目。
		// ★ 必须用**底图自己**当笔刷（而不是纯白 1x1）—— Slate 的 tint 是相乘的，
		//   即 Color * 贴图RGBA：于是黑罩只会出现在底图不透明的地方，
		//   抠掉黑色的透明区仍然全透，不会在游戏画面上糊出一个方方正正的暗斑。
		DrawBox(OutDrawElements, LayerId, AllottedGeometry, BGBrush, MapRect, MapSize, FLinearColor(0.f, 0.f, 0.f, 0.10f));
	}
	else
	{
		DrawBox(OutDrawElements, LayerId, AllottedGeometry, WhiteBrush, MapRect, MapSize, FLinearColor(0.05f, 0.07f, 0.10f, 0.82f));
	}

	// 原来这里还有一圈贴着正方形四边描的白框（每边 alpha 0.35）。底图换成抠掉黑背景的
	// Lotus 图之后，地图轮廓本身就是不规则形状、四角是透明的 —— 再描这个方框等于在空中
	// 画一个空框，所以去掉了。地图自己的白色描边已经够把边界说清楚。

	// 安装区：轮廓 + 半透明填充（画在图标底层）
	for (const TArray<FVector>& Corners : MinimapComp->GetPlantZoneOutlines())
	{
		if (Corners.Num() < 3) continue;
		TArray<FVector2D> Pts;
		Pts.Reserve(Corners.Num());
		for (const FVector& C : Corners)
		{
			Pts.Add(MinimapComp->WorldToMap(C) + Origin);
		}
		DrawZonePolygon(OutDrawElements, LayerId, AllottedGeometry, Pts);
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// 队友：同队、非自己、未死亡（组件从 GameState 客户端聚合，零带宽）
	TArray<FVector> AllyLocations;
	MinimapComp->GatherTeammateLocations(AllyLocations);
	for (const FVector& AllyLoc : AllyLocations)
	{
		FVector2D P = MinimapComp->WorldToMap(AllyLoc) + Origin;
		DrawIcon(OutDrawElements, LayerId, AllottedGeometry, AllyDotTexture, P, AllyIconSize, FLinearColor::White, 0.f);
	}

	// 可见敌人（红色方向箭头，数据来自服务器复制）
	for (const FMinimapEnemyInfo& Enemy : MinimapComp->GetVisibleEnemies())
	{
		FVector2D P = MinimapComp->WorldToMap(Enemy.Location) + Origin;
		DrawIcon(OutDrawElements, LayerId, AllottedGeometry, EnemyArrowTexture, P, EnemyIconSize, FLinearColor::White, Enemy.Yaw);
	}

	// 死亡红叉（死后 MinimapDeadMarkerLifetime 秒渐隐，时间由组件单一来源）
	for (const FMinimapDeadMarker& Marker : MinimapComp->GetDeadMarkers())
	{
		const float Elapsed = Now - Marker.DeathTime;
		if (Elapsed < 0.f || Elapsed >= MinimapComp->GetDeadMarkerLifetime()) continue;
		const float Alpha = FMath::Clamp(1.f - Elapsed / MinimapComp->GetDeadMarkerLifetime(), 0.f, 1.f);

		FVector2D P = MinimapComp->WorldToMap(Marker.Location) + Origin;
		DrawIcon(OutDrawElements, LayerId, AllottedGeometry, DeadMarkerTexture, P, DeadIconSize, FLinearColor(1.f, 1.f, 1.f, Alpha), 0.f);
	}

	// Spike：掉落/已安放的爆能器显示（数据由服务器复制给所有客户端）
	if (MinimapComp->GetSpikeData().bVisible)
	{
		FVector2D P = MinimapComp->WorldToMap(MinimapComp->GetSpikeData().Location) + Origin;
		DrawIcon(OutDrawElements, LayerId, AllottedGeometry, SpikeTexture, P, SpikeIconSize, FLinearColor::White, 0.f);
	}

	// 自己箭头：ally_arrow 纹理默认朝上。Slate 正角 = 屏幕顺时针（与 FCanvasTileItem 同约定，
	// 引擎源码 WidgetTransform.h ToSlateRenderTransform: FQuat2D(DegreesToRadians(Angle))）。
	// WorldToMap 已让世界 +X = 地图北，所以旋转 = 原始 control Yaw（朝 +X/yaw=0 时箭头朝上）。
	FVector2D OwnP = MinimapComp->WorldToMap(MinimapComp->GetOwnLocation()) + Origin;
	DrawIcon(OutDrawElements, LayerId, AllottedGeometry, SelfArrowTexture, OwnP, SelfIconSize, FLinearColor::White, MinimapComp->GetOwnYaw());

	return LayerId;
}
