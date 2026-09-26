// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Weapon/ProjectileWeapon.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Projectile.h"

AProjectileWeapon::AProjectileWeapon()
{
	// 子弹会飞：命中由投射物自己的碰撞决定，不用服务器回溯重算射线
	bUseServerSideRewind = false;
}

bool AProjectileWeapon::GetThrowOrigin(FVector& OutLocation, FRotator& OutRotation, const FVector& HitTarget) const
{
	if (!GetWeaponMesh()) return false;

	const USkeletalMeshSocket* MuzzleSocket = GetWeaponMesh()->GetSocketByName(MuzzleFlashSocket);
	if (MuzzleSocket)
	{
		const FTransform SocketTransform = MuzzleSocket->GetSocketTransform(GetWeaponMesh());
		
		OutLocation = SocketTransform.GetLocation();
		OutLocation = MuzzleSocket->GetSocketTransform(GetViewMesh()).GetLocation();
	}
	else
	{
		// 没有枪口 socket（换了模型还没配 socket）→ 退回武器自身位置。
		// 起点差一点点，但至少投射物出得来 —— 总好过"开火没反应"。
		OutLocation = GetWeaponMesh()->GetComponentLocation();
	}

	OutRotation = (HitTarget - OutLocation).Rotation();
	return true;
}

void AProjectileWeapon::ConfigureProjectile(AProjectile* SpawnedProjectile, const FVector& HitTarget)
{
	// 默认无配置。派生武器（Jett 飞刀）在自己身上覆盖。
}

AProjectile* AProjectileWeapon::SpawnProjectile(const FVector& Location, const FRotator& Rotation)
{
	if (!HasAuthority()) return nullptr;
	if (!ProjectileClass) return nullptr;

	UWorld* World = GetWorld();
	if (!World) return nullptr;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = GetOwner();
	SpawnParams.Instigator = Cast<APawn>(GetOwner());

	return World->SpawnActor<AProjectile>(ProjectileClass, Location, Rotation, SpawnParams);
}

void AProjectileWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget);

	if (!HasAuthority()) return;
	if (!ProjectileClass) return;

	FVector SpawnLocation;
	FRotator SpawnRotation;
	if (!GetThrowOrigin(SpawnLocation, SpawnRotation, HitTarget))
	{
		// 连武器网格都没有 —— 配置出了大问题，留一条日志免得变成"静默不开火"
		UE_LOG(LogTemp, Warning, TEXT("[投射物] %s 拿不到武器网格，投不出投射物"), *GetName());
		return;
	}

	// 生成之后立刻配置：伤害来源、归属武器之类的信息必须在**生成那一刻**写进投射物，
	// 不能等它命中时再去反查（那时候射手可能已经切枪了）
	AProjectile* Spawned = SpawnProjectile(SpawnLocation, SpawnRotation);
	ConfigureProjectile(Spawned, HitTarget);
}
