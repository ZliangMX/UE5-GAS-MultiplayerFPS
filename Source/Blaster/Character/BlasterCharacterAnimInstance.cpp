// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterCharacterAnimInstance.h"
#include "BlasterCharacter.h"
#include "GameFramework\CharacterMovementComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/BlasterTypes/CombatState.h"

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


	bIsInAir = BlasterCharacter1->GetCharacterMovement()->IsFalling();
	bIsAccelerating = BlasterCharacter1->GetCharacterMovement()->GetCurrentAcceleration().Size() > 0.f ? true : false;
	bWeaponEquipped = BlasterCharacter1->IsWeaponEquipped();
	EquippedWeapon = BlasterCharacter1->GetEquippedWeapon();
	bIsCrouched = BlasterCharacter1->bIsCrouched;
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