#include "BlasterMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Character.h"

UBlasterMovementComponent::UBlasterMovementComponent()
{
	// 预测/回放全部走 CMM 自带机制（本地 ControlledCharacterMove + ServerMove），无需额外设置。
}

void UBlasterMovementComponent::StartDash(const FVector& Direction, float Speed, float Duration)
{
	// 若上一次冲刺还没冲完（重复按键/连续冲刺）先恢复，保证状态干净
	StopDash();

	BlasterDashDirection = Direction.GetSafeNormal();
	if (BlasterDashDirection.IsNearlyZero())
	{
		BlasterDashDirection = FVector::ForwardVector;
	}
	BlasterDashSpeed = FMath::Max(Speed, 0.f);
	BlasterDashTimeRemaining = FMath::Max(Duration, 0.f);

	SetMovementMode(MOVE_Custom, (uint8)EBlasterCustomMovementMode::Dash);
	Velocity = FVector::ZeroVector;
}

void UBlasterMovementComponent::StopDash()
{
	if (MovementMode == MOVE_Custom && CustomMovementMode == (uint8)EBlasterCustomMovementMode::Dash)
	{
		SetMovementMode(MOVE_Walking);
	}
	BlasterDashTimeRemaining = 0.f;
	BlasterDashDirection = FVector::ZeroVector;
	BlasterDashSpeed = 0.f;
}

void UBlasterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	Super::PhysCustom(deltaTime, Iterations);

	if (CustomMovementMode != (uint8)EBlasterCustomMovementMode::Dash)
	{
		return;
	}

	// 确定性推进：客户端本地预测和服务器回放走同一份代码、同一状态（方向/速度/剩余时长都存本组件），
	// 两端位置一致 → 回包修正量小、无卡顿无回弹。
	BlasterDashTimeRemaining -= deltaTime;

	const float Step = BlasterDashSpeed * deltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(BlasterDashDirection * Step, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (Hit.bBlockingHit || BlasterDashTimeRemaining <= 0.f)
	{
		StopDash();
		OnBlasterDashFinished.Broadcast();
	}
}
