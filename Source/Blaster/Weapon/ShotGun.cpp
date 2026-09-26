// Fill out your copyright notice in the Description page of Project Settings.


#include "ShotGun.h"
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
	
	if (InstigatorController)
	{
		// 起点和单发武器走同一个入口（本机=第一人称眼位，其他机器=看得见的枪口）。
		// 所有钢珠都从这个点散出去 —— 和准星那条射线共起点，近距离才不会整片打偏。
		const FVector Start = GetShotOrigin();
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

			/*
			 * 弹道轨迹 + 弹孔：**每颗钢珠各来一份**。
			 *
			 * 和单发武器不同，这里故意不合并成一条 —— 霰弹本来就是八颗钢珠散着飞出去的，
			 * 画成一片扇形轨迹才是它该有的样子（也是各家 FPS 的通行做法）。
			 * 代价是每一枪最多 8 个贴花，所以 ImpactDecalLifeSpan 别设太大。
			 *
			 * 两个 Spawn 内部自己判断"没打中就不放"，所以这里不用再套 bBlockingHit。
			 */
			SpawnTracerFX(Start, HitTarget, FireHit);
			SpawnImpactDecal(FireHit);

			// ★ 同上：打空那颗钢珠的 ImpactPoint 是零向量，不判的话特效会跑到世界原点去。
			if (FireHit.bBlockingHit && ImpactParticles)
			{
				UGameplayStatics::SpawnEmitterAtLocation(
					GetWorld(),
					ImpactParticles,
					FireHit.ImpactPoint,
					FireHit.ImpactNormal.Rotation()
				);
			}
			if (FireHit.bBlockingHit && HitSound)
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
							KillerPC->PlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
						}
						else
						{
							KillerPC->ClientPlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
						}
					}
				}
			}
		}
	}	
}
