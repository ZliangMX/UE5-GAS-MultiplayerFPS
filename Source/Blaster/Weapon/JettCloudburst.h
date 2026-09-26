// Jett 的 C「逐风云 / Cloudburst」飞出去的那朵云。
//
// 生命周期：指尖生成 → 顺着角色视角直线飞 → **1.5 秒到期 或 撞到东西** → 绽放成烟球。
// 前半段（这朵云）是纯视觉，没有任何判定；后半段（烟球）直接生成一个 ACloveSmoke，
// 复用暮蝶封烟那套已经调好的"纯视觉球壳 + 复制"实现 —— 见 ACloveSmoke 的类注释，
// 烟不挡弹道也不挡视线，是刻意的。
//
// ⚠️ 放这个类**不需要**改 ACloveSmoke。烟雾半径和持续时间都通过 SpawnActorDeferred
//    在 BeginPlay 之前写进去（ACloveSmoke 在 BeginPlay 里 SetLifeSpan，所以必须在
//    FinishSpawning 之前改，晚了这一发就是暮蝶那 15 秒）。
//
// 为什么不用 AProjectile 当基类：那套是"武器打出去的东西"（带伤害、命中反馈、
// 爆炸伤害），云的命中什么都不做，只要一个"撞到了"的事件。硬套过去会带进一堆
// 用不上还要小心关掉的伤害逻辑。
//
// 网络：服务器权威跑飞行 + 碰撞，客户端只跟随（ProjectileMovementComponent 的
// 标准复制）。绽放特效走 NetMulticast，因为 Niagara 和 Montage 一样是纯本地表现。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JettCloudburst.generated.h"

class UProjectileMovementComponent;
class UStaticMeshComponent;
class USphereComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UAudioComponent;
class USoundBase;
class ACloveSmoke;

UCLASS()
class BLASTER_API AJettCloudburst : public AActor
{
	GENERATED_BODY()

public:
	AJettCloudburst();

	virtual void BeginPlay() override;

	// 服务器调用：设定飞行方向 + 速度，开始飞（方向由能力从角色视角算好传进来）
	void InitCloud(const FVector& InDirection);

	virtual void Tick(float DeltaSeconds) override;

	/*
	 * 这一趟"最久能被按住多久"（秒）—— 给空手保险丝用。
	 *
	 * 空手那段的时长由玩法决定（云飞多久 = 最多按多久），不是由动画长度决定，所以
	 * UJettCloudburstAbility::OnEmptyHandStarted 拿这个值去重设保险丝（见它的实现）。
	 * 留一点余量：真到点了也没关系，Bloom 会把按住这段一起收掉，这个值只是"万一没了下文"
	 * 时的兜底 —— 所以宁可大一点（大 = 多站一会儿，小 = 动画被切）。
	 */
	float GetMaxHoldDuration() const { return FlyDuration + FMath::Max(0.f, HoldFuseMargin); }

	// ——— 按住跟准心 ———
	// 只在"还按着 C"期间生效（读 ABlasterCharacter::IsCloudburstHoldActive），
	// 松手之后云保持最后一次被掰到的方向直线飞到绽放。
	// 只改**方向**，速度恒为 FlySpeed —— 用户的要求："不影响向前的速度"。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	bool bSteerToCrosshair = true;

