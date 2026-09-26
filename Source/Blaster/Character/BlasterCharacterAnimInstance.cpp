// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterCharacterAnimInstance.h"
#include "BlasterCharacter.h"
#include "GameFramework\CharacterMovementComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/BlasterTypes/CombatState.h"
#include "AnimNotify_ReloadFinished.h"
#include "Blaster/BlasterComponent/CombatComponent.h"

void UBlasterCharacterAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	
	BlasterCharacter1 = Cast<ABlasterCharacter>(TryGetPawnOwner());
	RightHandRotation=FRotator(0.f,180.f,0.f);
}

void UBlasterCharacterAnimInstance::NativeUpdateAnimation(float DeltaTime)
{
	Super::NativeUpdateAnimation(DeltaTime);

	if (BlasterCharacter1 == nullptr)
	{
		BlasterCharacter1 = Cast<ABlasterCharacter>(TryGetPawnOwner());
	}
	if (BlasterCharacter1 == nullptr)return;

	FVector Velocity = BlasterCharacter1->GetVelocity();
	Velocity.Z = 0.f;
	Speed = Velocity.Size();

	// 以角色朝向分解速度：ForwardSpeed / RightSpeed 可为负
	const FVector Forward = BlasterCharacter1->GetActorForwardVector();
	const FVector Right = BlasterCharacter1->GetActorRightVector();
	ForwardSpeed = FVector::DotProduct(Velocity, Forward);
	RightSpeed = FVector::DotProduct(Velocity, Right);

	// Direction: 速度方向与角色正前方的夹角，范围 -180 ~ 180（左负右正）
	const FVector VelocityDir = Velocity.GetSafeNormal();
	if (!VelocityDir.IsNearlyZero())
	{
		const float DegAngle = UKismetMathLibrary::DegAcos(FVector::DotProduct(Forward, VelocityDir));
		const FVector Cross = FVector::CrossProduct(Forward, VelocityDir);
		Direction = Cross.Z < 0.f ? -DegAngle : DegAngle;
	}
	else
	{
		Direction = 0.f;
	}

	/*
	 * 八向状态机用的方向：和上面 Direction 同一套角度口径，只是量化成 8 个扇区。
	 * 静止（速度 < MovementDirectionMinSpeed）时**不动它** —— 保留上一帧的方向。
	 * 原因见头文件：枚举里没有 Idle，归零会让减速停下那几帧在 N 和真实方向之间来回跳。
	 * 传给它的 Forward 用的是同一份"角色正前方"，两边保证不会算出不同口径。
	 */
	MovementDirection8::FromVelocity(Velocity, Forward,
		FMath::Square(MovementDirectionMinSpeed), MovementDirection);

	bIsInAir = BlasterCharacter1->GetCharacterMovement()->IsFalling();
	bIsAccelerating = BlasterCharacter1->GetCharacterMovement()->GetCurrentAcceleration().Size() > 0.f ? true : false;
	// 手里拿着武器且没切出 spike 才算“持枪”。掏出 spike（bSpikeDrawn）时即使 EquippedWeapon
	// 仍指向旧枪（客户端没等复制清空），也让动画机走空手机器。
	bWeaponEquipped = BlasterCharacter1->IsWeaponEquipped() && !BlasterCharacter1->IsSpikeDrawn();
	EquippedWeapon = BlasterCharacter1->GetEquippedWeapon();

	// 当前武器类型；空手（或掏出 spike）时给 EWT_MAX 哨兵值，别去解引用 EquippedWeapon。
	// ⚠ 不要图省事写成 BlasterCharacter1->GetWeaponType() —— 那个实现内部直接解引用
	//   EquippedWeapon，空手时会崩（UWushuFPAnimInstance 那边也特地绕开了它）。
	//
	// 持技能投掷物期间例外：那时枪是收着的（EquippedWeapon == nullptr），照这句只会得到 EWT_MAX，
	// 而这三个技能**站着待命那段姿势在动画蓝图里做**，ABP 得知道"拿的是哪个技能"才切得出姿势 ——
	// 所以先问复制的那个种类。和第一人称那边同一个套路，只是那边读本机预测、这边读复制的。
	// 动作那几段（拿起 / 丢出去 / 按住 / 收起）由 C++ 播一次性蒙太奇，槽位上有蒙太奇时蒙太奇说了算。
	ThrowableKind = BlasterCharacter1->GetReplicatedThrowableKind();

	if (ThrowableKind != EBlasterThrowableKind::None)
	{
		WeaponType = GetThrowableWeaponType(ThrowableKind);
	}
	// 掏出尖刺包期间：报 EWT_Spike。同投掷物那一条的理由 —— 那时枪是收着的，
	// 照下面的分支只会得到 EWT_MAX（空手姿势），ABP 分不出"空手"和"拿着包"。
	// 这里是所有机器都成立的那份（bSpikeDrawn 是复制的，远端玩家也有值）。
	else if (BlasterCharacter1->IsSpikeDrawn())
	{
		WeaponType = EWeaponType::EWT_Spike;
	}
	else
	{
		WeaponType = (bWeaponEquipped && EquippedWeapon != nullptr)
			? EquippedWeapon->GetWeaponType()
			: EWeaponType::EWT_MAX;
	}

	// 这个角色是谁（按英雄 blend 脸/动画用）。源头是 PlayerState 上复制的 Agent；
	// 取不到 PlayerState 时 GetAgent() 返回 None —— ABP 那边要先把 None 兜住。
	// 换英雄（大厅重选 / F9 循环 / 服务器随机 roll）会让它下一帧就变，ABP 里按它切混合即可。
	Agent = BlasterCharacter1->GetAgent();
	bIsCrouched = BlasterCharacter1->bIsCrouched;
	bEmptyHand = BlasterCharacter1->IsEmptyHandLocked();
	bAiming = BlasterCharacter1->IsAiming();
	TurningInPlace = BlasterCharacter1->GetTurningInPlace();
	bElimmed=BlasterCharacter1->IsElimmed();

	FRotator AimRotation = BlasterCharacter1->GetBaseAimRotation();
	FRotator MovementRotation = UKismetMathLibrary::MakeRotFromX(BlasterCharacter1->GetVelocity());
	FRotator DeltaRot = UKismetMathLibrary::NormalizedDeltaRotator(MovementRotation, AimRotation);
	DeltaRotation = FMath::RInterpTo(DeltaRotation, DeltaRot, DeltaTime, 6.f);
	YawOffset = DeltaRotation.Yaw;

	CharacterRotationLastFrame = CharacterRotation;
	CharacterRotation = BlasterCharacter1->GetActorRotation();
	const FRotator Delta = UKismetMathLibrary::NormalizedDeltaRotator(CharacterRotation, CharacterRotationLastFrame);
	const float Target = Delta.Yaw / DeltaTime;
	const float Interp = FMath::FInterpTo(Lean, Target, DeltaTime, 6);
	Lean = FMath::Clamp(Interp, -90.f, 90.f);

	AO_Yaw = BlasterCharacter1->GetAO_Yaw();
	AO_Pitch = BlasterCharacter1->GetAO_Pitch();
	RootRotationYaw = BlasterCharacter1->GetRootRotationYaw();

	//左手附着在武器上 START
	if (bWeaponEquipped && BlasterCharacter1->GetEquippedWeapon() && BlasterCharacter1->GetMesh())
	{
		LeftHandTransform = EquippedWeapon->GetWeaponMesh()->GetSocketTransform(WeaponLeftHandSocket, ERelativeTransformSpace::RTS_World);
		FVector OutPosition;
		FRotator OutRotation;
		BlasterCharacter1->GetMesh()->TransformToBoneSpace(CharacterRightHandBone, LeftHandTransform.GetLocation(), FRotator::ZeroRotator, OutPosition, OutRotation);
		LeftHandTransform.SetLocation(OutPosition);
		LeftHandTransform.SetRotation(FQuat(OutRotation));

        if(BlasterCharacter1->IsLocallyControlled())
        {
        	bLocallyControlled = true;
           FTransform RightHandTransform = EquippedWeapon->GetWeaponMesh()->GetSocketTransform(WeaponRightHandSocket,RTS_World);
           FRotator LookAtRotation = UKismetMathLibrary::FindLookAtRotation(RightHandTransform.GetLocation(),BlasterCharacter1->GetHitTarget())+FRotator(0.f,180.f,0.f);
        	LookAtRotation.Pitch=-LookAtRotation.Pitch;
           RightHandRotation =  FMath::RInterpConstantTo(RightHandRotation,LookAtRotation,DeltaTime,600.f);

        }
	
	}
	//END

	bUseFABRIK = BlasterCharacter1->GetCombatState() == ECombatState::ECS_Unoccupied;
	bUseAimOffsets = BlasterCharacter1->GetCombatState() != ECombatState::ECS_Reloading && !BlasterCharacter1->GetDisableGameplay();
	bTransformRightHand = BlasterCharacter1->GetCombatState() != ECombatState::ECS_Reloading && !BlasterCharacter1->GetDisableGameplay();
}
void UBlasterCharacterAnimInstance::FinishReload()
{
	/*
	 * 换弹结束的**唯一实现**就这一句 —— 反查角色、拦掉不该接的情况（霰弹枪）、调 FinishReloading()，
	 * 全在 UAnimNotify_ReloadFinished::TriggerFinishReload 里，和 C++ 那条动画通知是同一份代码，
	 * 所以从蓝图通知调还是从蒙太奇里的通知调，行为不会分叉。
	 *
	 * 传 this 而不是 MeshComp：Trigger 那边两种都吃（走 GetOwningActor()），
	 * 这里手上只有 this，也没有必要绕一层。
	 */
	UAnimNotify_ReloadFinished::TriggerFinishReload(this);
}

void UBlasterCharacterAnimInstance::FinishEmptyHand()
{
	// 空手那一段的收尾：状态放回 ECS_Unoccupied + 自动掏最强的武器。
	// 实现在 ABlasterCharacter::EmptyHandFinish()，和 C++ 那条 UAnimNotify_EmptyHandFinished
	// 共用同一个幂等门禁（状态不对就直接返回），所以从状态机里调重复了也不会掏两次枪。
	//
	// 反查角色用的是同一个工具函数（它走 GetOwningActor()，对 AnimInstance 也成立）。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(this);
	if (Character == nullptr) return;
	Character->EmptyHandFinish();
}

void UBlasterCharacterAnimInstance::EquipFinish()
{
	// 和 FinishReload() 同一条路：反查角色 → 调 UCombatComponent::EquipFinish()。
	// 实现在 UAnimNotify_EquipFinished 那条 C++ 通知里，两个入口共用，行为不会分叉。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(this);
	if (Character == nullptr) return;
	UCombatComponent* Combat = Character->GetCombatComponent();
	if (Combat == nullptr) return;
	Combat->EquipFinish();
}
