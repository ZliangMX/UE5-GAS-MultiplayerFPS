// 通用技能条（UMG Widget，NativePaint 全自绘）：把本地角色的全部技能横排显示。
//
// 数据链路：GetOwningPlayer → 本地角色 ASC → 遍历 ActivatableAbilities，收集所有
// UBlasterGameplayAbility 且 bShowInSkillBar=true 的，按 SkillSlotIndex 排序 →
// 每个技能 GetCooldownInfo(ASC) 拿充能/冷却，按 SkillType 画矢量图标 + 充能点 + 倒计时。
// 新技能接入只需在资产上配 SkillType / SkillSlotIndex，HUD 无需再改。
// 无 pawn（死亡/观战）时整块不画，天然兼容回合切换。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "SkillBarWidget.generated.h"

class UAbilitySystemComponent;
class UTexture2D;

UCLASS()
class BLASTER_API USkillBarWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	// 从 ASC 收集所有要显示在技能条上的技能（bShowInSkillBar 过滤 + 按 SkillSlotIndex 排序）
	void CollectVisibleSkills(const UAbilitySystemComponent* ASC, TArray<UBlasterGameplayAbility*>& OutSkills) const;

	// 画技能图标。Icon 有效就画贴图（按图标框等比缩放居中），否则按 SkillType 画矢量兜底形状。
	// SlotCenter 为图标框中心，IconHalf 为半边长；bDimmed = 没充能 → 整张图按 42% 透明度画灰
	//（素材里 _gray 那一版就是这个 42%，所以只需要一张白图，不用再导一份灰的）。
	void DrawSkillIcon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& SlotCenter, float IconHalf, EBlasterSkillType SkillType, UTexture2D* Icon, bool bDimmed) const;

	// --- 绘制辅助（widget 局部坐标；LayerId 递增保证后画的在上层）---
	static FSlateBrush MakeBrush(UTexture2D* Texture);
	void DrawBox(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color) const;
	void DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const;
	void DrawCircleFilled(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, const FLinearColor& Color) const;
	// 圆形扇形填充：从 StartRad 顺时针扫过 SweepRad（弧度）。SweepRad=2PI 即整圆。
	// 充能点"灰球内逐渐充满青色"用——Progress*2PI 就是 SweepRad。
	void DrawCircleSector(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, float StartRad, float SweepRad, const FLinearColor& Color) const;
	void DrawCircleRing(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, const FLinearColor& Color, float Thickness) const;
	void DrawText(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Position, const FString& Text, float FontSize, const FLinearColor& Color) const;
};
