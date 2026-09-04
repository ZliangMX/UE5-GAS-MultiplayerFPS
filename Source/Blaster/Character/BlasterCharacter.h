#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Interfaces/InteractWithCrosshairsInterface.h"
#include "Components/TimelineComponent.h"
#include "Blaster/BlasterTypes/CombatState.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"

#include "BlasterCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class UAbilitySystemComponent;
class UBlasterGameplayAbility;
class UNiagaraComponent;
class UNiagaraSystem;
class UBlasterMovementComponent;

UENUM(BlueprintType)
enum class ETurningInPlace : uint8
{
	ETIP_Left UMETA(DisplayName = "Truning Left"),
	ETIP_Right UMETA(DisplayName = "Turning Right"),
	ETIP_NotTurning UMETA(DisplayName = "Not Turning"),

	ETIP_MAX UMETA(DisplayName = "DefualtMAX")
};


UCLASS()
class BLASTER_API ABlasterCharacter : public ACharacter,public IInteractWithCrosshairsInterface
{
	GENERATED_BODY()

public:
	ABlasterCharacter(const FObjectInitializer& ObjectInitializer);
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	void DebugNetworkState();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void PostInitializeComponents() override;
	void PlayFireMontage();
	void PlayHitReactMontage();
	void PlayElimMontage();
	void PlayReloadMontage();
	void PlayThrowGrenadeMontage();
	// 后坐力：本机开火时镜头向上抬 + 轻微水平晃动，之后每帧自动回稳（只在本地玩家生效）。
	// Pitch/Yaw 每枪量、MaxPitch 本枪累积上限，全部由当前武器提供（逐武器差异化）。
	void AddRecoil(float Pitch, float Yaw, float MaxPitch);
	void Elim();
	UFUNCTION(Server,Reliable)
	void ServerElim();
	virtual void Destroyed() override;
	virtual void PossessedBy(AController* NewController) override;

	UPROPERTY(Replicated)
	bool bDisableGameplay = false;

	UPROPERTY(Replicated)
	bool bIsInvulnerable = false;

	// 测试机器人标记：无 Controller/PlayerState 的场景靶子。
	// 命中/击杀走测试规则（任意一方击杀都算成玩家的击杀，用于测试击杀标记），不参与正式计分。
	UPROPERTY()
	bool bIsTestBot = false;
	FORCEINLINE bool IsTestBot() const { return bIsTestBot; }
	void SetTestBot(bool bTest) { bIsTestBot = bTest; }

	UFUNCTION(BlueprintImplementableEvent)
	void ShowSniperScopeWidget(bool bShowScope);

	// --- Spike ---
	UFUNCTION(BlueprintImplementableEvent)
	void OnSpikePickedUp();

	UFUNCTION(BlueprintImplementableEvent)
	void OnSpikeDropped();

	UPROPERTY(Replicated)
	bool bCarryingSpike = false;

	UPROPERTY()
	class ASpike* OverlappingSpike = nullptr;

	// Server-authoritative reference to the spike this character is currently carrying
	UPROPERTY()
	class ASpike* CarriedSpike = nullptr;

	// --- GAS 技能系统 ---
	// 技能组件（冷却/充能/网络全走它）。挂角色上 → 每回合角色重建时充能自然重置（符合 Valorant）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Abilities")
	class UAbilitySystemComponent* AbilitySystemComponent;
	FORCEINLINE UAbilitySystemComponent* GetAbilitySystemComponent() const { return AbilitySystemComponent; }

	// --- 技能条"远程显示"复制状态（服务器权威，复制给所有客户端）---
	// 观战者/队友画别人的技能条时用：服务器每帧用自己的权威 ASC 重算 ReplicatedSkills，
	// 值与上一帧一致时引擎不会重发。拥有者自己存活时仍直接读本地 ASC（预测即时），不经过快照。
	UPROPERTY(Replicated)
	TArray<FBlasterReplicatedSkill> ReplicatedSkills;
	FORCEINLINE const TArray<FBlasterReplicatedSkill>& GetReplicatedSkills() const { return ReplicatedSkills; }