	/*
	 * 转向增益（1/秒）：航向与"准心那根轴"的夹角每差 1°，每秒补多少度。
	 *
	 * 这是个一阶（指数收敛）控制率，收敛时间常数 τ ≈ 1 / 增益：
	 *   增益 6  → τ ≈ 0.167s，大约 0.4 秒内肉眼看着已经贴到准心（用户选的"中等"档）
	 *   增益 6 时，偏 30°（约屏幕边缘的一半）对应的转速是 180°/s —— 正好是用户给的目标值
	 * 调大 = 云更"黏"准心、甩得快但容易过冲（看着像自己在飘）；调小 = 迟钝、甩不动。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	float SteeringGain = 6.f;

	// 转速上限（度/秒）。增益是线性的、偏角很大时会给出很夸张的转速（90° → 540°/s，
	// 看着像瞬移），所以封顶。用户给的上限是 240。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	float MaxSteeringRate = 240.f;

	// 死区（度）：航向和准心轴夹角小于它就不转。准心附近那点抖没有意义，
	// 而且一阶控制率在误差趋零时本来就会一直小幅修，看着像在抽。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	float SteeringDeadZone = 1.f;

	// 航向俯仰的硬夹（度）。不夹的话云可以被掰到垂直往上/往下飞，
	// 投影到屏幕上的位移趋近于零 —— 表现是"按着也不动了"，很难看出是俯角太大。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	float MaxSteeringPitch = 80.f;

	// GetMaxHoldDuration 里给保险丝留的余量（秒）
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Steering")
	float HoldFuseMargin = 0.5f;

	// 烟雾球（BP_CloveSmoke 或另建一个白色版本的 BP）。**必填** ——
	// 留空的表现是"云飞出去了、然后什么都没有"，很难往"没填烟雾类"上想，会打日志骂人。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Bloom")
	TSubclassOf<ACloveSmoke> SmokeClass;

	// 绽放后烟球半径（厘米）。Valorant 逐风云比暮蝶的烟小一圈，默认 200。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Bloom")
	float SmokeRadius = 200.f;

	// 绽放后烟球存活时长（秒）。用户要求 3 秒。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Bloom")
	float SmokeDuration = 3.f;

	// 飞行速度（cm/s）。1.5s 寿命 × 速度 = 最大飞行距离（默认 1500×1.5 ≈ 22 米）。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Flight")
	float FlySpeed = 1500.f;

	// 飞行时长（秒）：到点不论撞没撞到都绽放。用户要求 1.5 秒。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|Flight")
	float FlyDuration = 1.5f;

	// 飞行中那朵云的特效（Niagara）。留空时退回 CloudMesh 那个小球，
	// 保证"没配特效也看得见东西在飞"，不会出现暗屏上的隐形子弹。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|FX")
	TObjectPtr<UNiagaraSystem> CloudEffect;

	// 绽放瞬间的一次性特效（Niagara，炸开那一下）。留空只是少个特效，不影响烟球。
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|FX")
	TObjectPtr<UNiagaraSystem> BloomEffect;

	// 飞行音效（随云移动）
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst|FX")
	TObjectPtr<USoundBase> FlightSound;

protected:
	// 碰撞球。只有它参与"撞到东西"的判定（云的特效/网格都是 NoCollision）
	UPROPERTY(VisibleAnywhere, Category = "Cloudburst")
	TObjectPtr<USphereComponent> CollisionSphere;

	// 飞行特效载体（Niagara 组件挂在 actor 上，随 actor 复制到所有客户端）
	UPROPERTY(VisibleAnywhere, Category = "Cloudburst")
	TObjectPtr<UNiagaraComponent> CloudFXComp;

	// 没配 CloudEffect 时的可见兜底：引擎自带球（半径 50cm），构造里设成 CloudVisualScale 倍。
	UPROPERTY(VisibleAnywhere, Category = "Cloudburst")
	TObjectPtr<UStaticMeshComponent> CloudMesh;

	// 兜底球的缩放（只是让没配 Niagara 时也有个大小合理的团，配了特效就完全用不上）
	UPROPERTY(EditDefaultsOnly, Category = "Cloudburst")
	float CloudVisualScale = 0.6f;

	UPROPERTY(VisibleAnywhere, Category = "Cloudburst")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	UPROPERTY(VisibleAnywhere, Category = "Cloudburst")
	TObjectPtr<UAudioComponent> FlightSoundComp;

	// 撞到东西（只在服务器绑定）
	UFUNCTION()
	void OnCloudHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);

private:
	// 飞行到期（1.5 秒到）→ 绽放
	void OnFlyTimerExpired();

	/*
	 * 服务器每帧：还按着 C 的话，把航向朝"准心的那根轴"掰一点（见头文件里那几个参数）。
	 *
	 * 为什么在**服务器**算而不是射手本机算了发过来：
	 *   · 云的位置本来就是服务器权威 + 复制（ProjectileMovement 那套），谁算方向就得谁算位移；
	 *   · 远端玩家的 ControlRotation 在服务器上是复制的、拿得到，而"屏幕上偏了多少像素"
	 *     这种量服务器根本读不到（没有视口）—— 所以这里用的是**夹角**而不是屏幕坐标：
	 *     把"云相对镜头的位置"转到视角基底里，拿 Z/X、Y/X 当偏移量，等价于屏幕偏移的
	 *     tan 值，和视口大小 / FOV 都无关。
	 * 代价是远端玩家会按一个 RTT 前的视角来掰 —— 和移动预测同一类误差，可接受。
	 */
	void SteerTowardsCrosshair(float DeltaSeconds);

	// 服务器：生成烟球 + 广播绽放特效 + 关掉飞行态。只执行一次（bBloomed 守卫）。
	void Bloom();

	// 所有客户端：在这放一次性的绽放特效（Niagara 不复制，必须广播）
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastBloomFX(FVector Location, FRotator Rotation);

	// 本次是否已经绽放过了：碰撞和到期定时器可能在同一帧先后触发，
	// 没有这个守卫会生成两个烟球（而且两个都 3 秒）。
	bool bBloomed = false;

	FTimerHandle FlyTimer;
};
