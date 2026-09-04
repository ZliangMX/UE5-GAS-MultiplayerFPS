// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "Blaster/BlasterTypes/CombatState.h"
#include "CombatComponent.generated.h"


UCLASS(ClassGroup = (Custom),meta = (BlueprintSpawnableComponent))
class BLASTER_API UCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCombatComponent();
	friend class ABlasterCharacter;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void EquipWeapon(class AWeapon* WeaponToEquip);
	void Reload();
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void FinishReloading();

	void FireButtonPressed(bool bPressed);

	void ThrowGrenade();

	UFUNCTION(BlueprintCallable)
	void ShotgunShellReload();

	void ShowAttachGrenade(bool bShowGrenade);
	
	
	UFUNCTION()
	void JumpToShotGunEnd();

	UFUNCTION(BlueprintCallable)
	void ThrowGrenadeFinished();

	UFUNCTION(BlueprintCallable)
	void LaunchGrenade();
protected:
	virtual void BeginPlay() override;
	void SetAiming(bool bIsAiming);
	void Fire();

	UFUNCTION(Server,Reliable)
	void ServerSetAiming(bool bIsAiming);

	void AttachActorToRightHand(AActor* ActorToAttach);

	void AttachActorToLeftHand(AActor* ActorToAttach);

	UFUNCTION(Server,Reliable)
	void ServerFire(const FVector_NetQuantize& TraceHitTarget);

	UFUNCTION(NetMulticast,Reliable)
	void MulticastFire(const FVector_NetQuantize& TraceHitTarget);


	void TraceUnderCrosshairs(FHitResult& TraceHitResult);

	void SetHUDCrosshairs(float DeltaTime);

	UFUNCTION(Server,Reliable)
	void ServerReload();

	void HandleReload();

	int32 AmountToReload();

	UFUNCTION(Server,Reliable)
	void ServerThrowGrenade();
private:
	UPROPERTY()
	class ABlasterPlayerController* Controller;
	UPROPERTY()
	class ABlasterHUD* HUD;
	UPROPERTY(Replicated)
	class ABlasterCharacter* Character;

	UPROPERTY(EditAnywhere)
	float BaseWalkSpeed;

	UPROPERTY(EditAnywhere)
	float AimWalkSpeed;

	// 开镜（狙击）时的移动速度：比普通瞄准更慢，Valorant 式开镜减速
	UPROPERTY(EditAnywhere)
	float ScopedWalkSpeed;

	bool bFireButtonPressed;

	/*
	 *HUD and crosshair
	 */

	float CrosshairVelocityFactor;
	float CrosshairInAirFactor;
	float CrosshairAimFactor;
	float CrosshairShootFactor;
	
	FHUDPackage HUDPackage;
	
	FHitResult HitTarget;

	/*
	 *Aiming and FOV
	 */
	//Field of view when not aiming;set to the camera's base FOV in BeginPlay
	float DefaultFOV;

	UPROPERTY(EditAnywhere,Category="Combat")
	float ZoomedFOV=30.f;

	float CurrentFOV;

	UPROPERTY(EditAnywhere,Category="Combat")
	float ZoomInterpSpeed=20.f;

	void InterpFOV(float DeltaTime);

	/*
	 *Automatic fire
	 */
	FTimerHandle FireTimer;

	

	bool bCanFire=true;

	void StartFireTimer();
	void FireTimerFinished();

	bool CanFire();

	/*
	 *可学习压枪弹道（逐武器序列，见 AWeapon::RecoilPitchPattern/RecoilYawPattern）
	 */
	int32 RecoilShotCount = 0;        // 本次连发第几发（弹道序列索引）
	float LastRecoilShotTime = 0.f;   // 最近一次开火时刻
	float RecoilPatternResetDelay = 0.5f;  // 超过此间隔没开火 → 弹道重置回第 0 发
	void ResetRecoilPattern();

	TMap<EWeaponType, int32> CarriedAmmoMap;

	UPROPERTY(EditAnywhere)
	int32 StartingARAmmo = 30;

	UPROPERTY(EditAnywhere)
	int32 StartingPistolAmmo = 15;

	UPROPERTY(EditAnywhere)
	int32 StartingShotgunAmmo = 15;

	UPROPERTY(EditAnywhere)
	int32 StartingSniperAmmo = 20;
	
	void InitializeCarriedAmmo();

	void UpdateAmmoValues();
	void UpdateShotgunAmmoValues();
	
public:
	UPROPERTY(ReplicatedUsing = OnRep_PrimaryWeapon, VisibleAnywhere)
	AWeapon* PrimaryWeapon;

	UPROPERTY(ReplicatedUsing = OnRep_SecondaryWeapon, VisibleAnywhere)
	AWeapon* SecondaryWeapon;

	UFUNCTION()
	void OnRep_PrimaryWeapon(AWeapon* LastPrimaryWeapon);

	UFUNCTION()
	void OnRep_SecondaryWeapon(AWeapon* LastSecondaryWeapon);

	// Holster sockets (defined on the character's skeletal mesh)
	UPROPERTY(EditDefaultsOnly, Category = "Combat")
	FName PrimaryHolsterSocket = "PrimaryHolsterSocket";

	UPROPERTY(EditDefaultsOnly, Category = "Combat")
	FName SecondaryHolsterSocket = "SecondaryHolsterSocket";

	UPROPERTY(EditDefaultsOnly, Category = "Combat")
	FName MeleeHolsterSocket = "MeleeHolsterSocket";

	// 手持武器/掏出物品时挂到的角色骨骼网格 socket（握把/左手）
	UPROPERTY(EditDefaultsOnly, Category = "Combat")
	FName RightHandSocket = "RightHandSocket";

	UPROPERTY(EditDefaultsOnly, Category = "Combat")
	FName LeftHandSocket = "LeftHandSocket";

	void SwitchWeapon(EWeaponSlot Slot);
	void DrawSpike();
	void HolsterSpike();
	void StartSpikePlant();
	void CancelSpikePlant();
	void DropEquippedWeapon();
	void DropAllWeapons();
	void AttachActorToSocket(AActor* ActorToAttach, FName SocketName);
	FName GetHolsterSocketForWeapon(AWeapon* Weapon) const;

	void UpdateCarriedAmmo();
	void PlayEquipWeaponSound();
	void ReloadEmptyWeapon();

private:
	void EquipSlotWeapon(AWeapon* WeaponToEquip);
	void DropWeaponFromSlot(AWeapon* Weapon);
};
