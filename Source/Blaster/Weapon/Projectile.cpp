// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Weapon/Projectile.h"

#include "NiagaraFunctionLibrary.h"
#include "Components/BoxComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundCue.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/Blaster.h"

// Sets default values
AProjectile::AProjectile()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	SetRootComponent(CollisionBox);
	CollisionBox->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	CollisionBox->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECollisionChannel::ECC_WorldStatic, ECollisionResponse::ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECC_SkeletalMesh, ECollisionResponse::ECR_Block);


}

// Called when the game starts or when spawned
void AProjectile::BeginPlay()
{
	Super::BeginPlay();
	
	if (Tracer)
	{
		TracerComponent = UGameplayStatics::SpawnEmitterAttached(
			Tracer,
			CollisionBox,
			FName(),
			GetActorLocation(),
			GetActorRotation(),
			EAttachLocation::KeepWorldPosition
		);

	}
	if (HasAuthority())
	{
		CollisionBox->OnComponentHit.AddDynamic(this,&AProjectile::OnHit);
	}
}

void AProjectile::DestroyTimerFinished()
{
	Destroy();
}

void AProjectile::StartDestroyTimer()
{
	GetWorld()->GetTimerManager().SetTimer(
	DestroyTimer,
	this,
	&AProjectile::DestroyTimerFinished,
	DestroyTime
);
}

void AProjectile::SpawnTrailSystem()
{
	if (TrailSystem)
	{
		TrailSystemComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			TrailSystem,
			GetRootComponent(),
			FName(),
			GetActorLocation(),
			GetActorRotation(),
			EAttachLocation::KeepWorldPosition,
			false
			);
	}
}

void AProjectile::ExplodeDamage()
{
	APawn* FiringPawn = GetInstigator();
	AController* FiringController = FiringPawn->GetController();
	if (FiringController)
	{
		UGameplayStatics::ApplyRadialDamageWithFalloff(
			this,
			Damage,
			10.F,
			GetActorLocation(),
			DamageInnerRadius,
			DamageOuterRadius,
			1.f,
			UDamageType::StaticClass(),
			TArray<AActor*>(),
			this,
			FiringController
		);
	}
}

// Called every frame
void AProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AProjectile::Destroyed()
{
	Super::Destroyed();

	if (ImpactParticle)
	{
		UGameplayStatics::SpawnEmitterAtLocation(GetWorld(),ImpactParticle,GetActorTransform());
	}
	if (ImpactSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this,ImpactSound,GetActorLocation());
	}
	
}

void AProjectile::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                        FVector NormalImpulse, const FHitResult& Hit)
{
	// 命中反馈（OnHit 只在服务器绑定）：命中不同队敌人时，准星闪现命中标记 + 命中点飘出伤害数字给射手
	if (ABlasterCharacter* HitCharacter = Cast<ABlasterCharacter>(OtherActor))
	{
		APawn* FiringPawn = GetInstigator();
		ABlasterPlayerState* ShooterPS = FiringPawn ? FiringPawn->GetPlayerState<ABlasterPlayerState>() : nullptr;
		ABlasterPlayerState* HitPS = HitCharacter->GetPlayerState<ABlasterPlayerState>();
		if (ShooterPS && ((HitPS && HitPS != ShooterPS && HitPS->Team != ShooterPS->Team) || HitCharacter->IsTestBot()))
		{
			if (ABlasterPlayerController* ShooterPC = Cast<ABlasterPlayerController>(FiringPawn->GetController()))
			{
				if (ShooterPC->IsLocalController())
				{
					ShooterPC->ShowHitMarker();
					ShooterPC->ShowDamageNumber(Damage, Hit.ImpactPoint);
				}
				else
				{
					ShooterPC->ClientShowHitMarker();
					ShooterPC->ClientShowDamageNumber(Damage, Hit.ImpactPoint);
				}
			}
		}
	}

	Destroy();
}

