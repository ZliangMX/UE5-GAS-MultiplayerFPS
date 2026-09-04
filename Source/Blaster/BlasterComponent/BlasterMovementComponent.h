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

protected:
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;

private:
	FVector BlasterDashDirection = FVector::ZeroVector;
	float BlasterDashSpeed = 0.f;
	float BlasterDashTimeRemaining = 0.f;
};
