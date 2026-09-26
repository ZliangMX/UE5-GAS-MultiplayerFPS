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

	/*
	 * 中途放弃换弹（**不回填弹药**，状态放回 ECS_Unoccupied）。
	 *
	 * 和 FinishReloading 是一对：那条是"换弹播完了，把子弹压进去"，这条是"不换了"。
	 * 两者都幂等（状态不是 ECS_Reloading 就什么都不做），都在服务器上跑。
	 *
	 * 谁会用：**换手上的东西**那几条路。因为 CanChangeWeapon() 明确把 ECS_Reloading
	 * 挡在外面（那条限制是对的 —— 换弹中不该能切枪），所以任何"就是要立刻把手上这把换掉"
	 * 的动作都得先自己把换弹取消掉，否则 EquipSlotWeapon 会直接 return：
	 * 状态看着像成功了、手上还是原来那把枪。大招掏飞刀就是这种动作，见
	 * ABlasterCharacter::ServerStartBladeStorm。
	 *
	 * 换弹蒙太奇不在这里停 —— 调用方接的下一件事（掏枪/掏刀蒙太奇，bStopAllMontages=true）
	 * 会把它顶掉。所以**调完要紧接着换手上这把武器**，别单独留着它当"取消"用。
	 */
	void CancelReload();

	/*
	 * ——— 掏枪结束的接口（给动画蓝图 / 自定义动画通知调）———
	 *
	 * 什么时候该调：掏枪动画真正播完那一刻（第三人称/手模那条 Equip 蒙太奇的末帧）。
	 * 和 FinishReloading 是同一个形状，也是同一个道理：
	 *   · 它做的唯一一件事就是把状态从 ECS_Equip 放回 ECS_Unoccupied（顺便补一发空仓自动换弹、
	 *     把按住的左键续上）—— 也就是说「能开火了」这个时刻由动画说了算；
	 *   · 幂等：状态已经不是 ECS_Equip（服务器计时器先收的尾）就直接走人；
	 *   · 只在权威机上改状态，客户端调它不碰权威值。
	 *
	 * 两个入口（C++ 通知 UAnimNotify_EquipFinished / 两个 AnimInstance 上的 EquipFinish 蓝图节点）
	 * 最终都汇到这里，行为不会分叉。**别每帧调**，它是个"动作完成"的事件，不是查询。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void EquipFinish();

	void FireButtonPressed(bool bPressed);

	/*
	 * 右键 = Jett 飞刀「全扔」（Valorant 同款：一次性把剩下的刀全甩出去）。
	 *
	 * 返回 true 表示"这次右键已经被飞刀吃掉了" —— 调用方（ABlasterCharacter::AimStart）
	 * 据此决定不进瞄准。返回 false 表示手上不是飞刀，右键该干嘛干嘛（开镜）。
	 *
	 * 走的是**和左键完全同一条开火链路**（CanFire → Fire → ServerFire 的射速/弹药/阵亡/
	 * 回合禁用校验），只是让武器那一次 Fire() 走"全扔"分支。绕开这条路自己生成投射物
	 * 等于给右键开一个无校验的后门。
	 */
	bool TryThrowAllKnives();

	/*
	 * 右键 = 近战重击（手上是 3 号槽的刀时）。
	 *
	 * 返回 true 表示"这次右键已经被重击吃掉了" —— 调用方（ABlasterCharacter::AimStart）
	 * 据此决定不进瞄准。返回 false 表示手上不是近战，右键该干嘛干嘛（开镜）。写法上和
	 * TryThrowAllKnives 是一对。
	 *
	 * 同样走**和左键完全同一条开火链路**（CanFire → Fire → ServerFire 的射速/阵亡/回合禁用校验），
	 * 只是让武器那一次 Fire() 走重击分支。绕开这条路自己发 RPC 等于给右键开一个无校验的后门。
	 */
	bool TryMeleeHeavyAttack();

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

	/*
	 * ——— 移动速度（公开给 ABlasterCharacter）———
	 *
	 * 修速就是往 CharacterMovement->MaxWalkSpeed 写一个数。**两端都要写**：
	 * MaxWalkSpeed 不参与复制，它是「本机模拟用的参数」—— 只有负责预测的那台机器
	 * （也就是本机）自己设对了，本地预测出来的位置才和服务器算的一致。
	 */
	// 当前手持武器决定的跑动上限（还没折算瞄准/开镜）。空手 / 未知类型 = BaseWalkSpeed。
	float GetWeaponWalkSpeed() const;

	/*
	 * 把上面那个上限按 bIsAiming 折算好，写进 CharacterMovement。
	 *
	 * bIsAiming 由调用方**传进来**，不读 Character->IsAiming()：在客户端 bAiming 是等复制
	 * 过来的，会慢一拍 —— 用它会算错这一次的移速，误差刚好是一拍（表现为松开右键后
	 * 还按瞄准速度走一小段）。
	 */
	void ApplyMaxWalkSpeed(bool bIsAiming);

	// 按 WeaponType 查兜底表（AWeapon::MaxWalkSpeed 配了正数时不查这里）
	float GetDefaultWalkSpeedForWeaponType(EWeaponType Type) const;

