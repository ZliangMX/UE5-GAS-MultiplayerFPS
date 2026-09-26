// Phoenix Q「火球」落地后地上那一圈火。
//
// 视觉那半套照抄 ACloveSmoke（占位网格 + 服务器权威寿命 + 复制），但它多了这个类真正的本体：
// **服务器每 DamageTickInterval 结算一次范围内的角色**：
//   · 敌人 → UGameplayStatics::ApplyDamage（和枪械同一条伤害链路，自带护甲结算、伤害数字、
//     击杀归属）
//   · 施法者本人 → HealByAbility（Valorant 原版：火男踩自己的火持续回血）
//
// 判定形状是**圆柱**：水平距离 ≤ FireRadius 且竖直差不超 VerticalTolerance。
// 不做视线判定 —— 地上的一滩火，站进去就该烧，为什么要看得到。
//
// ⚠️ 工程里的伤害链路本身**没有友伤门槛**（武器那边只有"打中敌人"的命中反馈，
//    伤害照给）。所以"只伤敌人"这件事必须在**这里**做：靠 ABlasterPlayerState::Team 比队。
//    哪天想让火也烧队友，删掉 ApplyFireTick 里那段比队即可。
//
// ⚠️ 和枪械一样，伤害只在服务器产生；客户端的血量/伤害数字靠 Health 复制 + 广播 RPC 跟上。
//    这个类在客户端上就是一个只会播特效的空壳（tick 都没开）。
//
// 为什么不放 Weapon/ 目录：它不是"武器打出去的东西"，是技能的残留物 ——
// 和它的两个同伴（APhoenixFireball / GA 配置）一起放在 Abilities/ 里更好找。
//（和 ACloveSmoke 一样的理由。）

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PhoenixFireZone.generated.h"

class UStaticMeshComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UAudioComponent;
class USoundBase;
class UMaterialInterface;

UCLASS()
class BLASTER_API APhoenixFireZone : public AActor
{
	GENERATED_BODY()

public:
	APhoenixFireZone();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	// ——— 判定 ———

	// 火圈半径（厘米）。用户按 Valorant 原版给的量级：200 ≈ 2 米直径 4 米。
	// 构造时按网格资产的包围盒换算成缩放，所以调这个值**不需要**去 BP 里改 Scale。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float FireRadius = 200.f;

	// 存活时长（秒）。只在服务器计时，客户端的销毁由复制跟随。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float FireDuration = 4.f;

	// 伤害结算间隔（秒）。0.25 秒一跳 —— 站进去会看到血条一格一格掉，
	// 而不是"进去掉一大截"（那样看不出是持续伤害，也没机会逃）。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float DamageTickInterval = 0.25f;

	// 每跳对敌人的伤害。4 × 16 跳 ≈ 满打满算 64 点（还要先过护甲）——
	// Valorant 的 Hot Hands 是 60 点左右，量级对上：站在里面不动就废掉大半条命，
	// 但站着挨完整轮也打不死满血的人。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float DamagePerTick = 4.f;

	// 每跳给施法者本人的回血。3 × 16 跳 ≈ 48 点 —— 和"进去掉血"是同一个量级，
	// 所以这圈火对自己的意义是"回一口血"，不是无敌。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float HealPerTick = 3.f;

	/*
	 * 竖直容差（厘米）：以火圈所在的平面为基准，角色**胶囊中心**离它超过这个值就不算站在火里。
	 *
	 * 为什么要这个值：角色位置是胶囊中心（脚底往上约 88cm），和火圈贴地的高度天然差着一个
	 * 半个身位；同时它是"人在楼上/楼下"的唯一判据 —— 没有它，站在火正上方二层的人也会被烧。
	 *
	 * 200 的取值：站着不动 ΔZ≈88 ✓、跳起来（+最多约 120）✓、楼上/楼下差一层（300+）✗。
	 * 代价是"层高不到 2 米"的地形会误伤（这工程的地图没有这种地方）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	float VerticalTolerance = 200.f;

	// 是否对敌人造成伤害（关掉 = 只有视觉和施法者回血）
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	bool bDamageEnemies = true;

	// 施法者本人站在自己的火里是否回血（用户要求开）
	UPROPERTY(EditDefaultsOnly, Category = "FireZone")
	bool bHealCaster = true;

	// ——— 表现（占位视觉，用户要求逻辑优先）———

	// 火焰特效（Niagara）。留空时退回 FireMesh 那个平铺的发光圆盘 ——
	// 保证"没配特效也看得见地上有东西"，不会出现"踩上去莫名其妙掉血"。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone|FX")
	TObjectPtr<UNiagaraSystem> FireEffect;

	// 燃烧循环音效
	UPROPERTY(EditDefaultsOnly, Category = "FireZone|FX")
	TObjectPtr<USoundBase> LoopSound;

	// 火圈材质。BP 里指派；留空 = 网格自带材质（引擎那个灰的，依然看得见）。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone|FX")
	UMaterialInterface* FireMaterial;

	// 圆盘的厚度缩放（相对 XY 缩放）。占位网格是引擎自带圆柱（高 100），
	// 压扁成一个"贴地的圆盘"：0.05 → 5cm 厚，够不显眼又不至于被地面裁掉一半。
	UPROPERTY(EditDefaultsOnly, Category = "FireZone|FX")
	float ThicknessScale = 0.05f;

protected:
	// 火圈网格。默认是引擎自带圆柱（BP 里可以换成真正的火焰贴花/模型）。
	// 无论换成什么，只要它是以原点为中心、底面朝 ±Z 的圆盘，半径就由 FireRadius 统一决定。
	UPROPERTY(VisibleAnywhere, Category = "FireZone")
	TObjectPtr<UStaticMeshComponent> FireMesh;

	// 火焰特效载体（Niagara 组件挂在 actor 上，随 actor 复制到所有客户端）
	UPROPERTY(VisibleAnywhere, Category = "FireZone")
	TObjectPtr<UNiagaraComponent> FireFXComp;

	UPROPERTY(VisibleAnywhere, Category = "FireZone")
	TObjectPtr<UAudioComponent> LoopSoundComp;

private:
	// 服务器每 DamageTickInterval 一次的结算：敌人掉血、施法者回血
	void ApplyFireTick();

	FTimerHandle DamageTimer;
};
