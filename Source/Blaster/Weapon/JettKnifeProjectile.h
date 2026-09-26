// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Weapon/ProjectileBullet.h"
#include "JettKnifeProjectile.generated.h"

class AJettKnives;

/*
 * Jett 大招「刃风暴」扔出去的一把飞刀。
 *
 * ⚠️⚠️ **这个类已经停用了**（2026-09-18）—— 用户要求"改为射线检测，不要用 projectile"，
 *     飞刀改成继承 AHitScanWeapon、判定走一条 raycast，见 AJettKnives 的头文件。
 *     这里保留文件只是为了让 BP_JettKnifeProjectile 那个资产不变成"父类丢失"的坏资产：
 *     删掉这个类的话，编辑器一开就会在那个 BP 上报错。
 *
 *     要清理干净的话按这个顺序来：
 *       ① 编辑器里删掉 /Game/Blueprints/Weapon/BP_JettKnifeProjectile 资产
 *       ② 再删这两个源文件（.h/.cpp）
 *     ⚠️ 顺序反了会让编辑器加载 BP 时报 "parent class is missing"，得手动改 BP 的父类才能修。
 *
 *     同理，BP_JettKnives 上那个 ProjectileClass 属性现在没人读了，可以留着不管。
 *
 * ——— 以下是停用前的说明，留作参考 ———
 *
 * 和子弹投影物（AProjectileBullet）的三个关键不同：
 *
 * ① 网格是**骨骼网格**。MilitaryWeapSilver 包里一把刀都没有静态网格，
 *    Knife_A 是 USkeletalMesh，而基类 AProjectile 的 ProjectileMesh 是
 *    UStaticMeshComponent —— 所以这里自带一个 USkeletalMeshComponent，
 *    并把基类那个静态网格藏掉（留着不碍事，但别两个都显示）。
 *
 * ② 伤害/爆头倍率**固化在投射物自己身上**，命中时不再去问射手手里的武器。
 *    基类默认实现读的是"命中那一刻手持的武器"，刀飞在路上时射手完全可能已经按 1/2 切回枪，
 *    那一刀就会按枪的倍率结算。所以这里覆盖掉 ComputeDamageForHit。
 *    数值由投出它的那把 AJettKnives 在生成时灌进来（见 AJettKnives::ConfigureProjectile）。
 *
 * ③ 击杀要回填刀数。命中打死人时通过 OnProjectileKill 回到 SourceKnives 身上刷新。
 *    SourceKnives 是**投出这把刀的那把武器**（生成时记下），同样不能靠"当前手持武器"。
 *
 * ④ 这把刀还兼职当"大招的计时器"：投出它的 AJettKnives 靠"手上没刀 + 天上也没刀"判断
 *    大招该收了，所以**每一把刀销毁时都必须回报一次**（Destroyed → AJettKnives::OnKnifeFinished）。
 *    回报早一次（在 OnHit 里）会漏掉"被 DestroyTime 收走的刀"，那批刀一漏，大招就永远等不到收招。
 *    所以挂在 Destroyed 上 —— 命中销毁和超时销毁两条路都会经过它。
 */
UCLASS()
class BLASTER_API AJettKnifeProjectile : public AProjectileBullet
{
	GENERATED_BODY()

public:
	AJettKnifeProjectile();

	virtual void Tick(float DeltaTime) override;

	virtual void BeginPlay() override;
	virtual void Destroyed() override;

	// 生成那一刻由 AJettKnives::ConfigureProjectile 调用：灌入伤害、倍率、归属武器。
	// 参数写成"基本类型 + Source"而不是直接传武器引用，是为了让这个类不必依赖武器的字段可见性。
	void InitFromKnives(AJettKnives* Source, float InDamage, float InHeadshotMultiplier, FName InHeadBone,
		class USkeletalMesh* InKnifeMesh);

protected:
	virtual float ComputeDamageForHit(const FHitResult& Hit, bool& bOutHeadshot) const override;
	virtual void OnProjectileKill(class ABlasterCharacter* Victim) override;

	// 刀的骨骼网格。基类那个静态网格 ProjectileMesh 在构造里被隐藏。
	UPROPERTY(VisibleAnywhere, Category = "Knife")
	class USkeletalMeshComponent* KnifeMesh;

	// 这一刀的伤害（非爆头）。由投出它的武器灌入 —— 调数值去 BP_JettKnives 上改。
	float KnifeDamage = 50.f;

	// 爆头倍率。Valorant 里刃风暴身体 50 / 头 150，所以默认 3。
	float KnifeHeadshotMultiplier = 3.f;

	// 爆头判定的骨骼名（和 AWeapon 保持同一个默认值）
	FName KnifeHeadBoneName = TEXT("head");

	// 投出这把刀的武器。击杀刷新刀数要回到它身上。
	// 用弱引用：刀在天上飞的时候武器可能已经被销毁（大招到期/角色阵亡），
	// 强引用会拖着一个已经不该存在的武器不放。
	UPROPERTY()
	TWeakObjectPtr<AJettKnives> SourceKnives;

	// 飞行时的自旋速度（度/秒）。刀不跟着速度朝向，而是在空中翻滚 —— 看着才像"扔出去"。
	// 设成 0 就是不转。
	UPROPERTY(EditDefaultsOnly, Category = "Knife")
	float SpinSpeedDegPerSec = 1080.f;

	// 自旋轴（刀的本地空间）。默认绕本地 Y 轴翻跟头。
	UPROPERTY(EditDefaultsOnly, Category = "Knife")
	FVector SpinAxisLocal = FVector(0.f, 1.f, 0.f);
};