protected:
	virtual void BeginPlay() override;
	void SetAiming(bool bIsAiming);
	void Fire();

	UFUNCTION(Server,Reliable)
	void ServerSetAiming(bool bIsAiming);

	void AttachActorToRightHand(AActor* ActorToAttach);

	void AttachActorToLeftHand(AActor* ActorToAttach);

	// TraceHitTarget：准星射线上的一个点（客户端 TraceUnderCrosshairs 的落点）。服务器拿它定方向 ——
	//                 起点是服务器自己记的射手眼位，客户端在起点上零发言权。详见 ServerSideRewind。
	// ClientHitTime：客户端开火那一刻的服务器时间（PC->GetServerTime()），服务器用它做回溯还原
	// bThrowAll：这一次开火是不是 Jett 飞刀的"右键全扔"。**必须从客户端报上来** ——
	//              服务器生成投射物的那次 Fire() 发生在 MulticastFire 里，读的是**服务器自己那份**
	//              武器对象；客户端本地设的标志是传不过去的（设了就是"客户端看见 5 把刀飞出去、
	//              服务器只生成 1 把"）。普通武器一律 false。
	// bMeleeHeavy：这一次是不是近战的重击。同理必须报上来 —— 服务器/其他机器那一遍 Fire()
	//              读的是它们自己那份武器对象。同一发最多一个 true（近战不是飞刀，飞刀不是近战）。
	UFUNCTION(Server,Reliable)
	void ServerFire(const FVector_NetQuantize& TraceHitTarget, float ClientHitTime, bool bThrowAll, bool bMeleeHeavy);

	UFUNCTION(NetMulticast,Reliable)
	void MulticastFire(const FVector_NetQuantize& TraceHitTarget, bool bMeleeHeavy);


	void TraceUnderCrosshairs(FHitResult& TraceHitResult);

	void SetHUDCrosshairs(float DeltaTime);

	UFUNCTION(Server,Reliable)
	void ServerReload();

	void HandleReload();

	int32 AmountToReload();

	/*
	 * ——— 换弹 / 掏枪 / 扔雷的结束计时器 ——
	 *
	 * 只有服务器起这四条（客户端的状态全靠复制，不上闹钟）。为什么要有它们，
	 * 见 AWeapon::ReloadTime 的注释：结束时机以前靠蒙太奇尾部的动画通知，换个骨架就没了，
	 * 人卡在 ECS_Reloading / ECS_Equip / ECS_ThrowingGrenade 里开不了枪也切不了枪。
	 */
	void StartReloadTimer();            // ServerReload 之后调：整段换弹 / 霰弹枪压弹循环
	void ReloadTimerFinished();         // 到点 → FinishReloading
	void StartShotgunShellTimer();      // 霰弹枪：每 ShotgunShellTime 压一发
	void ShotgunShellTimerFinished();
	void ThrowGrenadeTimerFinished();   // 扔雷动作到点 → ThrowGrenadeFinished
	void StartEquipTimer();             // EquipSlotWeapon 末尾调：掏枪动画的兜底时长
	void EquipTimerFinished();          // 到点 → EquipFinish

	UFUNCTION(Server,Reliable)
	void ServerThrowGrenade();
