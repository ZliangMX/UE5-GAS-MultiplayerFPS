// 弹道轨迹的"假子弹"。
//
// 为什么需要它：射线武器（hitscan）在游戏里没有真正飞行的子弹，命中是开枪那一刻就算完的。
// 而**拖尾型**的轨迹特效（本项目 MilitaryWeapSilver 里那几个 P_*_Tracer_01 全是这种，
// 见 BP_Projectile / BP_ProjectileBullet 都是把它挂在投射物身上）根本不是"自己会飞的一颗光点"：
// 它的粒子是沿**发射体走过的路**一个个丢下来的"面包屑"，ribbon 再把面包屑连成一条拖尾
//（SpawnPerUnit + ParticleModuleTypeDataRibbon，材质 M_SmokeRibbon_01）。
//
// 把这种特效原地生成在枪口，就会得到"一坨卡在枪口不动的东西" —— 发射体不动，面包屑就全堆在原地。
// 所以这里生成一个看不见的小 actor，用 ProjectileMovementComponent 从枪口沿弹道飞到弹着点，
// 把轨迹特效挂在它身上，特效就拿到了它需要的那段"移动路径"。
//
// 纯表现：不用来碰撞、不结算伤害、不复制（每台机器的 Fire() 各自生成自己的那一份，
// 和弹着特效/弹孔同一条路子）。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BulletTracer.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class UParticleSystem;
class UParticleSystemComponent;
class UProjectileMovementComponent;
class USceneComponent;

UCLASS()
class BLASTER_API ABulletTracer : public AActor
{
	GENERATED_BODY()

public:
	ABulletTracer();

	/*
	 * 摆好这一发：起点/方向/飞多远，以及挂哪个特效。
	 *
	 *   InParticles / InNiagara —— 二选一（Niagara 优先，和 AHitScanWeapon 里的规则一致）
	 *   NiagaraEndParameter    —— Niagara 里接"终点"的那个参数（NAME_None = 不设）
	 *   EndPoint               —— 喂给那个参数的**世界坐标**（打空时是弹道方向上的最远点）
	 *   StartLocation / FlyDirection / FlyDistance —— 起点、方向（会被归一化）、飞多远
	 *
	 * 由 AHitScanWeapon::SpawnTracerFX 在 SpawnActor 之后立刻调用。
	 */
	void InitTracer(UParticleSystem* InParticles, UNiagaraSystem* InNiagara,
		FName NiagaraEndParameter, const FVector& EndPoint,
		const FVector& StartLocation, const FVector& FlyDirection, float FlyDistance);

	/*
	 * 飞行速度（厘米/秒）。**不影响拖尾的密度** —— 拖尾那些面包屑是按"走过多远"丢的
	 *（SpawnPerUnit），所以调快只是让这一条更快划过，不会变成断断续续的一串点。
	 * 想更接近真枪的手感就调大（真枪 5.56 出膛约 900 m/s），想看得清轨迹就调小。
	 */
	UPROPERTY(EditAnywhere, Category = "Tracer", meta = (ClampMin = "100.0"))
	float Speed = 40000.f;

	/*
	 * 飞完之后的"赖着不走"时间（秒）：拖尾粒子还要飘一会儿，这时候不能把 actor 销毁掉
	 *（销毁会连拖尾一起抹掉，表现是轨迹一到弹着点就整条凭空消失）。
	 * 这段时间里它停在终点不动，让粒子自己散完。
	 */
	UPROPERTY(EditAnywhere, Category = "Tracer", meta = (ClampMin = "0.0"))
	float TailLifeSpan = 1.f;

protected:
	// 到点了：停住不再往前飞（不销毁，见 TailLifeSpan）
	void StopFlight();

	// 根组件：只管位置/朝向，没有任何碰撞
	UPROPERTY(VisibleAnywhere, Category = "Tracer")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Tracer")
	TObjectPtr<UProjectileMovementComponent> Movement;

	// 实际挂上去的那个特效组件（Cascade / Niagara 只会有一个）
	UPROPERTY(Transient)
	TObjectPtr<UParticleSystemComponent> TracerParticlesComp;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> TracerNiagaraComp;

	FTimerHandle StopTimer;
};
