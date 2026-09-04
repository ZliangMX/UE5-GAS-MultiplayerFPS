// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponTypes.h"
#include "Sound/SoundCue.h"
#include "Weapon.generated.h"

struct FHitResult;

UENUM(BlueprintType)
enum class EWeaponState : uint8
{
	EWS_Initial UMETA(DisplayName = "Initial State"),
	EWS_Equipped UMETA(DisplayName = "Equipped"),
	EWS_Dropped UMETA(DisplayName = "Dropped"),

	EWS_MAX UMETA(DisplayName = "DefaultMAX")
};

UCLASS()
class BLASTER_API AWeapon : public AActor
{
	GENERATED_BODY()

public:
	AWeapon();
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	void SetHUDAmmo();
	virtual void OnRep_Owner() override;
	void ShowPickupWidget(bool bShowWidget);
	virtual void Fire(const FVector& HitTarget);
	void Dropped();
	void AddAmmo(int32 AmmoToAdd);
	/**
 *Textures for the weapon crosshairs
 */
	
	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairCenter;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairRight;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairLeft;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairTop;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairBottom;

	/*
	 *Zoomed FOV while aiming
	 */
	UPROPERTY(EditAnywhere)
	float ZoomedFOV=30.f;

	UPROPERTY(EditAnywhere)
	float ZoomInterpSpeed=20.f;
	
	/*
	 *Automatic fire
	 */
	UPROPERTY(EditAnywhere,Category="Combat")
	float FireDelay=.15f;

	UPROPERTY(EditAnywhere,Category="Combat")
	bool bAutomatic=true;

	UPROPERTY(EditAnywhere,Category="Combat")
	USoundCue* EquipSound;

	// 购买价格（经济系统，BuyPhase 时用 Credits 购买）
	UPROPERTY(EditAnywhere, Category = "Economy")
	int32 WeaponCost = 0;

	/*
	 *Enable or disable custom depth
	 */
	void EnableCustomDepth(bool bEnable);
	
protected:
	virtual void BeginPlay() override;

	// 开火特效（每枪统一）：枪口火光 + 枪声。在基类 AWeapon::Fire 里播放，
	// 所以命中扫描武器和投影武器（步枪）都有开火反馈。
	UPROPERTY(EditAnywhere, Category = "Combat")
	class UParticleSystem* MuzzleFlash;

	UPROPERTY(EditAnywhere, Category = "Combat")
	USoundCue* FireSound;

	// —— Socket 名（武器骨骼网格上）——
	// 枪口火光 / 弹壳抛出口的 socket。默认 "MuzzleFlash"/"AmmoEject"，
	// 换不同模型（如 Valorant 网格）时在武器蓝图里改。
	UPROPERTY(EditAnywhere, Category = "Socket")
	FName MuzzleFlashSocket = TEXT("MuzzleFlash");

	UPROPERTY(EditAnywhere, Category = "Socket")
	FName AmmoEjectSocket = TEXT("AmmoEject");

	// —— 逐武器差异化后坐力（四把枪在 Details 面板各自调）——
	// 每枪的相机后坐力俯仰量（度）：开火时本地镜头向上抬，随后自动回稳。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilPitch = 2.f;

	// 每枪的相机水平后坐力（度）：每枪在 ±RecoilYaw 间随机，制造弹道晃动感。
	// 步枪/霰弹用，狙击设 0（纯垂直大跳）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilYaw = 0.5f;

	// 本枪后坐力累积上限（度）：连发时镜头最多抬到这个角度（步枪连射的上限）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float MaxRecoilPitch = 6.f;

	// 本枪回稳速度：越大回正越快（手枪利落、狙击慢沉）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilRecoverySpeed = 8.f;

	// —— 可学习压枪弹道（可选，主要给全自动步枪配）——
	// 非空时，本次连发的第 N 发用序列第 N 个值（超出取最后一个），
	// 序列是固定的 → 弹道可记忆、可压枪（Valorant 式）。留空则退回上面 RecoilPitch/随机 Yaw。
	UPROPERTY(EditAnywhere, Category = "Combat")
	TArray<float> RecoilPitchPattern;

	// 每发水平偏移序列（正=右、负=左）。留空则退回随机 ±RecoilYaw。
	UPROPERTY(EditAnywhere, Category = "Combat")
	TArray<float> RecoilYawPattern;

