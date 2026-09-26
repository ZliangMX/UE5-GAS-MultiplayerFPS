#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Interfaces/InteractWithCrosshairsInterface.h"
#include "Components/TimelineComponent.h"
#include "Blaster/BlasterTypes/CombatState.h"
#include "Blaster/BlasterTypes/Agent.h"				// EBlasterAgent（GetAgent 的返回类型，动画蓝图按它挑脸）
#include "Blaster/BlasterTypes/MovementDirection.h"	// EMovementDirection8（空手蒙太奇挑分段用，要复制）
#include "Blaster/BlasterTypes/ThrowableKind.h"		// EBlasterThrowableKind（手上拿的是闪光还是火球，要复制）
#include "Blaster/Weapon/Weapon.h"
// 这里要的是 FBlasterEmptyHandMontages（技能资产上那三个空手动画槽）的**完整定义** ——
// BeginEmptyHand 要按它拷一份出来复制给远端机。反向不成立（那个头里只前向声明了 ABlasterCharacter），
// 所以不会绕回来成环。
#include "Blaster/Abilities/BlasterGameplayAbility.h"

#include "BlasterCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class AUltOrb;
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
	// 第一人称版蒙太奇：播在**手模**（FPArmsMesh）上，不是第三人称身体（GetMesh()）。
	// 开火/切枪/换弹/检视都走这一个入口，具体播哪条由武器决定
	//（AWeapon::PlayFPFireMontage 等四个包装函数）——「该用哪条动画」是武器的知识，
	//「手模在哪、该不该播」是角色的知识，所以资产从武器递进来。
	// 内部会挡掉非本地控制的机器（远端的手模不渲染，播了纯浪费）。
	// 蒙太奇要出画面，前提是 ABP_WushuFP 的 AnimGraph 里有 Slot 节点接住它。
	//
	// ⚠ 而且这条蒙太奇必须是**手模骨架**（FP_Wushu_S0_Skeleton）的，不能是别的骨架的
	//（典型踩法：把手刀那套 AB_Wushu_S0_X_* 填进来）—— 见 IsMontageCompatibleWithMesh。
	//
	// Asset 除了蒙太奇，还允许是**裸的 AnimSequence**（瓦的原始资产大多就是序列）：
	// 序列没有"槽位"概念，Montage_Play 播不了，于是这里按槽名现造一条**动态蒙太奇**再播
	//（和 PlayEmptyHandTrack 同一套做法）。包出来那条要用哪个槽由 SlotNameSource 决定 ——
	// 传一条**已经在这套网格上验证过能播**的蒙太奇（武器的 FPFireMontage），槽名从它里面取；
	// 不传就用 FallbackSlotName。
	void PlayFPArmsMontage(class UAnimSequenceBase* Asset,
	                       class UAnimSequenceBase* SlotNameSource = nullptr,
	                       FName FallbackSlotName = TEXT("DefaultSlot"),
	                       int32 DynamicMontageLoopCount = 1);

	// 把一条动画资产播在**第三人称身体**（GetMesh()）上。和上面那条的区别：
	//   · 不判本地控制 —— 身体是所有人眼里的你，每台机器都要播
	//   · 槽名默认 "UpperBody"（现役 ABP_BlasterCharacter 里那个 Slot 节点的名字）
	// 参数含义同 PlayFPArmsMontage。
	// 返回值 = 这条**真的播出去了**（资产空 / 骨架不匹配 / 拿不到动画实例都算没播）。
	// 要它的地方只有一个：上半身 + 下半身同时播时，第二条（下半身）得知道"场该不该由我来清"
	//（见 PlayThirdPersonUpperLower）。
	// bStopAllMontages 只给"同一个身体上两条不同名的槽要同时播"那一处传 false —— 默认 true
	//（引擎的 Montage_Play 里它是真·全停，理由见 PlayThrowableSet 那段）。
	bool PlayThirdPersonMontage(class UAnimSequenceBase* Asset,
	                            class UAnimSequenceBase* SlotNameSource = nullptr,
	                            FName FallbackSlotName = TEXT("UpperBody"),
	                            int32 DynamicMontageLoopCount = 1,
	                            bool bStopAllMontages = true);

	/*
	 * 第三人称身体上"上半身 + 下半身"两条一起播（两条不同名的槽，互不覆盖）。
	 *
	 * 为什么必须成对播：现役 ABP 的两个遮罩（Body = 脊柱/头颈 7 根、Arm = 手臂 53 根）里
	 * **没有腿和 Root**，腿永远来自 LowerBody 槽 —— 只喂 UpperBody 的话下包/拆包时腿一直是站姿。
	 * 全部理由见 .cpp 里的实现注释。
	 *
	 * UpperAsset 留空 → 只播下半身；LowerAsset 留空 → 只播上半身，两者都空就什么都不做。
	 * 返回值 = 上半身那条有没有播出去。
	 */
	bool PlayThirdPersonUpperLower(class UAnimSequenceBase* UpperAsset, class UAnimSequenceBase* LowerAsset,
	                               int32 UpperLoopCount = 1, int32 LowerLoopCount = 1);

	/*
	 * 把"正在播这条资产"的蒙太奇从某个网格上停掉（混合时间 0 = 立刻断）。
	 *
	 * 为什么不能直接 Montage_Stop(资产)：填的若是**裸 AnimSequence**，实际在播的是播放那一刻
	 * 现造出来的动态蒙太奇（另一个对象），拿序列本身去 Montage_Stop 匹配不上 ——
	 * 表现是"该停的没停，动画顺着往下播"。两种都认才停得干净（实现见 .cpp 里的
	 * StopMontageInstancesUsingAsset）。网格传 nullptr / 资产留空都安全（什么都不做）。
	 */
	void StopAnimAssetOnMesh(class USkeletalMeshComponent* MeshComp, const class UAnimSequenceBase* Asset,
	                         float BlendOutTime = 0.f);

	// 蒙太奇和网格是不是同一套骨架。**不一致就绝不能播**：
	// 引擎按**骨名**绑轨道，两套骨架里同名的骨会被照样改写。手模和刀骨架恰好都叫
	// Skeleton / Root，而两者根部的朝向差了 120°（见 .cpp 里的实现注释），
	// 一播手模的整条根链就被转掉 —— 而第一人称相机就挂在这条链末端的 Camera 骨上，
	// 表现是「整个视角被拧过去」，但没有任何报错。
	//
	// 两边任意一边取不到骨架时返回 true（放行）：这个函数只用来拦"明显绑错了骨架"
	// 这种配置事故，不该变成正常播放路径上的新失败点（动态蒙太奇等场景骨架可能为空）。
	static bool IsMontageCompatibleWithMesh(const class UAnimSequenceBase* Asset,
	                                        const class USkeletalMeshComponent* MeshComp);

	// 从 SlotNameSource 里取槽位名（取第一条 Slot 轨）；它不是蒙太奇 / 没有槽轨 / 槽名是 None
	// 时返回 Fallback。**只给"裸 AnimSequence 要被包成动态蒙太奇"那条路用** ——
	// 序列自己不带槽名，而槽名必须和动画图里那个 Slot 节点一致才出画，配错了引擎一声不吭。
	static FName ResolveAnimSlotName(const class UAnimSequenceBase* SlotNameSource, FName Fallback);

	/*
	 * 这个网格上，这条动画**还剩多久播完**（秒）。没在播 / 查不到 → 0。
	 *
	 * 用途是"别把正在播的攻击动画用下一条蒙太奇顶掉"：换枪时那些 equip 蒙太奇播在同一批槽位上，
	 * 后一条一上来前一条就没了（见 AJettKnives::GetThrowPresentationRemainingTime 的用法：
	 * 飞刀扔空后收招掏枪要等最后那一刀的表演播完 —— 用户 2026-09-18："这里要播完"）。
	 *
	 * ⚠ 量的是**本机这个实例的播放进度**，不是资产长度：单扔那条是蒙太奇跳段后往下播的，
	 *   资产全长（5 段）和实际剩余（最后一段）能差好几秒。
	 */
	float GetAnimRemainingTime(const class USkeletalMeshComponent* MeshComp,
	                           const class UAnimSequenceBase* Asset) const;

	/*
	 * 把一条动画资产播到某个动画实例上。上面两个 PlayXxxMontage 的公共下半段。
	 * （原话是"刀骨骼那条 AJettCharacter::PlayKnifeThrowAllMontage 也复用它" ——
	 *  那个函数 2026-09-18 已按用户要求删除，但入口本身还给别处用。）
	 *
	 *   · UAnimMontage  → Montage_Play（走哪条槽由蒙太奇自己带）
	 *   · 裸 AnimSequence → 按 DynamicMontageSlot 现造一条动态蒙太奇再播（详见 .cpp）
	 *
	 * **不做骨架检查** —— 每个网格填错骨架的后果和排查提示都不一样（手模被拧视角、
	 * 刀骨骼被拧根链），得由调用方自己判并打指名道姓的日志。
	 *
	 * DynamicMontageLoopCount 只在"裸序列 → 现造动态蒙太奇"那条路上生效（那一步本来就带这个参数），
	 * 默认 1 = 播一遍；要循环的动作（拆包的待命循环）传个大数。蒙太奇的话它**不起作用** ——
	 * 循环要写在蒙太奇自己里面（段自己指向自己）。
	 *
	 * bStopAllMontages 默认 true（每条新动作顶掉上一条）。传 false 只有一个用途：
	 * 同一条身体上两条**不同名的槽**要同时播（下包/拆包的上半身 + 下半身）——
	 * 那种情况下第二条必须留住第一条，见 PlayThirdPersonUpperLower。
	 *
	 * 返回值 = 真播出去了。
	 */
	static bool PlayAnimAssetOnInstance(class UAnimSequenceBase* Asset, class UAnimInstance* AnimInstance,
	                                    FName DynamicMontageSlot, int32 DynamicMontageLoopCount = 1,
	                                    bool bStopAllMontages = true);

	// 让第一人称那份枪械副本（FPWeaponMesh）跟着真枪播同一条动画。武器那边
	//（AWeapon::PlayGunAnimation）调的 —— 所以是 public。
	// 远端机器上副本根本不渲染，这里先挡掉；不是本地玩家就什么都不做。
	void PlayFPWeaponAnimation(class UAnimationAsset* Anim);

	// 本机第一人称**正在显示**的那把枪的副本网格；没在显示（不是本地玩家 / 这把武器没开副本）
	// 返回 nullptr。武器的枪口火光/抛壳要挂它身上（GetViewMesh 用的就是它）。
	// ⚠ 只给**纯表现**用：射线起点是眼位（GetFPEyeWorldLocation），和枪挂在哪个网格上无关。
	class USkeletalMeshComponent* GetVisibleFPWeaponMesh() const;
	void PlayHitReactMontage();
	void PlayElimMontage();
	void PlayReloadMontage();
	// 第三人称身体的掏枪蒙太奇。资产由当前武器给（AWeapon::ThirdPersonEquipMontage）。
	// 武器没配就什么都不做 —— 切枪本来也只有枪自己的 EquipAnimation + 手模那条，
	// 所以"没配"= 跟改动前完全一样。
	void PlayEquipMontage();
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

	/*
	 * 开镜覆盖层（狙击镜框 + 开关镜音效）。
	 *
	 * 原来整段在 BP_BlasterCharacter 的**事件图**里 —— 只有父类是那个 BP 的子蓝图才继承得到。
	 * 英雄 BP 的父类换成各自的英雄 C++ 类之后继承不到了，所以实现搬进 C++（这里）。
	 *
	 * 用 BlueprintNativeEvent 而不是普通函数，是为了两件事都不动：
	 *   · BP_BlasterCharacter 里那份老实现（测试机器人、没选英雄的兜底角色还在用它）继续覆盖 C++；
	 *   · 四个英雄 BP 没有实现 → 自动走下面这份 _Implementation。
	 * 等哪天把 BP_BlasterCharacter 那张图删掉，C++ 这份对它就自动生效了，不用再改代码。
	 */
	UFUNCTION(BlueprintNativeEvent)
	void ShowSniperScopeWidget(bool bShowScope);
	void ShowSniperScopeWidget_Implementation(bool bShowScope);

	// 覆盖层用到的资源 —— 照旧配在蓝图里，C++ 不带资源路径。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SniperScope")
	TSubclassOf<class UUserWidget> SniperScopeWidgetClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SniperScope")
	class USoundBase* SniperScopeZoomInSound = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SniperScope")
	class USoundBase* SniperScopeZoomOutSound = nullptr;

	// 控件里那条开关镜动画的名字（WBP_SniperScope 里叫 ScopeZoomIn）。
	// 原 BP 是拿这个动画**正放 = 开镜、倒放 = 关镜**，控件本身建一次就一直留在屏幕上。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SniperScope")
	FName SniperScopeAnimationName = TEXT("ScopeZoomIn");

	// 建过一次就一直留着（跟原 BP 一致：只创建一次，之后靠动画正放/倒放切开关）。
	// ⚠ 名字刻意**不叫** SniperScopeWidget —— BP_BlasterCharacter 里那个同名蓝图变量还在，
	//    C++ 这边再叫同名会把那个 BP 的编译撞挂。
	UPROPERTY(Transient)
	class UUserWidget* SniperScopeWidgetInstance = nullptr;

	// 取控件里那条开镜动画（UMG 动画是控件类上的对象，按名字找）
	class UWidgetAnimation* FindScopeAnimation() const;

	// --- Spike ---
	UFUNCTION(BlueprintImplementableEvent)
	void OnSpikePickedUp();

	UFUNCTION(BlueprintImplementableEvent)
	void OnSpikeDropped();

	UPROPERTY(Replicated)
	bool bCarryingSpike = false;

	UPROPERTY()
	class ASpike* OverlappingSpike = nullptr;

	/*
	 * Server-authoritative reference to the spike this character is currently carrying.
	 *
	 * 2026-09-22 起**也复制给拥有者**：本人第一人称手上那个包的副本（FPSpikeMesh）要拿它的
	 * 网格资产（ASpike::SpikeMesh 上的那套）和它挂了什么挂点。不给复制的话客户端这个指针永远是
	 * 空的，第一人称就只能再在角色上配一份网格 —— 换包模型要改两个地方，迟早对不上。
	 * 只有掏包的人自己需要它，所以用 COND_OwnerOnly。
	 */
	UPROPERTY(Replicated)
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

	// Sage 选中治疗 / 持技能投掷物 的服务器权威状态（复制给所有人 → 观战者高亮技能条对应槽位）。
	// 拥有者自己仍走本地预测的 IsSageHealSelecting()/IsThrowableHolding()（另设镜像避免覆盖本地预测值）。
	UPROPERTY(Replicated)
	bool bReplicatedSageSelecting = false;
	// 手上拿的是哪一种投掷物（None = 没拿）。
	// 为什么不是一个 bool：技能条要知道该高亮 E（闪光）还是 Q（火球），
	// 光知道"在持投掷物"不够 —— 见 EBlasterThrowableKind。
	// RepNotify：远端机（包括拥有者那台）靠它把**第三人称身体**那条动画轨道推到对应阶段
	//（手模那条不走复制 —— 别人的角色根本没有手模，见 PlayThrowableSet）。
	UPROPERTY(ReplicatedUsing = OnRep_ReplicatedThrowableKind)
	EBlasterThrowableKind ReplicatedThrowableKind = EBlasterThrowableKind::None;
	FORCEINLINE bool GetReplicatedSageSelecting() const { return bReplicatedSageSelecting; }
	FORCEINLINE EBlasterThrowableKind GetReplicatedThrowableKind() const { return ReplicatedThrowableKind; }

	// 复制到达 → 身体那条轨道跟着走。权威机**不会**响（RepNotify 只在客户端），
	// 权威那边由 ServerSetThrowableHolding 自己叫 —— 两处互斥，所以不会重播。
	UFUNCTION()
	void OnRep_ReplicatedThrowableKind();

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
	void SetSpikeDrawn(bool bDrawn);
	FORCEINLINE bool IsSpikeDrawn() const { return bSpikeDrawn; }

	// 是否处于下包区域内（服务器判定：查询所有 APlantZone 多边形，任意形状）
	bool IsInPlantZone() const;

	// 下包动画（服务器广播到所有客户端）
	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlantAnimation(bool bStart);

	/*
	 * 拆包那串蒙太奇（服务器广播到所有客户端）。
	 *
	 * bStart=true  = 掏出 defuse → 播完自动接 idle（循环）；
	 * bStart=false = 收起 defuse（打断和拆完都走这条），**不是**"回到空手"。
	 *
	 * 为什么和"进度"分开：进度从**掏出 defuse 那一刻**就开始走（用户要求），
	 * 和动画播到哪一段无关；这里只管画面上那串动作。
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDefuseAnimation(bool bStart);

	/*
	 * 安包/拆包时自动蹲下（服务器广播到所有客户端）。
	 *
	 * 为什么要多播到**本人那台**，不能只在服务器上调 Crouch()：
	 * 蹲下是 CMC 的**客户端预测**状态。服务器把 bWantsToCrouch 改了，但它是
	 * COND_SkipOwner 复制的 —— 本人那台收不到，下一次移动上报就会把蹲下顶回去，
	 * 结果就是"服务器蹲了、自己屏幕上没蹲，然后服务器又被掰回来"。
	 * 两边一起调才和玩家自己按 Ctrl 那条路完全一致。
	 *
	 * 实现里只认权威机 + 本人那台（其余机器调 Crouch 只会让模拟代理的 CMC 打架）。
	 * 玩家本来自己就蹲着的话不会被记成"自动蹲"，收尾也不会替他站起来。
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastSpikeAutoCrouch(bool bEnable);

	// 自动蹲下/起身（收到的多播调它）。见 MulticastSpikeAutoCrouch 的注释。
	void SetAutoCrouch(bool bEnable);

	// 这次蹲下是"安/拆包替玩家按的"（不是他自己按的）→ 收尾时该替他站起来。
	// 玩家本来蹲着进来的话这个标志保持 false，收尾不动他。
	bool bAutoCrouched = false;

	/*
	 * 第一人称手上那个包的副本（只本人可见）。
	 *
	 * 真包（ASpike）挂在第三人称身体的挂点上，而本人看自己的身体是关掉的
	 *（GetMesh()->SetOwnerNoSee），所以本人手里那个必须另开一份副本 ——
	 * 和 FPWeaponMesh（枪）/ HeldFireballFP（火球）完全同一个套路。
	 *
	 * 网格资产**从真包上取**（ASpike::SpikeMesh 当前那套），不在角色上另配一份：
	 * 换包模型只用改 BP_Spike。挂点在下面 FPSpikeSocket。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spike|FP Visual")
	TObjectPtr<class USkeletalMeshComponent> FPSpikeMesh;

	// 第一人称那份包挂在手模的哪个挂点上（socket 名或骨骼名，引擎两样都认）。
	// 默认 R_WeaponMaster —— 和手上那个火球同一个挂点（手模上 R_WeaponMaster / R_WeaponPoint /
	// MasterWeapon / WeaponADS 都是骨骼，名字里没 "Socket" 也照样能用）。
	// ⚠ 在 BP 里改这个值会在下次掏包时生效（运行时重挂），不用重启编辑器。
	UPROPERTY(EditDefaultsOnly, Category = "Spike|FP Visual")
	FName FPSpikeSocket = FName("R_WeaponMaster");

	/*
	 * 挂在挂点上之后的微调（**只用其中的位移和旋转**，缩放由代码按世界缩放归一）。
	 *
	 * 默认零 —— TRS 上包的网格原点正好落在挂点上（真包那边也是这个约定：ApplyMeshPivotCompensation
	 * 把 actor 反向挪了 MeshRelativeOffset，网格最终就贴在 socket 上）。
	 * 这个纯粹是给你在不改骨架的前提下把包在手里挪一下用的。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Spike|FP Visual")
	FTransform FPSpikeMeshOffset = FTransform::Identity;

	// 每帧刷第一人称那个副本的显隐/网格/挂点（Tick 里调，内部自己判是不是本人）。
	void UpdateFPSpikeVisual();

	bool bFPSpikeAttached = false;
	bool bFPSpikeVisible = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* InteractAction;
	// --- end Spike ---

	// --- Sage 治疗（选中治疗）---
	// 按 E 进入选中状态：收枪禁枪、准心指向队友显示其血条；再按 E 治疗该队友并自动掏枪。
	// 冷却 45s（GA_Sage_Heal 的冷却 GE = GE_Sage_HealCooldown，HasDuration 45 →
	// 用掉后走通用的"到点回充"路径，技能条上会画倒计时；用户 2026-09-19 从"每回合一次"改过来）。
	FORCEINLINE bool IsSageHealSelecting() const { return bSageHealSelecting; }
	FORCEINLINE ABlasterCharacter* GetSageHealTarget() const { return SageHealTarget; }
	FORCEINLINE float GetSageHealAmount() const { return SageHealAmount; }

	// 指定角色是否可作为治疗目标（同队/测试机器人、存活、在范围内）。本地扫描与服务器校验共用。
	bool IsValidSageHealTarget(const ABlasterCharacter* Other) const;

	// 是否处于"手上拿着技能投掷物"的状态（收枪收起、1/2/3 打断、换弹/捡枪/开镜全让路）。
	// 闪光（Phoenix E）和火球（Phoenix Q）共用这一套骨架，靠 GetHeldThrowableKind() 区分。
	FORCEINLINE bool IsThrowableHolding() const { return bThrowableHolding; }

	// 手上拿的是哪一种（None / Flash / Fireball）。只在 IsThrowableHolding() 为真时有意义。
	FORCEINLINE EBlasterThrowableKind GetHeldThrowableKind() const { return HeldThrowableKind; }

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
	/*
	 * F 键（绑在 EquipButtonAction 上，键位在 IMC 里从 E 挪到 F）：
	 *   · 站在大招球里 → 开始蓄力吃球（按住 3 秒，松开/移动/掏枪打断）
	 *   · 否则地面上有可捡武器 → 捡枪
	 * 两部分都走同一个键，因为都是"对着地上的东西按 F"这一类动作。
	 *
	 * ⚠️ 和 Spike 的安包/拆包**无关** —— 那两个仍然在 IA_Interact / IA_SelectSpike（4 键）上，
	 * 这里一根手指都不碰。别把这三件事混到一个 handler 里。
	 */
	void Pickup(const FInputActionValue& Value);

	// F 松开：如果是吃球就打断（一段式，进度清零）；不是吃球则什么都不做。
	void PickupCancel(const FInputActionValue& Value);

	// 打断吃球。1/2 掏枪时也会调（bRestoreWeapon=false，因为调用方紧接着自己会切武器）。
	// 权威机上直接调球，客户端走 ServerCancelOrbChannel RPC。
	void CancelOrbChannel(bool bRestoreWeapon = true);

	// 「脚下有没有能捡的枪」的统一门槛。捡枪键从 E 挪到 F 之后，E 已经不再和捡枪冲突，
	// 但这里剩下的 bDisableGameplay / 治疗选中 / 持闪光 三个门槛仍然要留着 ——
	// Dash 等技能处理器也调它来判断"该放技能还是该捡枪"，别因为换键就把这个判断删掉。
	// （函数名里的 "EKey" 是历史遗留，没改名以免影响读代码时的 gdb/搜索习惯，看注释即可）
	bool CanEKeyPickupWeapon() const;
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

	/*
	 * X 键 = 大招（Valorant 里大招就在 X 上）。和 E 键那套小技能是**两条独立的路**：
	 * 不走 CanEKeyPickupWeapon 那个"脚下有枪就优先捡枪"的分支 —— 站在枪旁边也该能放大招。
	 * 具体放哪个技能由 GetUltimateAbilityCDO() 找（DefaultAbilities 里 bIsUltimate 的那个）。
	 */
	void UltimatePressed(const FInputActionValue& Value);

	/*
	 * Q / C 两个独立技能键（Valorant 技能栏 C-Q-E-X 里的前两位，Jett 是 C=逐风云 / Q=腾空）。
	 *
	 * 和 E 键（Dash）是**两条不同的路**，别混：
	 *   · E 是"通用技能键"—— 一个角色只从它上面放**一个**技能，按 SkillType 优先级挑
	 *     （Sage 治疗 > Phoenix 曲线球 > Jett 冲刺），所以四个英雄共用一个 DashAction 就够。
	 *   · C/Q 是**各自独立**的技能格，三个技能并存、独立充能，必须一个键一个 Action。
	 *
	 * 和 X 大招一样不走 CanEKeyPickupWeapon 那条"脚下有枪优先捡枪"的分支 ——
	 * 站在枪旁边也该能放技能。
	 *
	 * 具体放哪个技能：扫 DefaultAbilities 找 SkillType == Updraft（Q）/ Cloudburst（C，Jett）
	 * / Wall（C，Phoenix）的那个 CDO。
	 * 找不到 = 这个角色没有这个技能，静默无反应（Sage 按 Q 就该什么都不发生）。
	 *
	 * ★ C 键现在有**两个**候选（逐风云 / 火墙），一个角色只会配其中一个，见 SkillCPressed。
	 */
	void SkillQPressed(const FInputActionValue& Value);
	void SkillCPressed(const FInputActionValue& Value);

	/*
	 * 松开 C。逐风云和火墙两个技能都用得上它 —— 其余技能（Q / E / X）是"按一下就完事"，
	 * 松手没有语义，所以只有 C 多绑了一条 ETriggerEvent::Completed（见
	 * SetupPlayerInputComponent 里那段注释）。
	 *
	 * 两边都进了持投掷物态，但"松开"的语义不一样：
	 *   · 逐风云：按住期间**云**才跟准心，松手 → 播 Outro 掏枪（见 EndCloudburstHold）
	 *   · 火墙：按住的是**左键**（发射那一下），松开 C 只是"收起还没发射的墙"
	 *     —— 已经在飞了就不理会（见 BlazePressed）
	 */
	void SkillCReleased(const FInputActionValue& Value);

	// ↑ 这两个处理函数在 protected 区，但下面的查询是**公开**的：
