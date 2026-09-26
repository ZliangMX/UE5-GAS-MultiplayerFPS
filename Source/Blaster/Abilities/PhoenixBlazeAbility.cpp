#include "Blaster/Abilities/PhoenixBlazeAbility.h"

#include "Blaster/Abilities/PhoenixBlazeBall.h"
#include "Blaster/Character/BlasterCharacter.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"

UPhoenixBlazeAbility::UPhoenixBlazeAbility()
{
	// 不走"先武装、再二段激活"那套（那是 Jett E 冲刺的形状）：
	// 按 C 那一下由**角色**接管（进持投掷物态、收枪、播拿起动画），它不激活能力，
	// 所以这里不能开 bUseArmedActivation —— 开了的话按 C 就会扣一层充能。
	bUseArmedActivation = false;
}

void UPhoenixBlazeAbility::ExecuteSkillAction()
{
	// LocalPredicted：客户端预测实例也会执行这里，Spawn 只能服务器做
	//（否则每台机器各生成一颗球 + 一面墙）
	if (!CurrentActorInfo || !CurrentActorInfo->IsNetAuthority())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo());
	if (!Char)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	// 没填 BallClass 就用 C++ 那个（不依赖任何 BP 资产，裸建也能跑）
	TSubclassOf<APhoenixBlazeBall> ClassToSpawn = BallClass;
	if (!ClassToSpawn)
	{
		ClassToSpawn = APhoenixBlazeBall::StaticClass();
	}

	// 方向 = 准心的完整朝向（含俯仰）。墙虽然长在地面上，但"往哪看球往哪飞"这条
	// 和逐风云一致；球飞高飞低由墙那边做地面投影吸收掉（见 APhoenixFlameWall::SnapToGround）。
	FRotator ViewRot = Char->GetActorRotation();
	if (const AController* C = Char->GetController())
	{
		ViewRot = C->GetControlRotation();
	}
	FVector Dir = ViewRot.Vector();
	if (!Dir.Normalize())
	{
		Dir = Char->GetActorForwardVector();
		ViewRot = Dir.Rotation();
	}

	// 眼睛位置 + 视角坐标系里的前/右/上偏移（和 UJettCloudburstAbility 同一个算法）。
	// 用 GetPawnViewLocation 而不是 ActorLocation：球该从"眼睛看到的那条线"上出去。
	const FRotationMatrix ViewMatrix(ViewRot);
	const FVector SpawnLoc = Char->GetPawnViewLocation()
		+ ViewMatrix.GetUnitAxis(EAxis::X) * SpawnForwardOffset
		+ ViewMatrix.GetUnitAxis(EAxis::Y) * SpawnRightOffset
		+ ViewMatrix.GetUnitAxis(EAxis::Z) * SpawnUpOffset;

	FActorSpawnParameters SpawnParams;
	// Owner 必须是施法者：球拿它去读"还按着左键吗"，墙拿它去判敌人/自己回血
	SpawnParams.Owner = Char;
	SpawnParams.Instigator = Char;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	APhoenixBlazeBall* Ball = GetWorld()->SpawnActor<APhoenixBlazeBall>(
		ClassToSpawn, SpawnLoc, Dir.Rotation(), SpawnParams);
	if (Ball)
	{
		// 方向在生成之后再给：出生时要先有一个合法的朝向，不然第一帧的转向基准是乱的
		Ball->InitBall(Dir);
	}

	UE_LOG(LogTemp, Log, TEXT("[火墙] THROW char=%s dir=%s spawn=%s ball=%s"),
		*Char->GetName(), *Dir.ToString(), *SpawnLoc.ToString(),
		Ball ? *Ball->GetName() : TEXT("null"));

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
