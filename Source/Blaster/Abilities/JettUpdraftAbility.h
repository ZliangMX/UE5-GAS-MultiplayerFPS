// Jett 的 Q「腾空 / Updraft」：按一下原地笔直起飞，不带任何水平位移。
//
// 实现就是给角色一个纯竖直的起跳速度（ACharacter::LaunchCharacter）。不自己推位移、
// 不走 UBlasterMovementComponent 的自定义冲刺模式 —— 腾空没有"持续施法"的阶段，
// 一个初速度交给 CharacterMovement 的重力和空气控制就够了，而且竖直运动本来
// 就在 CharacterMovement 的预测/回放链路里，客户端按下去立刻起飞、服务器权威一致。
//
// 充能：一格，**局内不恢复**。做法是配一个 DurationPolicy = Infinite 的冷却 GE ——
// 无限时长的效果不会到期，所以 GetTimeUntilNextCharge 恒为 0（HUD 不画倒计时），
// 充能点永远是灰的。回合结束角色整个被销毁重建（BlasterGameMode::RespawnAllPlayers），
// 新 ASC 上没有这个 GE，充能自然回满 —— 不需要任何"回合开始清冷却"的额外代码。
//
// 这个类**需要**一个蓝图子类（GA_Jett_Updraft），因为它要填 CooldownEffectClass 和图标。

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "JettUpdraftAbility.generated.h"

UCLASS()
class BLASTER_API UJettUpdraftAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UJettUpdraftAbility();

protected:
	virtual void ExecuteSkillAction() override;

	// 起飞初速度（cm/s，纯 Z 方向，会被直接写进竖直速度）。
	// ⚠️ 这个值**不是**起跳高度，高度 = v² / (2g)。g = 980 时：
	//    650 → 约 2.2m（能上到正常跳不上去的箱子）；1000 → 约 5.1m（Valorant 手感偏这个档）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Updraft")
	float UpdraftSpeed = 1000.f;

	// 起飞前把当前竖直速度清零（只清 Z，水平保留）。
	// 开启：不管是在下落还是在上跳，最终竖直速度都正好是 UpdraftSpeed，高度可预期。
	// 关闭：从高处掉下来时按 Q，下落速度会被直接相加，掉得越快飞得越高。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Updraft")
	bool bClearVerticalVelocity = true;
};
