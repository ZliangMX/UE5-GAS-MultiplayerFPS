// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Weapon.h"
#include "HitScanWeapon.generated.h"

/**
 * 
 */
UCLASS()
class BLASTER_API AHitScanWeapon : public AWeapon
{
	GENERATED_BODY()
public:
	virtual void Fire(const FVector& HitTarget) override;

	// 服务器回溯判定出的命中，回到武器这边结算（伤害/爆头/击杀确认音）
	virtual void ProcessServerRewindHit(const FHitResult& Hit) override;

	// 射手侧：算出这一枪实际打出去的终点（含散布）。本地判定、上报服务器、多播给其他机器
	// 用的都是同一个点 —— 散布只在射手这里摇一次。
	FVector ComputeTraceEnd(const FVector& HitTarget);

	/*
	 *这一枪的射线**起点**。不再从枪口出发了 —— 见下面的实现注释和 GetFPEyeWorldLocation。
	 *
	 *本机（射手自己开枪、本地预测那一次）：第一人称眼位。
	 *其他机器（MulticastFire 里给这一发补表现）：大家看得见的枪口。
	 */
	FVector GetShotOrigin() const;

protected:
	// 命中结算：普通射线命中和服务器回溯命中共用这一份。
	// bRewindConfirmation = true：这一发是服务器回溯判定出来的，只补结算（伤害 + 击杀确认音），
	//   表现（命中特效/音效/命中标记）已经由多播在各机器上放过了，这里再放一遍就是重复的。
	// bRewindConfirmation = false：普通开火（多播 / 本地预测），表现照常播；伤害只在权威机且没开回溯时结算。
	// virtual：AJettKnives 覆盖它来侦测"这一刀把人打死了" → 刷新回 5 把刀。
	// 覆盖时**记得调 Super**，伤害/击杀确认音/命中反馈全在基类那一份里。
	virtual void ProcessHit(const FHitResult& FireHit, AController* InstigatorController, bool bRewindConfirmation);

	// 弹道轨迹：从看得见的枪口画到弹着点（打空时画到 TracerMaxDistance 为止）。
	// TraceStart 是判定用的起点（眼位），只在"武器网格上没有枪口 socket"时用来兜底。
	// 单发武器每枪调一次；霰弹枪每颗钢珠调一次（就是一片扇形轨迹，符合霰弹的样子）。
	void SpawnTracerFX(const FVector& TraceStart, const FVector& HitTarget, const FHitResult& FireHit);

	// 弹孔：只在真打中、材质已填、且不是角色（除非开了 bSpawnDecalOnPawn）时贴一个。
	void SpawnImpactDecal(const FHitResult& FireHit);

	FVector TraceEndWithScatter(const FVector& TraceStart, const FVector& HitTarget);

	void WeaponTraceHit(const FVector& TraceStart, const FVector& HitTarget, FHitResult& OutHit);

	/*
	 * ============================ 命中表现：弹着点 ============================
	 * 下面这些属性都是"在武器蓝图里填上资产就行"的空位，C++ 负责在正确的时机、正确的位置放出来。
	 * 所有表现都在 Fire() 里就地生成 —— Fire() 经 MulticastFire 在每台机器上各跑一遍，
	 * 所以不需要为特效再加任何 RPC（和原来的 ImpactParticles/HitSound 是同一套路子）。
	 */

	// 弹着点特效（老的 Cascade 粒子）。只在**真打中了**的时候放：
	// 单发走 ProcessHit、霰弹枪走 ShotGun::Fire，两处都判了 bBlockingHit（打空的 ImpactPoint 是零向量）。
	UPROPERTY(EditAnywhere, Category = "FX|Impact")
	class UParticleSystem* ImpactParticles;

	// 弹着点音效（打在墙上那一下的"啪"）。
	UPROPERTY(EditAnywhere, Category = "FX|Impact")
	USoundCue* HitSound;

	/* ============================ 弹道轨迹（tracer） ============================ */

	/*
	 * 弹道轨迹特效。**两个槽填一个就行**：
	 *   · TracerEffect    —— Niagara（项目里技能特效都是这个，推荐）
	 *   · TracerParticles —— 老式 Cascade 粒子（AProjectile 上那个 Tracer 就是这种）
	 * 两个都填时 Niagara 优先；两个都空（默认）= 没有轨迹，和你现在看到的效果一样。
	 *
	 * 轨迹从**看得见的枪口**画到**弹着点**，方向沿射线：朝前发射型的特效（一条 Beam / 一条拉长的
	 * 面片）直接就能用，不用在特效里额外做朝向。
	 *
	 * 起点为什么不是判定起点（眼睛）：判定必须从眼睛出发（见 GetShotOrigin 的长注释），但画出来的
	 * 轨迹要是也从眼睛出发，射手自己会看到一条从镜头里射出去的光柱；用看得见的枪口才是对的。
	 */
	UPROPERTY(EditAnywhere, Category = "FX|Tracer")
	TObjectPtr<class UNiagaraSystem> TracerEffect;

	UPROPERTY(EditAnywhere, Category = "FX|Tracer")
	TObjectPtr<class UParticleSystem> TracerParticles;

