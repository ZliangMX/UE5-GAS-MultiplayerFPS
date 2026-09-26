// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Projectile.h"
#include "ProjectileBullet.generated.h"

/**
 * 
 */
UCLASS()
class BLASTER_API AProjectileBullet : public AProjectile
{
	GENERATED_BODY()

public:
	AProjectileBullet();

	virtual void OnHit(UPrimitiveComponent* HitComp,AActor* OtherActor,UPrimitiveComponent* OtherComp,FVector NormalImpulse,const FHitResult& Hit);

protected:
	// 这一发的最终伤害怎么算。bOutHeadshot 回报这一发是不是爆头（决定击杀音效）。
	//
	// 默认实现：从**射手当前手持武器**取爆头倍率（步枪 HeadshotMultiplier=4）。
	// ⚠️ 这个默认只对"手持武器打出的子弹"成立，而且它有个埋着的坑：
	//    子弹在路上的时候射手可以切枪，命中时读到的就是**另一把枪**的倍率。
	//    AJettKnifeProjectile 覆盖了它（倍率在生成那一刻就写进投射物自己身上）。
	virtual float ComputeDamageForHit(const FHitResult& Hit, bool& bOutHeadshot) const;

	// 这一发把人打死了（服务器判定：命中前血量 > 0、命中后 <= 0）。
	// 默认什么都不做；Jett 飞刀在这上面挂"击杀刷新飞刀数"。
	virtual void OnProjectileKill(class ABlasterCharacter* Victim) {}
};