private:
	UPROPERTY()
	class ABlasterPlayerController* Controller;
	UPROPERTY()
	class ABlasterHUD* HUD;
	UPROPERTY(Replicated)
	class ABlasterCharacter* Character;

	/*
	 * ——— 跑动速度：上限由**手持的武器**决定 ———
	 *
	 * 改之前是三个绝对值（600 / 450 / 350）写死在这里，换什么枪都一样快；现在拆成两层：
	 *
	 *   ① AWeapon::MaxWalkSpeed —— 这把武器自己的跑动上限。没填（<=0）时按下表按类型兜底。
	 *   ② 下面这几个数 —— 兜底表（每个武器类型一个）+ 瞄准/开镜的**系数**。
	 *
	 * 瞄准/开镜之所以从绝对值改成系数：一旦武器能改跑速，绝对值就会出现「一把配成 300 的枪
	 * 瞄准时反而跑到 450」这种倒挂。乘系数则任何跑速下都保证 跑 > 瞄准 > 开镜。
	 */
	// 兜底值。BaseWalkSpeed 同时兼任「突击步枪」和「空手 / 未知类型」两档 ——
	// 600 是改之前所有枪共用的那个数，所以没配过任何东西的行为和以前完全一致。
	UPROPERTY(EditAnywhere, Category = "Movement")
	float BaseWalkSpeed;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float PistolWalkSpeed;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float ShotgunWalkSpeed;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float SniperWalkSpeed;

	// 近战（3 号槽的刀）：跑得最快。Jett 大招那对飞刀也是 EWT_Melee，一起吃到。
	UPROPERTY(EditAnywhere, Category = "Movement")
	float MeleeWalkSpeed;

	// 瞄准时的速度 = 跑动上限 × 这个系数。默认 0.75 让 600 的步枪算出 450，
	// 和改成系数之前的 AimWalkSpeed 一模一样。
	UPROPERTY(EditAnywhere, Category = "Movement")
	float AimSpeedRatio;

	// 开镜（狙击）时的速度系数：比普通瞄准更慢，Valorant 式开镜减速。
	// 默认 0.5833 让 600 的步枪算出 350，和原来一样。
	UPROPERTY(EditAnywhere, Category = "Movement")
	float ScopedSpeedRatio;

	bool bFireButtonPressed;

	// 下一次 Fire() 是不是"右键全扔"。由 TryThrowAllKnives 设，Fire() 里读一次就清。
	// 必须是**一次性**的：它是"这一发"的属性，留着的话之后每一枪都会变成全扔。
	// 设它的还有 ServerFire_Implementation（服务器那次 Fire 在 MulticastFire 里，读了服务器自己那份）。
	bool bThrowAllNextShot = false;

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

	/*
	 * 换弹结束 / 霰弹枪压弹循环 / 扔雷结束 / 掏枪结束。四条都只在服务器上起。
	 * 名字对应上面 StartReloadTimer 那一组声明。
	 */
	FTimerHandle ReloadTimer;
	FTimerHandle ShotgunShellTimer;
	FTimerHandle ThrowGrenadeTimer;
	FTimerHandle EquipTimer;



	bool bCanFire=true;

	void StartFireTimer();
	void FireTimerFinished();

	bool CanFire();

	/*
	 * 「现在能不能换手上的东西」—— 切枪 / 掏尖刺包 / 技能结束掏回武器，都先问它。
	 *
	 * ECS_Unoccupied 和 **ECS_Equip 都算可以**：掏枪动画播到一半再按 2，应该把新枪换上来
	 * （瓦里就是这样），而不是按了毫无反应。
	 * 这正是 ECS_Equip 和 ECS_Reloading 最大的差别 —— 换弹中切枪是帮倒忙（枪都还没装上），
	 * 掏枪中切枪只是"换一把"，两条动画自然是后者顶掉前者。
	 *
	 * ★ 空手（ECS_EmptyHand，技能收尾那段蒙太奇）**刻意不在放行名单里**，而且和别的状态不一样：
	 *   它是设计上就"什么都不许插进来"的，不要哪天顺手把 ECS_Equip 那条理由套到它头上。
	 *   （切枪只是其中一件被挡的事，完整的清单见 ECombatState::ECS_EmptyHand 的注释。）
	 */
	bool CanChangeWeapon();

	/*
	 *服务器开火校验（射速 + 弹药 + 状态）
	 *
	 *背景：Fire() 只在开枪的本地机器跑，FireTimer/bCanFire 那套射速节流也只在本地生效；
	 *改过的客户端可以直接狂调 ServerFire → 服务器会把每一发都当成合法开火广播出去。
	 *所以服务器必须自己有一份节流和弹药判定，不信任客户端的时间线。
	 *只跑在权威机上，不参与复制。
	 */
	bool ValidateServerFire();

	// 射速令牌桶：按武器标称射速补令牌，每发消耗「一发的时长」。
	// 桶容量 = 一发 → 相邻两发因网络成簇到达时能一起放行，但平均射速不可能超过武器射速。
	float ServerFireRateCredit = 0.f;
	float ServerLastFireTime = -1000.f;
	bool bServerFireCreditInit = false;

	// 放行阈值：允许比标称射速快 (1 - Tolerance) 的瞬时抖动（定时器/帧粒度误差）
	UPROPERTY(EditDefaultsOnly, Category = "Fire Validation")
	float ServerFireRateTolerance = 0.9f;

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

	/*
	 * 近战槽（3 号槽）。
	 *
	 * 平时装的是**角色自带的那把刀**（AMeleeWeapon，每次重生由 RestorePlayerWeapons 发一把）；
	 * Jett 大招生效期间它会被 AJettKnives 顶掉，收招时再把刀放回来（见 StashedMeleeWeapon）。
	 *
	 * 为什么近战要单独占一个槽、而不是塞进副武器槽：
	 *   · "拿刀时按 1/2 能切回枪"要求刀得有个挂点收着（MeleeHolsterSocket），槽位是最自然的载体
	 *   · ACombatComponent::DropAllWeapons 只掉主副武器 → 近战槽天然不掉落，敌人捡不到对手的刀
	 *   · 死亡/大招到期时它由 DestroyMeleeWeapon 销毁（不是掉落）
	 */
	UPROPERTY(ReplicatedUsing = OnRep_MeleeWeapon, VisibleAnywhere)
	AWeapon* MeleeWeapon;

	/*
	 * 被大招飞刀顶掉的那把常驻近战武器（没有大招生效时是 nullptr）。
	 *
	 * 为什么要有这个中转：大招和常驻刀抢的是**同一个槽**（Valorant 里刃风暴本来也是"换掉你的刀"）。
	 * 原来的 EquipMeleeWeapon 是"顶掉就 Destroy()"，那是按"槽里只可能是大招的刀"写的；
	 * 常驻刀进来之后，Destroy 会把它**永久弄丢**（下一回合虽然会重发，但大招期间收招/切枪就再也拿不到了）。
	 * 所以改成收进这里、收招时放回槽位。
	 *
	 * 不复制：它是服务器自己的账本（客户端只需要看到槽位里现在是哪一把，那由 MeleeWeapon 复制过去）。
	 */
	UPROPERTY()
	AWeapon* StashedMeleeWeapon;

	UFUNCTION()
	void OnRep_PrimaryWeapon(AWeapon* LastPrimaryWeapon);

	UFUNCTION()
	void OnRep_SecondaryWeapon(AWeapon* LastSecondaryWeapon);

	UFUNCTION()
	void OnRep_MeleeWeapon(AWeapon* LastMeleeWeapon);

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

	// 把手持武器收回背/腰挂点，角色变成空手（拆包开始用；只收不发，不改槽位）
	void HolsterEquippedWeapon();

	// 掏出当前拥有的武器：主武器优先，没有主武器才掏副武器。
	// 拆包被中断时自动掏出（要求 CombatState == Unoccupied）
	void EquipBestOwnedWeapon();
	void DropEquippedWeapon();
	void DropAllWeapons();

	// 近战槽的武器（没拿刀时是 nullptr）。Jett 大招能力靠它判断"刀还在不在"。
	FORCEINLINE AWeapon* GetMeleeWeapon() const { return MeleeWeapon; }

	// 服务器：把武器塞进近战槽并掏出来。走 EquipSlotWeapon，所以收/掏/挂点/音效和切枪完全一致。
	// 槽里原有的那把（常驻的刀）会被收进 StashedMeleeWeapon，**不销毁** —— 大招收招时会放回来。
	void EquipMeleeWeapon(AWeapon* WeaponToEquip);

	/*
	 * 服务器：把武器放进近战槽但**不掏出来**（收在 MeleeHolsterSocket 上）。
	 *
	 * 给"角色天生自带"的近战武器用（ABlasterGameMode::RestorePlayerWeapons）：
	 * 出生时手上应该是枪或手枪，不是刀，所以不能走 EquipMeleeWeapon（那个会立刻掏出来，
	 * 而 RestorePlayerWeapons 里"没有主武器就保持手枪"的情况下会把刀顶到手上）。
	 * 玩家按 3（SelectMelee → SwitchWeapon(ESlot_Melee)）才切出来 —— 走的还是同一套切枪逻辑。
	 *
	 * 大招的飞刀不走这里：它要立刻掏出来，见 EquipMeleeWeapon。
	 */
	void GrantMeleeWeapon(AWeapon* WeaponToGrant);

	// 服务器：销毁近战槽的武器（大招到期 / 阵亡）。
	// **销毁而不是掉落**——近战槽本来就不在 DropAllWeapons 里，敌人不该捡到对手的刀。
	// 只清槽位和手上武器，不负责"掏回枪"（那是调用方的事，因为阵亡时不该掏）。
	//
	// bRestoreStashed：默认 true —— 把 StashedMeleeWeapon 里那把常驻的刀放回槽位（收招用）。
	// 角色被销毁（ABlasterCharacter::Destroyed）时必须传 false：那时候两把都得销毁，
	// 放回去的会变成一个挂在已销毁角色上的孤儿 actor，每回合漏一个。
	void DestroyMeleeWeapon(bool bRestoreStashed = true);
	void AttachActorToSocket(AActor* ActorToAttach, FName SocketName);
	FName GetHolsterSocketForWeapon(AWeapon* Weapon) const;

	void UpdateCarriedAmmo();
	void PlayEquipWeaponSound();
	void ReloadEmptyWeapon();

private:
	void EquipSlotWeapon(AWeapon* WeaponToEquip);
	void DropWeaponFromSlot(AWeapon* Weapon);
};
