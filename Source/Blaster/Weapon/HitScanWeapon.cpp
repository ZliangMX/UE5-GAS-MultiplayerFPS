// Fill out your copyright notice in the Description page of Project Settings.


#include "HitScanWeapon.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"
#include "DrawDebugHelpers.h"
#include "Kismet/KismetMathLibrary.h"
#include "WeaponTypes.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/BlasterTypes/Team.h"

void AHitScanWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget);

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn == nullptr) return;
	AController* InstigatorController = OwnerPawn->GetController();
	
	const USkeletalMeshSocket* MuzzleSocket = GetWeaponMesh()->GetSocketByName(MuzzleFlashSocket);
	if (MuzzleSocket && InstigatorController)
	{
		FTransform SocketTransform = MuzzleSocket->GetSocketTransform(GetWeaponMesh());
		FVector Start = SocketTransform.GetLocation();

		FHitResult FireHit;
		WeaponTraceHit(Start,HitTarget,FireHit);
		ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(FireHit.GetActor());
		// 爆头：命中头骨（BoneName == "head"）→ 伤害 × HeadshotMultiplier。
		// 用同一个 FinalDamage 造成伤害 + 显示伤害数字，保证数字和实际掉血一致。
		const float FinalDamage = GetDamageForHit(Damage, FireHit);
		if (BlasterCharacter && HasAuthority() && InstigatorController)
		{
			const float HealthBefore = BlasterCharacter->GetHealth();
			UGameplayStatics::ApplyDamage(
				BlasterCharacter,
				FinalDamage,
				InstigatorController,
				this,
				UDamageType::StaticClass()
			);

			// 击杀确认音效：这一发把目标打死（之前血量>0，现在<=0）→ 给击杀者客户端播"叮"
			if (BlasterCharacter->GetHealth() <= 0.f && HealthBefore > 0.f)
			{
				ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(InstigatorController);
				if (KillerPC)
				{
					const bool bHeadshot = IsHeadshot(FireHit);
					if (KillerPC->IsLocalController())
					{
						KillerPC->PlayKillSound(bHeadshot);
					}
					else
					{
						KillerPC->ClientPlayKillSound(bHeadshot);
					}
				}
			}	
	
		}
		if (ImpactParticles)
		{
			UGameplayStatics::SpawnEmitterAtLocation(
				GetWorld(),
				ImpactParticles,
				FireHit.ImpactPoint,
				FireHit.ImpactNormal.Rotation()
			);
		}

		// 命中反馈：命中不同队敌人时，射手视角准星闪现命中标记 + 命中点飘出伤害数字。
		// 本机（射手客户端）直接画，服务器转发给射手客户端；旁观者机器（非本机+非权威）什么都不做。
		if (BlasterCharacter)
		{
			ABlasterPlayerState* ShooterPS = OwnerPawn ? OwnerPawn->GetPlayerState<ABlasterPlayerState>() : nullptr;
			ABlasterPlayerState* HitPS = BlasterCharacter->GetPlayerState<ABlasterPlayerState>();
			if (ShooterPS && ((HitPS && HitPS != ShooterPS && HitPS->Team != ShooterPS->Team) || BlasterCharacter->IsTestBot()))
			{
				ABlasterPlayerController* ShooterPC = Cast<ABlasterPlayerController>(InstigatorController);
				if (ShooterPC)
				{
					if (ShooterPC->IsLocalController())
					{
						ShooterPC->ShowHitMarker();
						ShooterPC->ShowDamageNumber(FinalDamage, FireHit.ImpactPoint);
					}
					else if (HasAuthority())
					{
						ShooterPC->ClientShowHitMarker();
						ShooterPC->ClientShowDamageNumber(Damage, FireHit.ImpactPoint);
					}
				}
			}
		}
		if (HitSound)
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				HitSound,
				FireHit.ImpactPoint
			);
		}
	}
}

void AHitScanWeapon::WeaponTraceHit(const FVector& TraceStart, const FVector& HitTarget, FHitResult& OutHit)
{
	UWorld* World = GetWorld();
	if (World)
	{
		FVector End = bUseScatter ? TraceEndWithScatter(TraceStart,HitTarget) : TraceStart + (HitTarget - TraceStart)*1.25;
		World->LineTraceSingleByChannel(
					OutHit,
					TraceStart,
					End,
					ECollisionChannel::ECC_Visibility
				);
		if (OutHit.bBlockingHit)
		{
			
		}
	}	
}

FVector AHitScanWeapon::TraceEndWithScatter(const FVector& TraceStart, const FVector& HitTarget)
{
	FVector ToTargetNormalized = (HitTarget - TraceStart).GetSafeNormal();
	FVector SphereCenter = TraceStart + ToTargetNormalized * DistanceToSphere;
	FVector RandVec = UKismetMathLibrary::RandomUnitVector() * FMath::FRandRange(0.f,SphereRadius);
	FVector EndLoc = SphereCenter + RandVec;
	FVector ToEndLoc = EndLoc - TraceStart;

	/*
	DrawDebugSphere(GetWorld(),SphereCenter,SphereRadius,12, FColor::Red , true);
	DrawDebugLine(GetWorld(),TraceStart, FVector(TraceStart + ToEndLoc * TRACE_LENGTH / ToEndLoc.Size()),FColor::Cyan,true);
	*/
	
	return FVector(TraceStart + ToEndLoc * TRACE_LENGTH / ToEndLoc.Size());
	
}


