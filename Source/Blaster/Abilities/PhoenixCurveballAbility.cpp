#include "PhoenixCurveballAbility.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Controller.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Weapon/PhoenixCurveball.h"

UPhoenixCurveballAbility::UPhoenixCurveballAbility()
{
}

void UPhoenixCurveballAbility::ExecuteSkillAction()
{
	// LocalPredicted：客户端预测实例也会执行这里，Spawn 只能服务器做（否则双份球 + 双份判定）
	if (!CurrentActorInfo || !CurrentActorInfo->IsNetAuthority())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo());
	if (!Char || !CurveballClass)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	// 拐弯方向：客户端按 E 时 RPC 上来（按住左/右方向键决定），默认向右
	bool bCurveLeft = false;
	Char->ConsumePendingCurveballSide(bCurveLeft);

	// 抛出方向 = 准心水平朝向（Z 归零；无控制器回退面朝方向）
	FVector Dir = FVector::ZeroVector;
	if (const AController* C = Char->GetController())
	{
		Dir = C->GetControlRotation().Vector();
	}
	else
	{
		Dir = Char->GetActorForwardVector();
	}
	Dir.Z = 0.f;
	if (!Dir.Normalize()) Dir = Char->GetActorForwardVector();

	const FVector SpawnLoc = Char->GetActorLocation() + Dir * SpawnForwardOffset + FVector::UpVector * SpawnUpOffset;
	const FRotator SpawnRot = Dir.Rotation();

	UE_LOG(LogTemp, Log, TEXT("[Curveball] THROW char=%s dir=%s spawn=%s curveLeft=%d"),
		*Char->GetName(), *Dir.ToString(), *SpawnLoc.ToString(), bCurveLeft ? 1 : 0);

	// 服务器 spawn 复制到所有客户端；飞行/爆炸/盲判定全在服务器执行
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Char;
	SpawnParams.Instigator = Char;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APhoenixCurveball* Curveball = GetWorld()->SpawnActor<APhoenixCurveball>(CurveballClass, SpawnLoc, SpawnRot, SpawnParams);
	if (Curveball)
	{
		Curveball->InitCurveball(Dir, bCurveLeft);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
