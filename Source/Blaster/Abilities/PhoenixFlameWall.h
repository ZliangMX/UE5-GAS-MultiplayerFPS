// Phoenix C「火墙 / Blaze」飞出去之后，被那个隐形球**一路立起来**的那面墙。
//
// ——— 它是怎么长出来的（用户 2026-09-20 定的版式）———
// 不是"落点生成一整面墙"，而是**球飞到哪、墙就长到哪**：
// 球每走一段就往这里塞一个"地面坐标"（AddPathPoint），每两个相邻坐标之间长出一块竖直的板。
// 所以球被掰弯（按住左键控球）时，墙跟着弯 —— 和原版那面沿着弹道铺开的火墙是同一个形状。
//
// 判定形状是**折线**：到这条折线（投影到 XY）的水平距离 ≤ DamageHalfWidth 就算"站在墙里"，
// 和 APhoenixFireZone 那圈火是同一个思路（遍历全场角色 + 距离比较，见那边的类注释），
// 伤害/回血的规则也照抄它（敌人掉血、施法者自己回血，靠 ABlasterPlayerState::Team 比队）。
//
// 「阻隔视野」分两层，别混：
//   · 看：靠**贴在板子上的那套火焰材质** —— 板子从地面立到 WallHeight，材质默认是
//     M_PhoenixFlameWall_Flow（竖直流动的火，见构造函数里的 WallDefaultMaterialPath），想换就填 WallMaterial。
//   · 判定：2026-09-21 起实现了 `IVisionBlockerInterface` —— **闪光弹爆炸算盲判定时会问它**
//     （每块立起来的板子做线段-盒子相交）。所以站在墙后面不会被闪，
//     而闪光球、子弹、人**照样穿过去**（⚠ 它不是障碍物：碰撞全关，人可以直接走进火里挨烧）。
//     ★ 板子本身是**零厚度的面片**，判定用的那个盒子是代码自己按 WallThickness 造的
//       （见 BlocksVisionSegment）—— "看得见的那一个面"和"挡视线的那个体积"是两回事，互不影响。
//
// ★ 网络：**服务器只复制"折线坐标"这一个数组**，每台机器各自拿它去摆自己那 32 块板子
//   （见 OnRep_PathPoints / SyncSegments）。
//   为什么不像平常那样直接改组件的显隐和位置：组件的 bVisible/RelativeTransform 是**推送式复制**
//   （USceneComponent 的 FDoRepLifetimeParams.bIsPushBased = true），改它得自己标脏；
//   而"复制一个 FVector 数组 + 两端跑同一段摆板子的代码"完全不碰那套机制，行为两端天然一致。
//   代价是这个数组最多传 33 个坐标（约 400 字节），可以忽略。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Blaster/Interfaces/VisionBlockerInterface.h"

#include "PhoenixFlameWall.generated.h"

class UStaticMeshComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UMaterialInterface;
class USoundBase;
class UAudioComponent;

UCLASS()
class BLASTER_API APhoenixFlameWall : public AActor, public IVisionBlockerInterface
{
	GENERATED_BODY()

public:
	APhoenixFlameWall();

	// IVisionBlockerInterface：线段是否穿过这面墙。
	// 逐块板子判 —— 用的是**已经立起来的那几块**的包围盒（还没长到的板子是隐藏的，跳过），
	// 所以墙长到一半时挡的范围也就是长到一半的那一段。
	virtual bool BlocksVisionSegment(const FVector& From, const FVector& To) const override;

	virtual void BeginPlay() override;

	// ——— 服务器：长墙 ———

	/*
	 * 球报一个位置过来（世界坐标，取球心即可 —— 贴地由这里自己做）。
	 *
	 * 太近的点会被丢掉（否则 1 秒能塞上千个点，而这个数组是要复制的）：
	 * 只有水平距离超过 SegmentSpacing 才真的记一笔，所以"一段一块板"是天然的。
	 * 返回 true 表示真的长出去了（球那边不关心，写出来是给调试看日志用的）。
	 */
	bool AddPathPoint(const FVector& InWorldPoint);

	// 球开始飞那一拍：开伤害结算 + 上一条硬保险丝（见 cpp 里为什么）
	void BeginWall();

	// 球飞完那一拍：从这一刻起算 WallDuration（球没飞完不该开始倒计时）
	void FinishWall();

	// 已经长了几个点（调试/日志用）
	int32 GetPathPointCount() const { return PathPoints.Num(); }

	// ——— 可调参数 ———

	// 墙高（厘米）。原版那面火墙比人高一头，默认 250。
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall")
	float WallHeight = 250.f;