	// —— 爆头判定 ——
	// 命中骨骼名 == HeadBoneName（默认 "head"）→ 伤害 × HeadshotMultiplier。
	// 手枪/狙击/步枪配倍率；霰弹枪设 1（弹丸多，不做爆头判定）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float HeadshotMultiplier = 2.f;

	UPROPERTY(EditAnywhere, Category = "Combat")
	FName HeadBoneName = TEXT("head");

	void PlayFireEffects();

	UFUNCTION()
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult
	);

	UFUNCTION()
	void OnSphereEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex
	);

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	USkeletalMeshComponent* WeaponMesh;

	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class USphereComponent* AreaSphere;

	UPROPERTY(ReplicatedUsing = OnRep_WeaponState, VisibleAnywhere, Category = "Weapon Properties")
	EWeaponState WeaponState;

	UFUNCTION()
	void OnRep_WeaponState();

	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class UWidgetComponent* PickupWidget;

	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimationAsset* FireAnimation;

	UPROPERTY(EditAnywhere)
	TSubclassOf<class ACasing> CasingClass;

	UPROPERTY(ReplicatedUsing = OnRep_Ammo,EditAnywhere)
	int32 Ammo;

	UFUNCTION()
	void OnRep_Ammo();

	void SpendRound();

	UPROPERTY(EditAnywhere)
	int32 MagCapacity;

	UPROPERTY()
	class ABlasterCharacter* BlasterOwnerCharacter;
	UPROPERTY()
	class ABlasterPlayerController* BlasterOwnerController;

	UPROPERTY(EditAnywhere)
	EWeaponType WeaponType;
public:
	void SetWeaponState(EWeaponState State);
	FORCEINLINE USphereComponent* GetAreaSphere() const { return AreaSphere; }
	FORCEINLINE USkeletalMeshComponent* GetWeaponMesh() const { return WeaponMesh; }
	FORCEINLINE float GetZoomFOV() const { return ZoomedFOV; }
	FORCEINLINE float GetZoomInterpSpeed() const { return ZoomInterpSpeed; }
	bool IsEmpty();
	bool IsFull();
	FORCEINLINE EWeaponType GetWeaponType() const {return WeaponType;};
	// 手枪和霰弹枪都占副武器槽位（出生自带手枪，霰弹枪是可选副武器）
	FORCEINLINE bool IsSecondaryWeapon() const
	{
		return WeaponType == EWeaponType::EWT_Pistol || WeaponType == EWeaponType::EWT_Shotgun;
	}
	// 是否可开镜：目前只有狙击枪能开镜（右键出镜圈 + 强倍率）
	FORCEINLINE bool CanScope() const
	{
		return WeaponType == EWeaponType::EWT_SniperRifle;
	}
	FORCEINLINE int32 GetAmmo() const { return Ammo; }
	FORCEINLINE int32 GetMagCapacity() const { return MagCapacity; }
	FORCEINLINE int32 GetWeaponCost() const { return WeaponCost; }
	FORCEINLINE float GetRecoilPitch() const { return RecoilPitch; }
	FORCEINLINE float GetRecoilYaw() const { return RecoilYaw; }
	FORCEINLINE float GetMaxRecoilPitch() const { return MaxRecoilPitch; }
	FORCEINLINE float GetRecoilRecoverySpeed() const { return RecoilRecoverySpeed; }
	FORCEINLINE const TArray<float>& GetRecoilPitchPattern() const { return RecoilPitchPattern; }
	FORCEINLINE const TArray<float>& GetRecoilYawPattern() const { return RecoilYawPattern; }
	FORCEINLINE float GetHeadshotMultiplier() const { return HeadshotMultiplier; }

	// 命中是否为爆头（BoneName == HeadBoneName）。要拿到 BoneName，
	// 武器 trace 必须打中骨骼网格（角色 mesh 已设 Visibility Block，满足）。
	bool IsHeadshot(const FHitResult& Hit) const;
	// 按命中部位算最终伤害：爆头 × HeadshotMultiplier，否则原伤害。
	float GetDamageForHit(float BaseDamage, const FHitResult& Hit) const;

	// 跨回合继承时恢复弹匣弹药（服务器调用，复制到客户端）
	void SetAmmo(int32 NewAmmo);
};