public:
	// --- 技能配置查询（HUD / 技能条 Widget 要读它们的 SkillIcon 贴图）---
	// 从 DefaultAbilities 里找 bIsUltimate 的那个（一个角色只配一个）。
	// 和 GetCurveballAbilityCDO 同一套路：拿 CDO 读配置，不需要能力实例存在。
	// 公开的理由和大招点一样：这是**角色类的配置**，观战看队友时照样读得到，
	// 不像 ASC 里的能力实例那样只有本人有。
	const class UBlasterGameplayAbility* GetUltimateAbilityCDO() const;

	// 按技能类型从 DefaultAbilities 里找能力 CDO —— 通用版本。
	// GetSageHealAbilityCDO / GetCurveballAbilityCDO / GetSmokeAbilityCDO 那几个英雄专用的
	// 都等价于调它（各自保留是为了不动已经在跑的代码，不是各有各的逻辑）。
	// 返回 nullptr = 这个角色没有这个技能 —— 调用方据此静默跳过（Sage 按 Q 就该没反应）。
	const class UBlasterGameplayAbility* GetAbilityCDOBySkillType(EBlasterSkillType SkillType) const;

	/*
	 * ——— 火墙（Phoenix C）的对外接口 ———
	 *
	 * 这三个是**公开**的，因为飞出去的那颗球（APhoenixBlazeBall）要读它们：
	 *   · IsBlazeFiring —— 球每帧问"射手还按着左键吗"（松手就不跟准心了，自己直飞）
	 *   · StopBlazeFire —— 球飞完那一刻主动收尾（不用玩家记着松手）
	 * 其余（按下/发射/收枪掏枪）全在下面的私有区，外面只认这两个入口。
	 */
	bool IsBlazeFiring() const { return bBlazeFiring; }

	// 松手 / 球飞完 / 保险丝 —— 三条路都走它，自带幂等门禁。
	void StopBlazeFire();

	/*
	 * ——— 空手蒙太奇（Jett 放完 E / Q 那一段）———
	 *
	 * 入口 BeginEmptyHand 由技能在**动作做完那一刻**调（见 UBlasterGameplayAbility::
	 * StartEmptyHandIfConfigured），传进来的就是技能资产上填的那三个槽。
	 *
	 * 出口只有一个 EmptyHandFinish()（自带幂等门禁，重复调是空转），收尾的主路是**动画**：
	 *   · 身体那条（第三人称）蒙太奇上的收尾通知 UAnimNotify_EmptyHandFinished —— 所有机器
	 *     都播那条，所以"演完了"在每台机器上都是同一个时刻。手模（第一人称）那条通知
	 *     **不作数**，C++ 里直接滤掉（它只在射手本机播）。
	 *   · 保险丝（服务器上的 EmptyHandTimer）—— 备胎，恒起但正常不会响：通知一到就把它
	 *     清掉了。只在通知没响（资产上没挂 / 换骨架换 ABP 弄丢了 / 身体那条在服务器上
	 *     不 tick）时才到点收尾，把最坏情况从"卡在不可打断的空手里"降成"多站零点几秒"。
	 *     身体那条挂着通知时它的时长还会往后推 EmptyHandFuseExtraDelay，免得和通知抢跑。
	 *     见 StartEmptyHandTimer。
	 *   ⚠ 这两条**都不是"进空手"的入口**：进空手只有 BeginEmptyHand / RestartEmptyHand
	 *     那一拍。之前"冲刺结束再兜底进一次空手"那版是多余的第二次进 —— 冲刺动画从头
	 *     再播一遍就是这么来的。
	 *
	 * 为什么要走角色而不是留在能力里：状态（CombatState）和三条蒙太奇指针都要**复制**给
	 * 远端机（他们的机器上能力实例不跑表现逻辑），保险丝也必须跟着角色生死走。
	 *
	 * @param InWorldDirection 本次动作的**世界空间**方向提示（可以不归一化，函数里会自己规整）。
	 *        冲刺类能力把 DashDirection 传进来最准；拿不准就传零向量 ——
	 *        那时按"角色的速度方向"推，速度也太小（原地站着放技能）就退到 N（面朝方向）。
	 *        方向只影响挑哪一段，不影响"播不播"。
	 */
	void BeginEmptyHand(const FBlasterEmptyHandMontages& InMontages, const FVector& InWorldDirection = FVector::ZeroVector);

	/*
	 * 手模（第一人称手臂）。专用服务器上是 nullptr 之外的情形不成立 —— 组件一直在，
	 * 只是不渲染、不 tick；所以调用方要判的是"这条通知是不是挂在它上面"，不是"有没有"。
	 * 空手收尾通知用它区分"身体那条"和"手模那条"，见 UAnimNotify_EmptyHandFinished。
	 */
	class USkeletalMeshComponent* GetFPArmsMesh() const { return FPArmsMesh; }

	/*
	 * 空手中**再放一个技能**（E 逐风二段在 Q 腾空那段里冲出去）—— 用新技能的蒙太奇
	 * 把正在播的那条顶掉，收尾也随之改由新那条的通知负责（旧那条的通知一起作废）。
	 *
	 * 和 BeginEmptyHand 的区别只有一个：**已经在 ECS_EmptyHand 里时不早退**，
	 * 而是"停旧的 → 播新的 → 重新算收尾"。状态不动（还是 ECS_EmptyHand）、手上不动
	 * （枪早就收回挂点了，不用再走一遍 HolsterEquippedWeapon）—— 变的只有动画和收尾时机。
	 *
	 * 为什么要顶掉而不是"接着播 Q 的动画、人照冲"：
	 *   · 动画对不上 —— 人横着飞出去，屏幕上还在放 Q 那套抬手动作
	 *   · 时长对不上 —— 保险丝还按 Q 那段算，会在冲刺中途到点 → 冲刺还没走完枪已经掏出来了，
	 *     正好是 ECombatState::ECS_EmptyHand 那段注释里说的"枪和手对不上"
	 * 旧蒙太奇被停掉 = 它挂在尾帧的收尾通知也不会再响，不会和新的那条抢收尾权。
	 *
	 * 远端机不跑这个函数（HasAuthority 门禁），它们靠 EmptyHandRestartCount 那个世代号重播。
	 *
	 * @param InWorldDirection 同 BeginEmptyHand。
	 */
	void RestartEmptyHand(const FBlasterEmptyHandMontages& InMontages, const FVector& InWorldDirection = FVector::ZeroVector);

	/*
	 * 空手结束：放回 ECS_Unoccupied → 掏回最强的武器（主武器优先，没主武器掏副武器）。
	 *
	 * BlueprintCallable + 幂等：状态已经不是 ECS_EmptyHand（计时器先收的尾）就直接走人，
	 * 所以重复响、晚到的通知都无害。
	 * ⚠ 只有服务器这一步会真的改状态；客户端身上的这次调用是空转（状态靠复制回来），
	 *   这样才不会出现"客户端自己掏了枪、服务器那边还空着"的两头打架。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void EmptyHandFinish();

	/*
	 * 重设**这一段**空手的保险丝长度（秒）。只在服务器上、且真的在空手里时生效。
	 *
	 * 用途只有一个：这段空手该持续多久**由玩法决定、不是由动画长度决定**的时候。
	 * 默认那条保险丝是按动画长度算的（StartEmptyHandTimer），这种技能会算出一个太短的，
	 * 表现是"动画还在播、人已经被拉回 ECS_Unoccupied、枪掏出来了"。
	 * 例子：逐风云（C）按住控云那段，见 EndCloudburstHold 和 UJettCloudburstAbility::OnEmptyHandStarted。
	 *
	 * ⚠ 它**只是把到点时间往后挪**，不做别的：蒙太奇、状态、收枪/掏枪都不动。
	 *   所以调用方要自己保证"到点那一刻画面上的东西也演完了"（逐风云就是拿 Outro 的长度当参数）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat|EmptyHand")
	void SetEmptyHandFuseDuration(float InDuration);

	// 这段"不可打断的空手"正在进行中吗（ECS_EmptyHand）。
	// 技能侧（UBlasterGameplayAbility::CanActivateAbility）和几个输入口用它挡人。
	bool IsEmptyHandLocked() const { return CombatState == ECombatState::ECS_EmptyHand; }

	/*
	 * 这个角色是谁（EBlasterAgent：Jett / Sage / Phoenix / Clove）。
	 *
	 * **只是个转发**：唯一数据源是 PlayerState 上那个复制的 Agent（大厅选的 / 服务器随机补的），
	 * 这里不存第二份、也不复制一份 —— 两份状态迟早会不一致，而且不一致时谁对没人说得清。
	 * 想改它是走 `ABlasterPlayerController::ServerSelectAgent`（大厅）/ `ServerToggleAgent`（F9）。
	 *
	 * 拿不到 PlayerState 时返回 `None`（角色刚 spawn 出来、PlayerState 还没绑上的那几帧，
	 * 以及编辑器里不跑游戏的预览场景）。**没选英雄时也是 None** —— 需要"具体是哪个英雄"的地方
	 * （比如按英雄取脸/技能）必须先判 None，别拿它去索引数组。
	 *
	 * 动画蓝图那边**不用**绕这条路：UBlasterCharacterAnimInstance 每帧把它抄进了自己的
	 * `Agent` 变量，ABP 里直接读那个（省掉 GetPlayerCharacter + Cast）。
	 */
	UFUNCTION(BlueprintPure, Category = "Agent")
	EBlasterAgent GetAgent() const;

	/*
	 * ——— 逐风云（C）的"**按住**控云" ———
	 *
	 * 和上面这套空手的关系：**这段时间就是一段空手**（收枪 + ECS_EmptyHand + 掏枪被挡），
	 * 只不过"什么时候结束"不是动画说了算，是玩法说了算：
	 *
	 *   按下 C    → 服务器放云（UJettCloudburstAbility）+ 进空手（播 Intro→Loop 那条蒙太奇）
	 *   按住期间  → 云跟准心（AJettCloudburst 每帧读 IsCloudburstHoldActive()）；
	 *               这段时间掏枪是**挡住**的 —— ECS_EmptyHand 本来就挡，不需要额外做什么
	 *   松手 / 云绽放（到时限或撞墙）→ 播 Outro，Outro 播完才掏枪
	 *
	 * 所以出口和普通空手是同一个 EmptyHandFinish()，只是它由**收尾计时器**驱动：
	 * EndCloudburstHold 把保险丝重设成"Outro 那一段的长度"，到点收尾。
	 * 保险丝在这儿从"备胎"变成了**主路** —— 因为没有任何一条身体蒙太奇带收尾通知
	 * （时间轴型蒙太奇的分段名不是方向名，HasEmptyHandFinishNotify 那条路对不上）。
	 *
	 * ⚠ 和 E 冲刺那种空手最大的区别：那种是"动画播完 → 收尾"，这个是"松手/云没了 → 收尾"。
	 *   所以 UJettCloudburstAbility 必须覆写 OnEmptyHandStarted() 把保险丝按"云飞多久"
	 *   重设一次，否则它会按蒙太奇总长算 —— 云还在飞、人已经被拉回 ECS_Unoccupied 了。
	 */

	// 逐风云这一段"按住"还在不在（云还受控 / 空手还没收尾）。
	// 服务器上是权威值；远端机上是复制过来的那一份（用于跟播 Outro，见 OnRep_CloudburstHold）。
	bool IsCloudburstHoldActive() const { return bCloudburstHoldActive; }

	/*
	 * 服务器：开始按住。由 UJettCloudburstAbility 在**放出云那一拍**调
	 * （跟在 StartEmptyHandIfConfigured 后面，见它的实现）。
	 * 置位本身不会广播给本机 —— 权威机器上 RepNotify 不响，"开始"这一拍也不需要谁跟着做什么。
	 */
	void BeginCloudburstHold();

	/*
	 * 结束按住。**幂等**（没在按住 / 已经结束过就直接走人），三个调用点：
	 *   · 玩家松开 C（服务器本机直接调；远端客户端走 ServerEndCloudburstHold）
	 *   · 云绽放了（AJettCloudburst::Bloom → 还在按着也不等了，见"到时限/撞墙"那条规则）
	 *   · 空手被别的东西收尾了（保险丝到点之类）—— 这条是兜底，正常不会走到
	 *
	 * 做的事：
	 *   1. 清掉按住标志（云下一帧就不跟准心了）
	 *   2. **本机**跳 Outro 分段（只有权威机器会走这一步；客户端跟的是 OnRep_CloudburstHold）
	 *   3. 保险丝重设成 Outro 那一段的长度 → 到点 EmptyHandFinish() 掏枪
	 *
	 * 已经在空手外面（比如云快到的时候被打死了，空手被别处收掉了）时只清标志，不动别的。
	 */
	void EndCloudburstHold();

