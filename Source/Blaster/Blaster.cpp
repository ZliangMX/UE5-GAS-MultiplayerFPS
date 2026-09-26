// Copyright Epic Games, Inc. All Rights Reserved.

#include "Blaster.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, Blaster, "Blaster" );

bool ActivateNiagaraFX(UNiagaraComponent* Comp, UNiagaraSystem* Preferred)
{
	// 说明见 Blaster.h 里的那段长注释（组件 Asset 槽 vs actor 属性那个坑）
	if (Comp == nullptr) return false;

	UNiagaraSystem* Effect = Preferred ? Preferred : Comp->GetAsset();
	if (Effect == nullptr) return false;   // 两边都没配 → 让调用方走兜底视觉

	if (Comp->GetAsset() != Effect)
	{
		Comp->SetAsset(Effect);
	}

	// true = 重置并立刻开始。这些组件都是下一次生成时复用的同一个模板对象，
	// 不 reset 的话会继承上一发的粒子状态。
	Comp->Activate(true);
	return true;
}
