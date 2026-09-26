// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MinimapWidget.generated.h"

class UTexture2D;
class UMinimapComponent;

/**
 * 小地图 Widget：把旧的 ABlasterHUD::DrawMinimap（Canvas 每帧重画）整个解耦到 UMG widget 里。
 *
 * - 纯 NativePaint 自绘：NativeTick 里强制每帧重绘，NativePaint 里读 UMinimapComponent 并绘制全部内容。
 * - 旋转约定：Slate 正角 = 屏幕顺时针（底层 FQuat2D 矩阵 [[cos,sin],[-sin,cos]]，与
 *   FCanvasTileItem 的 FRotationMatrix 同一约定）。而 UMinimapComponent::WorldToMap 已把地图转成
 *   "世界 +X = 地图上（北）、+Y = 地图右"，所以箭头朝向直接用原始 control Yaw 作为旋转角
 *   （朝 +X 即 yaw=0 时箭头朝上），与用户修正后的 HUD 行为一致，无需任何补偿。
 *
 * 数据源：GetOwningPlayer() → Cast<ABlasterPlayerController> → GetMinimapComponent()，
 * 组件只读、零网络带宽（队友从 GameState 客户端聚合，敌人/死亡/spike 由服务器复制）。
 */
UCLASS()
class BLASTER_API UMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	// 本地玩家控制器上的小地图组件（无则返回 nullptr）
	UMinimapComponent* GetMinimapComponent() const;

	// AddToViewport 默认占满全屏；这里把 widget 固定为地图尺寸并贴视口左上角（仅一次）
	void ApplyViewportSize();

	// 是否已设置过视口尺寸
	bool bViewportSized = false;

	// --- 绘制辅助（均在 widget 局部坐标；LayerId 会递增，保证后画的在上层）---
	static FSlateBrush MakeBrush(UTexture2D* Texture);
	void DrawBox(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FSlateBrush& Brush, const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color) const;
	void DrawIcon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, UTexture2D* Texture, const FVector2D& Center, float SizePx, const FLinearColor& Color, float RotationDeg) const;
	void DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const;
	void DrawZonePolygon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const TArray<FVector2D>& Pts) const;

	// 2D 多边形是否凸（填充用；共线边跳过）
	static bool IsConvexPolygon(const TArray<FVector2D>& Pts);

	// 图标纹理（NativeConstruct 时从 /Game/UI/Minimap/Icons 加载）
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> SelfArrowTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> AllyDotTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> EnemyArrowTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> DeadMarkerTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> SpikeTexture;

	// 小地图底图（俯拍截图中央正方形，NativePaint 最底层铺满整块地图；未加载时退回纯色占位）
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BackgroundTexture;

	// 小地图外缘留白（像素）
	float MinimapCornerMargin = 12.f;

	// 小地图整体往下挪多少（像素）。X 位置由 MinimapCornerMargin 贴左边，
	// Y 单独拎出来是因为实跑发现贴屏幕最上沿太挤（顶部 HUD 也在上方）。
	// 整个 widget 的左上角 = (0, MinimapTopOffset)，地图本身再往内缩 MinimapCornerMargin。
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MinimapTopOffset = 100.f;
};
