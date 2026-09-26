// 见头文件的长注释：这里是"假子弹"本身的实现。形状很简单 ——
// 一个无碰撞的根组件 + ProjectileMovement 直线飞，特效挂在根组件上跟着走。

#include "Blaster/Weapon/BulletTracer.h"

#include "Components/SceneComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"

ABulletTracer::ABulletTracer()
{
	// 位移全交给 ProjectileMovementComponent（它自己会 tick），actor 本身不用 tick
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->SetUpdatedComponent(SceneRoot);
	// 朝向跟着速度走：挂在它上面的特效（尤其是 ribbon/拉长的面片）就自动对齐弹道方向了
	Movement->bRotationFollowsVelocity = true;
	// 不受重力：这是一条直线，不是抛物线（速度每帧被我们写死，重力也只会把它拽歪）
	Movement->ProjectileGravityScale = 0.f;
	// 不撞任何东西：命中在开枪那一刻就判定完了，这里的飞行纯是演出，
	// 撞上墙/角色停下来的话，轨迹会缺一段（而子弹明明打过去了）
	Movement->bSweepCollision = false;
	Movement->bShouldBounce = false;

	// 生成时总是成功：默认的碰撞处理会因为"出生点卡在别人身体里"直接判失败，
	// 表现就是"偶尔某一发没有轨迹"——一个很难复现、更难查的表现
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 纯表现：不复制，每台机器自己在 Fire() 里生成自己那一份（同弹着特效/弹孔）
	bReplicates = false;
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);
}

void ABulletTracer::InitTracer(UParticleSystem* InParticles, UNiagaraSystem* InNiagara,
	FName NiagaraEndParameter, const FVector& EndPoint,
	const FVector& StartLocation, const FVector& FlyDirection, float FlyDistance)
{
	// 方向兜底：全零方向会让这一发原地不动（表现就是"轨迹没出来"）
	FVector Dir = FlyDirection;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
		if (!Dir.Normalize())
		{
			Dir = FVector::ForwardVector;
		}
	}

	SetActorLocationAndRotation(StartLocation, Dir.Rotation());

	const float SafeSpeed = FMath::Max(Speed, 1.f);
	Movement->InitialSpeed = SafeSpeed;
	Movement->MaxSpeed = SafeSpeed;
	Movement->Velocity = Dir * SafeSpeed;

	// —— 特效：挂在根组件上（相对位置为零 = 就在子弹头上），跟着子弹走 ——
	if (InNiagara)
	{
		TracerNiagaraComp = UNiagaraFunctionLibrary::SpawnSystemAttached(
			InNiagara,
			SceneRoot,
			NAME_None,
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			EAttachLocation::KeepRelativeOffset,
			/*bAutoDestroy=*/true);

		// 终点参数（可选）：Beam 型的系统拿它接末端。这里传进来的名字**已经**补过 "User." 前缀了
		//（补前缀那段逻辑在 AHitScanWeapon::SpawnTracerFX，两处都用同一份规则）
		if (TracerNiagaraComp && NiagaraEndParameter != NAME_None)
		{
			TracerNiagaraComp->SetVariableVec3(NiagaraEndParameter, EndPoint);
		}
	}
	else if (InParticles)
	{
		TracerParticlesComp = UGameplayStatics::SpawnEmitterAttached(
			InParticles,
			SceneRoot,
			NAME_None,
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			EAttachLocation::KeepRelativeOffset,
			/*bAutoDestroy=*/true);
	}

	/*
	 * 飞完就停住，但**不销毁** —— 拖尾还挂在身上飘着。
	 *
	 * 停住是为了别飞过头：不停的话它会一直往前飞，拖尾就拖到弹着点后面去了
	 *（打中墙的那一发，轨迹会穿墙继续延伸，看着像子弹穿过去了）。
	 * 生命周期给"飞行时间 + 拖尾散尽的时间"，到点由引擎连身上挂的特效一起收掉。
	 */
	const float FlightTime = FMath::Max(FlyDistance, 0.f) / SafeSpeed;
	if (FlightTime > KINDA_SMALL_NUMBER)
	{
		GetWorldTimerManager().SetTimer(StopTimer, this, &ABulletTracer::StopFlight, FlightTime, /*bLoop=*/false);
	}
	else
	{
		// 贴脸开枪（距离约等于 0）：没有可飞的路程，当场停住出个表现就行
		StopFlight();
	}

	SetLifeSpan(FlightTime + FMath::Max(TailLifeSpan, 0.f));
}

void ABulletTracer::StopFlight()
{
	GetWorldTimerManager().ClearTimer(StopTimer);

	if (Movement)
	{
		// StopMovementImmediately 会同时把 Velocity 和 InitialSpeed 清掉，
		// 所以之后它不会再被重力/自身速度推着动 —— 就是停在弹着点上
		Movement->StopMovementImmediately();
	}
}
