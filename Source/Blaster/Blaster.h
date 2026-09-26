// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#define ECC_SkeletalMesh ECollisionChannel::ECC_GameTraceChannel1

class UNiagaraComponent;
class UNiagaraSystem;

/*
 * 把一个 Niagara 组件开起来（特效资产按优先级取）：
 *   ① Preferred —— actor 上的 UPROPERTY（如 APhoenixFireball::FlightEffect），**规范位置**
 *   ② 组件自己 Asset 槽里配的那个 —— 兜底
 * 返回 true = 组件已经带上有效资产并且激活了（调用方据此决定要不要藏掉兜底网格）。
 *
 * ⚠ 2026-09-21 踩过的坑（"我给火球配了 niagara 为啥局内看不见"）：
 *   BP 里换特效时，**组件的 Asset 槽**就在 Details 面板最上面，很自然就填那儿了；
 *   而这些组件在构造函数里一律 bAutoActivate=false（要等 BeginPlay 拿到方向/位置再开，
 *   不然生成瞬间会在原点先喷一下）。原来的写法是
 *       if (Effect && FXComp) { FXComp->SetAsset(Effect); FXComp->Activate(true); }
 *   —— 资产填在**组件**上时 Effect 是空的，整个 if 进不去：属性和组件都没开，
 *   于是"配了特效、局内什么都看不见"。火球那边还会露出兜底发光球（看着像特效没生效），
 *   另外三个（火墙/闪光球/火圈）连兜底都没有。
 *   现在两个地方填哪个都能用；推荐还是填 actor 上那个属性（换特效时不用去翻组件层级）。
 */
BLASTER_API bool ActivateNiagaraFX(UNiagaraComponent* Comp, UNiagaraSystem* Preferred);
