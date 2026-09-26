// Fill out your copyright notice in the Description page of Project Settings.

#include "AnimNotify_EmptyHandFinished.h"
#include "AnimNotify_ReloadFinished.h"	// 复用它的 ResolveCharacter（从动画上下文反查角色）
#include "BlasterCharacter.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_EmptyHandFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	// 反查逻辑和换弹/掏枪那两条通知共用一份（手模 / 角色网格 / 武器网格三种 owner 都吃），
	// 那边改动这边自动跟上，不会出现"三个入口行为不一样"。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(MeshComp);
	if (Character == nullptr) return;

	/*
	 * ★ 手模（第一人称手臂）上响的这条**不作数**。
	 *
	 * 收尾只认身体那条（ThirdPersonUpper / ThirdPersonLower）—— 它在所有机器上都播，包括专用
	 * 服务器；手模那条只在射手本机播（别的机器上 FPArmsMesh 连组件 tick 都是关的）。
	 * 拿手模那条当依据 = "这段空手演完了"这件事在不同机器上有不同的时刻，而收尾要改的是
	 * 复制出去的状态，只能有一个时刻。
	 *
	 * 具体后果（就是它逼出来的那条设计）：两条通知的时间差不到一帧，谁先到全看组件 tick 次序
	 * —— 手模先到的那次，状态已经被放回 ECS_Unoccupied，另一条收尾路（当时是冲刺结束的兜底）
	 * 再判断时幂等门禁失效，于是收枪 + 重进空手，冲刺动画从头再播一遍。
	 * 现在收尾只认"身体那条"，手模这条直接走人；身体那条没响时还有保险丝兜底
	 *（见 ABlasterCharacter::StartEmptyHandTimer）。
	 */
	if (MeshComp == Character->GetFPArmsMesh())
	{
		return;
	}

	/*
	 * 幂等（状态已经不是 ECS_EmptyHand 就直接走人）和"只有服务器真的收尾"都在 EmptyHandFinish()
	 * 里面，所以客户端这条通知响一下也无害：它只是"我这台机器的动画播完了"。
	 */
	Character->EmptyHandFinish();
}

FString UAnimNotify_EmptyHandFinished::GetNotifyName_Implementation() const
{
	return TEXT("Empty Hand Finished");
}
