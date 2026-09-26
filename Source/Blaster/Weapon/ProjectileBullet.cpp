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

	/*
	 * 初速默认值。**这里是"忘了填就完全不用"的静默失效点**：
	 * UProjectileMovementComponent 的 InitialSpeed/MaxSpeed 原生默认都是 0，
	 * 而速度为零不报任何错 —— 投射物会原地被重力拉着垂直掉下去，
	 * 看着像"投掷物没生效"，其实只是没给它速度。
	 *
	 * 以前没暴露：BP_ProjectileBullet 里手工填了 10000，所以子弹是好的。
	 * 但从 C++ 类新建的投射物 BP（Jett 飞刀）不会继承那个 BP 的值 →
	 * 第一版飞刀就是"扔出去只往下掉"。这里给个兜底，BP 里照样能覆盖成每把武器自己的手感。
	 */
	ProjectileMovementComponent->InitialSpeed = 10000.f;
	ProjectileMovementComponent->MaxSpeed = 10000.f;
}

float AProjectileBullet::ComputeDamageForHit(const FHitResult& Hit, bool& bOutHeadshot) const
{
	bOutHeadshot = false;

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter) return Damage;

	ABlasterCharacter* OwnerBlaster = Cast<ABlasterCharacter>(OwnerCharacter);
	if (!OwnerBlaster) return Damage;

	// ⚠️ 读的是"现在手持的武器"，不是"打出这一发的那把武器"。手持武器打出的子弹
	//    通常没问题（射手开枪到命中之间一般不会切枪），但这不是一条可靠的规则 ——
	//    派生类（飞刀）必须把倍率固化在投射物自己身上，别走这条路。
	AWeapon* Equipped = OwnerBlaster->GetEquippedWeapon();
	if (!Equipped) return Damage;

	bOutHeadshot = Equipped->IsHeadshot(Hit);
	return Equipped->GetDamageForHit(Damage, Hit);
}

void AProjectileBullet::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	// 兜底：绝不把伤害记到射手自己（或他自己身上挂着的东西）头上。
	// 正常情况已经被 AProjectile::BeginPlay 的 MoveIgnoreActors 挡在扫描之外了，
	// 但那条路依赖"移动扫描"这一个机制 —— 万一以后换成重叠事件、或者生成点整个
	// 埋在身体内部导致首帧就判定命中，这里必须还能兜住。
	// 直接 return（不销毁）：不能因为碰到了主人就把这一刀消耗掉。
	if (AActor* MyOwner = GetOwner())
	{
		if (OtherActor && (OtherActor == MyOwner || OtherActor->GetOwner() == MyOwner))
		{
			return;
		}
	}

	ACharacter* OwnerCharacter= Cast<ACharacter>(GetOwner());
	if (OwnerCharacter)
	{
		AController* OwnerController = OwnerCharacter->GetController();
		if (OwnerController)
		{
			// 伤害/爆头倍率由虚拟函数给出（默认取手持武器，飞刀覆盖成自己的数值）
			bool bHeadshot = false;
			float FinalDamage = ComputeDamageForHit(Hit, bHeadshot);

			ABlasterCharacter* HitCharacter = Cast<ABlasterCharacter>(OtherActor);
			const float HealthBefore = HitCharacter ? HitCharacter->GetHealth() : 0.f;
			UGameplayStatics::ApplyDamage(OtherActor,FinalDamage,OwnerController,this,UDamageType::StaticClass());
			// 击杀确认音效：这一发把目标打死（之前血量>0，现在<=0）→ 给击杀者客户端播"叮"
			if (HitCharacter && HasAuthority() && HitCharacter->GetHealth() <= 0.f && HealthBefore > 0.f)
			{
				ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(OwnerController);
				if (KillerPC)
				{
					// 投射物自己不是 AWeapon（AProjectileBullet : AProjectile），击杀反馈资产
					// 只能取**射手现在手持的那把武器**上挂的 —— 和上面 ComputeDamageForHit
					// 用的是同一个口径（那里也读 GetEquippedWeapon）。
					ABlasterCharacter* FiringChar = Cast<ABlasterCharacter>(OwnerCharacter);
					AWeapon* FiringWeapon = FiringChar ? FiringChar->GetEquippedWeapon() : nullptr;
					UWeaponKillIconSet* KillSet = FiringWeapon ? FiringWeapon->KillIconSet : nullptr;
					if (KillerPC->IsLocalController())
					{
						KillerPC->PlayKillSound(bHeadshot, KillSet, AWeapon::ComputeThisKillIndex(OwnerController));
					}
					else
					{
						KillerPC->ClientPlayKillSound(bHeadshot, KillSet, AWeapon::ComputeThisKillIndex(OwnerController));
					}
				}

				// 击杀回调：给"击杀后要给自己回点东西"的武器留的口子（Jett 飞刀刷新刀数）。
				// 只写在服务器分支里 —— 血量/击杀判定本来就是服务器权威的。
				OnProjectileKill(HitCharacter);
			}
			// 让基类命中反馈显示爆头后的伤害数字（投射物随后销毁，无副作用）
			Damage = FinalDamage;
		}
	}


	Super::OnHit(HitComp, OtherActor, OtherComp, NormalImpulse, Hit);
}
