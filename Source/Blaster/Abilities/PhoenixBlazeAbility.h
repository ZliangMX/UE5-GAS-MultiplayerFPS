// Phoenix C「火墙 / Blaze」的施放能力。
//
// 和 E 曲线闪光 / Q 火球同一套路数：**按 C 只是"武装"**（收枪、进持投掷物态，
// 见 ABlasterCharacter::EnterThrowableHold）—— 武装那一步不消耗充能、也不生成任何东西；
// 真正激活这个能力的是"按住左键发射"那一拍（ABlasterCharacter::ServerStartBlazeFire）。
// 能力只干一件事：按准心方向生成一颗 APhoenixBlazeBall，球自己飞、自己让墙长出来。
//
// ★ 为什么这个类非存在不可（而不是让 GA_Phoenix_Blaze 直接继承 UBlasterGameplayAbility）：
//   基类的 ExecuteSkillAction() 默认实现是 **StartDash()** —— 那是给冲刺类技能（Jett E）用的。
//   让一个"火墙"能力走基类默认实现，按 C 发射的那一下会把角色**往前冲一段**。
//   所以这里必须覆写 ExecuteSkillAction，把默认的冲刺换成"生成球"。
//
// ⚠️ LocalPredicted 下客户端预测实例也会跑 ExecuteSkillAction，Spawn 前必须守卫服务器权威
//    （否则每台机器各生成一颗球 + 一面墙，看着就是"墙长了两遍"）。
//
// 充能/冷却由基类那份机制管（GA_Phoenix_Blaze 上配 MaxCharges + CooldownEffectClass），
// 这里一行都不用写。

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"

#include "PhoenixBlazeAbility.generated.h"

class APhoenixBlazeBall;

UCLASS()
class BLASTER_API UPhoenixBlazeAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UPhoenixBlazeAbility();

protected:
	virtual void ExecuteSkillAction() override;

	// 发射出去的那颗球类。默认留空 = 用 C++ 的 APhoenixBlazeBall（见 cpp 里的兜底），
	// 所以不填也能用；要换成带特效的 BP 版本就填 BP_PhoenixBlazeBall。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Blaze")
	TSubclassOf<APhoenixBlazeBall> BallClass;

	/*
	 * 出生点：眼睛位置 + 视角坐标系里的前 / 右 / 上偏移（和逐风云那套同一个算法）。
	 *
	 * 球是**隐形**的，所以这里不需要"贴着指尖"那种精度（也就不用去骨架里找 socket 了）——
	 * 唯一要讲究的是**别出生在射手自己脚下**：墙是从球的位置开始长出来的，
	 * 出生点太靠前，第一块板就会糊在射手脸上（往前走 150cm ≈ 一个身位开外）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Blaze")
	float SpawnForwardOffset = 150.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Blaze")
	float SpawnRightOffset = 20.f;

	// 略微往下一压：墙贴地，球贴近视平线飞更接近"贴着地面铺过去"的观感
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Blaze")
	float SpawnUpOffset = -30.f;
};