	// 被闪（GE_FlashBlind）复制快照：GE 只复制给受害者本人，队友/观战者读不到它 → 服务器在
	// 闪光弹炸到本角色时写入"绝对服务器结束时间 + 爆炸点"，观战白闪 widget 据此显示。
	UPROPERTY(Replicated)
	float ReplicatedBlindServerEndTime = 0.f;
	UPROPERTY(Replicated)
	FVector ReplicatedBlindOrigin = FVector::ZeroVector;
	FORCEINLINE float GetReplicatedBlindServerEndTime() const { return ReplicatedBlindServerEndTime; }
	FORCEINLINE FVector GetReplicatedBlindOrigin() const { return ReplicatedBlindOrigin; }
	// 服务器调用：本角色被闪（只保留更晚的结束时间，配套爆炸点）
	void ServerReceiveFlashBlind(const FVector& Origin, float Duration);

	// Sage 选中治疗 / Phoenix 持球 的服务器权威状态（复制给所有人 → 观战者高亮技能条对应槽位）。
	// 拥有者自己仍走本地预测的 IsSageHealSelecting()/IsCurveballHolding()（另设镜像避免覆盖本地预测值）。
	UPROPERTY(Replicated)
	bool bReplicatedSageSelecting = false;
	UPROPERTY(Replicated)
	bool bReplicatedCurveballHolding = false;
	FORCEINLINE bool GetReplicatedSageSelecting() const { return bReplicatedSageSelecting; }
	FORCEINLINE bool GetReplicatedCurveballHolding() const { return bReplicatedCurveballHolding; }

	// --- 移动组件（自定义子类：冲刺位移走 CMM 预测）---
	// 用 SetDefaultSubobjectClass 顶替默认 UCharacterMovementComponent → 冲刺 = MOVE_Custom 自定义模式，
	// 客户端本地预测 + 服务器回放 + 平滑修正，按 E 立即开冲不卡。旧默认组件被替换，无残留。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
	TObjectPtr<class UBlasterMovementComponent> BlasterMoveComp;
	FORCEINLINE class UBlasterMovementComponent* GetBlasterMoveComp() const { return BlasterMoveComp; }

	// 出生默认技能（服务器 GiveAbility，蓝图/CDO 配置，例如 GA_Jett_Dash）
	UPROPERTY(EditDefaultsOnly, Category = "Abilities")
	TArray<TSubclassOf<class UBlasterGameplayAbility>> DefaultAbilities;

	// —— 技能武装（逐风）视觉 ——
	// 武装状态复制到所有客户端 → 队友也能看到风特效。本地玩家自己通过预测也能立刻显示。
	// 二段式技能（如 Jett E）第一段武装时由 UBlasterGameplayAbility::OnArmedChanged 调用。
	void SetSkillArmed(bool bArmed);
	FORCEINLINE bool IsSkillArmed() const { return bSkillArmed; }

	// 是否有任意技能处于武装状态（HUD 视角风特效用）。
	// 首选复制的 bSkillArmed（服务器/预测客户端都会调 SetSkillArmed），兜底遍历能力真实实例。
	bool IsAnySkillArmed() const;

	// 武装时身体周围的风特效（Niagara，可换：NS_WindTunnel / NS_Wind）
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	TObjectPtr<class UNiagaraSystem> SkillWindEffectAsset;

	void SetCarriedSpike(class ASpike* Spike) { CarriedSpike = Spike; }
	FORCEINLINE class ASpike* GetCarriedSpike() const { return CarriedSpike; }
	void DropCarriedSpike();
	void SetSpikeDrawn(bool bDrawn);
	FORCEINLINE bool IsSpikeDrawn() const { return bSpikeDrawn; }

	// 是否处于下包区域内（服务器判定：查询所有 APlantZone 多边形，任意形状）
	bool IsInPlantZone() const;

	// 下包动画（服务器广播到所有客户端）
	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlantAnimation(bool bStart);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* InteractAction;
	// --- end Spike ---

