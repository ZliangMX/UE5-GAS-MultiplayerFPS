// Fill out your copyright notice in the Description page of Project Settings.


#include "ShotGun.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "FramePro/FramePro.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"


void AShotGun::Fire(const FVector& HitTarget)
{
	AWeapon::Fire(HitTarget);
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn == nullptr) return;
	AController* InstigatorController = OwnerPawn->GetController();
	
	const USkeletalMeshSocket* MuzzleSocket = GetWeaponMesh()->GetSocketByName(MuzzleFlashSocket);
	if (MuzzleSocket && InstigatorController)
	{
		FTransform SocketTransform = MuzzleSocket->GetSocketTransform(GetWeaponMesh());
		FVector Start = SocketTransform.GetLocation();
		uint32 Hits = 0;

		TMap<ABlasterCharacter*, uint32> HitMap;
		// 记录每个被命中角色是否有任意一记钢珠爆头（用于击杀音爆头判定）
		TMap<ABlasterCharacter*, bool> HeadshotMap;
		for (uint32 i = 0;i<NumberOfPellets;i++)
		{
			FHitResult FireHit;
			WeaponTraceHit(Start, HitTarget, FireHit);

			ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(FireHit.GetActor());
			if (BlasterCharacter && HasAuthority() && InstigatorController)
			{
				if (HitMap.Contains(BlasterCharacter))
				{
					HitMap[BlasterCharacter]++;
				}
				else
				{
					HitMap.Emplace(BlasterCharacter, 1);
				}
				// 任意一记钢珠命中头骨都记为爆头
				HeadshotMap.Emplace(BlasterCharacter, HeadshotMap.FindRef(BlasterCharacter) || IsHeadshot(FireHit));
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
			if (HitSound)
			{
				UGameplayStatics::PlaySoundAtLocation(
					this,
					HitSound,
					FireHit.ImpactPoint,
					.5f,
					FMath::FRandRange(-.5f, .5f)
				);
			}
		}
		for (auto HitPair : HitMap)
		{
			if (HitPair.Key && HasAuthority() && InstigatorController)
			{
				const float HealthBefore = HitPair.Key->GetHealth();
				UGameplayStatics::ApplyDamage(
					HitPair.Key,
					Damage * HitPair.Value,
					InstigatorController,
					this,
					UDamageType::StaticClass()
				);	
				
				// 击杀确认音效：这一枪把目标打死（之前血量>0，现在<=0）→ 给击杀者客户端播"叮"
				if (HitPair.Key->GetHealth() <= 0.f && HealthBefore > 0.f)
				{
					ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(InstigatorController);
					if (KillerPC)
					{
						const bool bHeadshot = HeadshotMap.FindRef(HitPair.Key);
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
		}
	}	
}
