#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/SoftObjectPtr.h"
#include "UltOrb.generated.h"

class UNiagaraSystem;
class USoundBase;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

/*
 * 大招球（Ultimate Orb）—— 地图上**预先摆放**的拾取物。站在球里按住 F 蓄 3 秒，
 * 完成后 +1 大招点，每回合重生。
 *
 * ===== 拾取方式：按住 F 的一段式蓄力（和拆包同一套手感，但只有一段）=====
 * · 蓄力时**自动收枪**（CombatState 切 ECS_Defusing、武器收回挂点），和拆包完全一致
 * · 移动（水平速度 > ChannelCancelSpeed）或离地 → 打断
 * · 松开 F → 打断
 * · 蓄力中按 1/2 掏枪 → 打断（由角色那边主动调 CancelChannel，见 BlasterCharacter::SelectPrimary）
 * · **打断 = 进度清零重来**。这是和拆包唯一的语义差别：拆包是两段式（拆过 50% 松手保留在 50），
 *   大招球是一段式，没有中途保存点。别把 Spike 那套 SegmentPercent 抄过来。
 *
 * ===== 为什么「可用性」用复制属性而不是事件广播 =====
 * GameState->OnRoundPhaseChanged 只在服务器广播（GameMode 里 Broadcast 的），客户端收不到 ——
 * 靠它驱动重生，客户端上的球会永远显示「已被吃掉」。改成复制 bAvailable：服务器改、客户端 OnRep，
 * 各端自己把球画出来/藏起来，天然一致。
 *
 * ===== 为什么重叠判定只在服务器做（和 AWeapon 一模一样的模式）=====
 * 客户端的球根组件**不开碰撞**，重叠事件只在服务器触发；服务器把「谁正站在球里」写进角色的
 * OverlappingOrb（Replicated）下发。客户端按 F 时读的就是这个复制过来的指针 —— 不靠本地物理，
 * 也不会出现「各端算出的重叠对象不一致」。判定和发奖全是服务器权威的。
 *
 * ===== 状态机 =====
 *   bAvailable=true  ──按住 F──▶  bIsChanneling=true, ChannelProgress 0→100
 *        ▲                              │
 *        │                        完成 / 打断
 *        │                              ▼
 *   回合重置(SetAvailable)         完成→SetAvailable(false) + 加 1 点
 *                                  打断→进度清零，球留在原地（bAvailable 仍为 true）
 *
 * 摆放：Content Browser 里拖 AUltOrb（或它的 BP 子类）进关卡，actor 原点就是球心，不贴地。
 */
UCLASS()
class BLASTER_API AUltOrb : public AActor
{
	GENERATED_BODY()

public:
	AUltOrb();
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器：把球设回「可拾取」。回合重置时由 GameMode 批量调用（见 ResetAllUltOrbs）。
	// 非权威机调用会被忽略 —— 可用性是服务器权威的复制属性。
	void SetAvailable(bool bNewAvailable);

	// 开始蓄力（服务器调用；客户端走角色的 ServerPickup RPC 进来）。
	// 返回是否真的开始蓄力了 —— 调用方（角色）靠它决定"没吃上就继续试试捡枪"。
	// 返回 false 的情形：球已被吃 / 球正在被别人蓄 / 拿不到 PlayerState / 这人**大招已满**。
	// 最后一条是有意的 —— Valorant 里满大的人捡不起球，球留给队友，不会被白白踩掉。
	bool StartChannel(class ABlasterCharacter* Character);

	// 打断蓄力并**清零进度**（松开 F / 移动 / 掏枪 / 球被吃掉）。球留在原地，可以重来。
	// bRestoreWeapon：打断后是否自动掏回最好的枪。默认 true（松手/移动打断，人变回持枪）。
	// 按 1/2 主动掏枪时传 false —— 调用方紧接着自己会切到指定槽位，这里再掏一次是多余的、
	// 而且会连切两把武器在画面上抖一下。
	void CancelChannel(bool bRestoreWeapon = true);

	UFUNCTION(BlueprintPure, Category = "UltOrb")
	bool IsAvailable() const { return bAvailable; }

	UFUNCTION(BlueprintPure, Category = "UltOrb")
	float GetChannelProgress() const { return ChannelProgress; }

	// 是否是「这个人」正在蓄力（角色那边用来判断按 1/2 要不要先打断）
	bool IsChannelingBy(const class ABlasterCharacter* Character) const;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;

	// --- Components ---
	// 根组件 = 拾取球。角色能从它中间穿过去（只做 Overlap 不做 Block），站进去才能按 F 蓄力。
	// 半径直接改 PickupRadius（OnConstruction 会同步到组件，编辑器里视口实时跟手）。
	// ⚠️ 这个球**只在服务器有碰撞**（客户端上始终 NoCollision），见类注释。
	UPROPERTY(VisibleAnywhere, Category = "UltOrb|Components")
	USphereComponent* PickupSphere;

	// 视觉载体（子件）。自转 + 上下浮动全在 Tick 里本地算，不复制任何变换。
	// ⚠️ 浮动会**覆盖**网格的相对位置，所以想给球加高度偏移请挪 actor 本身，不要挪网格。
	UPROPERTY(VisibleAnywhere, Category = "UltOrb|Components")
	UStaticMeshComponent* OrbMesh;

