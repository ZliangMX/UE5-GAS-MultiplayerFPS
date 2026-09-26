// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "JettBladeStormAbility.generated.h"

/*
 * Jett 大招「刃风暴 / Blade Storm」。
 *
 * 按 X：掏出飞刀（AJettKnives）并进入刃风暴，Duration 秒后自动收刀。
 * 期间左键扔一把、右键一次性把剩下的全甩出去；扔完 / 到期 / 阵亡都会收刀。
 *
 * 扣点时机：**大招结束才扣**（bSpendUltPointsOnActivate = false）——
 * "飞刀用完，大招充能开始重新算"。这点和 Phoenix「回到标记点才扣」是同一套思路：
 * 时机按技能定，不在基类的"按下即扣"上。真正的扣点在
 * ABlasterCharacter::ServerEndBladeStorm()，扔完 / 到期 / 阵亡三条路共用它，只扣一次。
 * ⚠️ 所以大招生效期间大招点是**满的**（没花掉）—— 这期间击杀/吃球攒的点会被上限夹掉，
 *    结束时统清零重算。
 *
 * 数值在哪调：
 *   刀数 / 单刀伤害 / 爆头倍率 / 扔刀间隔 / 拖尾  → BP_JettKnives（飞刀武器蓝图）
 *   Duration（技能持续时间）                    → 本能力的蓝图子类
 * 所以这个类**需要**一个蓝图子类（因为要填 KnivesClass 和把 Duration 调顺手），
 * 不像 Phoenix 那个能直接塞进 DefaultAbilities。
 */
UCLASS()
class BLASTER_API UJettBladeStormAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UJettBladeStormAbility();

	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override;

protected:
	virtual void ExecuteSkillAction() override;

	// 飞刀武器类（BP_JettKnives）。**必填** —— 留空按 X 什么都不会发生，会打一条日志骂人。
	// 每次放大招现生成一把新的：刀数/伤害是每个 Jett 各自的，而且上一轮的刀不该被继承。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|BladeStorm")
	TSubclassOf<class AJettKnives> KnivesClass;

	// 大招持续时长（秒）。Valorant 里没有硬性倒计时（刀留到回合结束/阵亡），
	// 这里给一个上限让"自动收刀"有个兜底；设 <= 0 表示不自动收（刀一直在手上，
	// 直到扔完 / 阵亡 / 回合结束）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|BladeStorm")
	float Duration = 45.f;

	// 掏出飞刀时给几把。<= 0 用武器蓝图自己的 MagCapacity（推荐，数值只在一处调）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|BladeStorm")
	int32 KnifeCount = 0;
};
