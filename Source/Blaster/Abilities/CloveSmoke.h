// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Blaster/Interfaces/VisionBlockerInterface.h"
#include "CloveSmoke.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;

/*
 * 暮蝶封烟落地后的那团烟。
 *
 * 关键设计：**它是一个纯视觉的球壳，不参与任何碰撞判定**。
 * 网格整个 NoCollision —— 子弹穿过去、射线穿过去、人也能走过去，不挡弹道也不挡移动。
 * 「外面看不见里面」纯粹靠材质不透明实现，引擎侧没有为它开任何碰撞。
 *
 * ★ 2026-09-21 起它多了一个**纯逻辑**的身份：`IVisionBlockerInterface` 实现者 ——
 *   闪光弹爆炸算盲判定时会问它"这段视线被烟挡住了吗"（按 SmokeRadius 做线段-球相交）。
 *   所以「烟能挡闪光的判定」成立，而**子弹、闪光弹本体、人物移动照样穿过去**
 *   （用户原话：挡的是判定，不是阻挡闪光弹的飞行）。这条判定完全独立于碰撞系统。
 *
 * 这带来一个必须记住的后果：**除了闪光弹那一次判定，这团烟对服务器来说还是隐形的**。
 * 敌人躲在烟里服务器不知道，因为没有任何射线会被它挡住 —— 这是刻意的
 *（用户要求「子弹可以穿过」），不是漏做。
 *
 * 为什么不放 Weapon/ 目录：那儿放的是 PhoenixCurveball / 各种 Projectile，
 * 都是「武器打出去的东西」且大多继承 AProjectile。烟既不是武器也不是投射物，
 * 放在 Abilities/ 里和它的能力配置（GA_Clove_Smoke）挨着更好找。
 */
UCLASS()
class BLASTER_API ACloveSmoke : public AActor, public IVisionBlockerInterface
{
	GENERATED_BODY()

public:
	ACloveSmoke();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// IVisionBlockerInterface：线段是否穿过这团烟（球心 = actor 位置，半径 = SmokeRadius）。
	// 用**半径**而不是网格包围盒：SmokeRadius 就是这团烟的定义（铺开动画改的是网格缩放，
	// 判定从出生到消失一直是这个半径 —— 铺开那 0.35 秒里判成满半径，可以忽略）。
	virtual bool BlocksVisionSegment(const FVector& From, const FVector& To) const override;

	// 烟球半径（厘米）。构造时按网格资产的实际半径换算成缩放，
	// 所以调这个值**不需要**去 BP 里改 Scale。
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	float SmokeRadius = 250.f;

	// 存活时长（秒）。只在服务器计时，客户端的销毁由复制跟随。
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	float SmokeDuration = 15.f;

	// 铺开时长（秒）：球从 StartScaleRatio 涨到 1，同时材质 Fade 从 0 到 1。
	// 设 0 = 直接以最终大小出现，不铺开也不淡入。
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	float GrowInTime = 0.35f;

	// 铺开动画的起始缩放比（1 = 不涨，从最终大小开始）
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	float StartScaleRatio = 0.65f;

	// 材质里控制不透明度的标量参数名。材质上没有这个参数时静默不生效（不影响功能，
	// 只是没有淡入效果）。
	//
	// ⚠️ 自带的 M_CloveSmoke 是 **Opaque**（用户要求「不透视线」），没有这个参数，
	//    所以当前只有缩放铺开、没有透明度淡入。想要淡入得把材质改成 Translucent 并加一个
	//    名为 Fade 的标量参数接到 Opacity 上 —— 但那样就能看穿烟了，和「不透视线」冲突。
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	FName FadeParamName = TEXT("Fade");

	// 球壳材质。BP_CloveSmoke 里指派 M_CloveSmoke。
	// 留空 = 用网格自带的默认材质（引擎球体那个灰的）—— 依然看得见，不会出现
	// 「封烟成功但屏幕上什么都没有」这种最难查的表现。
	UPROPERTY(EditDefaultsOnly, Category = "Smoke")
	UMaterialInterface* SmokeMaterial;

protected:
	// 烟球网格。默认是引擎自带球体，可在 BP_CloveSmoke 里换成球壳/自制模型。
	// 无论换成什么，只要模型本身以原点为中心，半径就由 SmokeRadius 统一决定。
	UPROPERTY(VisibleAnywhere, Category = "Smoke")
	UStaticMeshComponent* SmokeMesh;

private:
	// 每个烟一份独立的材质实例 —— 共用父材质的话，若干个烟的 Fade 会互相覆盖
	// （先铺好的那团会被后铺的重新拉回 0）。
	UPROPERTY()
	UMaterialInstanceDynamic* FadeMID = nullptr;

	// 铺开动画起点（BeginPlay 时刻）。服务器和客户端各自记自己的，差异只在毫秒级，看不太出来。
	float SpawnTime = 0.f;

	// SmokeRadius 换算出来的最终缩放，Tick 每帧要在它基础上乘动画系数
	FVector BaseScale = FVector::OneVector;
};
