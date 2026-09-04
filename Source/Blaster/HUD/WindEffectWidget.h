// 视角风特效（UMG Widget，NativePaint 全自绘）：角色处于「技能武装（逐风）」状态时，
// 全屏画流动的风丝线条（Valorant Jett 逐风的屏幕风效）。无任何纹理资产，纯矢量。
//
// 数据链路：GetOwningPlayer → 角色 → 遍历 ASC 能力实例，任意 UBlasterGameplayAbility::IsArmed()
// 为真即显示（武装退出后 FadeAlpha 平滑淡出，避免突兀消失）。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WindEffectWidget.generated.h"

class UTexture2D;

UCLASS()
class BLASTER_API UWindEffectWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	static FSlateBrush MakeBrush(UTexture2D* Texture);
	void DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const;

	// 时间累计（驱动风丝流动动画）
	float WindTime = 0.f;
	// 武装显隐过渡（0=完全隐藏，1=完全显示）
	float FadeAlpha = 0.f;
	// 显隐过渡速度（/秒）
	float FadeSpeed = 4.f;
};
