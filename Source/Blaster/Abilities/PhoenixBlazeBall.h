// Phoenix C「火墙 / Blaze」飞出去的那颗球 —— **隐形、无碰撞**（用户 2026-09-20 的要求）。
//
// 它自己什么都不做，就是一趟"带着方向的飞行"：
//   · 位置每帧推进；按住左键期间航向**朝准心掰**（算法和 AJettCloudburst::SteerTowardsCrosshair
//     一模一样，那边已经调好了，这里照抄一份而不是抽公共函数 —— 见下面"为什么重复"）
//   · 每走一段就把自己的位置喂给墙（APhoenixFlameWall::AddPathPoint），墙在**地面投影**上长出来
//   · 飞完（FlyDuration ≈ 1 秒）→ 让墙开始倒计时 / 收掉射手那一段"按住控球"
//
// 为什么重复 AJettCloudburst 那套转向而不是抽出来共用：
//   那朵云的生命周期和这套耦合得很紧（撞墙、绽放、跟的是 bCloudburstHoldActive），
//   抽公共函数就得把"读谁的状态、什么时候停"一起参数化 —— 改动的收益不抵碰坏一条已经调好的手感。
//   代价是这里多 60 行，好处是两颗球可以各调各的手感（火墙要的是"1 秒内甩到位"）。
//
// ★ 为什么不用 AProjectile / ProjectileMovementComponent：
//   · 用户明确要求**碰到墙不会炸开**，也就是全程不需要任何碰撞事件；
//   · 速度方向每帧都可能被改写（跟准心），位置又只有服务器说了算，
//     直接手推比让 ProjectileMovement 每帧被覆盖一次更直白。
//
// 网络：服务器跑飞行 + 长墙，客户端只跟随位置（SetReplicateMovement）。
// 球本身没有任何视觉，所以客户端"跟不跟得上"其实看不出来 ——
// 留着复制是为了哪天加了飞行特效（BallEffect）别人也看得见。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PhoenixBlazeBall.generated.h"

class USceneComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class APhoenixFlameWall;

UCLASS()
class BLASTER_API APhoenixBlazeBall : public AActor
{
	GENERATED_BODY()

public:
	APhoenixBlazeBall();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// 服务器调用：设定飞行方向 + 速度，开始飞（方向由能力从角色视角算好传进来）
	void InitBall(const FVector& InDirection);

	// ——— 可调参数 ———

	/*
	 * 飞出去之后长出来的那面墙的类。
	 *
	 * 默认就是 C++ 的 APhoenixFlameWall（不填也能用）；要在 BP 里换一张贴图/材质，
	 * 就建一个 BP_PhoenixFlameWall 填到这里。留空 = 只飞不长墙（会打日志骂人）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Wall")
	TSubclassOf<APhoenixFlameWall> WallClass;

	/*
	 * 飞行速度（厘米/秒）。
	 *
	 * 墙有多长 = 这个数 × FlyDuration（球飞到哪，墙就从手里铺到哪），所以**改墙长就是改它**。
	 *   1800 → 约 18 米（原版那面墙的长度量级）
	 *   1440 → 约 14.4 米（2026-09-21 用户要求"现在的 0.8 倍"，1800 × 0.8）
	 * ⚠ 别去动 FlyDuration 来改长度：那个决定的是"甩出去要多久"，1 秒是手感，缩了会变成赶工。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Flight")
	float FlySpeed = 1440.f;

	// 飞行时长（秒）。用户要求"整个过程差不多 1 秒左右"，到点不管飞多远都收。
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Flight")
	float FlyDuration = 1.f;

	// 路径采样间隔的兜底（秒）：两次 AddPathPoint 之间至少隔这么久。
	// 真正决定"多长一块板"的是墙那边的 SegmentSpacing，这个只是防止高帧率下每帧都去问一遍。
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Flight")
	float PathSampleInterval = 0.05f;

	// ——— 跟准心（参数含义和 AJettCloudburst 同名参数完全一致，见那边的注释）———

	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Steering")
	bool bSteerToCrosshair = true;

	/*
	 * 跟手程度。2026-09-21 从 6 / 240 调到 12 / 720（用户："火墙再跟手一些"）。
	 *
	 * 为什么原来明显拖：球总共只飞 1 秒，而旧值下甩 30° 要 ~0.4 秒才收敛、90° 这种大甩
	 * 直接被 240°/s 的转速上限卡住 0.375 秒 —— 一秒里有小半秒在追准心，墙的尾段看着就是"慢半拍"。
	 * 新值：30° ≈ 0.25 秒、90° ≈ 0.125 秒。
	 *
	 * ⚠ 再往上调**不会更跟手了**：真正卡住观感的下一环是墙的取点间距
	 *   （APhoenixFlameWall::SegmentSpacing = 150cm ≈ 83ms 一块板）—— 球掰得再快，
	 *   墙也是一格一格长出来的。要更细腻得动那个值（顺带 MaxSegments 也要跟上，否则墙会变短）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Steering")
	float SteeringGain = 12.f;

	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Steering")
	float MaxSteeringRate = 720.f;

	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Steering")
	float SteeringDeadZone = 1.f;

	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Steering")
	float MaxSteeringPitch = 80.f;

	/*
	 * 调试：把球的轨迹画出来。
	 *
	 * **默认关**。但它是这颗球唯一的排查手段 —— 球是隐形的（用户要求），
	 * 出了"墙没长出来 / 长歪了"这种问题时，打开它就能看到球到底往哪飞了。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|Debug")
	bool bDrawDebugPath = false;

	// 飞行特效（可选，挂在球上）。**默认留空 = 完全隐形**（用户要求），想加就填。
	UPROPERTY(EditDefaultsOnly, Category = "Blaze|FX")
	TObjectPtr<UNiagaraSystem> BallEffect;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Blaze")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Blaze")
	TObjectPtr<UNiagaraComponent> BallFXComp;

	// 这一趟长出来的墙（服务器）。球只管"喂点"和"收尾"，墙的寿命/判定全是墙自己的事。
	TWeakObjectPtr<APhoenixFlameWall> ActiveWall;

private:
	// 服务器每帧：把航向朝准心掰一点（只在射手"还按着左键"期间）。
	// 复用 AJettCloudburst 那套视角基底里的算法，理由见头文件。
	void SteerTowardsCrosshair(float DeltaSeconds);

	// 把自己的位置喂给墙（内部按 SegmentSpacing 决定要不要真的长一块）
	void FeedWall();

	// 飞完了（到时 / 被打断）：让墙开始倒计时 + 收掉射手那段"按住控球" + 自己退场。只执行一次。
	void EndFlight();

	// 服务器：生成那面墙（在 BeginPlay 里，飞行之前）
	void SpawnWall();

	FTimerHandle FlyTimer;

	// 当前航向（单位向量）。速度大小恒为 FlySpeed，只有方向会被转向改写。
	FVector FlightDir = FVector::ForwardVector;

	// 到点收尾只走一次（到期定时器和"球被销毁"两条路都可能进来）
	bool bFlightEnded = false;

	float PathSampleAccumulator = 0.f;
};
