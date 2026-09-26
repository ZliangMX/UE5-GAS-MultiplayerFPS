// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Blaster/Weapon/WeaponTypes.h"		// EWeaponType（WeaponType 那张 UPROPERTY 要用）
#include "Blaster/BlasterTypes/MovementDirection.h"	// EMovementDirection8（八向状态机用）
#include "Blaster/BlasterTypes/Agent.h"		// EBlasterAgent（Agent 那张 UPROPERTY 要用）
#include "BlasterCharacter.h"
#include "BlasterCharacterAnimInstance.generated.h"



/**
 * 
 */
UCLASS()
class BLASTER_API UBlasterCharacterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaTime) override;

public:
	/*
	 * ——— 换弹结束的接口（给动画蓝图 / 自定义蓝图动画通知调）———
	 *
	 * 在动画蓝图的**事件图**里直接搜 "Finish Reload" 就能找到它；
	 * 想自己做个蓝图动画通知（继承 AnimNotify、在里头调这个）也可以。
	 * 不想碰蓝图的话，直接用 C++ 那条 UAnimNotify_ReloadFinished 拖进换弹蒙太奇就行，
	 * 三者最终都汇到同一个地方（见 .cpp）。
	 *
	 * 什么时候该调：换弹动画真正播完那一刻。**别每帧调**，也别在状态机里当过渡条件接 ——
	 * 它是个"动作完成"的事件，不是查询（要查询请自己读 ECS_Reloading）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void FinishReload();

	/*
	 * ——— 掏枪结束的接口（和 FinishReload 是一对）———
	 *
	 * 在 ABP 事件图里搜 "Equip Finish" 就能找到。调它的时机是掏枪动画播完那一刻
	 * （或者自己在 ABP 里做个无类的动画通知，事件里调它）。
	 * 不碰蓝图的话，直接用 C++ 那条 UAnimNotify_EquipFinished 拖进掏枪蒙太奇末帧也一样。
	 *
	 * 做的是：把战斗状态从 ECS_Equip 放回 ECS_Unoccupied —— 也就是"从这一刻起能开火、能换弹了"。
	 * **别每帧调**，它是动作完成事件，不是查询。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void EquipFinish();

	/*
	 * ——— 空手结束的接口（FinishReload / EquipFinish 的三弟）———
	 *
	 * 给"空手那一段走 ABP 状态机"准备的：状态机里没有蒙太奇，也就挂不了动画通知，
	 * 所以收尾只能由**状态机的退出条件**（或状态里的最后一个节点）调这个函数。
	 *
	 * 做的是：把战斗状态从 ECS_EmptyHand 放回 ECS_Unoccupied，并**自动掏出最强的武器**
	 * （主武器优先）。实现在 ABlasterCharacter::EmptyHandFinish()，和 C++ 那条
	 * UAnimNotify_EmptyHandFinished 共用同一个幂等门禁，重复调不会掏两次枪。
	 *
	 * ⚠ 和换弹/掏枪那两条不同的是：空手是"不可打断"的，所以这个函数**必须被调到**，
	 *   否则人卡在空手：开不了火、换不了弹、切不了枪、放不了技能，而且什么都不报。
	 *   服务器计时器（时长 = 三条里最长那条）是保险丝，只在"技能资产上填了蒙太奇槽"时才有；
	 *   走纯状态机、一个槽都不填的话就没有保险丝 —— 那种情况下更要保证这条一定能调到，
	 *   或者干脆在 ABP 的状态退出条件里用 `Length` 加一个"兜底超时"。
	 *
	 * 查询用 bEmptyHand，别每帧调这个。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void FinishEmptyHand();

private:
	UPROPERTY(BlueprintReadOnly, Category = Character, meta = (AllowPrivateAccess="true"))
	ABlasterCharacter* BlasterCharacter1;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float Speed;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float Direction;

	/*
	 * ——— 八向移动方向（给状态机用）———
	 *
	 * 和 Direction 是同一个角度（以角色正前方为 0°、右为正），只是量化成了 8 个 45° 扇区。
	 * 八向状态机 / 方向混合空间直接读这个变量，不用在蓝图里再做一串比较。
	 * 换算规则和角度口径都写在 Blaster/BlasterTypes/MovementDirection.h。
	 *
	 * ⚠ **速度太小时保留上一帧的值**（不是归到 N）：枚举里没有 Idle，硬归零等于凭空造出一个
	 *   "正在往前走"的假方向。所以状态机切 Idle 请用 Speed / bIsAccelerating 判，
	 *   不要指望这个变量表达"没在动"。阈值见 MovementDirectionMinSpeed。
	 *
	 * 默认 N：Persona 预览里没有 Pawn，NativeUpdateAnimation 直接 return，值就停在这儿。
	 */
	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	EMovementDirection8 MovementDirection = EMovementDirection8::N;

	// 算 MovementDirection 的速度下限（cm/s）。低于它就不更新方向、沿用上一帧。
	// 5 是"基本算站着"的量级；调大 = 方向更稳更迟钝，调小 = 小碎步也会改方向。
	UPROPERTY(EditDefaultsOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float MovementDirectionMinSpeed = 5.f;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float ForwardSpeed;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float RightSpeed;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bIsInAir;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bIsAccelerating;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bWeaponEquipped;

	/*
	 * 正在"空手"那一段（ECS_EmptyHand，Jett 放完 E / Q）。
	 *
	 * 和 bWeaponEquipped 的关系：空手时枪被收到挂点上，所以 bWeaponEquipped 也是 false ——
	 * **但反过来不成立**：掏出尖刺包时 bWeaponEquipped 同样是 false，却不是在空手里。
	 * 想在状态机里单独给空手做一条分支，必须用这个变量判。
	 *
	 * 这段是**不可打断**的（开火/换弹/切枪/技能全被挡），所以它不是个提示，是个真状态。
	 * 退出：状态机自己的退出条件里调 FinishEmptyHand()（见上面那个函数）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat", meta = (AllowPrivateAccess = "true"))
	bool bEmptyHand;

	/*
	 * 当前手持武器的类型（ABP_BlasterCharacter 里读它来切"持枪姿势"那套状态机）。
	 *
	 * **空手时是 EWT_MAX 哨兵值**，不是某个具体武器 —— EWeaponType 里没有 EWT_None，
	 * EWT_MAX 就是干这个用的（第一人称的 UWushuFPAnimInstance 用的是同一套约定）。
	 * 蓝图那边判断"有没有拿枪"请用 bWeaponEquipped，别拿 WeaponType 去比具体值。
	 *
	 * 除了枪，还会被置成 **EWT_Spike**（拿着尖刺包的时候，判定条件是
	 * ABlasterCharacter::IsSpikeDrawn()）。优先级：投掷物 > 尖刺包 > 枪。
	 * 下包/拆包那两段不算（走蒙太奇，不进动画机）。
	 *
	 * 默认值故意写成步枪：Persona 里预览动画时没有 Pawn、NativeUpdateAnimation 不跑，
	 * 有个非零默认值才能看到"持枪"那套姿势，不然预览永远是空手 pose。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Weapon", meta = (AllowPrivateAccess = "true"))
	EWeaponType WeaponType = EWeaponType::EWT_AssaultRifle;

	/*
	 * ——— 手上拿着技能投掷物期间（Phoenix C 火墙 / Q 火球 / E 曲线闪光）———
	 *
	 * 和第一人称那边（UWushuFPAnimInstance）同名同义，唯一区别是**读的是哪一份状态**：
	 * 那边读本机预测的（手模只在本机播），这边读**复制**那份（GetReplicatedThrowableKind）——
	 * 第三人称身体在所有机器上都要演，包括别人的屏幕。
	 *
	 * 用法：WeaponType 在这期间变成技能对应的值（EWT_PhoenixCurveball / Fireball / Blaze），
	 * 不再是 EWT_MAX —— ABP 里按 WeaponType 切"举着哪个技能待命"那层状态即可。
	 *
	 * ★ 动作那几段（拿起 / 丢出去 / 按住 / 收起）由 C++ 播**一次性蒙太奇**（见
	 *   ABlasterCharacter::PlayThrowableSet），槽位上有蒙太奇在出力时蒙太奇说了算，演完回落到
	 *   这里由 WeaponType 定出来的待命姿势 —— 所以这边**不需要**再报"演到哪一段"
	 *（原来那个 EBlasterThrowableAnimPhase 连同这条属性一起删了）。
	 *
	 * 手上没东西时 ThrowableKind = None（WeaponType 回到老规矩）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat", meta = (AllowPrivateAccess = "true"))
	EBlasterThrowableKind ThrowableKind = EBlasterThrowableKind::None;

	/*
	 * ——— 这个角色是谁（EBlasterAgent）———
	 *
	 * 用途：**按英雄挑/混合动画** —— 最直接的就是"每个英雄一套 Face 动画，按它 blend"。
	 * 值每帧从 ABlasterCharacter::GetAgent() 抄过来（源头是 PlayerState 上那个复制的 Agent：
	 * 大厅选的英雄 / 服务器给 None 随机补的）。**没选英雄时是 None**，所以 ABP 里必须先把
	 * None 归到某个默认脸（或者干脆不 blend 那些 Face 槽），别拿它直接去索引数组/开关。
	 *
	 * 故意用 BlueprintReadWrite 而不是 BlueprintReadOnly：有 Pawn 时 C++ 每帧覆盖它（蓝图上手改的
	 * 值下一帧就没了，等于只读），但**没 Pawn 时**（Persona 预览）NativeUpdateAnimation 直接 return，
	 * 这时可以在 Persona 的 Anim Preview Editor 面板里手动切英雄、把四张脸挨个看一遍，
	 * 不用为了看 Clove 的脸去 PIE 里重开一局。
	 *
	 * 默认值给的是第一个可玩英雄（BlasterAgent::FirstPlayable = Jett）——
	 * 和 WeaponType 默认给步枪同一个道理：预览时得有个真实值，给 None 什么都看不到。
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Agent", meta = (AllowPrivateAccess = "true"))
	EBlasterAgent Agent = BlasterAgent::FirstPlayable;

	class AWeapon* EquippedWeapon;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bIsCrouched;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bAiming;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float YawOffset;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float Lean;

	FRotator CharacterRotationLastFrame;

	FRotator CharacterRotation;

	FRotator DeltaRotation;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float AO_Yaw;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float AO_Pitch;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	float RootRotationYaw;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	FTransform LeftHandTransform;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	ETurningInPlace TurningInPlace;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	FRotator RightHandRotation;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	FTransform TestTransform;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bLocallyControlled;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bElimmed;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bUseFABRIK;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bUseAimOffsets;

	UPROPERTY(BlueprintReadOnly, Category = Movement, meta = (AllowPrivateAccess = "true"))
	bool bTransformRightHand;

	// —— 握持 socket/骨骼名（ABP 里可改）——
	// 左手要握到的武器网格 socket（默认 "LeftHandSocket"）
	UPROPERTY(EditDefaultsOnly, Category = "Socket", meta = (AllowPrivateAccess = "true"))
	FName WeaponLeftHandSocket = TEXT("LeftHandSocket");

	// 武器网格上代表右手握把的 socket（默认 "hand_r"，Valorant 武器网格叫这个名字）
	UPROPERTY(EditDefaultsOnly, Category = "Socket", meta = (AllowPrivateAccess = "true"))
	FName WeaponRightHandSocket = TEXT("hand_r");

	// 角色骨骼网格上右手骨骼名（默认 "hand_r"）
	UPROPERTY(EditDefaultsOnly, Category = "Socket", meta = (AllowPrivateAccess = "true"))
	FName CharacterRightHandBone = TEXT("hand_r");	
	
	
};