	/*
	 * 轨迹特效里的"终点"参数名（可选）。填了的话每发子弹都会把这个参数设成弹着点的**世界坐标**，
	 * Niagara 里就能拿它接一条 Beam 的末端 —— 这是 Beam 型轨迹最常用的做法（特效在枪口生成，
	 * 末端跟着参数跑）。
	 *   · 默认 "User.BeamEnd"，你自己的特效里叫什么就改成什么
	 *   · 名字里**不带点**时会被当成用户参数、自动补 "User." 前缀（Niagara 用户参数的全名是 User.XXX）
	 *   · 留空（NAME_None）→ 完全不设参数，适合"在枪口生成、自己朝前飞"那类不用末端点的特效
	 * 注意是 **Vec3（世界坐标）**，不是距离。
	 */
	UPROPERTY(EditAnywhere, Category = "FX|Tracer")
	FName TracerEndParameter = TEXT("User.BeamEnd");

	// 打空时轨迹的最远长度（厘米，默认 200m）。不设上限的话，没打中就会把 TRACE_LENGTH(800m)
	// 原样画出来 —— 一条横穿整张地图的光柱。
	UPROPERTY(EditAnywhere, Category = "FX|Tracer", meta = (ClampMin = "100.0"))
	float TracerMaxDistance = 20000.f;

	/*
	 * 轨迹要不要"跟着子弹飞"（默认飞）。
	 *
	 *   · true（默认）—— 生成一颗看不见的假子弹（ABulletTracer）从枪口沿弹道飞到弹着点，
	 *     特效挂在它身上。**拖尾型**特效必须这样用：本项目的 P_*_Tracer_01 全是
	 *     "粒子沿着发射体走过的路一个个丢下来、ribbon 再连成一条拖尾"的资产
	 *     （所以 BP_Projectile / BP_ProjectileBullet 都把它挂在投射物身上），
	 *     原地生成的话粒子全堆在枪口 —— 表现就是"枪口一坨不动的粒子"。
	 *   · false —— 原地在枪口生成，让特效自己飞（自带速度的"一颗光点"型特效）。
	 *     另外 **Beam 型的 Niagara 也建议关掉**：Beam 本来就是"枪口→弹着点"一条完整的光，
	 *     跟着飞会变成一条一边缩短、一边前进的光柱。
	 */
	UPROPERTY(EditAnywhere, Category = "FX|Tracer")
	bool bTracerFliesWithBullet = true;

	// 假子弹的飞行速度（厘米/秒）。**不影响拖尾密度**（那种资产是按"走过多远"丢点的），
	// 只决定这条轨迹划过得多快。想更接近真枪手感就调大（5.56 出膛约 900 m/s），想看清轨迹就调小。
	UPROPERTY(EditAnywhere, Category = "FX|Tracer", meta = (ClampMin = "100.0"))
	float TracerSpeed = 40000.f;

	/* ============================ 弹孔（decal） ============================ */

	/*
	 * 弹孔材质。⚠ 必须是 **Deferred Decal 材质域**的材质，普通材质拖进来是不显示的
	 *（Material Domain 设成 "Deferred Decal"，把弹孔贴图接到 BaseColor/Emissive，
	 *  再连一条 Opacity —— 贴图带 alpha 的话就用它的 alpha）。
	 * 光有一张弹孔 Texture 是填不进这个槽的：引擎的贴花只能挂在材质上，得先用那张贴图建一个材质。
	 *
	 * 默认状态下每个弹孔 20 秒后自己消失（ImpactDecalLifeSpan），留空 = 不打弹孔。
	 */
	UPROPERTY(EditAnywhere, Category = "FX|Decal")
	TObjectPtr<class UMaterialInterface> ImpactDecalMaterial;

	// 弹孔的三轴尺寸（厘米）。X 是"贴着表面的厚度"，得比贴花离墙面的距离大一点才不会陷进墙里
	//（3~5 比较稳）；Y/Z 是弹孔在墙上的**直径**。
	UPROPERTY(EditAnywhere, Category = "FX|Decal")
	FVector ImpactDecalSize = FVector(4.f, 8.f, 8.f);

	// 弹孔存活秒数。填 0 = 永不消失（一局下来墙面上会堆几千个贴花，掉帧，慎用）。
	UPROPERTY(EditAnywhere, Category = "FX|Decal", meta = (ClampMin = "0.0"))
	float ImpactDecalLifeSpan = 20.f;

	// 打在**角色**身上要不要留弹孔。默认 false —— 人的网格一直在动，贴花却留在原地，
	// 看到的就是"弹孔飘在半空中"。
	UPROPERTY(EditAnywhere, Category = "FX|Decal")
	bool bSpawnDecalOnPawn = false;

	UPROPERTY(EditAnywhere)
	float Damage = 20.f;

private:

	/*
	 *Trace end with scatter
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon Scatter")
	float DistanceToSphere = 800.f;
	
	UPROPERTY(EditAnywhere, Category = "Weapon Scatter")
	float SphereRadius = 75.f;

	UPROPERTY(EditAnywhere, Category = "Weapon Scatter")	
	bool bUseScatter = false;
};
