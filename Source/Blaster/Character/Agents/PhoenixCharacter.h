// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "PhoenixCharacter.generated.h"

/*
 * Phoenix（火男）—— BP_PhoenixCharacter 的 C++ 父类。
 *
 * 目前是空的"英雄专属代码落点"，继承层次见 JettCharacter.h 里的说明。
 *
 * 将来要搬进来的是两块：
 *   ① 曲线球（闪光）/ 火球 的**持投掷物**状态：EnterThrowableHold / ExitThrowableHoldLocal /
 *      ThrowCurveball（左右拐方向走 ServerSetPendingCurveballSide）/ ThrowFireball /
 *      ServerSetThrowableHolding / ThrowableHolsteredWeapon / bThrowableHolding /
 *      HeldThrowableKind。
 *   ② 大招「再来一次」：RunItBackLocation / RunItBackRotation / ServerReturnToRunItBack
 *      （以及 ServerElim 里那条"死亡拦截"分支）。
 * ⚠️ ② 和 Jett 的刃风暴共用 ActiveUltimate 那套状态机（在基类），搬的时候别把状态机一起搬走。
 */
UCLASS()
class BLASTER_API APhoenixCharacter : public ABlasterCharacter
{
	GENERATED_BODY()

public:
	APhoenixCharacter(const FObjectInitializer& ObjectInitializer);
};
