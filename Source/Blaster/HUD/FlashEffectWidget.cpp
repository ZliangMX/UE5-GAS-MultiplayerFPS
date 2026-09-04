#include "FlashEffectWidget.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/SlateRenderer.h"
#include "SlateMaterialBrush.h"
#include "GameFramework/PlayerController.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

void UFlashEffectWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	APlayerController* PC = GetOwningPlayer();
	const ABlasterCharacter* Char = PC ? Cast<ABlasterCharacter>(PC->GetPawn()) : nullptr;
	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(PC);
	const UAbilitySystemComponent* ASC = Char ? Char->GetAbilitySystemComponent() : nullptr;

	// GE 总时长从 CDO 的 Duration 读（兜底 1.75s），Intensity 归一化用。两个数据来源共用。
	if (FlashBlindEffectClass)
	{
		const UGameplayEffect* Def = FlashBlindEffectClass->GetDefaultObject<UGameplayEffect>();
		if (Def && Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
		{
			float Dur = 1.75f;
			if (Def->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Dur) && Dur > 0.f)
			{
				BlindTotalDuration = Dur;
			}
		}
	}

	BlindTimeRemaining = 0.f;
	BlastWorldPos = FVector::ZeroVector;

	// 数据来源二选一：
	//   观战中（本人已死，自己身上没有 GE）→ 读被观察队友复制的白闪快照
	//     （服务器在闪光命中的瞬间写 ReplicatedBlindServerEndTime/Origin，见
	//       BlasterCharacter::ServerReceiveFlashBlind；剩余 = 结束服务器时间 - 同步服务器时间）。
	//   存活 → 直接查本地 ASC 的活跃 GE（原链路，预测即时）。
	const bool bSpectating = BPC && BPC->IsSpectating();
	if (bSpectating)
	{
		if (const ABlasterCharacter* Target = BPC->GetSpectateTargetCharacter())
		{
			const float EndServerTime = Target->GetReplicatedBlindServerEndTime();
			if (EndServerTime > 0.f)
			{
				const float ServerNow = BPC->GetServerTime();
				BlindTimeRemaining = FMath::Max(0.f, EndServerTime - ServerNow);
				if (BlindTimeRemaining > 0.f)
				{
					BlastWorldPos = Target->GetReplicatedBlindOrigin();
				}
			}
		}
	}
	else if (ASC && FlashBlindEffectClass)
	{
		// 查询该 GE 的所有活跃实例剩余时间（多颗闪光叠加取最长的）
		FGameplayEffectQuery Query;
		Query.EffectDefinition = FlashBlindEffectClass;
		const TArray<float> Remaining = ASC->GetActiveEffectsTimeRemaining(Query);
		for (const float R : Remaining)
		{
			BlindTimeRemaining = FMath::Max(BlindTimeRemaining, R);
		}

		// 爆炸点世界位置：读活跃 GE 的 EffectCauser（闪光弹本体，Detonate 时 AddEffectCauser(this)）
		const UGameplayEffect* Def = FlashBlindEffectClass->GetDefaultObject<UGameplayEffect>();
		const FActiveGameplayEffectsContainer& Container = ASC->GetActiveGameplayEffects();
		for (FActiveGameplayEffectsContainer::ConstIterator It = Container.CreateConstIterator(); It; ++It)
		{
			const FActiveGameplayEffect& ActiveGE = *It;
			if (ActiveGE.Spec.Def == Def)
			{
				if (const AActor* Causer = ActiveGE.Spec.GetContext().GetEffectCauser())
				{
					BlastWorldPos = Causer->GetActorLocation();
					break;
				}
			}
		}
	}

	// 被闪期间每帧重绘（白屏在渐隐）；无被闪时不画省开销
	if (BlindTimeRemaining > 0.f)
	{
		if (!bPrevBlind) // 只在进入被闪瞬间打一次日志，定位白屏是否到达客户端
		{
			UE_LOG(LogTemp, Log, TEXT("[FlashWidget] Blind start remaining=%.2f total=%.2f"), BlindTimeRemaining, BlindTotalDuration);
		}
		bPrevBlind = true;
		Invalidate(EInvalidateWidgetReason::Paint);
	}
	else if (bPrevBlind)
	{
		// 刚从被闪退出：不重绘的话 Slate 会留着上一帧的白屏 quad，
		// 主动强制重绘一帧（NativePaint 见 Intensity<=0.01 直接返回，quad 即被清掉）。
		bPrevBlind = false;
		Invalidate(EInvalidateWidgetReason::Paint);
	}
	else
	{
		bPrevBlind = false;
	}
}