protected:
	// ↑ 恢复原来的访问级别：下面这些仍然是 protected 的实现细节

	void Drop(const FInputActionValue& Value);
	/*
	 * 检视武器（转枪/看弹匣）。纯表现，不参与任何战斗状态机 ——
	 * 播放的动画是当前武器的 FPInspectMontage，各武器蓝图自己挂。
	 * 键位在 IMC 里，绑到 InspectAction（新建 IA_Inspect 后在 BP_BlasterCharacter 里填上）。
	 */
	void Inspect(const FInputActionValue& Value);
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

	// ——— 两台相机：永远只有一台是激活的（归属由 RefreshFPRig() 一处决定）———
	//   · FollowCamera —— 第三人称弹簧臂相机。**别人看你**、以及你观战别人时用它。
	//   · FPCamera      —— 第一人称相机，挂在手模的 Camera 骨骼上，只有本人用它。
	// 取用入口永远是 GetFollowCamera()（本机给 FPCamera，其余给 FollowCamera）。
	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class USpringArmComponent* CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class UCameraComponent* FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	class UCameraComponent* FPCamera;

	// 相机绑在手模的哪根骨骼上。默认 "Camera"。
	// 这根骨骼在网格空间里约在脚底上方 149cm（即眼高）—— UpdateFPRig() 的枢轴就是它。
	// 换手模时只改这个名字，不用动代码。
	UPROPERTY(EditDefaultsOnly, Category = "Camera")
	FName FPCameraBone = TEXT("Camera");

	// 眼位在角色局部空间的位置（厘米）。手模和第三人称身体的骨架原点都在**脚底**，
	// 相机骨骼在参考姿态下离脚底约 149cm —— 和 FPCameraBone 那条注释是同一个数。
	// 服务器给**远端角色**算眼位时用这个常量（那些角色身上没有活着的第一人称相机）。
	UPROPERTY(EditDefaultsOnly, Category = "Camera")
	FVector FPEyeLocalOffset = FVector(0.f, 0.f, 149.f);

	/*
	 *【蹲下过渡】视角要往下移多少（幅度）+ 移到哪了（进度）。
	 *
	 *FPCrouchDropDistance：蹲到位时的下移量，由 Tick 每帧从胶囊实时采样
	 *（= CDO 半高 − 当前半高，默认 88−40 = 48）。**起身后胶囊长回站高、采样值变 0，但这里不覆盖**
	 *—— 起身那半段过渡还要用它，覆盖了视角会在第一帧就弹回站高。
	 *
	 *FPCrouchDropAlpha：过渡进度 0..1，Tick 里按 FPCrouchInterpTime 秒匀速推进
	 *（Valorant 的蹲下是快速下沉而不是瞬间跳变，所以加这一段）。
	 *
	 *两者都在 Tick 里推进、**每台机器都跑**：本机用它带动相机和手模，服务器用它给远端角色
	 *记回溯帧里的眼位。规则同一套，回溯射线的起点才不会和准星错开。
	 *（这也是为什么不能塞进 UpdateFPRig —— 那个函数在服务器上对远端角色根本不 tick。）
	 */
	float FPCrouchDropDistance = 0.f;
	float FPCrouchDropAlpha = 0.f;

	// 蹲下/起身时视角下移的过渡时长（秒）。<=0 表示不插值，瞬间到位。
	// 2026-09-22 从 0.5 调到 0.25（用户："蹲下的插值调的再快点，第一人称蹲的快一点"）——
	// 全程匀速（见 Tick 里那段），所以这个数就是"蹲下去总共花多久"。
	UPROPERTY(EditAnywhere, Category = "Camera")
	float FPCrouchInterpTime = 0.25f;

	// 胶囊"现在比站立时矮了多少"（站立/起身后是 0）。只给 Tick 采样用。
	float ComputeFPCapsuleCrouchDelta() const;

public:
	/*
	 * 【眼位】—— 开火射线起点 / 服务器回溯起点的**唯一来源**。
	 *
	 * 本地玩家：直接给 FPCamera 的世界位置（UpdateFPRig 每帧就把相机摆在眼位上）。
	 * 其余情况：GetMesh() 的变换 × FPEyeLocalOffset —— 和 UpdateFPRig 用的是同一套「骨架原点在脚底」
	 *   约定，所以服务器上给任何角色（含远端玩家）都能算出同一个点。
	 *
	 * 这里**不用**手模的 GetBoneLocation 去读相机骨骼的实际位置：远端角色的手模在服务器上根本不 tick
	 * （见 RefreshFPRig 里的 SetComponentTickEnabled(bLocal)），姿势不保证被求值过。
	 * 用常量则和手臂摆什么姿势无关 —— 服务器要的就是一个对所有人都确定的值。
	 *
	 * 为什么起点是眼睛而不是枪口：玩家是按屏幕上的准星瞄准的，而准星对应的那条线就是**相机射线**
	 * （CombatComponent::TraceUnderCrosshairs 从相机位置出发）。从枪口出发的射线和它差一个
	 * 「枪口到眼睛」的基线，近距离时这个夹角大到能明显打偏。两边都从眼位出发，射线才和看到的准星重合。
	 */
	FVector GetFPEyeWorldLocation() const;

