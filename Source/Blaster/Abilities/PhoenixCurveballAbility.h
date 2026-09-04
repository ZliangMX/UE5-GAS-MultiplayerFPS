// Phoenix 曲线球技能：左键/右键抛出一个沿弧线飞行的闪光球（APhoenixCurveball）。
// 施放 = 读取客户端 RPC 上来的拐弯方向（左键=左拐 / 右键=右拐），按准心水平朝向抛掷。
// ⚠️ LocalPredicted 客户端预测实例也会执行 ExecuteSkillAction，Spawn 前必须守卫服务器权威。

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "PhoenixCurveballAbility.generated.h"

class APhoenixCurveball;

UCLASS()
class BLASTER_API UPhoenixCurveballAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UPhoenixCurveballAbility();

protected:
	virtual void ExecuteSkillAction() override;

	// 抛掷用的曲线球类（headless 配 BP_PhoenixCurveball_C）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Curveball")
	TSubclassOf<APhoenixCurveball> CurveballClass;

	// 出生点相对角色面朝/身高偏移（前方 150cm、眼睛高度 160cm 抛出，避免贴地）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Curveball")
	float SpawnForwardOffset = 150.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Curveball")
	float SpawnUpOffset = 160.f;
};
