// Fill out your copyright notice in the Description page of Project Settings.


#include "ProjectileBullet.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Character.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"

AProjectileBullet::AProjectileBullet()
{
	ProjectileMovementComponent = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovementComponent"));
	ProjectileMovementComponent->bRotationFollowsVelocity = true;
	ProjectileMovementComponent->SetIsReplicated(true);
}

void AProjectileBullet::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	ACharacter* OwnerCharacter= Cast<ACharacter>(GetOwner());
	if (OwnerCharacter)
	{
		AController* OwnerController = OwnerCharacter->GetController();
		if (OwnerController)
		{
			// 爆头倍率从持有武器取（步枪 HeadshotMultiplier=4）：命中头骨 → ×倍率
			float FinalDamage = Damage;
			bool bHeadshot = false;
			if (ABlasterCharacter* OwnerBlaster = Cast<ABlasterCharacter>(OwnerCharacter))
			{
				if (AWeapon* Equipped = OwnerBlaster->GetEquippedWeapon())
				{
					FinalDamage = Equipped->GetDamageForHit(Damage, Hit);
					bHeadshot = Equipped->IsHeadshot(Hit);
				}
			}
			ABlasterCharacter* HitCharacter = Cast<ABlasterCharacter>(OtherActor);
			const float HealthBefore = HitCharacter ? HitCharacter->GetHealth() : 0.f;
			UGameplayStatics::ApplyDamage(OtherActor,FinalDamage,OwnerController,this,UDamageType::StaticClass());
			// 击杀确认音效：这一发把目标打死（之前血量>0，现在<=0）→ 给击杀者客户端播"叮"
			if (HitCharacter && HasAuthority() && HitCharacter->GetHealth() <= 0.f && HealthBefore > 0.f)
			{
				ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(OwnerController);
				if (KillerPC)
				{
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
			// 让基类命中反馈显示爆头后的伤害数字（投射物随后销毁，无副作用）
			Damage = FinalDamage;
		}
	}


	Super::OnHit(HitComp, OtherActor, OtherComp, NormalImpulse, Hit);
}