/*
 *【蹲下】第一人称眼位/手模要往下移多少（世界空间，厘米）。站立时是 0。
 *
 *取值 = 胶囊当前比默认矮了多少（默认 88 − 蹲下 40 = 48cm）。为什么是这个量：
 *引擎蹲下时把胶囊（根组件）往下挪这一段，同时把身体网格的相对 Z 往上补**同一段**
 *（UCharacterMovementComponent::Crouch 里 bCrouchMaintainsBaseLocation 那句 MoveComponent，
 *  加 ACharacter::OnStartCrouch 里那句 MeshRelativeLocation.Z += HeightAdjust）
 *—— 一降一升正好抵消，所以**身体网格的世界变换根本不动**。
 *而第一人称的眼睛是从身体网格变换算出来的（见 UpdateFPRig），于是不补这一段的话，
 *蹲下在第一人称里毫无表现：视角不移、准星不动，但胶囊和受击盒已经矮下去了 ——
 *结果就是「蹲在矮墙后面仍然看得见、仍然打得着」，而别人看你整个人都在墙后。
 *
 *两处必须用同一个值：UpdateFPRig()（本地相机 + 手模）和 GetFPEyeWorldLocation()（服务器给远端角色
 *记的回溯眼位）。只改一处就又回到「准星射线起点 ≠ 回溯射线起点」。
 *
 *不写死成 48：胶囊半高在 BP 里可以改，蹲下值也可能被 CharacterMovement 调过，实时相减才不会两边对不上。
 *
 *【过渡】下移不是瞬间到位的：幅度先由 Tick 采样（FPCrouchDropDistance），再乘过渡进度
 *（FPCrouchDropAlpha，0↔1 用 FPCrouchInterpTime 秒匀速走完）。起身那半段也有过渡 ——
 *胶囊一复原、"实时差值"就变 0，所以幅度必须保持住而不是每帧重算，细节见 Tick 里那段注释。
 */
float GetFPCrouchDrop() const;

