// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimNotify_KnifeConsumed.h"

#include "AnimNotify_ReloadFinished.h"	// 复用它的 ResolveCharacter（从动画上下文反查角色）
#include "Blaster/Character/Agents/JettCharacter.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_KnifeConsumed::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	/*
	 * 反查角色的逻辑和换弹/掏枪/空手那几条通知共用一份
	 *（ANimNotify_ReloadFinished::ResolveCharacter —— 手模 / 角色网格 / 武器网格三种 owner 都吃）。
	 * 那边改动这边自动跟上，不会出现"几个入口行为不一样"。
	 */
	AJettCharacter* Jett = Cast<AJettCharacter>(UAnimNotify_ReloadFinished::ResolveCharacter(MeshComp));

	// 查不到、或者不是捷风 → 什么都不做。
	// 查不到是正常情况：Persona 里预览动画、编辑器里空跑都会走到这里，不是错误。
	// 不是捷风才需要判 —— 这条通知挂在捷风自己的蒙太奇上，但资产被复制到别的角色身上、
	// 或者以后有人共用这条蒙太奇时，不该去动别人的状态。
	if (Jett == nullptr) return;

	/*
	 * 让角色按当前弹匣重算 5 把小刀的显隐。
	 *
	 * 传到这里的只有"哪一帧"，没有"第几把"—— 那一份状态（还剩几把）在读弹匣的那一刻现算，
	 * 所以这里不需要判断是谁在播、播的是第几段、是不是本机。
	 * （详细理由见头文件：通知只负责时机，数量以武器弹匣为准。）
	 */
	Jett->SyncKnivesToAmmo();
}

FString UAnimNotify_KnifeConsumed::GetNotifyName_Implementation() const
{
	return TEXT("Knife Consumed");
}
