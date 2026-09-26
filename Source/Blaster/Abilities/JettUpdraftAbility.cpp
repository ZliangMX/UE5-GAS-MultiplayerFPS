#include "JettUpdraftAbility.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UJettUpdraftAbility::UJettUpdraftAbility()
{
	// 一次性技能：按下即施放，没有武装（逐风那种两段式）阶段
	bUseArmedActivation = false;

	// 一格充能，局内不恢复（冷却 GE 配成 Infinite，见头文件）
	MaxCharges = 1;
}

void UJettUpdraftAbility::ExecuteSkillAction()
{
	ACharacter* Char = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Char)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	// ⚠️ 这里**故意不判服务器权威**。LocalPredicted 下客户端预测实例也会跑进来，
	// 而我们要的正是它跑：本机按 Q 立刻起飞（不等服务器来回一趟），服务器那份自己也算一遍，
	// 两边结果一致 → SmoothCorrection 不需要修正。抛投类技能（曲线球/风墙）必须守权威是因为
	// SpawnActor 只能服务器做，这里只是改自己的速度，两边都算才是对的。
	//
	// 和 Jump() 一样走 LaunchCharacter 而不是直接改 Velocity：它会走完整的
	// PerformMovement → ServerMove 回放链路，服务器回包不会把这次起飞"修正掉"。
	FVector LaunchVelocity = FVector(0.f, 0.f, UpdraftSpeed);

	if (!bClearVerticalVelocity)
	{
		// 叠加当前竖直速度（上升中按 Q 会更高，下落中按 Q 会被抵消一部分）
		LaunchVelocity.Z += Char->GetVelocity().Z;
	}

	// bXYOverride = false（水平速度原样保留，腾空不带位移）
	// bZOverride   = true （竖直速度直接写成上面算的值 = 清掉旧的下落/上升速度）
	Char->LaunchCharacter(LaunchVelocity, /*bXYOverride=*/false, /*bZOverride=*/true);

	// 即放即结束：起飞是瞬时动作，能力不留实例。
	// 充能已由基类在服务器侧扣掉（ApplyChargeCooldown）。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
