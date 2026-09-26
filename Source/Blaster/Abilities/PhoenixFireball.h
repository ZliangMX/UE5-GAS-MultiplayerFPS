// Phoenix 的 Q「火球 / Hot Hands」扔出去的那颗球。
//
// 生命周期：手上生成 → 顺着视角（**含俯仰**，可以往脚下砸）直线飞 →
//   · 飞满 MaxRange（25 米）→ 丢平、改成垂直下坠（"飞到头就直着掉下来"）
//   · 或者中途撞到人/墙/地 → 就地结束飞行
// → 落地那一拍生成一个 APhoenixFireZone（地上那圈火）。
//
// ⚠️ 火圈铺在"球正下方的地面"，不是"球停住的位置"：撞在人身上时球停在半空、
//    撞墙时贴在墙面上，而火都得落在它们脚下的地上 —— 见 ResolveGroundPoint。
//
// 为什么不用 AProjectile 当基类：和 AJettCloudburst 同一个理由 —— 那套是"武器打出去的东西"
//（自带伤害、命中反馈、爆炸范围伤害），而这里的"命中"本身就是要生成火圈，硬套过去
// 会带进一堆用不上、还要小心关掉的伤害逻辑。
//
// 网络：服务器权威跑飞行（ProjectileMovement 的标准复制），"该下坠了"/"撞停了"也只在服务器判定；
// 落地特效走 NetMulticast —— Niagara 和 Montage 一样不复制，每个客户端各自播。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PhoenixFireball.generated.h"

class UProjectileMovementComponent;
class UStaticMeshComponent;
class USphereComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UAudioComponent;
class USoundBase;
class UMaterialInterface;
class APhoenixFireZone;

UCLASS()
class BLASTER_API APhoenixFireball : public AActor
{
	GENERATED_BODY()

public:
	APhoenixFireball();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// 服务器调用：给初速度、开始飞。方向由能力从角色视角算好传进来（含俯仰分量）。
	void InitFireball(const FVector& InDirection);

	// ——— 飞行 ———

	// 飞行速度（cm/s）。用户要求"速度没有特别快"：1800 ≈ 18 米/秒，
	// 飞满 25 米约 1.4 秒 —— 看得清轨迹、也躲得开。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Flight")
	float FlySpeed = 1800.f;

	// 直线段射程（厘米）。飞满就丢平下坠。用户要求 25 米。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Flight")
	float MaxRange = 2500.f;

