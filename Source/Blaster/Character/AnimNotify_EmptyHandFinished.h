// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_EmptyHandFinished.generated.h"

/*
 * ——— 空手蒙太奇结束的动画通知 ———
 *
 * 和 UAnimNotify_ReloadFinished / UAnimNotify_EquipFinished 是三兄弟，形状用法一模一样，
 * 管的对象是 ECS_EmptyHand：它把状态放回 ECS_Unoccupied 并**自动掏出最强的武器**
 *（主武器优先，没主武器掏副武器），也就是「这段空手演完了」由**动画**说了算。
 *
 * 怎么用：拖进**身体那条**动画（技能资产上的 ThirdPersonUpper / ThirdPersonLower）的收尾时刻。
 * 挂在手模（FirstPerson）那条上**不作数**，会被 C++ 直接滤掉 —— 那条只在射手本机播，
 * 拿它收尾等于"这件事在不同机器上有不同时刻"，而收尾要改的是复制出去的状态。
 * 函数自带幂等门禁，上下半身两条都挂也没关系：先到的收尾、后到的空转。
 *
 * ★ 填裸 AnimSequence 也照样会响：那种情况下 C++ 会按槽名现造一条动态蒙太奇把序列塞进去播，
 *   而引擎 FAnimMontageInstance::HandleEvents 会把 SlotAnimTracks 里那条序列自己的通知
 *   一起派发（见 ABlasterCharacter::PlayEmptyHandTrack 的注释）。不用为了这条通知去做蒙太奇。
 *
 * ⚠ 和保险丝（ABlasterCharacter::StartEmptyHandTimer）的关系：这条通知是**主路**，
 *   保险丝是备胎，两边都恒起但不会抢 —— 通知一到，收尾时顺手就把保险丝清掉；
 *   通知没响（这条资产上没挂 / 只挂在别的分段 / 换骨架换 ABP 弄丢了），保险丝才到点收尾。
 *   挂着这条通知时保险丝的时长会额外往后推 EmptyHandFuseExtraDelay，所以正常情况
 *   通知一定先到，顺序不取决于同一帧里组件 tick 的次序。
 *   ABlasterCharacter::EmptyHandFinish() 自带幂等门禁，重复响也不会掏两次枪。
 *
 * ⚠ 想在 ABP 事件图里接（AnimNotify_XXX 那种事件），别用这条 C++ 通知：带通知类的通知
 *   **不会**触发 ABP 的 AnimNotify_* 事件（引擎里两条路是互斥的）。那种做法要改用轨道右键
 *   菜单里的 "New Notify..."（只给名字、不给类），事件里再调角色上的 EmptyHandFinish。
 */
UCLASS(meta = (DisplayName = "Empty Hand Finished"))
class BLASTER_API UAnimNotify_EmptyHandFinished : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
