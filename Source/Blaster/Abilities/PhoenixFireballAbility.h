// Phoenix Q「火球 / Hot Hands」的施放能力。
//
// 和 E 曲线闪光同一套路数：**Q 只是"拿起火球"**（收枪、进持球态，见
// ABlasterCharacter::EnterThrowableHold），真正丢出去那一下才激活这个能力，
// 能力只干一件事 —— 按准心方向生成一颗 APhoenixFireball。
// 所以这个类没有"按住/松开"那套，也没有充能之外的时序逻辑：球出生之后
// 飞多远、什么时候下坠、落在哪，全是球自己的事。
//
// ⚠️ LocalPredicted 客户端预测实例也会执行 ExecuteSkillAction，Spawn 前必须守卫服务器权威
//    （否则每个客户端各生成一颗球，看着"球凭空多出来一份"）。
//
// 充能/冷却由基类那份机制管（GA_Phoenix_HotHands 上配 MaxCharges + CooldownEffectClass），
// 这里一行都不用写。

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"

#include "PhoenixFireballAbility.generated.h"

class APhoenixFireball;

UCLASS()
class BLASTER_API UPhoenixFireballAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UPhoenixFireballAbility();

protected:
	virtual void ExecuteSkillAction() override;

	// 抛出去的那颗火球类（BP 里配 BP_PhoenixFireball_C）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Fireball")
	TSubclassOf<APhoenixFireball> FireballClass;

	// 出生点相对角色面朝/身高偏移（前方 150cm、胸口高度 160cm 抛出，避免贴地/贴脸）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Fireball")
	float SpawnForwardOffset = 150.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Fireball")
	float SpawnUpOffset = 160.f;
};
