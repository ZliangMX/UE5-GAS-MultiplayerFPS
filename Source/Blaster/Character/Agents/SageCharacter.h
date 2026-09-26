// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "SageCharacter.generated.h"

/*
 * Sage（贤者）—— BP_SageCharacter 的 C++ 父类。
 *
 * 目前是空的"英雄专属代码落点"，继承层次见 JettCharacter.h 里的说明。
 *
 * 将来要搬进来的是**选中治疗**那一整段：进入/退出选人（EnterSageHealSelect /
 * ExitSageHealSelectLocal）、服务器执行治疗（ServerSageHeal）、目标筛选
 *（UpdateSageHealTarget / IsValidSageHealTarget）、冷却核销（ApplySageHealUsed）
 * 以及 SageHealAmount / SageHealRange / SageHolsteredWeapon 这些成员。
 * 注意那些成员现在被 HUD（技能条高亮）和 PC（E 键）按 ABlasterCharacter* 调。
 */
UCLASS()
class BLASTER_API ASageCharacter : public ABlasterCharacter
{
	GENERATED_BODY()

public:
	ASageCharacter(const FObjectInitializer& ObjectInitializer);
};
