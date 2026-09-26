#include "BlasterMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Character.h"

UBlasterMovementComponent::UBlasterMovementComponent()
{
	// 预测/回放全部走 CMM 自带机制（本地 ControlledCharacterMove + ServerMove），无需额外设置。
}

void UBlasterMovementComponent::StartDash(const FVector& Direction, float Speed, float Duration)
{
	// ★ 必须在 StopDash() 之前读：StopDash 会把移动模式切成 Walking/Falling，
	//   而"起手时人在不在空中"决定了竖直速度要不要接续、退出时回哪个模式。
	//   判定用 CMC 自己的口径（MOVE_Falling/Flying），不行走地面/斜坡，
	//   否则地面冲刺第一帧就会把地面残值怼进地板、被 bBlockingHit 提前掐断。
	const bool bAirborneAtStart = (MovementMode == MOVE_Falling || MovementMode == MOVE_Flying);

	// 若上一次冲刺还没冲完（重复按键/连续冲刺）先恢复，保证状态干净
	StopDash();

	BlasterDashDirection = Direction.GetSafeNormal();
	if (BlasterDashDirection.IsNearlyZero())
	{
		BlasterDashDirection = FVector::ForwardVector;
	}
	// 冲刺只接管水平方向：Z 分量单独由 BlasterDashVerticalVelocity 负责
	BlasterDashDirection.Z = 0.f;
	if (BlasterDashDirection.IsNearlyZero())
	{
		BlasterDashDirection = FVector::ForwardVector;
	}

	BlasterDashSpeed = FMath::Max(Speed, 0.f);
	BlasterDashTotalDuration = FMath::Max(Duration, 0.f);
	BlasterDashTimeRemaining = BlasterDashTotalDuration;

	// 速度曲线：起手最快、末尾降到 EndSpeedScale 倍（详见头文件注释）。
	// 峰值 = 2·平均/(1+EndScale)，是为了让加速度曲线在时长上的积分仍然等于 平均×时长
	// → 总位移不变。EndScale=1 时峰值==名义速度 == 改之前那个"全程匀速"。
	const float EndScale = FMath::Clamp(DashEndSpeedScale, 0.f, 1.f);
	BlasterDashPeakSpeed = (EndScale < 1.f)
		? (BlasterDashSpeed * 2.f / (1.f + EndScale))
		: BlasterDashSpeed;
	BlasterDashCurrentSpeed = BlasterDashPeakSpeed;

	// ★ Q 腾空中按 E：竖直速度原样带进冲刺，冲刺只改水平方向。
	//   站在地上时不接（Velocity.Z 在地面本就是 ~0 或地面约束的小残值）。
	const float InheritedZ = (bInheritVerticalVelocity && bAirborneAtStart) ? Velocity.Z : 0.f;
	BlasterDashVerticalVelocity = (FMath::Abs(InheritedZ) > 1.f) ? InheritedZ : 0.f;

	// 水平交给脚本位移、竖直交给 BlasterDashVerticalVelocity —— 速度本体在冲刺中保持为零，
	// 与改之前一致（下游读到 0 速度的语义不变），残余速度只在退出那一帧写回。
	Velocity = FVector::ZeroVector;

	SetMovementMode(MOVE_Custom, (uint8)EBlasterCustomMovementMode::Dash);
}

void UBlasterMovementComponent::StopDash()
{
	// 外部主动打断（取消技能/死亡/重新冲刺）：直接停干净，与改之前一致
	FinishDash(/*bCarryVelocity=*/false);
}

void UBlasterMovementComponent::FinishDash(bool bCarryVelocity)
{
	if (MovementMode == MOVE_Custom && CustomMovementMode == (uint8)EBlasterCustomMovementMode::Dash)
	{
		// 退出时不再硬清零：把当前速度（曲线末尾已经降下来的那一份）交还给目标模式，
		// 剩下的由 CMC 自己收尾 —— 走路用 BrakingDecelerationWalking 刹、下落交给重力。
		// 这才是"结尾慢慢减"的后半段：与冲刺末段的速度连续，没有突变。
		// 撞墙/落地结束的冲刺不算在内（那是"撞停"，速度本就该丢掉）。
		if (bCarryVelocity && bCarryExitVelocity)
		{
			const bool bAirborne = (BlasterDashVerticalVelocity != 0.f);
			float ExitSpeed = FMath::Max(BlasterDashCurrentSpeed, 0.f);
			if (!bAirborne)
			{
				// 地面只保留到 MaxWalkSpeed：再快就不是"冲刺收尾"而是地上打滑了
				// （不能用 GetMaxSpeed()：此刻还是 MOVE_Custom，它返回的是 MaxCustomMovementSpeed）
				ExitSpeed = FMath::Min(ExitSpeed, MaxWalkSpeed);
			}

			Velocity = BlasterDashDirection * ExitSpeed;
			Velocity.Z = bAirborne ? BlasterDashVerticalVelocity : 0.f;
			SetMovementMode(bAirborne ? MOVE_Falling : MOVE_Walking);
		}
		else
		{
			Velocity = FVector::ZeroVector;
			SetMovementMode(BlasterDashVerticalVelocity != 0.f ? MOVE_Falling : MOVE_Walking);
		}
	}

	BlasterDashTimeRemaining = 0.f;
	BlasterDashTotalDuration = 0.f;
	BlasterDashDirection = FVector::ZeroVector;
	BlasterDashSpeed = 0.f;
	BlasterDashPeakSpeed = 0.f;
	BlasterDashCurrentSpeed = 0.f;
	BlasterDashVerticalVelocity = 0.f;
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
	// ★ 速度曲线也必须是纯函数（只看剩余时长与配置），任何随机/外部输入都会让两端对不上。
	const float EndScale = FMath::Clamp(DashEndSpeedScale, 0.f, 1.f);
	const float Alpha = (BlasterDashTotalDuration > 0.f)
		? FMath::Clamp(1.f - BlasterDashTimeRemaining / BlasterDashTotalDuration, 0.f, 1.f)
		: 1.f;
	BlasterDashCurrentSpeed = BlasterDashPeakSpeed * (EndScale + (1.f - EndScale) * (1.f - Alpha));

	BlasterDashTimeRemaining -= deltaTime;

	FVector Delta = BlasterDashDirection * (BlasterDashCurrentSpeed * deltaTime);

	// 竖直：只在起手时接续了竖直速度（Q 腾空中按 E / 下落中按 E）才动 Z。
	// 地面冲刺 BlasterDashVerticalVelocity 恒为 0，这里一帧都不会触发 —— 不会怼地板。
	if (BlasterDashVerticalVelocity != 0.f)
	{
		BlasterDashVerticalVelocity += GetGravityZ() * deltaTime;	// GetGravityZ() 是负数，已含 GravityScale
		Delta.Z = BlasterDashVerticalVelocity * deltaTime;
	}

	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (Hit.bBlockingHit || BlasterDashTimeRemaining <= 0.f)
	{
		// 只有"跑满时长自然结束"才把残余速度交还给走路/下落；
		// 撞墙/落地这种撞停不该再带着速度走。
		FinishDash(!Hit.bBlockingHit && BlasterDashTimeRemaining <= 0.f);
		OnBlasterDashFinished.Broadcast();
	}
}
