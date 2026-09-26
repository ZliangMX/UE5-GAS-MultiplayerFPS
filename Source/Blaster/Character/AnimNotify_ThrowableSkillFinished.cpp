// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimNotify_ThrowableSkillFinished.h"
#include "AnimNotify_ReloadFinished.h"
#include "BlasterCharacter.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_ThrowableSkillFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(MeshComp);
	if (Character == nullptr) return;

	/*
	 * 只报"是手模还是身体"，**不过滤任何一条** —— 两条轨道的状态各管各的
	 *（手模那条只在射手本机有，身体那条别人也看得到），通知在哪儿响就切哪条，
	 * 摆错了最差也就是那一条不切（另一条的计时器兜底），不会有副作用。
	 *
	 * 判定用组件指针比对（和 EmptyHandFinished 同一招）：手模就是 FPArmsMesh，
	 * 不是它就是身体。注意**不要**改成"看 GetLocalRole"或者"看骨骼名" ——
	 * 射手本机那两个组件同时都在，只有指针能区分。
	 */
	const bool bFirstPerson = (MeshComp == Character->GetFPArmsMesh());
	Character->NotifyThrowableSkillFinished(bFirstPerson, Animation);
}

FString UAnimNotify_ThrowableSkillFinished::GetNotifyName_Implementation() const
{
	return TEXT("Throwable Skill Finished");
}
