// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "Blaster/BlasterTypes/MovementDirection.h"	// EMovementDirection8（八向状态机用）
#include "Blaster/BlasterTypes/ThrowableKind.h"		// 持技能期间的 ThrowableKind
#include "WushuFPAnimInstance.generated.h"

class ABlasterCharacter;

/*
 * 第一人称手臂（FPArmsMesh）的动画实例 —— ABP_WushuFP 的父类。
 *
 * 为什么不直接在 ABP 里建蓝图变量：蓝图变量得自己在事件图里每帧填，
 * 而这些东西的源头都在角色身上（武器类型、是否在空中、速度），填一遍纯属重复劳动。
 * 写在这里，ABP 里就能当普通变量直接读，一根线都不用连。
 * （Python 也没有「新建蓝图变量」的接口 —— 5.4 的 UBlueprintEditorLibrary 里
 *   只有 ReparentBlueprint / CompileBlueprint 这类函数，没有 AddMemberVariable，
 *   所以只能建 C++ 类再让 ABP 继承。）
 *
 * 用法：Content/Blueprints/Character/Animation/ABP_WushuFP → Class Settings
 *       → Parent Class 选 WushuFPAnimInstance，然后 AnimGraph 里读下面这些变量。
 *
 * 第一人称和第三人称是两套独立的动画机（手模用的是瓦的 Wushu 骨架，不是 Epic 骨架），
 * 所以这里不复用 UBlasterAnimInstance：那个类一大半成员（FABRIK、YawOffset、Lean、
 * 转身原地转）对第一人称手臂毫无意义。但**同名变量的含义两边保持一致**
 * （bIsInAir、bWeaponEquipped、Speed），省得以后来回换算。
 */
