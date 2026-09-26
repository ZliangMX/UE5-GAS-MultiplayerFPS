// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CloveSmokeMapWidget.generated.h"

class UTexture2D;
class UMinimapComponent;
class ABlasterPlayerController;

/*
 * 暮蝶的封烟选点界面：屏幕正中一张**圆形**俯视地图，玩家点一下选落点。
 *
 * 交互（全部由 ABlasterPlayerController 的状态机驱动，这里只做「显示 + 报告点击」）：
 *   · 左键点在圆内 → 报告一个世界坐标，画一个白环（**追加**，不是覆盖）
 *   · 右键         → 报告"齐放"（把已选的点全部交出去）
 *   · E            → 由 PC 处理（关图，已选的点一起丢掉）
 *
 * 能选几个点由 PC 说了算 = 手上还剩几层充能（暮蝶默认 2）。剩 2 层时可以连着点两个球、
 * 按一次右键把两团烟一起放出去；只剩 1 层时就是"选一个、放一个"的老行为。
 * 这个 widget 只负责把 PC 给的数组画出来（每个点一个环 + 序号），不自己数充能。
 *
 * 三个关键实现点：
 *
 * ① 圆形底图。Slate 没有「圆形 Image」，所以用 MakeCustomVerts 画一个三角扇：
 *    中心顶点 UV = (0.5, 0.5)，边缘顶点 UV = (0.5 + 0.5cos, 0.5 + 0.5sin)。
 *    等价于把正方形底图「按圆裁剪」，不需要任何 mask 贴图。
 *
 * ② 底图来源就是小地图那张 BG_Minimap。它是俯拍截图的**中央正方形**裁切，恰好等于
 *    UMinimapComponent 的 [MapOrigin ± MapHalfSizeCm] 范围 —— 所以 WorldToMap /
 *    MapToWorld 这套换算直接复用小地图组件，不引入第二套标定（那才是最容易错的地方）。
 *
 * ③ 点击要落在「画出来的那个圆」里，就必须让**画圆的数学**和**反算点击的数学**是同一套：
 *    两边都走 ComputeDisc + ScreenToMapPixel，绝不在两处各写一份比例。
 *
 * 关于鼠标为什么不落到游戏里：NativeOnMouseButtonDown 返回 FReply::Handled()，
 * 事件在 Slate 这一层就被吃掉，不会再传给 viewport —— 所以开着地图时左右键**不会**
 * 开火/开镜，不需要再去 FireStart/AimStart 里加额外的门禁。
 */
UCLASS()
class BLASTER_API UCloveSmokeMapWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual bool Initialize() override;

	// 地图圆直径占视口短边的比例（0.72 ≈ 参考图里那张图的大小）
	UPROPERTY(EditDefaultsOnly, Category = "SmokeMap")
	float MapScreenRatio = 0.72f;

	// 圆环线宽（像素）
	UPROPERTY(EditDefaultsOnly, Category = "SmokeMap")
	float RingThickness = 3.f;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	ABlasterPlayerController* GetBlasterPC() const;
	UMinimapComponent* GetMinimapComponent() const;

	// --- 布局：画图和反算共用这一份 ---
	// OutRadiusPx = 圆的屏幕半径；OutMapSizePx = 底图边长（像素，来自小地图组件）
	bool ComputeDisc(const FGeometry& Geometry, FVector2D& OutCenter, float& OutRadiusPx, float& OutMapSizePx) const;

	// 屏幕点 → 底图内像素（相对底图左上角）。点在圆外返回 false（此时不该改选中状态）
	bool ScreenToMapPixel(const FGeometry& Geometry, const FVector2D& ScreenPos, FVector2D& OutMapPixel) const;

	// 底图内像素 → 屏幕点（画已选中那个环用）
	static FVector2D MapPixelToScreen(const FVector2D& MapPixel, float MapSizePx, const FVector2D& Center, float RadiusPx);

	// --- 绘制辅助 ---
	void DrawDisc(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, UTexture2D* Texture, const FVector2D& Center, float RadiusPx, const FLinearColor& Color) const;
	void DrawRing(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float RadiusPx, const FLinearColor& Color, float Thickness) const;
	void DrawText(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FString& Text, const FVector2D& Center, const FLinearColor& Color, int32 FontSize) const;

	// 俯拍底图（和小地图同一张）。加载失败时退化成纯色圆，功能不受影响
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> BackgroundTexture;
};
