// Phoenix 曲线球（闪光弹）：E 抛出后沿弧线飞行，空中/撞墙爆炸。
// 爆炸时：半径内、视线未被遮挡、且视角朝向爆炸点的角色（含队友/自己，Valorant 式）被闪。
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
	bool bCurveLeft = true;
	float Age = 0.f;
	bool bDetonated = false;
	FTimerHandle DestroyTimer;
};