UCLASS()
class BLASTER_API UWushuFPAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaTime) override;

	/*
	 * ——— 换弹结束的接口（给动画蓝图 / 自定义蓝图动画通知调）———
	 *
	 * 和 UBlasterCharacterAnimInstance::FinishReload() 是同一件事、同一个实现
	 * （都转调 UAnimNotify_ReloadFinished::TriggerFinishReload），
	 * 第一人称这套单独留一个只是因为这个 ABP 的父类不是那个类，蓝图里够不着。
	 *
	 * ⚠ 手模动画**只在本机播**，所以从这个入口调的话：本机（含房主自己）能收尾，
	 *   但**服务器上别人的手模不播动画** —— 那些人的换弹收尾只能靠服务器计时器兜。
	 *   想让"换弹结束"这件事在所有机器上同步、由动画说了算，通知请挂**第三人称**那条蒙太奇。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void FinishReload();

	/*
	 * 掏枪结束的接口，和 FinishReload 一对（都转调 C++ 那条通知里的同一个实现）。
	 *
	 * 第一人称这套**特别适合**挂掏枪结束：掏枪/换弹这两条手模蒙太奇只在本机播，
	 * 而"什么时候能开火"本来就只影响射手本人 —— 手模动画播完就是最准的时机。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void EquipFinish();

	/*
	 * 空手结束的接口（FinishReload / EquipFinish 的三弟），和第三人称那个同名函数
	 * 是同一件事、同一个实现（都转调 ABlasterCharacter::EmptyHandFinish）。
	 *
	 * ⚠ 和换弹/掏枪不同的是：这两个是**不可打断**状态下唯一的出口，所以必须被调到。
	 *   而且手模动画**只在本机播**（服务器上别人的手模不播动画），所以第一人称状态机这条路
	 *   只能管本机 —— 而且**它不含复制出去的状态**，只能在收尾通知之外当个补充
	 *（EmptyHandFinish 在非权威机上是空转的，真正的状态收尾只有服务器那次算）。
	 *   走"一个蒙太奇槽都不填"（bEnableEmptyHandWithoutMontage）那条路时，状态机才是唯一的
	 *   收尾依据，那时服务器靠保险丝（ABlasterCharacter::EmptyHandFallbackDuration）兜底。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void FinishEmptyHand();

private:
	// 自己持有的角色。手模是角色身上的组件，所以 TryGetPawnOwner() 拿到的就是它。
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	ABlasterCharacter* BlasterCharacter1;

	/*
	 * 当前手持武器的类型 —— 第一人称换枪/换姿势的主开关。
	 *
	 * 空手时是 **EWT_MAX**（枚举里没有 None，借 EWT_MAX 当哨兵值 ——
	 * 和 UE 自己的习惯一致，EWT_MAX 本来就是"数量/无效"那个值）。
	 * 所以 ABP 里判断"有没有枪"优先看 bWeaponEquipped，别去和 EWT_MAX 比。
	 *
	 * 除了枪，还会被置成 **EWT_Spike**（拿着尖刺包的时候，判定条件是
	 * ABlasterCharacter::IsSpikeDrawn()）。它排在投掷物判定之后、武器判定之前，
	 * 所以优先级是：投掷物 > 尖刺包 > 枪。ABP 里给 EWT_Spike 单独做一条姿势即可。
	 * 下包/拆包那两段不算（那两段走蒙太奇，不进动画机）。
	 *
	 * 默认值**故意写成步枪**而不是 EWT_MAX：运行时每帧都会被 NativeUpdateAnimation
	 * 覆盖掉，这个默认值只有 Persona 预览用得上 —— 预览里没有 Pawn
	 * （TryGetPawnOwner 返回空，NativeUpdateAnimation 直接 return），
	 * 值就停在这儿，写成步枪正好让预览显示最常用的那条分支。
	 * 也正因为如此，NativeInitializeAnimation 里**不要**再赋一次值，
	 * 否则预览会被那个赋值带跑（Persona 会初始化预览实例）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	EWeaponType WeaponType = EWeaponType::EWT_AssaultRifle;

	// 手里有枪，含义和第三人称那个同名变量完全一致：
	// 掏出尖刺包（bSpikeDrawn）时即使 EquippedWeapon 还指着旧枪，也算"空手"。
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	bool bWeaponEquipped;

	// 是否在空中（跳起/下落）。第一人称的跳落姿势用它切。
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	bool bIsInAir;

	/*
	 *是否蹲着 —— 第一人称蹲姿的主开关（ABP_WushuFP 里用它切 CrouchIn / 蹲持 / CrouchOut）。
	 *
	 *直接取 ACharacter::bIsCrouched：引擎自带、且是复制属性（replicatedUsing=OnRep_IsCrouched），
	 *所以远端玩家在这台机器上也有正确值，姿势和别人的观察一致。
	 *不用 CharacterMovement->bWantsToCrouch：那是"想不想蹲"（按下蹲键那一刻就真），
	 *而 bIsCrouched 是"胶囊真的变矮了"——和 GetFPCrouchDrop()（眼位下移）用的是同一个判定，
	 *两边不会差一帧，手模和视角不会打架。
	 *
	 *默认 false，理由同 WeaponType/Speed（Persona 预览里没有 Pawn，值就停在这儿）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	bool bIsCrouched = false;

	/*
	 * 角色当前速度（cm/s）—— 第一人称的走/跑摆动、呼吸、持枪摆动幅度都用它。
	 *
	 * 含义和第三人称那个同名变量**完全一致**：水平速度（Velocity 去掉 Z 再取长度），
	 * 所以跳起来那一瞬间的竖直速度不会把手臂姿势带跑。
	 * 参考值（CombatComponent 里那三个默认值）：站着 0、瞄准 450、开镜 350、正常移动 600。
	 *
	 * 默认 0，Persona 预览里就是"站定"—— 预览没有 Pawn，NativeUpdateAnimation 直接 return，
	 * 值停在这儿（理由同 WeaponType，别在 NativeInitializeAnimation 里再赋一次）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	float Speed = 0.f;

	/*
	 * ——— 八向移动方向（给第一人称的手模状态机用）———
	 *
	 * 和第三人称那个同名变量**完全同一套口径**（都用 MovementDirection8::FromVelocity）：
	 * 以角色正前方为 0°、右为正，量化成 8 个 45° 扇区。本工程 bUseControllerRotationYaw = true，
	 * 角色 Yaw 跟着视角，所以"相对角色"和"相对镜头"是同一个东西，两个 ABP 可以共用一套方向。
	 *
	 * ⚠ 速度太小时保留上一帧的值，不会归到 N（枚举里没有 Idle，归零等于造假方向）。
	 *   要切"没在动"的状态请用 Speed，别用这个。
	 *
	 * 默认 N：Persona 预览里没有 Pawn，NativeUpdateAnimation 直接 return，值停在这儿。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	EMovementDirection8 MovementDirection = EMovementDirection8::N;

	// 算 MovementDirection 的速度下限（cm/s）。含义和第三人称那份一致。
	UPROPERTY(EditDefaultsOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	float MovementDirectionMinSpeed = 5.f;

	/*
	 * 正在"空手"那一段（ECS_EmptyHand）—— 手模这边单独做空手姿态/状态机时用。
	 *
	 * 和 bWeaponEquipped 的区别同第三人称：空手时 bWeaponEquipped 也一定是 false，
	 * 但掏出尖刺包时它同样是 false，那两种情况要在动画上分开就只能看这个变量。
	 *
	 * 收尾调 FinishEmptyHand()。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	bool bEmptyHand = false;

	/*
	 * ——— 持技能投掷物期间（Phoenix C 火墙 / Q 火球 / E 曲线闪光）———
	 *
	 * 那三个技能**站着待命那段姿势在动画蓝图里做**，ABP 只需要知道"拿的是哪一个" ——
	 * 就是上面的 WeaponType：持技能期间枪是收着的，这里会报
	 * EWT_PhoenixCurveball / EWT_PhoenixFireball / EWT_PhoenixBlaze（不是 EWT_MAX），
	 * ABP 的状态机照 WeaponType 切即可，和 EWT_BladeStorm 一个套路。
	 *
	 * ★ 光有 WeaponType 就够：动作那几段（拿起 / 丢出去 / 按住 / 收起）是 C++ 播的**一次性蒙太奇**
	 *   （见 ABlasterCharacter::PlayThrowableSet），槽位上蒙太奇在出力时蒙太奇说了算，
	 *   演完自然回落到这里由 WeaponType 定出来的待命姿势 —— 所以动画实例**不需要**
	 *   再报"演到哪一段"（原来那个 EBlasterThrowableAnimPhase 连同这条属性一起删了）。
	 *
	 * 下面这个是给蓝图调试看的（手上拿的是哪一个，肉眼核对用）。
	 * 手上没东西时是 None。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "FP", meta = (AllowPrivateAccess = "true"))
	EBlasterThrowableKind ThrowableKind = EBlasterThrowableKind::None;
};