	// --- Sage 治疗（选中治疗）---
	// 按 E 进入选中状态：收枪禁枪、准心指向队友显示其血条；再按 E 治疗该队友并自动掏枪。
	// 每回合只能用一次（冷却 GE 为无限时长 → 技能条变灰点；角色每回合重建自然重置）。
	FORCEINLINE bool IsSageHealSelecting() const { return bSageHealSelecting; }
	FORCEINLINE ABlasterCharacter* GetSageHealTarget() const { return SageHealTarget; }
	FORCEINLINE float GetSageHealAmount() const { return SageHealAmount; }

	// 指定角色是否可作为治疗目标（同队/测试机器人、存活、在范围内）。本地扫描与服务器校验共用。
	bool IsValidSageHealTarget(const ABlasterCharacter* Other) const;

	// 是否处于持闪光状态（Phoenix E 收枪持球，左键=左拐/右键=右拐抛掷，1/2 打断）
	FORCEINLINE bool IsCurveballHolding() const { return bCurveballHolding; }

	// 治疗目标的回血入口（服务器调用；目标客户端 HUD 由 Health 复制 + OnRep 更新）
	void HealByAbility(float Amount);

	// 服务器侧：读取并消费客户端发来的冲刺方向（GetLastMovementInputVector 对远端角色恒为 0）
	bool ConsumePendingDashDirection(FVector& OutDir);

	// 服务器侧：读取并消费客户端发来的曲线球拐弯方向（左/右）
	bool ConsumePendingCurveballSide(bool& OutCurveLeft);
	
	UFUNCTION(BlueprintCallable)
	EWeaponType GetWeaponType(){return EquippedWeapon->GetWeaponType();}

protected:
	virtual void BeginPlay() override;
	//Input

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void BlasterJump(const FInputActionValue& Value);
	void Equip(const FInputActionValue& Value);
	void Reload(const FInputActionValue& Value);
	void BlasterCrouch(const FInputActionValue& Value);
	void AimStart(const FInputActionValue& Value);
	void AimEnd(const FInputActionValue& Value);
	void FireStart(const FInputActionValue& Value);
	void FireEnd(const FInputActionValue& Value);
	void ThrowGrenade(const FInputActionValue& Value);
	void Interact(const FInputActionValue& Value);
	void InteractCancel(const FInputActionValue& Value);
	void Buy(const FInputActionValue& Value);
	void Dash(const FInputActionValue& Value);
	void Drop(const FInputActionValue& Value);
	void SelectPrimary(const FInputActionValue& Value);
	void SelectSecondary(const FInputActionValue& Value);
	void SelectMelee(const FInputActionValue& Value);
	void SpikePressed(const FInputActionValue& Value);
	void SpikeReleased(const FInputActionValue& Value);
	// F9：切换本机角色 Jett/Sage（调试，见 PC ServerToggleAgent）
	void ToggleAgentPressed();
	void AimOffset(float DeltaTime);
	virtual void Jump() override;
	UFUNCTION()
	void ReceiveDamage(AActor* DamagedActor,float Damage,const UDamageType* DamageType,class AController* InstigatedComponent,AActor* DamageCauser);
	void UpdateHUDHealth();
	void PollInit();
private:
	// 服务器每帧用权威 ASC 重算 ReplicatedSkills（见 Tick）
	void RefreshReplicatedSkills();

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class USpringArmComponent* CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class UCameraComponent* FollowCamera;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UWidgetComponent* OverheadWidget;

	UPROPERTY(Replicated,ReplicatedUsing = OnRep_OverlappingWeapon)
	class AWeapon* OverlappingWeapon;

	UFUNCTION()
	void OnRep_OverlappingWeapon(AWeapon* LastWeapon);

