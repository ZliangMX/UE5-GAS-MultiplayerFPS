#include "WindEffectWidget.h"
#include "Engine/Engine.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "GameplayAbilitySpec.h"

void UWindEffectWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	WindTime += InDeltaTime;

	// 目标透明度：本机任意技能处于武装 → 1，否则 0
	const ABlasterCharacter* Char = GetOwningPlayer() ? Cast<ABlasterCharacter>(GetOwningPlayer()->GetPawn()) : nullptr;
	const float Target = (Char && Char->IsAnySkillArmed()) ? 1.f : 0.f;
	FadeAlpha = FMath::FInterpTo(FadeAlpha, Target, InDeltaTime, FadeSpeed);

	// 纯自绘 widget 默认不每帧重绘，手动强制（风丝在动）
	Invalidate(EInvalidateWidgetReason::Paint);
}

FSlateBrush UWindEffectWidget::MakeBrush(UTexture2D* Texture)
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

void UWindEffectWidget::DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const
{
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	TArray<FVector2f> Points;
	Points.Add(FVector2f(A));
	Points.Add(FVector2f(B));
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geom, MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	++LayerId;
}

int32 UWindEffectWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (FadeAlpha <= 0.01f) return LayerId;

	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const float Vw = Size.X;
	const float Vh = Size.Y;
	if (Vw <= 1.f || Vh <= 1.f) return LayerId;

	// 风丝：自右向左流动（风掠过镜头），位置用 i 派生的确定性伪随机，避免每帧抖动
	const int32 N = 26;
	for (int32 i = 0; i < N; ++i)
	{
		const float R1 = FMath::Frac(i * 0.618033988f);
		const float R2 = FMath::Frac(i * 3.7f);
		const float R3 = FMath::Frac(i * 1.3f);
		const float R4 = FMath::Frac(i * 2.9f);
		const float R5 = FMath::Frac(i * 5.1f);
		const float R6 = FMath::Frac(i * 7.7f);
		const float R7 = FMath::Frac(i * 9.1f);

		const float BaseY = R1 * Vh;
		const float Speed = 80.f + R2 * 260.f;            // px/s
		const float Len = 60.f + R3 * 180.f;
		const float Thick = 1.2f + R4 * 2.0f;
		// 大部分近乎水平（斜率小），每 5 条一条明显斜向
		const float Slope = ((R5 - 0.5f) * 0.4f) + ((i % 5 == 0) ? (R5 - 0.5f) * 0.8f : 0.f);
		const float BaseAlpha = 0.16f + R6 * 0.45f;

		// 循环流动：Cycle 0→1 完成一次 Vw+Len 的穿越
		const float Total = Vw + Len + 60.f;
		const float Cycle = FMath::Frac(WindTime * (Speed / Total) + R7);
		const float X = (Vw + 30.f) - Cycle * Total;

		// 轻微上下摆 + 透明度脉动，风更有"活"的感觉
		const float Y = BaseY + FMath::Sin(WindTime * 1.3f + i * 1.7f) * 18.f;
		const float Pulse = 0.55f + 0.45f * FMath::Sin(WindTime * 2.0f + i * 2.3f);
		const float Alpha = FadeAlpha * BaseAlpha * FMath::Max(0.f, Pulse);
		if (Alpha <= 0.01f) continue;

		// 尾端再带一条细残影，形成"拉丝"感
		const FVector2D A(X, Y);
		const FVector2D B(X - Len, Y + Slope * Len);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, A, B, FLinearColor(0.9f, 0.97f, 1.f, Alpha), Thick);

		// 细残影（前段后 1/3，更淡）
		const FVector2D C = B;
		const FVector2D D = B - FVector2D(Len * 0.4f, Slope * Len * 0.4f);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, C, D, FLinearColor(0.9f, 0.97f, 1.f, Alpha * 0.45f), Thick * 0.6f);
	}

	return LayerId;
}
