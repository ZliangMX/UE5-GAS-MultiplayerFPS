// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_LobbyHeldFireball.generated.h"

/*
 * ——— "他手上那颗火球现在出现 / 消失"的动画通知（大厅选人动画用）———
 *
 * 用户的需求原话："火男的动画留个动画通知给我，要在那个时机往 R_WeaponPoint 上绑个闪光弹的火球"。
 * 也就是**哪一帧算"火球搓出来了"由动画说了算**，不由 C++ 按资产长度猜秒数 ——
 * 和 UAnimNotify_KnifeConsumed（飞刀离手）、UAnimNotify_ThrowableSkillFinished（投掷收尾）
 * 是同一个思路，只是这条管的是大厅展示用的角色。
 *
 * ── 怎么用 ──
 * 把这条通知拖进**选人动画**（Content/ValorantAssets/CharSelect 下那几条，比如
 * CS_Phoenix_S0_CharSelect_Intro / CS_Phoenix_S0_Idle）里"火球该出现"的那一帧上。
 * 想让它到某个时刻消失，就在那一刻再摆一条，把 bShow 取消勾选。
 *
 * ★ 裸 AnimSequence 上摆的通知**照样会响**，不用为了这条通知去做蒙太奇：
 *   大厅是用 USkeletalMeshComponent::PlayAnimation 播序列的，走单节点动画实例，
 *   而它内部同样会把通知塞进 NotifyQueue 再派发
 *   （引擎源码：AnimSingleNodeInstanceProxy.cpp 的 FAnimNode_SingleNode::Update_AnyThread
 *   → AnimInstanceProxy.cpp 的 TickRecord.SourceAsset->TickAssetPlayer(..., NotifyQueue, ...)
 *   → UAnimInstance::TriggerAnimNotifies）。这一点和 ABlasterCharacter 那边
 *   "裸序列当资产、通知也会响"的结论是同一条。
 *
 * ── 它做的事只有一件：反查到大厅的展示 actor，让它开关火球 ──
 * 挂点、大小、偏移都在 ALobbyAgentShowcase 上（FireballPlacement），
 * 通知只负责"什么时候"；改挂点不用动动画，改时机不用动 C++。
 *
 * ⚠️ 这是**类通知**（UAnimNotify 子类），不会触发动画蓝图里那些 AnimNotify_* 事件 ——
 *    要蓝图那边同时收到的话得用 UAnimNotify_BlueprintSpawnable 或者加个 AnimNotifyState。
 *    （和 UAnimNotify_ThrowableSkillFinished 踩的是同一个坑。）
 */
UCLASS(meta = (DisplayName = "Lobby Held Fireball"))
class BLASTER_API UAnimNotify_LobbyHeldFireball : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

protected:
	// 勾上（默认）= 火球出现并挂到 FireballPlacement.Socket（默认 R_WeaponPoint）上；
	// 取消勾选 = 把火球收起来。
	//
	// 只在"该英雄确实挂了这个通知"时有意义 —— 通知是**全局按资产**摆的，
	// 谁播这条动画谁就吃这条通知（选人动画只有大厅在播，不用担心串味）。
	UPROPERTY(EditAnywhere, Category = "Lobby", meta = (DisplayName = "显示火球"))
	bool bShow = true;
};