	/*
	 * 下坠段的重力倍率（相对世界重力）。
	 *
	 * 注意下坠距离其实很短：球是从胸口高度（约 160cm）平着飞出去的，"直着掉下来"
	 * 到地面只有一米多，所以这个值影响的是"最后那一下有多干脆"，不是飞行轨迹。
	 * 4 倍重力 ≈ 0.28 秒砸到地，看着像沉下去；1 倍（世界重力）会明显飘。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Flight")
	float DropGravityScale = 4.f;

	// 保险丝（秒）：飞这么久还没落地就强行落地生成火圈。
	// 只在"球一直没碰到任何东西"（飞出地图边缘/掉进无底洞）时兜底 —— 没有它，
	// 这一发就是"扔出去了、然后什么都没有"，最难查。正常飞行+下坠远用不到这个值。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Flight")
	float MaxFlightTime = 6.f;

	// ——— 落地 ———

	// 落地生成的火圈类（BP_PhoenixFireZone）。**必填** —— 留空的表现是
	// "球飞出去了、炸了一下、地上什么都没有"，很难往"没填火圈类"上想，会打日志骂人。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Land")
	TSubclassOf<APhoenixFireZone> FireZoneClass;

	// 找地面时往下打多深（厘米）。球撞在人身上时要从胸口往下找到他脚下的地。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Land")
	float GroundTraceDownDistance = 1000.f;

	// 找地面时先往上抬多少再往下打（厘米）：避免球贴地时起点已经在面片下面、射线朝下打空。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Land")
	float GroundTraceUpOffset = 50.f;

	// 落地后这颗球还留多久（秒）：只是让爆炸特效播完，不参与任何判定。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|Land")
	float DespawnDelay = 0.5f;

	// ——— 表现（占位视觉，用户要求逻辑优先）———

	// 飞行中那颗球的特效（Niagara）。留空时退回 FireMesh 那个发光小球 ——
	// 保证"没配特效也看得见东西在飞"，不会出现暗屏上的隐形投掷物。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|FX")
	TObjectPtr<UNiagaraSystem> FlightEffect;

	// 落地瞬间的一次性特效（Niagara）。留空只是少个特效，不影响火圈。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|FX")
	TObjectPtr<UNiagaraSystem> LandEffect;

	// 飞行音效（随球移动）
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|FX")
	TObjectPtr<USoundBase> FlightSound;

	// 落地音效
	UPROPERTY(EditDefaultsOnly, Category = "Fireball|FX")
	TObjectPtr<USoundBase> LandSound;

protected:
	// 碰撞球。只有它参与"撞到东西"的判定（特效/网格都是 NoCollision）
	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<USphereComponent> CollisionSphere;

	// 飞行特效载体（Niagara 组件挂在 actor 上，随 actor 复制到所有客户端）
	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UNiagaraComponent> FlightFXComp;

	// 没配 FlightEffect 时的可见兜底：引擎自带球（半径 50cm），构造里设成 VisualScale 倍。
	// 兜底材质可以在 BP 里换（默认是引擎那个灰的 —— 依然看得见，这就够了）。
	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UStaticMeshComponent> FireMesh;

	// 兜底球的缩放（只是让没配 Niagara 时也有个大小合理的团，配了特效就用不上它了）
	UPROPERTY(EditDefaultsOnly, Category = "Fireball")
	float VisualScale = 0.4f;

	// 兜底球 / 特效的材质。BP 里指派；留空 = 网格自带材质。
	UPROPERTY(EditDefaultsOnly, Category = "Fireball")
	UMaterialInterface* FireMaterial;

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	UPROPERTY(VisibleAnywhere, Category = "Fireball")
	TObjectPtr<UAudioComponent> FlightSoundComp;

	// 撞到东西（只在服务器绑定）
	UFUNCTION()
	void OnFireballHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);

private:
	// 飞出 MaxRange → 丢平、打开重力，转成垂直下坠段
	void BeginDrop();

	// 真正结束飞行：停住、关判定、放落地特效、生成火圈。只认第一次（bLanded 守卫）。
	// ImpactPoint 用于找落点地面；HitActor（可为空）在找地面时被忽略 —— 撞到人时
	// 不能把他自己当"地面"停住。
	void LandAt(const FVector& ImpactPoint, AActor* HitActor);

	// 保险丝到点：飞太久还没落地，就地强行落地
	void OnFlightFuseExpired();

	// 从 From 往下找脚下的地面（找不到就退回 From 本身）
	FVector ResolveGroundPoint(const FVector& From, AActor* IgnoredActor) const;

	// 所有客户端：播一次性的落地特效 + 音效（Niagara 不复制，必须广播）
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastLandFX(FVector Location);

	// 已经飞过的直线距离（厘米）。只在服务器累计 —— 客户端各自算会因为
	// 帧率/到来的速度包不同步而"我这边先下坠"。
	float TraveledDistance = 0.f;

	// 已经进入下坠段
	bool bDropping = false;

	// 已经落地并生成过火圈：碰撞回调和保险丝定时器可能在同一帧先后触发，
	// 没有这个守卫会铺两圈火（而且是两份伤害）。
	bool bLanded = false;

	// InitFireball 时记下的施法者（Owner）。SpawnActorDeferred 之前要 IgnoreActorWhenMoving，
	// 防止刚生成就撞在自己身上。
	UPROPERTY()
	TObjectPtr<AActor> MyOwner;

	FTimerHandle FlightFuseTimer;
};
