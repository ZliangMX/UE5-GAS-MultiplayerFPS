// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimNotify_EquipFinished.h"
#include "AnimNotify_ReloadFinished.h"	// 复用它的 ResolveCharacter（从动画上下文反查角色）
#include "BlasterCharacter.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_EquipFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	// 反查逻辑和换弹那条通知共用一份（手模 / 角色网格 / 武器网格三种 owner 都吃），
	// 所以那边改动这边自动跟上，不会出现"两个入口行为不一样"。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(MeshComp);
	if (Character == nullptr) return;

	UCombatComponent* Combat = Character->GetCombatComponent();
	if (Combat == nullptr) return;

	/*
	 * 霰弹枪那种"逐发"的特例在这里不需要 —— 掏枪就是一段，没有循环。
	 * 幂等（状态已经不是 ECS_Equip 就直接走人）和"只在权威机上改状态"都在 EquipFinish() 里面，
	 * 所以客户端这条通知响一下也无害：它只是"我本机的动画播完了"。
	 */
	Combat->EquipFinish();
}

FString UAnimNotify_EquipFinished::GetNotifyName_Implementation() const
{
	return TEXT("Equip Finished");
}