	/*
	 * 挡视线的判定厚度（厘米）。
	 *
	 * ★ 它**不影响外观**：板子现在是一张零厚度的面片（2026-09-21 改的，为了配那套火焰材质），
	 *   所以这个厚度纯粹是给 BlocksVisionSegment 造那个判定盒子用的。
	 *   给 20 是因为"贴着墙站也算躲好了"：判定盒子太薄的话，斜着看/擦着墙边容易漏判，
	 *   表现就是"明明躲在墙后还是被闪"。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall")
	float WallThickness = 20.f;

	// 隔多远长一块板（厘米）。也就是折线取点的最小间距 —— 调小 = 墙更贴合弯道但更费板子/带宽。
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall")
	float SegmentSpacing = 150.f;

	/*
	 * 板子总数上限。32 块 × 1.5 米 = 48 米，球 1 秒（1800cm/s）最多飞 18 米，绰绰有余。
	 *
	 * ⚠ **在 BP 里改这个数没有用**（改大改成 64 也只会多记点、不会多出板子）：
	 *   组件的数量是 C++ 构造函数里就定死的（见上面"为什么预建组件"），而 BP 的类默认值
	 *   是在构造函数**之后**才盖上去的 —— 那时这排 Segment 已经建完了。
	 *   真正生效的上限是 min(这个值, Segments.Num())（见 GetEffectiveMaxSegments），
	 *   所以想真的加长，只能改这里再重编 C++，或者调大 SegmentSpacing。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall")
	int32 MaxSegments = 32;

	// 墙存活多久（秒，从球飞完算起）。整面墙一起消失 —— 原版也是这样。
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall")
	float WallDuration = 8.f;

	// ——— 判定（默认值和 APhoenixFireZone 保持一致："伤害逻辑和火球一样"）———

	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	float DamageTickInterval = 0.25f;

	// 每跳对敌人的伤害（火球是 4）
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	float DamagePerTick = 4.f;

	// 每跳给施法者本人的回血（火球是 3）
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	float HealPerTick = 3.f;

	/*
	 * 离墙多远算"站在火里"（厘米，水平距离，判到折线为止）。
	 * 墙本身只有 20cm 厚，但角色是个半径 42 的胶囊，贴着墙走的时候中心离墙心也有半米 ——
	 * 100 是"贴着墙就烧"的量级（想更宽容就调大，想只有真的站进火里才烧就调到 40 左右）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	float DamageHalfWidth = 100.f;

	// 竖直容差（厘米）：判据和火圈完全一样，见 APhoenixFireZone::VerticalTolerance 那段
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	float VerticalTolerance = 200.f;

	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	bool bDamageEnemies = true;

	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|Damage")
	bool bHealCaster = true;

	// ——— 表现（占位，逻辑优先）———

	/*
	 * 墙的材质。留空 = 用构造函数里指定的那套默认火焰材质（M_PhoenixFlameWall_Flow，竖直流动的火）。
	 * 换成别的材质就给这里填（★ 面片是双面的，自写材质记得勾 Two Sided，否则从背后看不见）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|FX")
	UMaterialInterface* WallMaterial;

	// 整面墙的火焰/烟特效（Niagara，挂在墙 actor 上）。留空就只有那些板子。
	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|FX")
	TObjectPtr<UNiagaraSystem> WallEffect;

	UPROPERTY(EditDefaultsOnly, Category = "FlameWall|FX")
	TObjectPtr<USoundBase> LoopSound;

protected:
	UPROPERTY(VisibleAnywhere, Category = "FlameWall")
	TObjectPtr<USceneComponent> SceneRoot;

	/*
	 * 预建好的一排板子，平时全藏着。
	 *
	 * 每块板是一张 **1 米见方的面片**（/Engine/BasicShapes/Plane），靠 WorldScale 拉到
	 * 长 × 墙高 —— 所以它是"一个面"，不是一块有厚度的板（厚度只在判定里存在，见 WallThickness）。
	 * 朝向在 PlaceSegment 里定：局部 X 沿墙、局部 Y 朝上，法线自然垂直于墙面。
	 *
	 * 为什么是"预先建一堆"而不是"长一块 Spawn 一块"：动态生成的组件要参与复制得处理
	 * 注册时机和标脏一堆事，而这面墙的形状本来就只是个**折线数组** ——
	 * 复制数组、两端各摆各的板子，代码短、两端一致、不存在"客户端少了一块"的可能。
	 * 代价只是 32 个空组件（NoCollision、隐藏，不画不判）。
	 */
	UPROPERTY(VisibleAnywhere, Category = "FlameWall")
	TArray<TObjectPtr<UStaticMeshComponent>> Segments;

	UPROPERTY(VisibleAnywhere, Category = "FlameWall")
	TObjectPtr<UNiagaraComponent> WallFXComp;

	UPROPERTY(VisibleAnywhere, Category = "FlameWall")
	TObjectPtr<UAudioComponent> LoopSoundComp;

	/*
	 * 墙的折线（**世界坐标，已经贴过地**）。
	 *
	 * 服务器：每长一块就 Add 一个（AddPathPoint）。客户端：由本属性复制过来。
	 * 两端都拿它跑 SyncSegments() —— 这是这面墙唯一的"形状数据"。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_PathPoints)
	TArray<FVector> PathPoints;

	UFUNCTION()
	void OnRep_PathPoints();

	// 拿现在的 PathPoints 去摆那 32 块板子（服务器长完就调，客户端由 OnRep 调）
	void SyncSegments();

	/*
	 * 真正生效的板子上限 = min(MaxSegments, 已经建出来的组件数)。
	 * 两个数在 BP 里能对不上（见 MaxSegments 那段），所有"还能不能再长一块"的判断都走这里，
	 * 免得记了一个摆不出来的点 —— 那面墙会在这里**无声地断掉一截**。
	 */
	int32 GetEffectiveMaxSegments() const
	{
		return FMath::Max(0, FMath::Min(MaxSegments, Segments.Num()));
	}

	// 把一块板摆到 A→B 之间（两端都已经贴地）
	void PlaceSegment(UStaticMeshComponent* Segment, const FVector& A, const FVector& B);

	/*
	 * 把球的位置按到地面上（往下打一条线）。
	 * 打不到东西（飞在空地上方）就用原来的 Z —— 宁可墙浮在半空，也不要它掉到地底下去。
	 */
	FVector SnapToGround(const FVector& InWorldPoint) const;

	// 服务器每 DamageTickInterval 一次：折线附近的人掉血 / 施法者回血
	void ApplyWallTick();

private:
	FTimerHandle DamageTimer;
};