	UPROPERTY(VisibleAnywhere,BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UCombatComponent* Combat;

	UFUNCTION(Server,Reliable)
	void ServerEquipButtonPressed();

	UFUNCTION(Server,Reliable)
	void ServerDrop();

	UFUNCTION(Server,Reliable)
	void ServerEquipSlot(EWeaponSlot Slot);

	UFUNCTION(Server,Reliable)
	void ServerStartSpikePlant();

	UFUNCTION(Server,Reliable)
	void ServerCancelSpikePlant();

	UFUNCTION(Server,Reliable)
	void ServerStartDefuse();

	UFUNCTION(Server,Reliable)
	void ServerCancelDefuse();

	UPROPERTY(Replicated)
	bool bAiming;

	// 技能武装状态（复制到所有客户端，OnRep 显隐风特效）。本地玩家还靠预测即时显示。
	UPROPERTY(ReplicatedUsing = OnRep_SkillArmed)
	bool bSkillArmed = false;

	UFUNCTION()
	void OnRep_SkillArmed();

	// 风环绕特效组件（附加到 mesh，武装时 Activate）
	UPROPERTY(VisibleAnywhere, Category = "Abilities|Visual")
	TObjectPtr<class UNiagaraComponent> SkillWindEffect;

	void UpdateSkillArmedVisual();

	// Spike draw/hold state (drawn = in hand, holstered = on back)
	UPROPERTY(ReplicatedUsing = OnRep_SpikeDrawn)
	bool bSpikeDrawn = false;

	UFUNCTION()
	void OnRep_SpikeDrawn();

	// Long-press 4 to plant spike
	FTimerHandle SpikeHoldTimer;
	bool bSpikeHoldThresholdReached = false;
	UPROPERTY(EditDefaultsOnly, Category = "Spike")
	float SpikeHoldThreshold = 0.25f;
	void SpikeHoldTimerFinished();

	UPROPERTY(ReplicatedUsing = OnRep_EquipWeapon, VisibleAnywhere)
	AWeapon* EquippedWeapon;

	//Carried ammo for the currently-equipped weapon
	UPROPERTY(ReplicatedUsing = OnRep_CarriedAmmo)
	int32 CarriedAmmo;

	UFUNCTION()
	void OnRep_CarriedAmmo();

	UFUNCTION()
	void OnRep_EquipWeapon(AWeapon* LastWeapon);

	UFUNCTION(Server,Reliable)
	void ServerAO_Yaw(float Yaw);

	UPROPERTY(ReplicatedUsing = OnRep_ReplicatedAO_Yaw)
	float ReplicatedAO_Yaw;

	UFUNCTION()
	void OnRep_ReplicatedAO_Yaw();

	float AO_Yaw;
	FRotator InterpAO_Yaw;
	float AO_Pitch;
	FRotator AO_Rotation;
	FRotator StartingAimRotation;

	ETurningInPlace TurningInPlace;
	void TurnInPlace(float DeltaTime);

	UFUNCTION(Server,Reliable)
	void ServerUseControllerYaw(bool bUse);
	/*
	* Animation montage
	*/

	UPROPERTY(EditAnywhere,Category="Combat")
	class UAnimMontage* FireWeaponMontage;

	UPROPERTY(EditAnywhere,Category="Combat")
	UAnimMontage* HitReactMontage;

	UPROPERTY(EditAnywhere,category="Combat")
	UAnimMontage* ElimMontage;

	UPROPERTY(EditAnywhere,category="Combat")
	UAnimMontage* ReloadMontage;

	UPROPERTY(EditAnywhere,Category = "Combat")
	UAnimMontage* ThrowGrenadeMontage;

	UPROPERTY(EditAnywhere,Category = "Combat")
	UAnimMontage* PlantMontage;

	void HideCameraIfCharacterClose();

	float CameraThreshold = 200.f;

	// 后坐力相机偏移（度）：正 = 镜头向上抬。开火时累加，每帧回稳到 0。
	// 累积上限 / 回稳速度按当前手持武器取（逐武器差异化，见 AWeapon）。
	void UpdateRecoil(float DeltaTime);
	float RecoilPitchOffset = 0.f;
	float RecoilYawOffset = 0.f;

	/*
	 *Player Health
	 */

	UPROPERTY(EditAnywhere,Category="Player Stats")
	float MaxHealth = 100.f;

	UPROPERTY(ReplicatedUsing=OnRep_Health,VisibleAnywhere, Category = "Player Stats")
	float Health=100.f;

	UFUNCTION()
	void OnRep_Health();

	class ABlasterPlayerController* BlasterPlayerController;

	UPROPERTY(ReplicatedUsing = OnRep_Elim)
	bool bElimmed=false;

	UFUNCTION()
	void OnRep_Elim();

	FTimerHandle ElimTimer;

	UPROPERTY(EditDefaultsOnly)
	float ElimDelay=3.0f;

	void ElimTimerFinished();

	/*
	*Dissolve effect
	 */
	UPROPERTY(VisibleAnywhere)
	UTimelineComponent* DissolveTimeline;

	FOnTimelineFloat DissolveTrack;

	UFUNCTION()
	void UpdateDissolveMaterial(float DissolveValue);
	void StartDissolve();

	//Dynamic instance that we can change at runtime
	UPROPERTY(VisibleAnywhere,Category="Elim")
	UMaterialInstanceDynamic* DynamicDissolveMaterialInstance;

	//Material instance set on the Blueprint, used with the dynamic material instance
	UPROPERTY(EditAnywhere,Category="Elim")
	UMaterialInstance* DissolveMaterialInstance;

	UPROPERTY(EditAnywhere)
	UCurveFloat* DissolveCurve;

	UPROPERTY(EditAnywhere)
	UParticleSystem* ElimBotEffect;

	UPROPERTY(VisibleAnywhere)
	UParticleSystemComponent* ElimBotComponent;

	UPROPERTY()
	class ABlasterPlayerState* BlasterPlayerState;

	/*
	Grenade
	 */

	// 手雷挂载在角色骨骼网格上的 socket（默认 "GrenadeSocket"，换模型时改）
	UPROPERTY(EditDefaultsOnly, Category = "Combat", meta = (AllowPrivateAccess = "true"))
	FName GrenadeSocket = TEXT("GrenadeSocket");

	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* AttachedGrenade;

	UPROPERTY(ReplicatedUsing = OnRep_CombatState)
	ECombatState CombatState = ECombatState::ECS_Unoccupied;

	UFUNCTION()
	void OnRep_CombatState();

	// --- Sage 治疗（私有状态与服务器逻辑）---
	void SageHealPressed(const FInputActionValue& Value);
	void EnterSageHealSelect();
	void ExitSageHealSelectLocal();
	void PerformSageHeal(ABlasterCharacter* Target);
	bool HasSageHealAbility() const;

	// --- Phoenix 曲线球（持闪光状态，私有逻辑；交互：E 进持球收枪，左键=左拐/右键=右拐抛，1/2 打断）---
	bool HasCurveballAbility() const;
	const class UBlasterGameplayAbility* GetCurveballAbilityCDO() const;
	bool IsCurveballAvailable() const;
	void CurveballPressed();
	void EnterCurveballHold();
	void ExitCurveballHoldLocal();
	void ThrowCurveball(bool bCurveLeft);

	UFUNCTION(Server, Reliable)
	void ServerSetCurveballHolding(bool bHolding);

	// 服务器退出持球（掏回枪）
	void ServerExitCurveballHold();

	// 是否处于持球状态（本地；服务器用自身副本做校验）
	bool bCurveballHolding = false;
	// 持球时收起的武器（进入时记录，抛掷/打断后掏回）
	UPROPERTY()
	TObjectPtr<class AWeapon> CurveballHolsteredWeapon;
	bool IsSageHealAvailable() const;
	const class UBlasterGameplayAbility* GetSageHealAbilityCDO() const;
	void ApplySageHealUsed();
	void UpdateSageHealTarget();

	UFUNCTION(Server, Reliable)
	void ServerSetSageHealSelecting(bool bSelecting);

	UFUNCTION(Server, Reliable)
	void ServerSageHeal(ABlasterCharacter* Target);

	// 服务器退出选中（掏回武器）；客户端本地退出见 ExitSageHealSelectLocal
	void ServerExitSageHealSelect();

	// 是否处于选中状态（本地；服务器用自身副本做校验）
	bool bSageHealSelecting = false;
	// 选中时收起的武器（进入时记录，退出/治疗后掏回）
	UPROPERTY()
	TObjectPtr<class AWeapon> SageHolsteredWeapon;
	// 当前准心指向的队友（本地每帧扫描，HUD 画血条用）
	UPROPERTY()
	TObjectPtr<ABlasterCharacter> SageHealTarget;

	UPROPERTY(EditDefaultsOnly, Category = "Abilities|SageHeal")
	float SageHealAmount = 60.f;

	UPROPERTY(EditDefaultsOnly, Category = "Abilities|SageHeal")
	float SageHealRange = 900.f;

	// --- Jett 冲刺方向（客户端 → 服务器）---
	// 服务器读不到远端玩家的 GetLastMovementInputVector（LastControlInputVector 只在本地角色更新，
	// 服务器上远端角色恒为 0，冲刺会错误地朝面朝方向）。客户端按 E 时把冲刺方向 RPC 上来，
	// StartDash 优先消费它；回退逻辑保留给 host 本地（LastInputVector 可用）。
	UPROPERTY()
	FVector PendingDashDirection = FVector::ZeroVector;
	bool bHasPendingDashDirection = false;

	UFUNCTION(Server, Reliable)
	void ServerSetPendingDashDirection(const FVector& Dir, bool bHasDirection);

	// --- Phoenix 曲线球拐弯方向（客户端 → 服务器）---
	// 服务器读不到远端玩家输入，客户端按 E 时把「按住左/右方向键」RPC 上来决定弧线方向。
	UPROPERTY()
	bool bPendingCurveballLeft = false;
	bool bHasPendingCurveballSide = false;

	UFUNCTION(Server, Reliable)
	void ServerSetPendingCurveballSide(bool bCurveLeft);
public:
	//Input begin
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputMappingContext* DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* EquipButtonAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* CrouchAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* AimAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* FireAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* ReloadAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* ThrowGrenadeAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* DropAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* PrimarySlotAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* SecondarySlotAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* MeleeSlotAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* SpikeSlotAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* BuyAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* DashAction;


	void SetAiming(bool bIsAiming);
	void SetEquippedWeapon(AWeapon* WeaponToEquip);
	void SetOverlappingWeapon(AWeapon* Weapon);
	void SetCarriedAmmo(int32 Ammo);
	void SetCombatState(ECombatState CombatStateTemp);
	bool IsWeaponEquipped();
	bool IsAiming();
	FORCEINLINE float GetAO_Yaw()const { return AO_Yaw; }
	FORCEINLINE float GetAO_Pitch()const { return AO_Pitch; }
	AWeapon* GetEquippedWeapon();
	FORCEINLINE ETurningInPlace GetTurningInPlace()const { return TurningInPlace; }
	FORCEINLINE float GetRootRotationYaw() const { return AO_Rotation.Yaw; }
	FORCEINLINE FVector GetHitTarget() const;
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }
	FORCEINLINE bool IsElimmed() const { return bElimmed; }
	FORCEINLINE float GetHealth()const { return Health; }
	FORCEINLINE float GetMaxHealth()const { return MaxHealth; }
	void ResetHealth() { Health = MaxHealth; UpdateHUDHealth(); }
	FORCEINLINE int32 GetCarriedAmmo() const{return CarriedAmmo;}
	ECombatState GetCombatState();
	FORCEINLINE UCombatComponent* GetCombatComponent()const { return Combat; }
	FORCEINLINE bool GetDisableGameplay() const { return bDisableGameplay; }
	FORCEINLINE UAnimMontage* GetReloadMontage() const {return ReloadMontage;}
	FORCEINLINE UStaticMeshComponent* GetAttachedGrenade() const {return AttachedGrenade;}
};
