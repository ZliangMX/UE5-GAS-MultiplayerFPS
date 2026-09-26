// Jett 冲刺位移用的自定义移动组件。
// 冲刺 = MOVE_Custom 的自定义模式（EBlasterCustomMovementMode::Dash），在 PhysCustom 里每帧
// SafeMoveUpdatedComponent 推进。CharacterMovementComponent 自带完整的**客户端预测**机制
// （本地 ControlledCharacterMove + ServerMove 回放 + SmoothCorrection 平滑修正）——
// 本地玩家按 E 立即开冲（不卡），服务器权威回放 + 回包修正（防作弊、撞墙一致）。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "BlasterMovementComponent.generated.h"

// 自定义移动模式编号（MOVE_Custom 时的 CustomMovementMode）
UENUM(BlueprintType)
enum class EBlasterCustomMovementMode : uint8
{
	None UMETA(Hidden),
	// 冲刺（Jett E 第二段 / 任何顺风冲刺类技能）
	Dash UMETA(DisplayName = "Dash"),
	MAX UMETA(Hidden)
};

// 冲刺结束（自然到期 / 撞墙停）通知：能力绑定它做 EndAbility 收尾。
// 非动态多播（仅 C++ 用），绑定用 AddUObject + 普通成员函数。
DECLARE_MULTICAST_DELEGATE(FBlasterDashFinishedSignature);

UCLASS()
class BLASTER_API UBlasterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UBlasterMovementComponent();

	// 开始冲刺（客户端/服务器都调用：客户端本地预测、服务器权威回放）。
	// Direction 会被归一化；Speed/Duration 由能力提供（DashSpeed / DashDuration）。
	// 重复调用 = 重新开始冲刺（先恢复上次冲刺）。
	void StartDash(const FVector& Direction, float Speed, float Duration);

	// 结束冲刺并恢复行走（不广播 OnBlasterDashFinished——被动打断如取消/死亡用）
	void StopDash();

	// 冲刺自然结束 / 撞墙停（能力绑定，用于 EndAbility 收尾）
	FBlasterDashFinishedSignature OnBlasterDashFinished;

	// —— 冲刺手感调参 ——
	// 冲刺速度曲线（0~1）：冲刺内速度从峰值线性降到 本值×峰值。
	//   1.0 = 全程匀速（改动前的老行为）
	//   0.0 = 一路减到静止（最"软"的收尾）
	//   0.1 = 末尾还剩一成（默认）
	// ★ DashSpeed 的含义仍是"平均速度"：速度曲线首尾取平均后恒等于它，
	//   所以调本值只改"起手多猛 / 结尾多软"，**不改变冲刺总距离**。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blaster|Dash")
	float DashEndSpeedScale = 0.1f;

	// 冲刺期间是否接续起手那一刻的竖直速度（Q 腾空中按 E：水平交给冲刺，上升继续）。
	// 只在"起手时人在空中"生效；站在地上（含斜坡）时 Velocity.Z 恒为 ~0，本项无影响。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blaster|Dash")
	bool bInheritVerticalVelocity = true;

	// 冲刺结束时把"已经降下来"的残余速度交还给目标移动模式（走路/下落），
	// 由 CMC 的刹车（BrakingDecelerationWalking）/重力自然收尾，而不是最后一帧硬清零。
	// 关掉 = 回到"结尾一刀切停"。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blaster|Dash")
	bool bCarryExitVelocity = true;

protected:
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;

private:
	// 冲刺收尾的统一出口：bCarryVelocity = 是否把残余速度交还给走路/下落
	void FinishDash(bool bCarryVelocity);

	FVector BlasterDashDirection = FVector::ZeroVector;
	// 名义速度 = 平均速度（能力传进来的 DashSpeed），只用于推算峰值
	float BlasterDashSpeed = 0.f;
	float BlasterDashTimeRemaining = 0.f;
	// 本次冲刺的总时长 / 起手峰值速度 / 当前这一帧的速度（退出时要用它做残余速度）
	float BlasterDashTotalDuration = 0.f;
	float BlasterDashPeakSpeed = 0.f;
	float BlasterDashCurrentSpeed = 0.f;
	// 冲刺期间接管的竖直速度（0 = 地面冲刺，本帧不做任何 Z 位移）
	float BlasterDashVerticalVelocity = 0.f;
};
