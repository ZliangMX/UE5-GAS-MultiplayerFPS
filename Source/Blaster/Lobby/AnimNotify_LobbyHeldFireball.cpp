// Fill out your copyright notice in the Description page of Project Settings.

#include "AnimNotify_LobbyHeldFireball.h"

#include "LobbyAgentShowcase.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_LobbyHeldFireball::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (MeshComp == nullptr)
	{
		return;
	}

	// 大厅的展示 actor 把网格（ShowcaseMesh）当根组件，所以 owner 就是它自己 ——
	// 不需要像 UAnimNotify_ReloadFinished 那样从挂载链上反查角色。
	ALobbyAgentShowcase* Showcase = Cast<ALobbyAgentShowcase>(MeshComp->GetOwner());
	if (Showcase == nullptr)
	{
#if WITH_EDITOR
		// 编辑器里搓帧预览（Persona）、或者在别的地方播这条动画时，owner 不是大厅的展示 actor。
		// 预览是常态，不该刷警告；真正"摆错地方"那种情况在游戏里才报。
		if (MeshComp->GetWorld() != nullptr && !MeshComp->GetWorld()->IsGameWorld())
		{
			return;
		}
#endif
		UE_LOG(LogTemp, Warning,
			TEXT("[Lobby] 火球通知挂在 '%s' 上，但它的 owner 不是 ALobbyAgentShowcase（是 %s）—— 这条通知只对大厅选人动画有效，本次忽略"),
			*GetNameSafe(Animation), *GetNameSafe(MeshComp->GetOwner()));
		return;
	}

	Showcase->SetHandFireballVisible(bShow);
}

FString UAnimNotify_LobbyHeldFireball::GetNotifyName_Implementation() const
{
	// 通知轨道上显示的名字（带显/隐状态，一条轨道上摆两条时一眼能分清）
	return bShow ? TEXT("火球出现") : TEXT("火球消失");
}
