// 被闪白屏效果（UMG Widget）：收到 GE_FlashBlind 期间全屏暖橙径向渐变渐隐。
// 渲染 = M_FlashBlind（UI 域材质，中心亮核→边缘深橙褐径向渐变），NativePaint 每帧
// set 材质参数 Center(爆炸点屏幕UV)/RadiusScale/Exponent/CoreColor/EdgeColor/Intensity。
// 数据链路：GetOwningPlayer → 角色 ASC → GetActiveEffectsTimeRemaining(FlashBlindEffectClass)
// 取最大剩余 → Intensity = f(剩余/总时长)。GE 复制到客户端后本 Widget 自动显示（零 RPC 链路）。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "FlashEffectWidget.generated.h"

class UGameplayEffect;
class UMaterialInterface;
class UMaterialInstanceDynamic;

UCLASS()
class BLASTER_API UFlashEffectWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	// 被闪时施加的 GE（headless 配 GE_FlashBlind）——按它查活跃实例剩余时间并取总时长
	UPROPERTY(EditDefaultsOnly, Category = "FlashEffect")
	TSubclassOf<UGameplayEffect> FlashBlindEffectClass;

	// 全屏闪光材质（headless 配 M_FlashBlind）：中心亮核→边缘深橙褐径向渐变
	UPROPERTY(EditDefaultsOnly, Category = "FlashEffect")
	TObjectPtr<UMaterialInterface> FlashMaterial;

private:
	// 动态材质实例（NativePaint 每帧 set 参数：Center/RadiusScale/Exponent/CoreColor/EdgeColor/Intensity）
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FlashMID;

	// 当前最长的剩余被闪时间（多颗闪光叠加取最长；无被闪 = 0）
	float BlindTimeRemaining = 0.f;
	// GE 总时长（从 CDO 读，兜底 1.75s），Intensity 归一化用
	float BlindTotalDuration = 1.75f;
	// 上一帧是否有被闪（进入/退出被闪瞬间打日志定位链路）
	bool bPrevBlind = false;
	// 闪光弹爆炸位置（世界坐标，从活跃 GE 的 EffectCauser=闪光弹读；无则回退屏幕中心）
	FVector BlastWorldPos = FVector::ZeroVector;
};