int32 UFlashEffectWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (BlindTimeRemaining <= 0.01f) return LayerId;

	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X <= 1.f || Size.Y <= 1.f) return LayerId;

	// Intensity：剩余时间归一化。刚中弹 =1，随盲效果衰减渐隐。
	// 1.45 倍系数让前段保持饱和（短暂停留后才开始退），符合「全屏闪光渐隐」。
	const float Ratio = FMath::Clamp(BlindTimeRemaining / FMath::Max(BlindTotalDuration, 0.01f), 0.f, 1.f);
	const float Intensity = FMath::Clamp(Ratio * 1.45f, 0.f, 1.f);
	if (Intensity <= 0.01f) return LayerId;

	// 爆炸点在屏幕里的位置：世界坐标投影（Detonate 时 EffectCauser=闪光弹）。
	// 取不到（投影失败/无 Causer）时回退屏幕中心。
	FVector2D BlobCenter = Size * 0.5f;
	if (GetOwningPlayer() && !BlastWorldPos.IsZero())
	{
		FVector2D ScreenPos;
		if (GetOwningPlayer()->ProjectWorldLocationToScreen(BlastWorldPos, ScreenPos))
		{
			BlobCenter.X = FMath::Clamp(ScreenPos.X, 0.f, Size.X);
			BlobCenter.Y = FMath::Clamp(ScreenPos.Y, 0.f, Size.Y);
		}
	}

	// 屏幕像素(左上原点,y 向下) → 材质 TexCoord(0) 的 brush UV(左上原点,y 向下)：
	// uv.X = px.X / ViewportW, uv.Y = px.Y / ViewportH
	FVector2D ViewportSize = Size;
	if (GetOwningPlayer())
	{
		int32 VpX = 0, VpY = 0;
		GetOwningPlayer()->GetViewportSize(VpX, VpY);
		if (VpX > 1 && VpY > 1)
		{
			ViewportSize = FVector2D((float)VpX, (float)VpY);
		}
	}
	const FVector2D CenterUV(FMath::Clamp(BlobCenter.X / FMath::Max(ViewportSize.X, 1.f), 0.f, 1.f),
		FMath::Clamp(BlobCenter.Y / FMath::Max(ViewportSize.Y, 1.f), 0.f, 1.f));

	// 动态材质实例（延迟创建）；把每帧参数灌进材质，用材质 brush 画全屏
	// NativePaint 是 const：FlashMID 成员用 const_cast<TObjectPtr<>&> 转可变引用后创建
	TObjectPtr<UMaterialInstanceDynamic>& MidRef = const_cast<TObjectPtr<UMaterialInstanceDynamic>&>(FlashMID);
	if (!MidRef && FlashMaterial)
	{
		MidRef = UMaterialInstanceDynamic::Create(FlashMaterial, const_cast<UFlashEffectWidget*>(this));
	}
	if (!MidRef)
	{
		return LayerId;
	}

	MidRef->SetVectorParameterValue(TEXT("Center"), FLinearColor(CenterUV.X, CenterUV.Y, 0.f, 0.f));
	MidRef->SetScalarParameterValue(TEXT("Intensity"), Intensity);

	const FSlateMaterialBrush MaterialBrush(*MidRef, FVector2f(1.f, 1.f));
	const FPaintGeometry Geom = AllottedGeometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geom, &MaterialBrush, ESlateDrawEffect::None);
	++LayerId;

	return LayerId;
}