private:
	/*
	 * ———————————————— 第一人称 rig ————————————————
	 *
	 * 组件树（第一人称这条链上就只有这些，中间没有任何辅助层）：
	 *
	 *     GetMesh() ─→ FPArmsMesh ─┬─→ FPCamera      （挂 FPCameraBone 骨骼，相对变换为零）
	 *                              └─→ FPWeaponMesh  （枪械副本，挂在武器给的 socket 上）
	 *
	 * 手模的**世界变换每帧由 UpdateFPRig() 直接写**，不再靠弹簧臂/后坐力容器这类中间节点。
	 * 那个函数只做两件事，都是绕着一个点——【眼睛】：
	 *   · 让手模的 Camera 骨骼精确落在眼睛上（位置）
	 *   · 让手模的朝向 = 视角朝向 × 后坐力（旋转）
	 * 于是「抬头低头 / 后坐」都是绕眼睛的纯旋转，镜头只转不移；而相机挂在骨骼上，
	 * 手臂动画的呼吸又会带着镜头动（瓦/COD 的做法）。
	 *
	 * 为什么不能把手模直接挂在胶囊或 GetMesh() 上就完事：手模骨架的原点在**脚底**
	 * （相机骨骼离它 149cm）。挂在脚底那一层上、什么都不做的话，抬头低头时整条手臂
	 * 会绕脚底画一个半径 1.5m 的圆弧 —— 看起来是视角在平移而不是在原地转。
	 * 把原点挪到眼睛上就是 UpdateFPRig() 里那两行，不需要为此再摆一个空节点。
	 *
	 * 为什么不是"手模挂在相机下、相机挂胶囊"：那样相机的位姿是你指定的死值，
	 * 手臂的呼吸/后坐就带不动镜头了（失去上面那条特性）。
	 */

	// 瓦的 Wushu 第一人称手臂。只本人可见（OnlyOwnerSee），其余机器上既不渲染也不求值动画。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	class USkeletalMeshComponent* FPArmsMesh;

	/*
	 * 第一人称的**枪械副本**（viewmodel）。存在的理由 —— 为什么不把真枪直接挂到手模上：
	 *
	 * 真枪是**远端玩家看到的那把枪**，位置就是第三人称身体的手。挪到手模上，别人看到的枪
	 * 就飘到不属于它的地方去了；而且真枪网格还挂着枪口 socket（枪口火光/抛壳/给别人补表现时的
	 * 射线起点都读它），手模的手和第三人称身体的手不在一个地方，挪过去这些位置全变。
	 *
	 * 所以：真枪留在第三人称手上，第一人称**另外显示一份副本**，挂在手模的挂点上。要不要用副本是**每把武器**各自决定的（AWeapon::bUseFPViewModel），
	 * 挂点也用武器给的（AWeapon::FPWeaponSocket）。
	 * 副本只本人可见、不碰撞、不投影 —— 纯表现，不参与任何判定。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	class USkeletalMeshComponent* FPWeaponMesh;

	/*
	 * 【往手模上挂任何东西之前必读：100 倍缩放】
	 *
	 * FP_Wushu_S0_Skeleton 的根骨骼缩放是 100（瓦的米制曲线 → UE 厘米的补偿，参考姿态和所有
	 * 动画里都是），所以手模骨骼的 ComponentSpace 变换整条链都带 100 倍。引擎的附着语义是
	 *     子组件世界变换 = 子相对变换 × 父骨骼世界变换
	 *（SceneComponent.cpp:664 CalcNewComponentToWorld_GeneralCase），也就是说
	 * **挂在手模任意骨骼上的东西，世界缩放 = 自己的相对缩放 × 100** —— 直接挂会大 100 倍。
	 *
	 * 补偿办法是在 **socket 资产**里把 RelativeScale 设成 0.01（不是运行时改组件的相对缩放）：
	 * 这样 Persona 预览和运行时（Socket->AttachActor）都自动生效，而且三种附着规则
	 *（SnapToTarget / KeepRelative / KeepWorld）算下来都是 1 倍，不用管引擎走哪条。
	 * 第三人称身体（SK_EpicCharacter）的根骨骼缩放是 1，现有走身体的挂枪路径没这个问题，别去动它。
	 */

	// 前一把因为"显示第一人称副本"而对本人隐藏起来的真枪。
	// 换枪/收枪时必须把它还原 —— 它接下来要么丢在地上，要么被别人看到，不能停在隐蔽态。
	//
	// ⚠ 这里的"隐藏"是 SetOwnerNoSee（**只对本人**不可见），和武器状态里的
	// EWS_Holstered（对**所有人**不可见，用 SetVisibility 实现）是两回事，各有各的用途，
	// 别拿一个当另一个用。武器的 EWS_Holstered 现在能区分"在手上/收起来"了
	//（见 AWeapon 的枚举注释），但这一份记录还得留着：要不要走副本是**角色**的决定
	//（取决于本机是不是本地玩家、这把枪有没有配副本网格），武器自己不知道。
	UPROPERTY()
	AWeapon* FPViewmodelHiddenWeapon;

	// 按当前武器的设置刷新副本：显示哪套网格、挂在手模的哪根骨骼上、要不要显示，
	// 并顺手处理"真枪对本人隐藏/还原"。换枪/收枪（SetEquippedWeapon）和"本人/别人"身份
	// 变化（RefreshFPRig）时调；内部自己判断是不是本地玩家。
	void UpdateFPWeaponMesh();

	// —— 第一人称 rig 的每帧更新 ——
	// RefreshFPRig：**一次性**决定这台机器上第一人称这一套是开还是关（相机归属、手模与副本的
	//   显隐和 tick、第三人称身体对本人隐藏）。每帧调，但答案没变时只比一个 bool 就返回。
	void RefreshFPRig();

	// UpdateFPRig：后坐力回稳 + 把手模摆到眼睛上（相机跟着走）。非本人直接返回。
	void UpdateFPRig(float DeltaTime);

	// 「这台机器上我是不是本人」的缓存值，供 RefreshFPRig 判断要不要重新铺一遍。
	// 不能用默认值代替"还没判过"：第一帧必须无条件铺一次。
	bool bFPRigStateKnown = false;
	bool bFPRigIsLocal = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UWidgetComponent* OverheadWidget;

	UPROPERTY(Replicated,ReplicatedUsing = OnRep_OverlappingWeapon)
	class AWeapon* OverlappingWeapon;

	UFUNCTION()
	void OnRep_OverlappingWeapon(AWeapon* LastWeapon);

	// 正在重叠的大招球（服务器判定，由 AUltOrb 的重叠委托写入）。
	// **不复制** —— 和 OverlappingWeapon 不同，客户端不需要知道"我站在球里"：
	// 按 F 时客户端直接发 ServerPickup RPC，服务器拿自己这份 OverlappingOrb 决定吃球还是捡枪。
	// 少复制一个 Actor 引用，也少一处"客户端说了算"的可能。
	UPROPERTY()
	class AUltOrb* OverlappingOrb = nullptr;

	// 当前正在蓄力的球（仅服务器有值）。角色按 1/2 掏枪时靠它找到该打断哪个球。
	// 不复制：客户端的打断路径走 ServerCancelOrbChannel RPC，不需要本地知道。
	UPROPERTY()
	class AUltOrb* ChannelingOrb = nullptr;

	UPROPERTY(VisibleAnywhere,BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UCombatComponent* Combat;

	// 服务器回溯组件：记录本角色的历史位置，服务器用它重算客户端「开火那一刻」的命中
	UPROPERTY(VisibleAnywhere, meta = (AllowPrivateAccess = "true"))
	class ULagCompensationComponent* LagCompensation;

	UFUNCTION(Server,Reliable)
	void ServerEquipButtonPressed();

	// F 键的统一服务器入口：服务器自己判断"该吃球还是该捡枪"，不采信客户端的选择。
	// 客户端上的 OverlappingOrb 是空的（重叠判定只在服务器做），所以客户端只能走这条 RPC。
	UFUNCTION(Server,Reliable)
	void ServerPickup();

	// 打断吃球（客户端松开 F / 按 1、2 掏枪时调）。服务器从 ChannelingOrb 找到那个球来打断。
	UFUNCTION(Server,Reliable)
	void ServerCancelOrbChannel(bool bRestoreWeapon);

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

	/*
	 * 扔雷动作要多久（秒）。0 = 自动：按 ThrowGrenadeMontage 的长度算，没挂就兜底 1.5 秒。
	 *
	 * 和 AWeapon::ReloadTime 是一回事：ECS_ThrowingGrenade 的出口不能依赖蒙太奇里的动画通知
	 * （那条通知的实现挂在动画蓝图里，换骨架/换 ABP 就没了），所以时长挪到 C++ 里，
	 * 由 UCombatComponent::ServerThrowGrenade_Implementation 起计时器。
	 */
	UPROPERTY(EditAnywhere, Category = "Combat")
	float ThrowGrenadeTime = 0.f;

	/*
	 * ——— 尖刺包：掏出 / 下包 / 拆包的整套动画 ———
	 *
	 * 2026-09-22 从"10 个槽摊在角色蓝图上"改成了"一份共享资产"。
	 *
	 * 为什么改：工程里五个角色蓝图（Blaster / Sage / Clove / Jett / Phoenix）**互为兄弟** ——
	 * 各自继承自己的原生类（ABlasterCharacter / ASageCharacter / ...），谁都不是谁的子类
	 * （之前以为"英雄 BP 都继承 BP_BlasterCharacter"是被 `ABP_BlasterCharacter` 这个子串骗了）。
	 * 于是槽填在 BP_BlasterCharacter 上、PIE 里跑 BP_SageCharacter 时全是空，
	 * 而空槽的表现是**两个 Play* 函数第一行就静默 return**：不打日志、不打警告、什么都不发生。
	 *
	 * 瓦的那套爆能器动画本来就跟英雄无关，所以五个角色蓝图指同一个 USpikeAnimSet 就行，
	 * 以后加英雄也不用再管这批槽。槽位含义、骨架要求、各类触发点都写在 SpikeAnimSet.h 里。
	 *
	 * ⚠ 五个角色蓝图里这个指针**已经都指好了**（2026-09-22 脚本设的）。新加角色 BP 时记得设。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Spike")
	TObjectPtr<class USpikeAnimSet> SpikeAnims;

	/*
	 * 取动画集。没配（SpikeAnims == nullptr）时返回 nullptr 并**只警告一次**。
	 *
	 * 加这一行的理由就是上面那个坑：动画播不出来在这条链上是完全静默的，
	 * 排查一次要翻日志、对 BP 继承关系。留一行指名的警告，下次一眼就能看出来。
	 * 不想要的话直接返回 SpikeAnims.Get() 即可，其余地方不用动。
	 */
	const USpikeAnimSet* GetSpikeAnimSet(const TCHAR* Where);

	// GetSpikeAnimSet 的"只警告一次"标志（按角色实例算，不是全局）。
	bool bWarnedMissingSpikeAnimSet = false;

	// 掏出包时播那两条（身体 + 手模各一）。两个调用点：SetSpikeDrawn(true) 和 OnRep_SpikeDrawn。
	void PlaySpikeEquipAnimations();

	// 拆包动画的两段推进（拆包开始时调）、收尾（打断/拆完都调）。
	// 两个网格分开推：两份资产时长通常不一样，共用一个计时器的话短的那条会在长的那条
	// 播完之前一直僵在最后一帧上。
	void PlayDefuseDrawAnimations();
	void PlayDefuseIdleFP();
	void PlayDefuseIdleTP();
	void PlayDefuseStopAnimations();

	// "掏出"播完 → 接"待命"的两个计时器（各管一个网格）。
	FTimerHandle DefuseIdleTimerFP;
	FTimerHandle DefuseIdleTimerTP;

	// 后坐力相机偏移（度）：正 = 镜头向上抬。开火时累加，每帧由 UpdateFPRig() 回稳到 0。
	// 累积上限 / 回稳速度按当前手持武器取（逐武器差异化，见 AWeapon）。
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

	/*
	 *护甲（Valorant 式护盾）：先于血量被打掉，**每 1 点护甲 = 1 点有效血量**。
	 *
	 *算法（见 ReceiveDamage）：每一发伤害里 `ArmorAbsorptionRatio` 那部分划给护甲池，
	 *剩下的**永远直接进血量**；护甲按它实际吃下的量 1:1 扣减，打穿后剩余伤害照常落到血上
	 *（总伤害守恒，不凭空蒸发）。
	 *
	 *所以 `ArmorAbsorptionRatio` 不改总有效血量，改的是**分配方式**：
	 *  · 0.66（默认，≈Valorant「护盾吸收 2/3 伤害」）：每发 66% 被甲吃掉、34% 直接掉血。
	 *    穿着甲也一直在掉一点血 —— 甲碎的那一刻你已经是残血。
	 *  · 1.0：纯「前置血包」。甲整发整发地吃，甲碎之前血量纹丝不动。
	 *两种设置下总有效血量一样（轻甲 +25、重甲 +50），只是「甲碎时剩多少血」完全不同。
	 *
	 *当前数值：轻甲 25 → 125 有效血量，重甲 50 → 150（和 Valorant 一致）。
	 *
	 *护甲**跨回合保留**（血量每回合回满，甲不回）—— 和武器继承同一套机制：
	 *换回合销毁角色前存进 PlayerState（BlasterGameMode::SavePlayerWeapons），重生后恢复。
	 */
	UPROPERTY(EditAnywhere, Category = "Player Stats")
	float MaxArmor = 50.f;

	UPROPERTY(ReplicatedUsing=OnRep_Armor, VisibleAnywhere, Category = "Player Stats")
	float Armor = 0.f;

	UFUNCTION()
	void OnRep_Armor();

	UPROPERTY(EditAnywhere, Category = "Player Stats")
	float ArmorAbsorptionRatio = 0.66f;

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
	 * 死亡收尸：死亡蒙太奇播完（+ CorpseHideDelay 秒淡出）后把尸体隐藏。
	 * 时机由权威机裁定 —— 各端自己的蒙太奇起播时刻有网络抖动，由服务器统一决定
	 * 再复制给所有端，避免各端消失时间不一致。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ElimmedHidden)
	bool bElimmedHidden = false;

	UFUNCTION()
	void OnRep_ElimmedHidden();

	// 死亡蒙太奇播完的回调（挂在 AnimInstance 的 OnMontageEnded 上）
	UFUNCTION()
	void OnElimMontageEnded(UAnimMontage* Montage, bool bInterrupted);

	// 蒙太奇播完后隔多久收尸（留点时间给溶解淡出跑完）
	UPROPERTY(EditDefaultsOnly, Category = "Elim")
	float CorpseHideDelay = 0.5f;

	FTimerHandle ElimHideTimer;

	void ScheduleCorpseHide(float Delay);
	void HideElimmedCorpse();
	void ApplyElimmedHidden();

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

	/*
	 * ——— 空手蒙太奇：三条动画 + 三个槽名 + 兜底计时器 ———
	 *
	 * 这三条是**技能资产上那三个槽的副本**，只为了复制给远端机（他们的机器上不跑能力表现逻辑，
	 * 只能从角色身上读）。服务器在 BeginEmptyHand 里抄一份并随 CombatState 一起推过去；
	 * 远端机的 OnRep_CombatState 收到 ECS_EmptyHand 时读它们播动画。
	 *
	 * 属性复制和 RepNotify 的时序是安全的：RepNotify 在这一整个 bunch 都写完之后才回调
	 *（引擎 FObjectReplicator::ReceivedBunch → CallRepNotifies），所以 OnRep_CombatState
	 * 里读到的一定是同一次的这三条，不会拿到上一轮的残留。
	 *
	 * 填 AnimSequence 或 AnimMontage 都行（见 FBlasterEmptyHandMontages 的注释）。
	 */
	UPROPERTY(Replicated)
	TObjectPtr<class UAnimSequenceBase> EmptyHandMontageFP;

	UPROPERTY(Replicated)
	TObjectPtr<class UAnimSequenceBase> EmptyHandMontageUB;

	UPROPERTY(Replicated)
	TObjectPtr<class UAnimSequenceBase> EmptyHandMontageLB;

	/*
	 * ——— 本次空手动画走**哪个方向的分段** ———
	 *
	 * 八个方向的动画做在**一个蒙太奇**里（8 个 section，名字 N/NE/E/SE/S/SW/W/NW）。
	 * 服务器在 BeginEmptyHand 里算好方向存这儿，和 CombatState 同一批复制过去；
	 * 各端在 OnRep_CombatState 里按它 Montage_JumpToSection —— 所有人跳到同一段。
	 *
	 * 属性复制的顺序是安全的：RepNotify 在这一整个 bunch 都写完之后才回调
	 *（FObjectReplicator::ReceivedBunch → CallRepNotifies），所以 OnRep 里读到的方向
	 * 一定是这一批的，不会拿到上一轮的残留 —— 和上面三条蒙太奇指针同一个道理。
	 */
	UPROPERTY(Replicated)
	EMovementDirection8 EmptyHandDirection = EMovementDirection8::N;

	/*
	 * ——— 空手里的"再来一次"世代号（RestartEmptyHand 的远端信号）———
	 *
	 * 为什么需要它，而不是让上面几条自己触发重播：
	 *   · 状态没变（EmptyHand → EmptyHand），引擎按**值**比对，没有变化就什么都不复制
	 *   · 三条蒙太奇指针**正好可能没变**（Q 和 E 填的是同一条资产时），方向同理
	 * ⇒ 光改上面几个复制属性，远端机永远收不到"重播"这个事件。
	 *
	 * 所以服务器每次 RestartEmptyHand 就把它 +1，远端机在 OnRep 里用**当前**那几条重播。
	 * 只在 RestartEmptyHand 里自增（BeginEmptyHand 不动它）—— 首次进入空手那条路
	 * 已经有 OnRep_CombatState 负责了，两边都发等于播两遍。
	 *
	 * uint8 够用：一次空手撑死重入一两次，值绕回来了也只是"少一次重播"，
	 * 而重播这件事本身是幂等的（播同一条 → 顶掉自己）。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_EmptyHandRestart)
	uint8 EmptyHandRestartCount = 0;

	// 远端机：空手中又放了个技能 → 用刚复制到的几条蒙太奇重播一遍
	UFUNCTION()
	void OnRep_EmptyHandRestart();

	/*
	 * ——— 三个槽的名字 ———
	 *
	 * 必须和两个动画蓝图里 **Slot 节点**的名字一字不差。
	 *
	 * ★★ 上下半身要**各播各的**，所以这三个不能同名（这条是引擎规则，不是选择）：
	 *   **Slot 的身份纯粹是名字**。引擎在 FAnimInstanceProxy::SlotEvaluatePose
	 *   （AnimInstanceProxy.cpp:1841）里是"遍历所有蒙太奇实例，凡是 IsValidSlot(这个名字)
	 *   的都收进来一起混"—— 它**不区分是哪个 Slot 节点在问**。
	 *   所以动画蓝图里放两个同名 Slot 节点（上下肢各一个）时：
	 *     · 只播**一条**动画 → 两个节点各拿走自己那半边骨骼 → 整条动画照常盖满全身，正常
	 *     · 同时播**两条**（上半身一条 + 下半身一条）→ 两个节点都会把两条收进来、
	 *       按权重归一化后 **50/50 混在一起**（1+1 → 各 0.5），上下半身都变成两条动画的
	 *       糊状混合，而不是"上半身播 A、下半身播 B"
	 *   现役 ABP_BlasterCharacter 里本来就有 "UpperBody" / "LowerBody" 两个 Slot 节点，
	 *   下面两个默认值就是它们。你要是另建了两个节点（比如都叫 DefaultSlot），
	 *   要么改这两个属性（EditDefaultsOnly，角色蓝图里改，不用重编译），
	 *   要么把那两个节点改名 —— **别让它们同名**。
	 *   真撞上同名时 PlayEmptyHandMontages 会打一条 Warning 提醒，不会闷声糊。
	 *
	 * ⚠ 名字对不上（或蓝图里压根没有这个 Slot 节点）时引擎**不报错**，只是静默什么都不播
	 *   —— "配了动画却没反应"先查这里。
	 *   以后在 ABP 里给 Slot 节点改名，这里跟着改就行，不用动 C++。
	 *
	 * 第一人称（ABP_WushuFP）和第三人称是**两个不同的动画实例**，各查各的蓝图，
	 * 名字撞不上，所以 FP 用引擎默认的 "DefaultSlot" 就行。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	FName EmptyHandFPSlotName = TEXT("DefaultSlot");

	// 第三人称上半身：对齐 ABP_BlasterCharacter 的 UpperBody 节点
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	FName EmptyHandUBSlotName = TEXT("UpperBody");

	// 第三人称下半身：对齐 ABP_BlasterCharacter 的 LowerBody 节点。
	// ★ 必须和 UB **不同名**（同名 = 上下半身 50/50 糊成一条，见上面的说明）
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	FName EmptyHandLBSlotName = TEXT("LowerBody");

	/*
	 * 算方向时的速度阈值（cm/s）：速度**低于**它就认为"算不出方向"，
	 * 用传进来的方向提示，提示也没有就退到 N（面朝方向）。
	 *
	 * 5 太小、容易在减速尾巴上乱跳；20~50 比较稳。改它只是让"哪个方向的动画"更保守，
	 * 不会影响别的任何东西。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	float EmptyHandDirectionMinSpeed = 20.f;

	/*
	 * 保险丝时长，**只在三条槽全空时**才用得上（算不出蒙太奇长度）。
	 *
	 * 什么时候会三条全空：技能上一个槽都没填，但勾了 bEnableEmptyHandWithoutMontage
	 *（动画全靠 ABP 那边自己播），角色这边没有任何长度信息可算。
	 *
	 * ⚠ 把它设成**不小于**状态机的长度。短了就是"动画播到一半，人被拉回 ECS_Unoccupied"
	 *   —— 表现是空手动画被硬切一刀，而且切完立刻开始掏枪。
	 *   长了没关系：正常的收尾由 ABP 的退出条件调 FinishEmptyHand()，保险丝只是备胎。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	float EmptyHandFallbackDuration = 0.5f;

	/*
	 * 保险丝为了给收尾通知**让路**而额外加的余量（秒）—— 只在身体那条蒙太奇挂着收尾通知时加。
	 *
	 * 为什么恒起保险丝还要往后推：通知和保险丝的时刻本来就挨得近（冲刺实测：通知 0.4998s、
	 * 按分段长度算出来 0.6167s，差 0.117s），不留余量就是竞速 —— 谁先到全看同一帧里组件
	 * tick 的次序，两台机器能跑出两个结果。推后之后顺序是确定的：
	 *   通知先到（正常）→ EmptyHandFinish 顺手把保险丝清掉，这个值多大都没有表现
	 *   通知没响      → 保险丝就是唯一出口，人到这一刻才被放出来
	 * 所以它只在"通知没响"时看得见，表现是"人比正常多空手站这么一会儿"。
	 *
	 * 调大 = 更安全、卡手更久；调小 = 更跟手、和通知抢跑的风险更大。别小于 0。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|EmptyHand")
	float EmptyHandFuseExtraDelay = 0.5f;

	// 空手的保险丝（只在服务器上起）。通知一到就被 EmptyHandFinish 清掉，所以正常情况它
	// 根本不会响 —— 只在通知没响时才收尾。见 StartEmptyHandTimer。
	FTimerHandle EmptyHandTimer;

	/*
	 * BeginEmptyHand / RestartEmptyHand 的公共实现。
	 *
	 * bRestart = false：已经在空手里就早退（幂等，原来的行为）。
	 * bRestart = true ：已经在空手里就把旧的顶掉（停蒙太奇 + 重开保险丝），
	 *                   并且**跳过**收枪 / 退瞄准 / SetCombatState —— 那几件在空手中
	 *                   已经是既成事实（同值赋值也不会产生复制，反而白跑一趟）。
	 */
	void ApplyEmptyHand(const FBlasterEmptyHandMontages& InMontages, const FVector& InWorldDirection, bool bRestart);

	// 按机器分工播这三条：第一人称只在本机，第三人称上下半身在所有机器。
	// 跳哪个分段由 EmptyHandDirection 决定。
	void PlayEmptyHandMontages();

	/*
	 * 在一个动画实例上按**槽名**播一条空手动画（裸序列会现造动态蒙太奇），
	 * 然后跳到 SectionName 那一段。返回是否真的播出去了。
	 *
	 * bStopAllMontages 见实现里的注释；SectionName 传 NAME_None = 不跳段（整条播）。
	 */
	static bool PlayEmptyHandTrack(class UAnimInstance* AnimInstance, class UAnimSequenceBase* Asset,
		FName SlotName, EMovementDirection8 Direction, bool bStopAllMontages);

	// 把已在播的空手蒙太奇停掉（本机局部操作，各端各自收尾时调）。
	// 只停这三条带槽名的轨道，不动别人 —— 见 EmptyHandFinish 里为什么要停。
	void StopEmptyHandMontages();

	// 世界空间方向 → 八向枚举。速度太小且没给提示时返回 N（面朝）。
	EMovementDirection8 ResolveEmptyHandDirection(const FVector& InWorldDirection) const;

	// 起保险丝（只在服务器上有效）。时长 = 所跳分段的长度；身体那条蒙太奇挂着收尾通知时
	// 再加 EmptyHandFuseExtraDelay 往后推，保证通知先到（通知一到就会把它清掉）。
	// 整套理由见实现里的注释。
	void StartEmptyHandTimer();

	// 保险丝到点 → EmptyHandFinish()
	void EmptyHandTimerFinished();

	// ——— 持投掷物（Phoenix E 闪光 / Q 火球 / C 火墙）的动作动画 ———

	/*
	 * 资产在**技能**上（UBlasterGameplayAbility 的 ThrowableEquipMontages / ThrowMontages /
	 * FireMontages / LiftMontages，每组三段槽），角色这边只管"什么时候、在哪台机器上、往哪个动画实例播"。
	 *
	 * ★ 站着待命那一段**不在这里**：它归动画蓝图 —— 持着东西期间两个动画实例把 WeaponType
	 *   报成 GetThrowableWeaponType(Kind)（见 WushuFPAnimInstance / BlasterCharacterAnimInstance），
	 *   ABP 按 WeaponType 切到对应的持械状态，姿势一直维持到 WeaponType 变回去。
	 *   所以 C++ 播的全是**一次性动作**（拿起 / 丢出去 / 按住 / 收起）：没有循环段、没有段与段接续、
	 *   也不需要记住"现在演到哪一段"。
	 *
	 * 两条轨道（手模 / 身体）分开叫，因为驱动来源不一样：
	 *   · 手模 → 本机按下技能键那一刻（本地预测，不等服务器；别人的机器上没这条）
	 *   · 身体 → 服务器（ServerSetThrowableHolding / ServerStartBlazeFire）+ 远端机收到复制
	 *           （OnRep_ReplicatedThrowableKind / Multicast*）—— 每台机器只会走其中一条
	 *
	 * ★ bHoldLastFrame：只有"拿起"那一段传 true。
	 *
	 *   拿起那条现造出来的动态蒙太奇，引擎默认 Enable Auto Blend Out = true ——
	 *   播到最后一帧就自己往 ABP 的持械姿势混出去。而 ABP 那边
	 *   （TP_Phoenix_S0_*_Idle_UB / FP_Phoenix_S0_*_Idle）是另一条动画、自己的播放进度，
	 *   混进去的那一下在画面上就是"掏出动作又演了一遍"（用户 2026-09-22 报的不死鸟三个技能
	 *   放完 equip 之后"重新掏"）。关掉它 = 停在末帧不动，等丢出去那条蒙太奇或
	 *   StopThrowableMontages 来接管 —— 和拆包"掏出"那两条蒙太奇的处理是同一个做法。
	 *
	 *   丢出去 / 按住 / 收起这三条**不能**跟着关：它们演完就该让 ABP 接着（那是设计上要的交接）。
	 */
	void PlayThrowableSet(bool bFirstPerson, const struct FBlasterThrowableMontages& Set,
	                      bool bHoldLastFrame = false);

	// 四段动作各一个入口（内部查技能的 CDO 再去对应那组槽）。火墙那两段只有火墙走得到。
	void PlayThrowableEquip(bool bFirstPerson, EBlasterThrowableKind Kind);
	void PlayThrowableThrow(bool bFirstPerson, EBlasterThrowableKind Kind);
	void PlayThrowableFire(bool bFirstPerson);
	void PlayThrowableLift(bool bFirstPerson);

	/*
	 * 收尾段（丢出去 / 收起）**开始演**了 —— 掏枪要排在它后面（见 QueueOrEquipAfterThrow），
	 * 而且要等它演完的通知来兑现。bLift = 这一下是"收起"（火墙）而不是"丢出去"。
	 *
	 * 为什么在本机按下那一刻就记（而不是等身体那条动画真播）：本机退出持投掷物态这一帧
	 * 就要回答"枪是排队还是立刻掏"，而身体那条要等服务器多播回来才播 —— 等不起。
	 */
	void BeginThrowableFinisher(EBlasterThrowableKind Kind, bool bLift);

	/*
	 * 收尾段演完了：清掉"在等收尾"，兑现那次延后的掏枪。**两个来源共用**：
	 *   · 第三人称动画末尾的通知（正门，见 NotifyThrowableSkillFinished）
	 *   · ThrowableFinisherTimer（备胎：通知漏摆时按身体那条资产的长度兜底）
	 */
	void ThrowableFinisherFinished();

	// 在等收尾段演完吗（掏枪要排在它后面）
	bool IsWaitingForThrowableFinisher() const { return bWaitingForThrowableFinisher; }

	// 手上那个技能的投掷物动画全停掉（退出持投掷物、按技能键打断那两条路）。
	// 收尾段正在演时**不许停**（那一帧正好在掏枪，停掉等于把丢出去的动作切一刀）。
	void StopThrowableMontages(EBlasterThrowableKind Kind);

	/*
	 * 收尾段在身体上播的是哪两条（上半身 / 下半身）。
	 *
	 * 唯一的作用是**认通知**：收尾段的末尾通知一到就该掏枪，但火墙"按住"那一段的末尾通知
	 * 会在"松手 → 收起"已经起播之后才响（玩家按得早）—— 不认资产的话，那条晚一拍的通知
	 * 会把刚起播的收起动作当场切掉、枪也提前亮出来。
	 */
	TObjectPtr<const class UAnimSequenceBase> ThrowableFinisherAssetUB;
	TObjectPtr<const class UAnimSequenceBase> ThrowableFinisherAssetLB;

	// 在等收尾段演完（上面那两条可能为空：那个技能没配身体那条资产）
	bool bWaitingForThrowableFinisher = false;

	// 收尾段的备胎计时器（通知漏摆时兜底掏枪）。通知一到就清掉它。
	FTimerHandle ThrowableFinisherTimer;

	// 备胎在资产长度之外多留的余量：给通知留出先到的机会，也让资产自己 blend out 完
	static constexpr float ThrowableFinisherFallbackExtra = 0.15f;

