// 购买阶段的出生屏障（"激光墙"）。
//
// ——— 怎么用 ———
// 在关卡里直接摆（拖一个 Cube 那样转、缩放成你要的长度和高度），**不用在 GameMode 上登记什么**：
// GameMode 每回合进入购买阶段时会把场上所有 ABlastBarrier 一起点亮，购买阶段一结束立刻收掉
//（见 ABlasterGameMode::SetBarrierWallsActive）。
//
// 位置朝向全靠你自己摆 —— 所以这个 actor 只干两件事：显形/隐形、开碰撞/关碰撞。
// 尺寸也没有单独的"长度""高度"参数：直接在编辑器里缩放 actor 就行（Cube 是 100×100×100，
// 缩放 5 倍就是 5 米）。
//
// ——— 为什么状态要复制，而不是服务端直接改显隐 ———
// GameMode 只存在于服务端，所以"购买阶段开始/结束"这个判断天然只在服务端发生。
// 要是服务端直接 SetVisibility 就完了，客户端永远看不到墙 —— 而且组件的 bVisible 是
// **推送式复制**（USceneComponent 的 FDoRepLifetimeParams.bIsPushBased = true），
// 想靠它传状态还得自己标脏，两边很容易不一致。
// 所以这里复制的就是**一个 bool**（bBarrierActive），两端各自跑同一段 ApplyState()。
// 同一套思路见 APhoenixFlameWall（那边复制的是一个折线数组）。
//
// ★ 唯一需要注意的摆放约束：让墙**完整盖住出生区**（左右两边顶到地形/掩体上），
//   否则人能从缝里走出去，购买阶段就没有"限制行动区域"的效果了。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BlastBarrier.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;

UCLASS()
class BLASTER_API ABlastBarrier : public AActor
{
	GENERATED_BODY()

public:
	ABlastBarrier();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器：点亮 / 收掉这面墙（GameMode 每回合调，客户端调它无效 —— 客户端等复制过来）。
	// 开了 BlueprintCallable 是为了能在关卡蓝图里手动试（比如接个触发器看开合效果）。
	UFUNCTION(BlueprintCallable, Category = "Barrier")
	void SetBarrierActive(bool bNewActive);

	UFUNCTION(BlueprintPure, Category = "Barrier")
	bool IsBarrierActive() const { return bBarrierActive; }

	// 想给某几面墙单独换材质（比如某一面标红）就在这里填，留空 = 用构造函数里的默认材质
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Barrier")
	UMaterialInterface* BarrierMaterial;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Barrier")
	TObjectPtr<UStaticMeshComponent> WallMesh;

	/*
	 * 墙的开关状态。
	 *
	 * 服务器改它，客户端由 OnRep_BarrierActive 收到 —— 两端各自把那一个 mesh 的显隐和碰撞
	 * 设成一样，而不是靠复制组件属性。初始值 false：BeginPlay 时是"关"，
	 * 等 GameMode 进购买阶段才点亮（所以热身阶段场上没有墙）。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_BarrierActive)
	bool bBarrierActive = false;

	UFUNCTION()
	void OnRep_BarrierActive();

	// 把 bBarrierActive 落到 mesh 上（显隐 + 碰撞）。两端的唯一出口。
	void ApplyState();

private:
	// 构造期加载的默认材质（M_BlastBarrier）在不在 —— 不在的话 BeginPlay 吼一声，
	// 表现是墙有形状但材质是引擎默认灰，不报错，属于"看得见但看不出问题"那种坑。
	bool bDefaultMaterialLoaded = false;
};
