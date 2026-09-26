// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_KnifeConsumed.generated.h"

/*
 * ——— 飞刀离手的动画通知 ———
 *
 * 用户在需求里明确要的："每攻击一下少一个（不可见即可），这个消失的时机你留一个动画通知给我"。
 * 也就是"哪一帧算扔出去了"由**动画**说了算，不由 C++ 猜时间。
 *
 * 怎么用：拖进**刀骨骼那条攻击蒙太奇**（AJettCharacter::KnifeAttack，资产是 AB_Wushu_S0_X_Attack）
 * 每一段里"刀已经脱手"的那一帧上。5 段各挂一条（同一个类，挂 5 次，段号靠蒙太奇自己的分段区分）。
 *
 * —— 它做的事只有一件：反查到角色，让它按当前弹匣重算一遍 5 把小刀的显隐 ——
 *
 * **"还剩几把"不是这个通知数的**，读的是武器的弹匣（AJettKnives::Ammo —— 它已经复制、
 * 已经本地预测、HUD 上显示的也是它）。通知只负责"什么时候看一眼"。
 * 这么分工解决了三件事，而它们用"每响一次藏一把"的写法都得单独补修正：
 *   · 蒙太奇被跳段、或连播到后面几段（每次扔刀都是从头重播再跳段，后面几段的刀可能被播到）
 *     → 重算还是同一个结果，不会多藏刀
 *   · 击杀刷新把刀补回 5 把 → 下一次通知照样算得对
 *   · 服务器把客户端本地预测的刀数纠正回来 → 下一次通知照样算得对
 *
 * ⚠️ 挂在**手模**那条（AJettKnives::FPFireMontage）上也能生效（这里不筛网格 ——
 *    重算是幂等的，同一帧响两次结果一样，删掉那层筛选纯粹是为了"挂哪条都对"）。
 *    但一般挂在刀骨骼那条上就够了：它在所有机器上都播，和"每个人屏幕上看到的刀一样多"对得上。
 *
 * ⚠️ 想在 ABP 事件图里接（AnimNotify_XXX 那种事件），别用这条带类的通知：
 *    带通知类的通知**不会**触发 ABP 的 AnimNotify_* 事件（引擎里两条路是互斥的）。
 *    那种做法要改用轨道右键菜单里的 "New Notify..."（只给名字、不给类），
 *    事件里再调 AJettCharacter::SyncKnivesToAmmo()。和 UAnimNotify_EmptyHandFinished 是同一个坑。
 *
 * ⚠️ 段里没挂这条通知也不会出大问题：只是"那一把刀"要等到**下一刀**的通知才会消失
 *   （显隐每次都是重算的，不是增量的）—— 表现上像是"慢了一刀"，但不会错到底。
 */
UCLASS(meta = (DisplayName = "Knife Consumed"))
class BLASTER_API UAnimNotify_KnifeConsumed : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