	/*
	 * 拾取半径（cm）：角色胶囊中心到这个距离内就能按 F 蓄力。
	 * 参考值 —— 球直径 50cm（默认缩放）+ 角色胶囊半径约 34cm，取 60 ≈ 擦着球边站着就能按。
	 * 这个值只影响服务器判定；客户端上的球本身不参与碰撞。
	 */
	UPROPERTY(EditAnywhere, Category = "UltOrb|Pickup")
	float PickupRadius = 60.f;

	// 蓄满一次要按住多久（秒）。Valorant 的球是瞬间吃的，3 秒是这边自己定的手感值。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|Pickup")
	float ChannelDuration = 3.f;

	// 蓄力中水平速度超过这个值就打断（站定才能吃）。
	// 和 Spike 的 MovementCancelSpeed 取同一个量级 —— 两者手感应该一致，改一个记得看另一个。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|Pickup")
	float ChannelCancelSpeed = 50.f;

	// 蓄满一次加多少大招点。Valorant 是 1 点，保留成属性是为了以后做「双倍球」之类的活动。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|Pickup")
	int32 UltPointValue = 1;

	// 球体网格（软引用，PostInitializeComponents 里懒加载）。
	// 默认指向引擎自带球体 —— 保证没配任何资产的裸 BP/裸 Actor 也能看见球、能调。
	// 换成 Valorant 的 ult orb 模型时直接改 BP Details；顺手把 OrbMeshScale 一起调
	// （引擎球的直径是 100cm，Valorant 的球大约 50cm，所以默认缩放 0.5）。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|Visual")
	TSoftObjectPtr<UStaticMesh> OrbMeshAsset;

	UPROPERTY(EditAnywhere, Category = "UltOrb|Visual")
	float OrbMeshScale = 0.5f;

	// 自转速度（度/秒）。用世界时间驱动（不是逐帧累加），保证后进玩家看到的相位和一直在场的玩家一致。
	UPROPERTY(EditAnywhere, Category = "UltOrb|Visual")
	float SpinSpeed = 90.f;

	// 上下浮动：振幅（cm）与频率（Hz，即每秒几个来回）
	UPROPERTY(EditAnywhere, Category = "UltOrb|Visual")
	float BobHeight = 8.f;

	UPROPERTY(EditAnywhere, Category = "UltOrb|Visual")
	float BobSpeed = 0.5f;

	// --- FX：都留空则什么都不播（不报错），素材在 BP Details 里填 ---
	// 蓄满瞬间：各端在球原位播一次（走 NetMulticast，所以正在吃的人自己也能看到/听到）。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|FX")
	UNiagaraSystem* PickupEffect;

	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|FX")
	USoundBase* PickupSound;

	// 回合重生瞬间的音效。留空则不响 —— 几个球一起重生时可能有点吵，建议只给一个轻提示音或干脆不填。
	UPROPERTY(EditDefaultsOnly, Category = "UltOrb|FX")
	USoundBase* RespawnSound;

private:
	// 是否还可拾取（服务器权威、复制）。true = 球在场上、能蓄力；false = 已被吃掉、等下次回合重置。
	// 构造默认 true，所以开局第一回合不用特殊处理 —— 球一出生就是可拾取的。
	UPROPERTY(ReplicatedUsing = OnRep_bAvailable)
	bool bAvailable = true;

	UFUNCTION()
	void OnRep_bAvailable();

	// 蓄力进度 0-100（服务器权威）。蓄力者自己的进度条是走 ClientUpdateSpikeProgress 直推的，
	// **不依赖**这两个值。
	//
	// 那为什么还是复制？因为球是数量很少的关卡 Actor（一场几个），代价是一个 float + 一个 bool，
	// 换来的是"任何一台机器都能读到这颗球此刻的真实状态"。排查"按了 F 进度条没出来"这类问题时，
	// 能在客户端直接看球的 ChannelProgress/bIsChanneling 比只能看服务器日志省事得多。
	// 哪天要加"这颗球正在被别人吃"的提示，数据也已经在了。
	UPROPERTY(Replicated)
	float ChannelProgress = 0.f;

	// 是否正在被蓄力。客户端不参与计时，只读来做表现/排查。
	UPROPERTY(Replicated)
	bool bIsChanneling = false;

	// 当前蓄力者 / 他的控制器。只在服务器有值，不复制 —— 两台机器不需要知道"谁在吃"
	// （进度条只推给蓄力者本人，见 PushChannelUI）。
	UPROPERTY()
	class ABlasterCharacter* CurrentChanneller = nullptr;

	UPROPERTY()
	class ABlasterPlayerController* ChannellerController = nullptr;

	// --- 重叠：把「谁站在球里」写进角色的 OverlappingOrb（服务器→客户端复制），不在本地发奖 ---
	UFUNCTION()
	void OnPickupBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnPickupEndOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

	// 服务器：蓄满结算 —— 发点、消耗球、广播特效。只有这一个地方会真正"吃掉"球。
	void CompleteChannel();

	// 各端：按 bAvailable 应用可见性与（服务器侧的）碰撞开关。幂等，BeginPlay / OnRep / SetAvailable 都调。
	void ApplyAvailabilityVisual();

	// 蓄力者自己的进度条：非负 = 显示并设百分比，负数 = 隐藏（和拆包共用 HUD 上那一条）
	void PushChannelUI(float Progress);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayPickupFX();

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayRespawnFX();
};
