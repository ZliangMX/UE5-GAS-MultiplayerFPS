// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Weapon/JettKnifeProjectile.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/ProjectileMovementComponent.h"

#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Weapon/JettKnives.h"

AJettKnifeProjectile::AJettKnifeProjectile()
{
	// 自带的骨骼网格（Knife_A 是骨骼网格，基类那个静态网格挂不了它）
	KnifeMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("KnifeMesh"));
	KnifeMesh->SetupAttachment(RootComponent);
	KnifeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 碰撞/命中判定一律交给基类的 CollisionBox：网格只负责好看，
	// 让它也参与碰撞会出现"明明扎到人了却不掉血"（或者反过来）。
	KnifeMesh->SetGenerateOverlapEvents(false);
	KnifeMesh->SetCastShadow(true);
	// 刀是小物件，默认的骨骼网格包围盒会把它遮没，也无谓地撑大剔除体积
	KnifeMesh->SetBoundsScale(1.f);

	// 基类的静态网格藏掉：两个网格重在一起会闪 z-fighting，而且静态网格上也没法挂刀
	if (ProjectileMesh)
	{
		ProjectileMesh->SetVisibility(false);
		ProjectileMesh->SetHiddenInGame(true);
		ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// 刀不跟着速度方向摆 —— 扔出去是在空中翻跟头的（自旋在 Tick 里做）
	if (ProjectileMovementComponent)
	{
		ProjectileMovementComponent->bRotationFollowsVelocity = false;
		// 扎到东西就停，别弹开：飞刀不该弹
		ProjectileMovementComponent->bShouldBounce = false;

		/*
		 * 初速。**不设的话飞刀只会原地往下掉** ——
		 * UProjectileMovementComponent 的 InitialSpeed 默认就是 0，而"速度为零"不报任何错，
		 * 表现只是投射物被重力拉着垂直落地。子弹能飞是因为 BP_ProjectileBullet 里手工填了 10000，
		 * 从 C++ 类裸建的投射物 BP（这个飞刀）就会漏掉这一项。
		 *
		 * 6000（=60 m/s）：比子弹（10000）慢得多，玩家看得见刀飞过去，但 20 米只要 0.33 秒、不影响手感。
		 * 想调飞行速度就改 BP_JettKnifeProjectile 上 ProjectileMovementComponent 的 InitialSpeed/MaxSpeed。
		 */
		ProjectileMovementComponent->InitialSpeed = 6000.f;
		ProjectileMovementComponent->MaxSpeed = 6000.f;

		/*
		 * 重力压到接近 0：Valorant 的刃风暴是**直线飞**的，20 米掉 8 厘米左右，
		 * 留着这点只是为了看着不像激光。想要明显的抛物线下坠感就调大这个值（1.0 = 和子弹一样）。
		 */
		ProjectileMovementComponent->ProjectileGravityScale = 0.1f;
	}
}

void AJettKnifeProjectile::BeginPlay()
{
	Super::BeginPlay();

	// 给飞刀一个寿命上限。基类那个 DestroyTimer 只有手雷和火箭在用，
	// 子弹/飞刀**从来没启动过** —— 一把没打中任何东西的刀会永远往前飞、永远不销毁。
	// 对大招来说这就等于"永远等不到最后一把刀落定"，所以必须补上这一下。
	// 时长 = AProjectile::DestroyTime（默认 3 秒，EditAnywhere，可在 BP_JettKnifeProjectile 里调）。
	// 6000 cm/s × 3s = 180 米，实战够长了；真扔飞了也就是 3 秒后大招自动收。
	//
	// 只在权威机起：客户端上销毁它纯属本地行为，服务器那份还在，命中判定会错位。
	// 客户端的销毁由服务器复制过来。
	if (HasAuthority())
	{
		StartDestroyTimer();
	}
}

void AJettKnifeProjectile::Destroyed()
{
	Super::Destroyed();

	// ⚠️ 这里原来会调 AJettKnives::OnKnifeFinished()，给武器数"还有几把刀在天上"。
	// 飞刀改成射线之后（2026-09-18，见 AJettKnives 的头文件）那个计数整个不需要了 ——
	// 命中判定和扣刀发生在同一次 Fire() 里、击杀刷新也是同步完成的，没有"刀还在飞"这个窗口。
	// 这个类现在只是留着让 BP_JettKnifeProjectile 不变成父类丢失的坏资产，不再有人生成它，
	// 所以这里**故意什么都不做**（不是漏了）。
}

void AJettKnifeProjectile::InitFromKnives(AJettKnives* Source, float InDamage, float InHeadshotMultiplier,
	FName InHeadBone, USkeletalMesh* InKnifeMesh)
{
	SourceKnives = Source;
	KnifeDamage = InDamage;
	KnifeHeadshotMultiplier = InHeadshotMultiplier;
	KnifeHeadBoneName = InHeadBone;

	if (KnifeMesh && InKnifeMesh)
	{
		KnifeMesh->SetSkeletalMesh(InKnifeMesh);
	}
}

float AJettKnifeProjectile::ComputeDamageForHit(const FHitResult& Hit, bool& bOutHeadshot) const
{
	// 不改用基类那条"读射手当前手持武器"的路 —— 见类头注释 ②。
	// 爆头判定和 AWeapon::IsHeadshot 一个套路：命中骨骼名 == HeadBoneName。
	bOutHeadshot = !KnifeHeadBoneName.IsNone() && Hit.BoneName == KnifeHeadBoneName;

	return bOutHeadshot ? KnifeDamage * KnifeHeadshotMultiplier : KnifeDamage;
}

void AJettKnifeProjectile::OnProjectileKill(ABlasterCharacter* Victim)
{
	// 击杀刷新刀数（用户要求）：把刀补满。没有 SourceKnives（武器已经销毁）就什么也不做 ——
	// 那种情况下大招本来就结束了，刀数补不补都无所谓。
	if (AJettKnives* Knives = SourceKnives.Get())
	{
		Knives->RefillKnives();
	}
}

void AJettKnifeProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (KnifeMesh && SpinSpeedDegPerSec != 0.f)
	{
		// 绕本地轴翻滚。用 AddLocalRotation 而不是 AddActorLocalRotation：
		// 只转网格，不动根组件 —— 根组件（CollisionBox）的朝向是投射物运动/复制用的，别搅进去。
		const FVector Axis = SpinAxisLocal.GetSafeNormal();
		if (!Axis.IsNearlyZero())
		{
			KnifeMesh->AddLocalRotation(FQuat(Axis, FMath::DegreesToRadians(SpinSpeedDegPerSec * DeltaTime)));
		}
	}
}
