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
	/*
	 * 别撞到射手自己（以及射手身上挂着的武器）。
	 *
	 * 子弹从枪口出膛，生成点天然在身体外面，所以以前没暴露过这个问题。
	 * 但"从手里出去"的投射物（Jett 飞刀）生成点就落在射手自己的骨骼网格里 ——
	 * CollisionBox 明确 Block 了 ECC_SkeletalMesh，于是第一帧扫描就命中自己，
	 * 伤害记在射手头上。表现就是"扔刀把自己扔死了"。
	 *
	 * 关掉的是**移动扫描**里对 Owner 的判定（ProjectileMovementComponent 走的正是
	 * 带扫描的移动，会读这张 MoveIgnoreActors 表），所以投射物会直接穿过射手飞出去，
	 * 而不是停在他身体里 —— 比"命中自己后判断成无效再销毁"要好，后者刀会当场消失。
	 *
	 * 注意这里关的是投射物对**射手**的判定，不影响射手以外任何人。
	 */
	if (AActor* MyOwner = GetOwner())
	{
		CollisionBox->IgnoreActorWhenMoving(MyOwner, true);

		// 挂在他身上的东西：手里的枪、收在挂点上的枪、爆能器……
		// （武器网格本身在 AWeapon 构造里就是 NoCollision，这里是兜底，
		//   免得以后哪个模型改回可碰撞又冒出"我的枪挡住了我的刀"）
		TArray<AActor*> AttachedToOwner;
		MyOwner->GetAttachedActors(AttachedToOwner, /*bResetArray=*/true, /*bRecursivelyIncludeAttachedActors=*/true);
		for (AActor* Attached : AttachedToOwner)
		{
			if (Attached)
			{
				CollisionBox->IgnoreActorWhenMoving(Attached, true);
			}
		}
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