public:
	/*
	 * "这一段演完了" —— 由动画通知 UAnimNotify_ThrowableSkillFinished 在动画末尾叫。
	 *
	 * ★ 这是收尾那一下的**正门**：什么时候算演完、什么时候掏枪，由用户在蒙太奇上摆的
	 *   那个通知说了算。代码这边的计时器只是**备胎**（通知漏摆时兜底），两条路都进
	 *   ThrowableFinisherFinished，先到的生效、后到的天然是空操作。
	 *
	 * bFirstPerson = 通知是从手模上响的。**这种一律不理会**：通知按约定摆在第三人称动画末尾，
	 *   而掏枪是身体 + 手模一起的事，以身体那条为准（所以配资产时让手模那条比身体短一点）。
	 * FinishedAsset = 响通知的那条资产，用来认"是当前这条收尾资产吗"。
	 */
	void NotifyThrowableSkillFinished(bool bFirstPerson, const class UAnimSequenceBase* FinishedAsset);

private:

	// 这个种类（闪光 / 火球 / 火墙）的技能 CDO —— 四段动画挂在它身上。任何机器都能调
	//（扫 DefaultAbilities，和 GetCurveballAbilityCDO 同一个路子）。没有这个技能 → nullptr。
	const class UBlasterGameplayAbility* GetThrowableAbilityCDO(EBlasterThrowableKind Kind) const;

	// 在一个动画实例上按槽名播一条投掷物动画（裸序列现造动态蒙太奇）。返回是否真播出去了。
	// 全是一次性的（LoopCount = 1）—— "举着待命"那条循环在动画蓝图里，不走这儿。
	// bStopAllMontages 的取舍和 PlayEmptyHandTrack 一样。
	// bHoldLastFrame = 关掉现造那条蒙太奇的 Enable Auto Blend Out（停在末帧不往 ABP 混出），
	// 只有"拿起"传 true，理由见 PlayThrowableSet。
	static bool PlayThrowableTrack(class UAnimInstance* AnimInstance, class UAnimSequenceBase* Asset,
		FName SlotName, bool bStopAllMontages, bool bHoldLastFrame = false);

	// 丢出去那一下**别人也要看得见**。本机按下时已经先播了（预测），这条 RPC 只为别人的屏幕：
	// 客户端 → 服务器（可靠）→ 多播。
	UFUNCTION(Server, Reliable)
	void ServerNotifyThrowableThrown(EBlasterThrowableKind Kind);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastThrowableThrown(EBlasterThrowableKind Kind);

	/*
	 * 火墙那两段（按住 / 收起）在**别人屏幕上**的身体动画 —— 和上面 Throw 那对同构，
	 * 只是两段共用一条路：bFiring = true 是"按住左键了"，false 是"松手 / 球飞完，收起"。
	 * 手模那条只有射手本人有，本机按下那一刻已经自己播过。
	 *
	 * 不用传种类：这两段是火墙专利（闪光 / 火球都是一按就出去，走上面 Throw 那条）。
	 *
	 * 谁调：
	 *   · 按住：服务器在 ServerStartBlazeFire 里直接多播（按左键那一下本来就要过服务器）
	 *   · 收起：射手那台（松开左键 / 球飞完）先请求，服务器再多播
	 * 用 Reliable：这是一段"别人屏幕上必须演出来的"动作，丢了的话身体会一直举着。
	 */
	UFUNCTION(Server, Reliable)
	void ServerNotifyThrowableBlazePhase(bool bFiring);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastThrowableBlazePhase(bool bFiring);

	// ——— 逐风云（C）按住控云（公开接口那段的实现与状态）———

	/*
	 * 逐风云那一段的 Outro 分段名（默认 "Outro"，见 UJettCloudburstAbility 里那条蒙太奇）。
	 * 时间轴型蒙太奇是按名字跳段的，段名改了这里也要跟着改 ——
	 * 找不到这一段时 EndCloudburstHold 会**直接收尾**（不播 Outro、立刻掏枪），不会卡住。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Cloudburst")
	FName CloudburstOutroSection = TEXT("Outro");

	/*
	 * 逐风云"按住"的标志。**要复制**：射手本机得靠它去跳 FP 那段的 Outro
	 *（客户端机器拿不到权威那次 EndCloudburstHold 的调用），别人的机器靠它跳身体那条。
	 * 权威机器上 RepNotify 不会响，所以"开始按住"不需要谁跟着做什么；
	 * 但**权威机器自己**在结束时要走一遍本机跳段（见 EndCloudburstHold），两条路不重叠，
	 * 也就不会出现"同一段 Outro 被跳两次、从头又播一遍"。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_CloudburstHold)
	bool bCloudburstHoldActive = false;

	// 远端机收到"不按了" → 把正在播的空手蒙太奇跳到 Outro 分段（射手本机跳手模那条，
	// 别人的机器跳身体那条；空手已经收尾了就什么都不做）。
	UFUNCTION()
	void OnRep_CloudburstHold();

	// 客户端松手时告诉服务器（权威机器上 SkillCReleased 直接调 EndCloudburstHold，不走这条）。
	// Reliable：丢了的话保险丝也会兜底，但那条要等整个 FlyDuration + Outro，手感差得多。
	UFUNCTION(Server, Reliable)
	void ServerEndCloudburstHold();

	/*
	 * 把正在播的空手蒙太奇跳到 CloudburstOutroSection 那一段（本机局部操作）。
	 * 手模那条只在射手本机播，所以两个调用点分工明确：
	 *   · 权威机器（房主 / 单机）→ EndCloudburstHold 里调
	 *   · 其余客户端 → OnRep_CloudburstHold 里调
	 * 跳段前会确认当前**不在**那一段上：重复跳会把位置设回段首、Outro 从头再播一遍。
	 */
	void JumpEmptyHandToCloudburstOutro();

	// 上面那个的单个轨道版：资产不是蒙太奇 / 没有这一段 / 这一条没在播 → 静默跳过。
	void JumpEmptyHandTrackToOutro(class UAnimInstance* AnimInstance, class UAnimSequenceBase* Asset);

	// --- Sage 治疗（私有状态与服务器逻辑）---
	void SageHealPressed(const FInputActionValue& Value);
	void EnterSageHealSelect();
	void ExitSageHealSelectLocal();
	void PerformSageHeal(ABlasterCharacter* Target);
	bool HasSageHealAbility() const;

	// --- 大招（X 键）查找 ---
	// GetUltimateAbilityCDO / GetAbilityCDOBySkillType 已挪到上面的公开区（HUD 要读 SkillIcon）

	// --- Phoenix 曲线球 / 火球：共用的"手里拿着技能投掷物"状态 ---
	// 交互（用户定的）：
	//   E  ←→ 曲线闪光：E 拿起（收枪）→ 左键=左拐抛 / 右键=右拐抛 → 掏回枪；再按 E 或 1/2/3 可打断
	//   Q  ←→ 火球：    Q 拿起（收枪）→ **左键丢出去**（2026-09-21 用户改的，之前是右键）→ 掏回枪；再按 Q 或 1/2/3 可打断
	//
	// 两个技能共用下面这一整套（互斥：同时只可能拿着一个，见 HeldThrowableKind）：
	//   · 拿起/放下 = EnterThrowableHold / ExitThrowableHoldLocal（本地预测 + 服务器 RPC 兜底）
	//   · 抬起枪的时机、1/2/3 打断、换弹/捡枪/开镜让路 —— 全都是看 bThrowableHolding 一个标志
	// 各自不同的只有"哪个键进"和"哪个键丢"，那两处分派在 CurveballPressed / FireballPressed
	// 和 AimStart / FireStart 里，一眼能看全。

	// ——— 曲线闪光（Phoenix E）———
	bool HasCurveballAbility() const;
	const class UBlasterGameplayAbility* GetCurveballAbilityCDO() const;
	bool IsCurveballAvailable() const;
	void CurveballPressed();
	void ThrowCurveball(bool bCurveLeft);

	// ——— 火球（Phoenix Q）———
	bool HasFireballAbility() const;
	const class UBlasterGameplayAbility* GetFireballAbilityCDO() const;
	bool IsFireballAvailable() const;
	void FireballPressed();
	// 丢出去（左键 FireStart 调）：本地立即退出持球态（服务器掏枪经复制跟上），并服务器侧生成火球
	void ThrowFireball();

	// ——— 火墙（Phoenix C）———
	// 交互（用户定的）：C 武装（收枪）→ **按住左键发射**（球隐形无碰撞、朝准心飞，
	// 地上跟着长烟墙）→ 松手 / 球飞完 → 收尾掏枪；武装期间再按 C = 收起来。
	bool HasBlazeAbility() const;
	const class UBlasterGameplayAbility* GetBlazeAbilityCDO() const;
	bool IsBlazeAvailable() const;
	void BlazePressed();

	// 按住左键那一下：本地切到"发射/控球"态 + 让服务器生成球。只在还没发射时有效
	void StartBlazeFire();

	// 服务器：生成球（激活技能 = 扣充能）并进入"控球"态。客户端按左键时走这条
	UFUNCTION(Server, Reliable)
	void ServerStartBlazeFire();

	/*
	 * 火墙"按住控球"这一段。
	 *
	 * ★ 和逐风云那个 bCloudburstHoldActive 是**同一种东西但不同存在形式**：
	 *   逐风云那个要复制（客户端得靠它跳 Outro 分段），火墙这个只有服务器读得到就够
	 *   （球是服务器在飞，问的就是服务器上的这一份），所以它是普通成员，不是复制属性。
	 *   各端各自预测着写：射手本机按左键时置位、松手时清零，服务器收到 RPC 后做同样的事。
	 */
	bool bBlazeFiring = false;

	/*
	 * 控球段的保险丝（秒）。
	 *
	 * 正常路径永远不会响：松手、按住那段演完（= 到时间）、球飞完都会收尾。
	 * 它兜的是"球根本没生成出来"（没充能 / 服务器没能激活能力 / 球类没配）——
	 * 没有它的话射手会永远卡在持墙态，连枪都掏不出来。
	 *
	 * 取值要**大于**"按住 + 收起"两段动画加起来（0.983 + 0.683 ≈ 1.67 秒，用户定成这个技能的
	 * 封墙最长时间）：比它短的话，动画那条正常路径会被保险丝抢先打断。
	 * 2.5 秒 > 1.67 秒，还有余量，宁可多站一会也不要卡死。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Combat|Blaze")
	float BlazeFireFuse = 2.5f;

	FTimerHandle BlazeFireFuseTimer;

	// ——— 两个技能共用的持投掷物骨架 ———

	// 进入持投掷物态：先收镜再收枪（SetAiming 内部要求有手持武器），记录收起的那把枪
	void EnterThrowableHold(EBlasterThrowableKind Kind);

	/*
	 * 本地退出持投掷物态并把枪掏回来（本地预测；服务器 RPC 兜底）。
	 * 1/2/3 打断走的也是它。
	 *
	 * ★ 掏枪这一步是**异步**的：如果手上那条正播着"丢出去"那一下，枪会等到那一动画播完
	 *   才真的掏出来（见 QueueOrEquipAfterThrow）。这是用户明确要求的
	 *   ——「技能放完要等 throw 动画播完再切换状态掏枪」。
	 *
	 * bTellServer = false：调用方已经确定服务器也知道要退出了（远端机的 OnRep 那条路），
	 * 不用再发一次 RPC。
	 */
	void ExitThrowableHoldLocal(bool bTellServer = true);

	/*
	 * ——— 「等投掷那一下播完再掏枪」———
	 *
	 * 原来退出持投掷物态是**立刻**把枪掏回来的，于是"丢出去"的动作还在手上播，
	 * 枪已经从背上飞到手里了 —— 表现是动画演到一半被枪切掉（用户 2026-09-20 报的）。
	 * 现在走这条路：要掏的那把先存进 ThrowablePendingEquipWeapon，
	 * 等收尾那一段演完了再真掏（判据见 IsWaitingForThrowableFinisher / ThrowableFinisherFinished）。
	 *
	 * 只有"还在播丢出去 / 收起那一下"才会延后；被打断（按 1/2/3 / 再按一下技能键）是立刻掏枪。
	 */
	void QueueOrEquipAfterThrow(class AWeapon* Weapon);

	// 看看能不能把等着的枪掏出来了（收尾段演完、玩家没有自己换枪时才掏）
	void TryFlushPendingThrowEquip();

	// 手上那个占位火球（第一人称/第三人称各一个）的显隐 —— Q 火球和 E 闪光共用这一个球，
	// C 火墙不显示（它是按住放墙，不是拎着球丢）
	void UpdateHeldThrowableVisual();

	// 本地按下的"拿起/放下"同步给服务器（服务器要跟着收枪/掏枪，并且复制给观战者）
	UFUNCTION(Server, Reliable)
	void ServerSetThrowableHolding(bool bHolding, EBlasterThrowableKind Kind);

	// 服务器退出持投掷物态（掏回枪）。客户端本地退出走 ExitThrowableHoldLocal
	void ServerExitThrowableHold();

	// 是否处于持投掷物状态（本地；服务器用自身副本做校验）
	bool bThrowableHolding = false;
	// 手上拿的是哪一种（本地预测值；复制给观战者的那份是 ReplicatedThrowableKind）
	EBlasterThrowableKind HeldThrowableKind = EBlasterThrowableKind::None;
	// 持投掷物时收起的武器（进入时记录，抛掷/打断后掏回）
	UPROPERTY()
	TObjectPtr<class AWeapon> ThrowableHolsteredWeapon;

	/*
	 * 等"丢出去"那一下播完再掏的那把枪（见 QueueOrEquipAfterThrow）。
	 *
	 * 和 ThrowableHolsteredWeapon 的区别只有时序：那个是"已经交还给手了"，
	 * 这个是"还在等动画"。同一时刻最多只有一个非空 —— 退出持投掷物态时把前者搬到这里。
	 */
	UPROPERTY()
	TObjectPtr<class AWeapon> ThrowablePendingEquipWeapon;

	// ——— 手上那个占位火球（用户要求：占位视觉、逻辑优先）———
	//
	// 两个组件挂在不同骨架上，各自服务一类观众：
	//   · HeldFireballFP 挂**手模**（FPArmsMesh）→ 只自己看得见（第一人称）
	//   · HeldFireballTP 挂**第三人称身体**（GetMesh()）→ 别人看得见
	// 两个都用 KeepWorld 缩放规则挂上去 —— 手模那个挂点的局部缩放是 0.01（枪械副本那一套），
	// 继承下来的话这个球会小到看不见。
	UPROPERTY(VisibleAnywhere, Category = "Abilities|Visual")
	TObjectPtr<class UStaticMeshComponent> HeldFireballFP;

	UPROPERTY(VisibleAnywhere, Category = "Abilities|Visual")
	TObjectPtr<class UStaticMeshComponent> HeldFireballTP;

	// 手上那颗球的**特效**组件，和上面两个占位球一一对应（同样的挂点、同样的可见性规则）。
	// 填了 HeldFireballEffect 就开这两个、藏上面那两个；没填就反过来。
	UPROPERTY(VisibleAnywhere, Category = "Abilities|Visual")
	TObjectPtr<class UNiagaraComponent> HeldFireballFX_FP;

	UPROPERTY(VisibleAnywhere, Category = "Abilities|Visual")
	TObjectPtr<class UNiagaraComponent> HeldFireballFX_TP;

	// 手上火球的大小（世界缩放）。0.3 × 半径 50 的引擎球 ≈ 15cm 的团，
	// 和握在手里的东西一个量级。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	float HeldFireballScale = 0.3f;

	// 手上火球的材质（占位）。留空 = 引擎默认材质（依然看得见）。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	class UMaterialInterface* HeldFireballMaterial;

	/*
	 * ——— 手上那颗球的**特效**（2026-09-21 加：用户要"armed 的时候就要能看见火球"）———
	 *
	 * 填了这个就用 Niagara 当手上的球，上面那套占位球（网格 + 材质）自动藏掉；
	 * 留空就还是现在这个发光球 —— 和投掷物那边（APhoenixFireball::FlightEffect）
	 * 完全同一个规矩：**填在属性或组件的 Asset 槽里都认**，两边都空才退回占位。
	 *
	 * 想和飞出去那颗长得一模一样，就把 FlightEffect 用的那个资产也填到这儿
	 *（同一个 Niagara 资产可以同时挂在两个地方 —— 这里没有"只能挂一处"的限制）。
	 *
	 * ⚠ 这里**不加** HeldFireballScale：那个 0.3 是给半径 50 的引擎球换算的，
	 *   套到 Niagara 上是把用户的特效整体缩小 3.3 倍。特效按资产自己的大小播，
	 *   真要调有下面那个 HeldFireballEffectScale。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	class UNiagaraSystem* HeldFireballEffect;

	// 手上特效的缩放（1 = 特效资产原大小）。只在填了 HeldFireballEffect 时有意义。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	float HeldFireballEffectScale = 1.f;

	/*
	 * 手模上挂火球的挂点。**socket 名和骨骼名都行** —— 引擎两样都认
	 *（does_socket_exist("R_WeaponMaster") 实测 True，因为骨架里 R_WeaponMaster 是根骨骼）。
	 *
	 * ⚠★ 2026-09-21 踩过的坑：这个值改了**只在运行时那一次 AttachToComponent 里生效**。
	 *   构造函数里那句 SetupAttachment(FPArmsMesh, HeldFireballFPSocket) **永远用的是
	 *   C++ 默认值** —— 构造函数跑的时候 BP 的类默认值还没套到这个对象上。
	 *   所以"在 BP 里把插槽名改了"这件事，如果只有构造函数那次挂载，是**完全不生效**的：
	 *   实测 BP_PhoenixCharacter 上属性已经改成 R_WeaponPoint，而组件上真正生效的挂点
	 *   仍然是 C++ 默认的 WeaponADSSocket（读 get_attach_socket_name() 得到）。
	 *   现在由 UpdateHeldThrowableVisual() 在第一次真正持火球时按这里的值重新挂一次。
	 *
	 * 默认值从 WeaponADSSocket 换成 R_WeaponMaster：WeaponADSSocket 在 FP_Wushu_S0 这套
	 * 手模上**既不是 socket 也不是骨骼**（bone_index=-1），引擎会退到"挂在手模原点" ——
	 * 表现是一团火糊在镜头上。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	FName HeldFireballFPSocket = TEXT("R_WeaponMaster");

	// 第三人称身体上的挂点。同理换成骨架里真实存在的 R_WeaponMaster（RightHandSocket 在这套
	// Wushu 骨架上不存在，bone_index=-1，会退到身体原点）。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|Visual")
	FName HeldFireballTPSocket = TEXT("R_WeaponMaster");

	// socket 是否存在只查一次（查不到会按引擎自己的兜底挂在网格原点，会打一条 Warning）。
	// 缓存是为了别每帧问 USkeletalMeshComponent。
	bool bHeldFireballVisualReady = false;

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

	// --- Clove 暮蝶：封烟（E 键开地图选点）---
	//
	// 和 Sage 治疗一样是「E 键技能」，而且是同一套骨架：从 DefaultAbilities 里按
	// SkillType 找到那张配置用的能力 CDO → 读它的冷却 GE 当次数用 → 手写状态机 + Server RPC。
	//
	// 但**界面和输入不在这里** —— 开地图要抢鼠标、要切输入模式，那是 PlayerController 的活
	//（和 BuyMenu 同一套 bShowMouseCursor / SetInputMode）。更关键的是：阵亡时
	// ABlasterCharacter::MulticastElim 会 DisableInput(PC) 把角色的输入组件摘出栈，
	// 角色的 E 再也收不到。而「死后放烟」是暮蝶的招牌，所以 E 的入口另一半必须挂在
	// PC 自己的按键绑定上（见 ABlasterPlayerController::OnSmokeKeyPressed）。
	// 角色这边只负责：转发 E、按 SkillType 找配置、执行服务器权威的落地与扣次数。
	bool HasSmokeAbility() const;
	const class UBlasterGameplayAbility* GetSmokeAbilityCDO() const;

	// IsCloveSmokeAvailable / GetCloveSmokeCharges / ServerPlaceCloveSmoke(s) 都是
	// public 的 —— 调用方是 ABlasterPlayerController（界面和 E 键在那儿）。
	// 声明见文件下方的 public 段。
	// 扣一次充能（和 ApplySageHealUsed 同一套路：拿冷却 GE 的 CDO 直接挂到自己 ASC）
	void ApplyCloveSmokeUsed();

	// 在指定世界坐标落一团烟：向下打射线找地板 → 生成 ACloveSmoke → 扣一次充能。
	// 返回 false = 没落地（射线打空 / 没配烟蓝图 / 没充能），**这种情况不扣充能**。
	// 只在服务器上有意义（调用方负责判 HasAuthority）。
	// ServerPlaceCloveSmoke（单点）和 ServerPlaceCloveSmokes（批量）共用它。
	bool PlaceCloveSmokeAt(const FVector& WorldLocation);

	// 要生成的烟 Actor（填 BP_CloveSmoke）。空 = 点确定什么都不发生。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|CloveSmoke")
	TSubclassOf<class ACloveSmoke> CloveSmokeClass;

	// 落点射线：从 (施法者Z + Up) 往下打 Down 这么深，取第一个地面命中。
	// 起点跟着**施法者**而不是地图固定高度 —— 这样自动选中玩家当前所在的那一层楼，
	// 多层建筑（A 大庭上下层）才不会永远只封到最上面那层。
	UPROPERTY(EditDefaultsOnly, Category = "Abilities|CloveSmoke")
	float CloveSmokeTraceUpOffset = 500.f;

	UPROPERTY(EditDefaultsOnly, Category = "Abilities|CloveSmoke")
	float CloveSmokeTraceDownDepth = 3000.f;

	// --- 大招「生效中」状态（通用，Phoenix 再来一次 / Jett 刃风暴共用）---
	//
	// 两个大招的"生效实体"长得完全不一样：
	//   · Phoenix「再来一次」—— 状态就在角色自己身上（标记点 + 满血满甲拉回 + 阵亡不真死）
	//   · Jett「刃风暴」   —— 状态是手里那把飞刀（掏出 / 扔完 / 收刀）
	// 但「还剩几秒」「结束时该收回什么」这两件事 HUD 和死亡结算都要问，所以统一记在角色上：
	// 用 ActiveUltimate 记住"当前是哪一个"，收尾时按它分派（见 ServerEndUltimate）。
	//
	// 状态放角色上（而不是能力实例上）的原因：
	//   ① 死亡拦截点在 ABlasterCharacter::ServerElim —— 所有致死路径（枪械/爆炸/坠落）的公共出口；
	//   ② HUD 要读它（技能条大招槽在生效期间显示倒计时，而不是一直显示"就绪"）。
	// 两个能力本体都只负责"按下 X 时启动 + 立刻结束生命周期"，见各自的类注释。
public:
	// 扔雷动作的实际时长（秒）：ThrowGrenadeTime > 0 就用它，否则按 ThrowGrenadeMontage
	// 的长度，都没有则兜底。UCombatComponent::ServerThrowGrenade 的服务器计时器用它。
	// 属性本身在 private 区 —— EditAnywhere 在详情面板里照样能配。
	float GetThrowGrenadeDuration() const;

	// 当前正在生效的大招。复制到各端：观战/队友能看到，HUD 也读它。
	UPROPERTY(ReplicatedUsing = OnRep_ActiveUltimate)
	EActiveUltimate ActiveUltimate = EActiveUltimate::EUA_None;

	FORCEINLINE bool IsUltimateActive() const { return ActiveUltimate != EActiveUltimate::EUA_None; }

	// 是不是「再来一次」在生效。**阵亡拦截只认它** —— 刃风暴不拦死亡（人该死还是死，只是收刀）。
	FORCEINLINE bool IsRunItBackActive() const { return ActiveUltimate == EActiveUltimate::EUA_RunItBack; }

	// 是不是「刃风暴」在生效。Jett 飞刀那条链（能力再按一次 X = 呼出/收起、显隐判断）都读它。
	FORCEINLINE bool IsBladeStormActive() const { return ActiveUltimate == EActiveUltimate::EUA_BladeStorm; }

	// 大招结束的绝对服务器时间（= PC->GetServerTime() 的时间轴；0 = 未生效）。HUD 倒计时用。
	UPROPERTY(Replicated)
	float UltimateEndTime = 0.f;

	// 服务器：进入大招生效状态 —— 记时间、起到期定时器、广播。
	// 由 UPhoenixRunItBackAbility / UJettBladeStormAbility 调用（只在各自的服务器分支）。
	// 具体大招要做的前置动作（记标记点 / 生成飞刀）由调用方在**这之前**自己做完。
	void ServerStartUltimate(EActiveUltimate Kind, float Duration);

	// 服务器：起 Jett 大招「刃风暴」——生成飞刀塞进近战槽并掏出来，然后进入大招生效状态。
	// 为什么生成动作在角色侧而不在能力里：要 World、要 Combat、要给 actor 设 Owner/Instigator，
	// 角色手上全都有；能力那边只有一个 ActorInfo。
	void ServerStartBladeStorm(TSubclassOf<class AJettKnives> KnivesClass, float Duration, int32 KnifeCount);

	/*
	 * 服务器：大招生效期间**再按一次 X** —— 在"手上拿着飞刀"和"收起来、掏回枪"之间切换。
	 *
	 * 用户 2026-09-18 的要求："x期间装备别的枪要把knife隐藏了，然后再次按x可以呼出飞镖
	 * 并且重新播放equip动画"。也就是 **X 是飞刀的呼出/收起开关**（Valorant 里刃风暴本来就是
	 * 这么一个可收起的技能），飞刀不再跟"近战槽里有什么"绑定：
	 *   · 拿别的枪 → 飞刀丢在挂点上，挂架那 5 把刀跟着隐藏（见 AJettCharacter::SyncKnivesToAmmo）
	 *   · 再按 X   → 飞刀掏回手上 + 重播掏出动画（手模/身体由 EquipSlotWeapon 播，
	 *                刀骨骼那条由 AJettCharacter 的"已掏出"上升沿播）
	 *
	 * 只在刃风暴生效期间有效，别的状态（没开大 / 已经收招）什么都不做 ——
	 * "没开大时按 X"是开始大招，那条路在能力里（ServerStartBladeStorm），不走这里。
	 */
	void ServerToggleBladeStormKnives();

	// 服务器：手上的飞刀扔空了（由 AJettKnives::NotifyIfDepleted 上报）。
	// 只排一个下一帧的定时器，不立刻收招 —— 调用栈这时候还压在 AJettKnives::Fire() 里，
	// 直接收招会把那把刀自己 Destroy 掉，上层（UCombatComponent::Fire）后面还要拿它发 ServerFire。
	void ServerRequestBladeStormEnd();

	// 服务器：把"还没结算的大招"正常收一遍（含扣大招点）。
	// 回合切换是直接 Destroy 角色的（见 ABlasterGameMode::RespawnAllPlayers），走不到
	// ServerEndUltimate —— 不补这一下，回合结束时手上还拿着飞刀的 Jett 就把大招白嫖了。
	// 收过尾的角色（ActiveUltimate 已清）调它什么也不做。
	void ServerSettleActiveUltimate();

