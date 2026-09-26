// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "CloveCharacter.generated.h"

/*
 * Clove（暮蝶）—— BP_CloveCharacter 的 C++ 父类。
 *
 * 目前是空的"英雄专属代码落点"，继承层次见 JettCharacter.h 里的说明。
 *
 * 将来要搬进来的是**封烟**那一整段：地图选点界面确定后落烟
 *（PlaceCloveSmokeAt / ServerPlaceCloveSmoke / ServerPlaceCloveSmokes）、
 *充能核销（ApplyCloveSmokeUsed）、以及 CloveSmokeClass / CloveSmokeTraceUpOffset /
 * CloveSmokeTraceDownDepth 这几个参数。
 * 注意这几个函数现在是 **public** 的 —— ABlasterPlayerController 持着
 * ABlasterCharacter* 直接调（界面点确定那条路），搬的时候要一起改成具体英雄类。
 */
UCLASS()
class BLASTER_API ACloveCharacter : public ABlasterCharacter
{
	GENERATED_BODY()

public:
	ACloveCharacter(const FObjectInitializer& ObjectInitializer);
};
