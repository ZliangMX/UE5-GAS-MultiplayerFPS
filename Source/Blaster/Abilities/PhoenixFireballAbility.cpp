#include "Blaster/Abilities/PhoenixFireballAbility.h"

#include "Blaster/Abilities/PhoenixFireball.h"
#include "Blaster/Character/BlasterCharacter.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"

UPhoenixFireballAbility::UPhoenixFireballAbility()
{
	// 不走"先武装、再二段激活"那套（那是 Jett E 冲刺的形状）：
	// 火球是"按 Q 拿起 → 左键丢"，拿起那一步不消耗充能也不激活能力，
	// 真正激活就是丢出去这一下。
	bUseArmedActivation = false;
}

void UPhoenixFireballAbility::ExecuteSkillAction()
{
	// LocalPredicted：客户端预测实例也会执行这里，Spawn 只能服务器做
	//（否则每台机器各生成一颗球 —— 表现是"我丢了一颗，服务器和别人那边有两颗"）
	if (!CurrentActorInfo || !CurrentActorInfo->IsNetAuthority())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo());
	if (!Char || !FireballClass)
	{
		if (Char && !FireballClass)
		{
			// 没配火球类：按了 Q、手上亮了、左键一丢什么都没有。这条日志是唯一的线索。
			UE_LOG(LogTemp, Warning,
				TEXT("[火球] %s 没有填 FireballClass，按 Q 只会退出持球态、不会丢出东西（请在 GA_Phoenix_HotHands 里填 BP_PhoenixFireball_C）"),
				*GetName());
		}
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	/*
	 * ★ 和 E 曲线闪光最大的区别：**方向是全 3D 的，不把 Z 归零**。
	 *
	 * 曲线闪光的球只会平着飞（Z 归零 = 它是个纯粹的"往哪个方向撇"的选择），
	 * 而火球要能把火铺在自己脚下 —— 抬头往天上丢或者低头往脚前砸都必须是玩家的自由。
	 * 所以这里直接用准心的完整朝向（含俯仰），球就沿着这条线飞。
	 */
	FVector Dir = FVector::ZeroVector;
	if (const AController* C = Char->GetController())
	{
		Dir = C->GetControlRotation().Vector();
	}
	else
	{
		Dir = Char->GetActorForwardVector();
	}
	if (!Dir.Normalize()) Dir = Char->GetActorForwardVector();

	// 偏移量是**世界竖直**方向（UpVector）而不是"沿着视线再抬一点"：
	// 这样低头往脚前砸的时候出生点不会被推到地面以下。
	const FVector SpawnLoc = Char->GetActorLocation() + Dir * SpawnForwardOffset + FVector::UpVector * SpawnUpOffset;
	const FRotator SpawnRot = Dir.Rotation();

	UE_LOG(LogTemp, Log, TEXT("[火球] THROW char=%s dir=%s spawn=%s"),
		*Char->GetName(), *Dir.ToString(), *SpawnLoc.ToString());

	// 服务器 spawn 会复制到所有客户端；飞行/下坠/落地判定全在服务器执行
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Char;
	SpawnParams.Instigator = Char;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APhoenixFireball* Fireball = GetWorld()->SpawnActor<APhoenixFireball>(FireballClass, SpawnLoc, SpawnRot, SpawnParams);
	if (Fireball)
	{
		Fireball->InitFireball(Dir);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