protected:
	// virtual：英雄子类要在大招生效/结束的那一刻做自己的视觉开关
	//（AJettCharacter 用它显隐那套飞刀 + 播掏出动画）。
	// 覆盖时记得调 Super —— 基类这份现在是空的，但将来"开大/收招的音效、镜头抖动"会长在这里。
	UFUNCTION()
	virtual void OnRep_ActiveUltimate();

	// 服务器：大招收尾。按 ActiveUltimate 分派到具体收尾逻辑，并清掉生效状态。
	// **到期定时器和死亡拦截共用这一个出口**，所以清状态只写在这一个地方，两路都不会漏。
	// bFromDeath：这次收尾是"阵亡导致的作废"而不是正常到期 —— 具体收尾据此决定
	//            要不要做"活着才该做的事"（Jett 收刀后掏回枪：阵亡时不用掏，
	//             紧接着的死亡流程就会把枪全掉光，掏一下只会白播一次掏枪音效）。
	void ServerEndUltimate(bool bFromDeath = false);

	// Phoenix 收尾：传送回标记点 + 回满血甲 + 扣大招点。
	// 名字没有 "Server" 后缀是因为它同时被定时器和 ServerElim 调，都是服务器上下文。
	//
	// ⚠️ 扣点就在这里 —— **不在按下 X 的时候**（用户明确要求"回到原点之后再扣"）。
	// 阵亡回程（ServerElim 拦下来）和到期回程（定时器）共用 ServerEndUltimate 这一个出口，
	// 两条路都会走到这里，所以扣点只写在这一处，不会漏也不会重复。
	void ServerReturnToRunItBack();

	// Jett 收尾：销毁手上的飞刀（销毁而不是掉落），bReEquipWeapon 时把枪掏回来，
	// 并**扣掉大招点**（扣点时机在这里 —— 不在按下 X 的时候）。
	// 扔空 / 到期 / 阵亡三条路都汇到 ServerEndUltimate 再进这里，所以只扣一次。
	void ServerEndBladeStorm(bool bReEquipWeapon);

	// 下一帧的收招口（SetTimerForNextTick 要求无参成员函数）
	void OnBladeStormDepletedNextTick();

	/*
	 * 收招时"把手上的武器换回来"这一步，要等的那段表演还剩多久（秒）。0 = 不用等，立刻换。
	 *
	 * 只认近战槽里那把飞刀（大招生效期间它一定在槽里）—— 表演是**它**播的。
	 * 必须在 DestroyMeleeWeapon 之前调：那之后飞刀这个 actor 就没了，量不到动画进度。
	 */
	float ResolveBladeStormEquipDelay() const;

	// 上面那个延迟到点后的回调：把枪掏回来。无参，给 SetTimer 用。
	void OnBladeStormReEquipDelayElapsed();

