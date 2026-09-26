// Phoenix 曲线球（闪光弹）：E 抛出后沿弧线飞行，空中/撞墙爆炸。
// 爆炸时：半径内、视线未被遮挡、且视角朝向爆炸点的角色（含队友/自己，Valorant 式）被闪。
//
// ★ 视线遮挡有**两**道（2026-09-21 加的第二道）：
//   ① 世界几何：从爆炸点往受害者头部打一条 ECC_Visibility 的 LineTrace（地形/建筑挡）；
//   ② 挡视线的东西：遍历场上实现 `IVisionBlockerInterface` 的 actor 逐个问一遍
//      —— 目前是 APhoenixFlameWall（火墙）和 ACloveSmoke（烟）。
//   两道都只管**判定**：火墙/烟本身仍是全 NoCollision，闪光球照样从它们中间飞过去
//  （用户原话：挡的是判定，不是阻挡闪光弹的飞行）。
// 服务器权威飞行 + 复制移动，客户端纯跟随；盲判定在服务器执行（GAS GE 施加受害者，
// 受害者客户端靠 GE 复制 + HUD 读剩余时间画全屏白闪）。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PhoenixCurveball.generated.h"

class UGameplayEffect;
class UStaticMeshComponent;
class UPointLightComponent;
class UAudioComponent;
class USoundBase;
class UNiagaraComponent;
class UNiagaraSystem;

UCLASS()
class BLASTER_API APhoenixCurveball : public AActor
{
	GENERATED_BODY()

public:
	APhoenixCurveball();
	virtual void Tick(float DeltaTime) override;

	// 服务器调用：设定飞行参数开始飞行（bCurveLeft=true 向左弧，false 向右弧）
	void InitCurveball(const FVector& InDirection, bool bCurveLeftIn);

protected:
	virtual void BeginPlay() override;

	// 服务器：对范围内符合条件（距离 + LOS + 朝向）的角色施加 GE_FlashBlind，然后隐藏销毁
	void Detonate();
	void DestroyTimerFinished();

	UPROPERTY(VisibleAnywhere, Category = "Curveball")
	TObjectPtr<UStaticMeshComponent> CurveballMesh;

	UPROPERTY(VisibleAnywhere, Category = "Curveball")
	TObjectPtr<UPointLightComponent> CurveballLight;

	// 飞行音效播放组件（随球移动）
	UPROPERTY(VisibleAnywhere, Category = "Curveball")
	TObjectPtr<UAudioComponent> FlightSoundComp;

	// 被闪时施加的 GE（headless 配到 GE_FlashBlind）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Blind")
	TSubclassOf<UGameplayEffect> FlashBlindEffectClass;

	/*
	 * ——— 表现 ———
	 *
	 * 飞行中那颗球的特效（Niagara）。**规范位置就是这里**（和 APhoenixFireball::FlightEffect 同一个名字、
	 * 同一个用法）；填在下面 FlightFX 组件的 Asset 槽里也认（见 Blaster.h 的 ActivateNiagaraFX 注释）。
	 *
	 * 留空且组件上也没配 → 退回构造里那个占位球（CurveballMesh 的引擎球 + 点光），
	 * 保证"没配特效也看得见东西在飞"，不会出现暗屏上的隐形闪光球。
	 * 配了特效就把占位球藏掉（点光保留：它负责"墙上那片亮"那种环境感，和特效不冲突）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|FX")
	TObjectPtr<UNiagaraSystem> FlightEffect;

	// 飞行特效载体。挂根上跟着走；bAutoActivate=false，等 BeginPlay 拿齐资产再开
	UPROPERTY(VisibleAnywhere, Category = "Curveball")
	TObjectPtr<UNiagaraComponent> FlightFXComp;

	// 初始前进速度（沿抛出方向；巷战快闪：0.45s 内飞 ~5 米就爆）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Flight")
	float InitialSpeed = 1100.f;

	// 弧线段侧向加速度（用户要求弧小/平缓 → 降到 1200；出生即带初始侧向速度更早拐弯）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Flight")
	float CurveAccel = 1200.f;

	// 弧线持续时间（= Lifetime，全程弧线拐弯，弧线拉长）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Flight")
	float CurveTime = 0.45f;

	// 总飞行时长（到期爆炸；巷战快闪 → 0.45s，飞行距离 ≈ 495cm，FlashRadius 2000 覆盖）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Flight")
	float Lifetime = 0.45f;

	// 爆炸作用半径（> 飞行距离，抛球者看着球必被闪）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Blind")
	float FlashRadius = 2000.f;

	// 飞行呼啸音效（headless 生成的 whoosh，播放时随球移动）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Flight")
	TObjectPtr<USoundBase> FlightSound;

	// 被闪判定角（半角 = 此值/2：受害者视角方向与"受害者→爆炸点"方向夹角 < 半角才被闪）
	UPROPERTY(EditDefaultsOnly, Category = "Curveball|Blind")
	float FlashFOVDegrees = 120.f;

private:
	FVector Velocity = FVector::ZeroVector;
	// 抛掷方向的**水平右向**（= Up × Forward）。侧向加速度必须沿它施加 ——
	// ★ 别用 `FVector::RightVector`：那是**世界**的 (0,1,0)，和玩家面朝哪无关。
	//   拿它当"右"的话，朝 +X 抛看着是对的，朝 -X 抛左右就正好反了，朝 ±Y 抛更是变成
	//   顺着/逆着弹道推（球忽快忽慢、根本不拐）。见 InitCurveball。
	FVector CurveRight = FVector::RightVector;
	bool bCurveLeft = true;
	float Age = 0.f;
	bool bDetonated = false;
	FTimerHandle DestroyTimer;
};
