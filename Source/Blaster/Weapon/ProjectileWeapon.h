// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Weapon/Weapon.h"
#include "ProjectileWeapon.generated.h"

/**
 * 
 */
UCLASS()
class BLASTER_API AProjectileWeapon : public AWeapon
{
	GENERATED_BODY()
public:
	AProjectileWeapon();

	virtual void Fire(const FVector& HitTarget) override;

	// 伤害由飞出去的那个投射物命中时自己结算（见 AProjectileWeapon::Fire 尾部的注释），
	// 服务器这边不需要回溯 —— 这条同时让 UCombatComponent::ServerFire 不再对
	// "bUseServerSideRewind=false" 报警（投射物武器的构造里本来就是 false）。
	virtual bool AppliesOwnDamage() const override { return true; }

protected:
	// 投射物生成后配置它（伤害 / 归属武器等）。默认什么都不做。
	//
	// 存在的理由是 Jett 飞刀：它的伤害/爆头倍率**必须写进投射物自己身上**，
	// 不能让投射物命中时去读"射手当前手持武器" —— 刀飞在路上的这段时间里射手
	// 完全可能已经按 1/2 切回枪了，那一刀的伤害就会按枪的倍率算。
	// 顺带：飞刀击杀要回填刀数，也得靠这里把"投出这把刀的武器"塞给投射物。
	virtual void ConfigureProjectile(class AProjectile* SpawnedProjectile, const FVector& HitTarget);

	// 出刀点（位置 + 朝向）。默认取武器网格上的 MuzzleFlashSocket。
	// 拿不到 socket 时**退回武器自身位置**——以前是直接不生成投射物，
	// 结果是"开枪没反应"而日志一片安静，换个模型忘配 socket 就会踩。
	virtual bool GetThrowOrigin(FVector& OutLocation, FRotator& OutRotation, const FVector& HitTarget) const;

	// 在指定位置/朝向生成一个投射物并配置它（服务器上下文；非权威机直接返回 nullptr）。
	// 单发的 Fire() 用一次，AJettKnives 的"全扔"循环用 N 次 —— 两边共用同一条生成+配置路径，
	// 免得"全扔"那条路漏掉 ConfigureProjectile（漏了就是伤害按默认值算，很难查）。
	class AProjectile* SpawnProjectile(const FVector& Location, const FRotator& Rotation);

private:
	UPROPERTY(EditAnywhere)
	TSubclassOf<class AProjectile> ProjectileClass;


};