protected:
	// 开大瞬间的位置/朝向。只在服务器读写 —— 客户端只知道"回到哪"的结果，由传送复制过去。
	UPROPERTY()
	FVector RunItBackLocation = FVector::ZeroVector;
	UPROPERTY()
	FRotator RunItBackRotation = FRotator::ZeroRotator;

	// 大招到期定时器（到期 = 大招自动结束）
	FTimerHandle UltimateTimer;

	// 飞刀扔空后的"下一帧收招"定时器（见 ServerRequestBladeStormEnd）。
	// 存在的意义只有一个：别在 AJettKnives::Fire() 的调用栈里销毁那把刀。
	FTimerHandle BladeStormDepleteTimer;

	// 收招时"等最后那一刀的表演播完再掏枪"的定时器（见 ResolveBladeStormEquipDelay）。
	FTimerHandle BladeStormReEquipTimer;

	// 定时器落地口。为什么要单独一个而不是直接把 ServerEndUltimate 塞进 SetTimer：
	// SetTimer 要求回调是**无参**成员函数，而 ServerEndUltimate 带一个 bFromDeath 参数
	//（带默认值也不行 —— SetTimer 是模版取地址，默认值不参与重载解析）。
	void OnUltimateTimerFired();

	// --- Phoenix 曲线球拐弯方向（客户端 → 服务器）---
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

	// 大招（X）。新建 IA_Ultimate 后在 BP_BlasterCharacter 里填上，再去 IMC 里绑到 X。
	// 留空 = 按 X 什么也不发生（不影响其它输入）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* UltAction;

	// 检视武器。和 UltAction 一样的做法：新建 IA_Inspect 后在 BP_BlasterCharacter 里填上，
	// 再去 IMC 里绑个键（Valorant 是 Y）。留空 = 按那个键什么也不发生。
	// 播的动画是当前武器的 FPInspectMontage（见 AWeapon），各武器蓝图自己挂。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* InspectAction;

	// Q / C 两个普通技能键（Valorant 的技能栏 C-Q-E-X 里的前两位）。
	// 和 UltAction 一样的做法：新建 IA_SkillQ / IA_SkillC 后在 BP_BlasterCharacter 里填上，
	// 再去 IM_BlasterChatcter 里绑到 Q / C。留空 = 按那个键什么也不发生。
	//
	// 为什么要独立两个 Action、不复用 DashAction（E）：
	//   E 那个是"通用技能键"，按角色配的技能**只有一个**（见 Dash() 里那串 if）；
	//   而 C/Q/E 在 Valorant 里是三个并存、各自独立充能的技能，共用一个键位就放不出三个。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* SkillQAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	UInputAction* SkillCAction;


	void SetAiming(bool bIsAiming);
	void SetEquippedWeapon(AWeapon* WeaponToEquip);

	/*
	 * 「手上那把武器换了」的通知口（换枪 / 收枪 / 掏刀都算）。
	 *
	 * **两条路径各调一次**，缺一边就会有一半机器收不到：
	 *   · 服务器：SetEquippedWeapon（UCombatComponent::EquipSlotWeapon 里）
	 *   · 客户端：OnRep_EquipWeapon（EquippedWeapon 复制到达 —— 客户端**不跑**上面那个函数）
	 * 两边都是"值已经设好"之后调，所以实现里读 GetEquippedWeapon() 拿到的是新武器。
	 *
	 * 目前唯一的用处：Jett 那套飞刀的显隐 —— 手上不是飞刀时那 5 把刀要跟着消失
	 *（见 AJettCharacter::SyncKnivesToAmmo）。放在这里而不是武器/组件里，是因为
	 * "手上是什么"这件事只有角色知道，而英雄子类正好能覆写。
	 *
	 * 覆盖时记得调 Super（基类这份是空的 —— 但以后要加"换枪音效/镜头"之类会长在这里）。
	 */
	virtual void OnEquippedWeaponChanged();

	void SetOverlappingWeapon(AWeapon* Weapon);

	// 由 AUltOrb 的重叠委托调用（服务器）。进入范围直接赋值；离开范围要走 ClearOverlappingOrb
	// 以免 A 球的 EndOverlap 把还没离开的 B 球引用清掉。
	void SetOverlappingOrb(AUltOrb* Orb);
	void ClearOverlappingOrb(AUltOrb* Orb);

	// 由 AUltOrb 在开始/结束蓄力时调用（服务器），只用于按 1/2 时的打断寻址。
	void SetChannelingOrb(AUltOrb* Orb);

	void SetCarriedAmmo(int32 Ammo);
	void SetCombatState(ECombatState CombatStateTemp);
	bool IsWeaponEquipped();
	bool IsAiming();
	// 收掉「本机正开着的狙击镜」。回合切换 / 丢 pawn 时调 —— 镜圈是视口上的 widget，
	// 不随角色销毁，必须显式收（详见 .cpp 里的实现注释）。
	void HideSniperScopeIfAiming();
	FORCEINLINE float GetAO_Yaw()const { return AO_Yaw; }
	FORCEINLINE float GetAO_Pitch()const { return AO_Pitch; }
	AWeapon* GetEquippedWeapon();
	FORCEINLINE class ULagCompensationComponent* GetLagCompensation() const { return LagCompensation; }
	FORCEINLINE ETurningInPlace GetTurningInPlace()const { return TurningInPlace; }
	FORCEINLINE float GetRootRotationYaw() const { return AO_Rotation.Yaw; }
	FORCEINLINE FVector GetHitTarget() const;
	// 「当前生效的相机」：
	//   本地玩家 → 第一人称相机（挂手模 Camera 骨骼）
	//   其他人     → 第三人称弹簧臂相机（观战看到的目标角色走这条，PC 里 SetViewTarget 后
	//                引擎调 CalcCamera 取第一个激活的相机，远端角色激活的正是 FollowCamera）
	FORCEINLINE UCameraComponent* GetFollowCamera() const
	{
		return (FPCamera && IsLocallyControlled()) ? FPCamera : FollowCamera;
	}
	FORCEINLINE bool IsElimmed() const { return bElimmed; }

	// --- Clove 暮蝶：封烟（公开给 ABlasterPlayerController）---
	// 详细说明见上方 Clove 段的注释。这几个必须 public：界面和 E 键都在 PC 上。
	//
	// 还剩几层充能（0..MaxCharges）。界面拿它决定"最多能选几个点"，
	// 服务器拿它决定"这一次齐放最多落几团"。
	int32 GetCloveSmokeCharges() const;

	// 还有没有充能。活着/死后都问这一个函数。
	bool IsCloveSmokeAvailable() const { return GetCloveSmokeCharges() > 0; }

	// 服务器权威落地（单个落点）。
	// 由本地 PC 调用。PC 拥有本角色，所以这条 Server RPC 能从客户端正确路由回服务器
	//（服务器上则直接执行 _Implementation）。
	UFUNCTION(Server, Reliable)
	void ServerPlaceCloveSmoke(const FVector& WorldLocation);

	/*
	 * 服务器权威落地（**一次放多个**）。
	 *
	 * 暮蝶有 2 层充能时可以在地图上连着点两个球，然后按一次右键把两个一起放出去 ——
	 * 这个是那条路。
	 *
	 * 为什么是**一条** RPC 而不是在 PC 上连着调两遍 ServerPlaceCloveSmoke：
	 *   · 两遍就是两次往返，中途任何一次被丢弃/乱序都会变成"只放了一个，另一个充能白扣"
	 *   · 更要紧的是充能：服务端要在**同一帧内**把 N 个落点一起对着"当前剩余充能"做一次判定。
	 *     拆成两条 RPC 时，第一条挂上冷却 GE 之后第二条才进来，判定依据已经变了 ——
	 *     单看每一步都合法，合起来却是"用 1 层充能放了 2 团烟"。
	 *
	 * 数组长度**不可信**（改过的客户端可以塞一百个坐标进来），所以服务端自己按充能数截断。
	 */
	UFUNCTION(Server, Reliable)
	void ServerPlaceCloveSmokes(const TArray<FVector>& WorldLocations);

	FORCEINLINE float GetHealth()const { return Health; }
	FORCEINLINE float GetMaxHealth()const { return MaxHealth; }
	void ResetHealth() { Health = MaxHealth; UpdateHUDHealth(); }

	// --- 护甲（Valorant 式护盾）---
	FORCEINLINE float GetArmor()const { return Armor; }
	FORCEINLINE float GetMaxArmor()const { return MaxArmor; }
	// 服务器：写护甲（购买 / 换回合恢复）。负数按 0，上限 MaxArmor。
	// 权威机不会收到自己的 RepNotify，所以这里自己刷一次 HUD。
	void SetArmor(float NewArmor);
	FORCEINLINE int32 GetCarriedAmmo() const{return CarriedAmmo;}
	ECombatState GetCombatState();
	FORCEINLINE UCombatComponent* GetCombatComponent()const { return Combat; }
	FORCEINLINE bool GetDisableGameplay() const { return bDisableGameplay; }
	FORCEINLINE UAnimMontage* GetReloadMontage() const {return ReloadMontage;}
	FORCEINLINE UStaticMeshComponent* GetAttachedGrenade() const {return AttachedGrenade;}
};
