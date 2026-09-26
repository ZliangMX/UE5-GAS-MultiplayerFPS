#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "BlasterPlayerController.generated.h"

class UBuyMenu;
class AWeapon;
class UMinimapComponent;
class USoundBase;
class ABlasterCharacter;
class UWeaponKillIconSet;
class UMaterialInterface;

// 选英雄的 Agent 枚举已移到 BlasterTypes/Agent.h（EBlasterAgent）。

UCLASS()
class BLASTER_API ABlasterPlayerController : public APlayerController
{
	GENERATED_BODY()
public:
	ABlasterPlayerController();

	// 小地图逻辑全部封装在挂载在 PC 上的组件里
	UMinimapComponent* GetMinimapComponent() const { return MinimapComponent; }

	// 血量和护甲一起推（同一条链路，避免两处各刷一半导致 HUD 不一致）
	void SetHUDHealth(float Health, float MaxHealth, float Armor, float MaxArmor);

	// 护甲两档的档位/价格，给购买菜单显示用（菜单不自己写死数字，和服务器扣钱用同一张表）。
	// 数值本体在下面 Buy|Armor 那几个 EditDefaultsOnly 里调。
	FORCEINLINE int32 GetLightArmorAmount() const { return LightArmorAmount; }
	FORCEINLINE int32 GetLightArmorCost() const { return LightArmorCost; }
	FORCEINLINE int32 GetHeavyArmorAmount() const { return HeavyArmorAmount; }
	FORCEINLINE int32 GetHeavyArmorCost() const { return HeavyArmorCost; }
	void SetHUDScore(float Score);
	void SetHUDDefeats(int32 Defeats);
	void SetHUDWeaponAmmo(int32 Ammo);
	void SetHUDCarriedAmmo(int32 Ammo);
	void SetHUDMatchCountDown(float CountDownTime);
	void SetHUDAnnouncementCountdown(float CountDownTime);
	void SetHUDTime();

	// --- 射击反馈 ---
	// 命中反馈：命中不同队敌人时准星闪现命中标记（本机直接调 / 服务器 RPC 转发）
	void ShowHitMarker();
	UFUNCTION(Client, Reliable)
	void ClientShowHitMarker();

	// 伤害数字：受伤时在伤害来源方向显示漂浮数字（只发给受伤玩家的客户端）
	void ShowDamageNumber(float Damage, const FVector& DamageOrigin);
	UFUNCTION(Client, Reliable)
	void ClientShowDamageNumber(float Damage, const FVector& DamageOrigin);

	// 受击方向指示：被击中时屏幕边缘显示红色弧形指向伤害来源（只发给受伤玩家的客户端）
	void ShowDamageDirection(const FVector& DamageOrigin);
	UFUNCTION(Client, Reliable)
	void ClientShowDamageDirection(const FVector& DamageOrigin);

	// 击杀确认标记：屏幕中下方弹出，RoundKills = 本回合已击杀数（决定造型档位，1..6）
	// KillIconSet = 击杀时手上那把枪配的图标集（GameMode 那边取好传过来），可为空
	void ShowKillMarker(int32 RoundKills, UWeaponKillIconSet* KillIconSet);
	UFUNCTION(Client, Reliable)
	void ClientShowKillMarker(int32 RoundKills, UWeaponKillIconSet* KillIconSet);
	void HideKillMarker();

	// 击杀确认音效（Valorant 式"叮"）：只给击杀者客户端播放，爆头用更高音的头杀音。
	// KillSet   = 打死人时手上那把枪的击杀反馈资产，可为空 —— 它里面 KillIndex 那一段
	//             优先于下面两条，没配那一段才轮到 KillConfirmSound / HeadshotKillSound。
	// KillIndex = 本杀是第几杀（1 起），由武器在**服务器**上算好传进来（见 AWeapon::ComputeThisKillIndex）。
	void PlayKillSound(bool bHeadshot, UWeaponKillIconSet* KillSet, int32 KillIndex);
	UFUNCTION(Client, Reliable)
	void ClientPlayKillSound(bool bHeadshot, UWeaponKillIconSet* KillSet, int32 KillIndex);
	// --- 射击反馈 end ---

	// --- 击杀确认音效资产（EditDefaultsOnly，PC 蓝图里可换）---
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	USoundBase* KillConfirmSound;
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	USoundBase* HeadshotKillSound;
	// --- end ---

	// Team / Round HUD
	void SetHUDTeam(ETeam Team);
	void SetHUDTeamScore(int32 ScoreA, int32 ScoreB);
	void SetHUDAliveCount(int32 AliveA, int32 AliveB);
	void SetHUDSpikeStatus(const FString& Status, float Progress = -1.f);

	// 每帧推送的进度/倒计时，丢一帧无所谓，Unreliable 避免积压可靠队列
	UFUNCTION(Client, Unreliable)
	void ClientUpdateSpikeProgress(const FString& Status, float Progress);
	void SetHUDRoundResult(const FString& ResultText);
	void SetHUDTeamSwap(bool bShowSwap);

	UFUNCTION(Server, Reliable)
	void ServerToggleReady();

	// 当前英雄。单一数据源 = PlayerState 上复制的 Agent（Lobby 选的 / 服务器随机补的）。
	// 服务器读它决定出生角色类；客户端用它驱动 UI。无 PS 时返回 None。
	EBlasterAgent GetAgent() const;

	// Lobby 选英雄（客户端 → 服务器，落到 PlayerState 复制给所有人）
	UFUNCTION(Server, Reliable)
	void ServerSelectAgent(EBlasterAgent NewAgent);

	// F9 调试：Jett → Sage → Phoenix 循环切换本机角色（走同一份 PlayerState.Agent，重生即时生效）
	UFUNCTION(Server, Reliable)
	void ServerToggleAgent();

	UFUNCTION(Server, Reliable)
	void ServerStartGame();

	UFUNCTION(Server, Reliable)
	void ServerSwitchTeam(ETeam TargetTeam);

	// --- 经济系统 / 购买菜单 ---
	void ToggleBuyMenu();
	void AddBuyMenu();
	void CloseBuyMenu();

	// --- Clove 暮蝶：封烟选点界面 ---
	//
	// 为什么状态机在 PC 而不是角色（这是整块实现里最反直觉的一点）：
	// 阵亡时 ABlasterCharacter::MulticastElim 会 DisableInput(PC)，角色的输入组件被
	// 摘出输入栈，角色的 E 再也收不到 —— 而「死后放烟」正是暮蝶的招牌。
	// PC 的输入组件不受影响（同 APlayerController::BuildInputStack 的注释，Tab 记分板
	// 也是靠这一点）。加上开地图要抢鼠标光标、切输入模式，本来就是 PC 的活
	//（和 BuyMenu 同一套 bShowMouseCursor / SetInputMode）。
	//
	// 所以：E 的入口有两条 —— 活着时角色 Dash() 转发过来，阵亡时由本类自己绑的
	// E 键直接调（见 OnSmokeKeyPressed）。两条最终都进 SmokeMapKeyPressed()。
	//
	// 交互（用户定义）：**左键选点、右键齐放、E 只开关界面**。
	//   没开 + 还有充能  → 开图（E）
	//   开着             → 关图（E），已选的落点一起丢掉
	//   左键（圆内）     → **追加**一个点，显示一个圈
	//   右键 + 已选点    → 把已选的点**一次性全部**放出去（一条服务器 RPC）+ 关图
	//   右键 + 没选点    → 什么也不做（右键不是"退出"，退出只有 E）
	// 所以 E 不再兼"确认"：按 E 关图 = 放弃这次选点。
	//
	// 能选几个点 = 手上还剩几层充能（暮蝶默认 2 层 → 最多选 2 个球，一次右键一起放）。
	// 只有 1 层时行为就是以前那样：选一个、右键放一个。
	void SmokeMapKeyPressed();
	bool IsSmokeMapOpen() const { return bSmokeMapOpen; }

	// 右键齐放。由 UCloveSmokeMapWidget 回调。
	void SmokeMapDeployPressed();

	// 由 UCloveSmokeMapWidget 在左键时回调。只往数组里追加，不碰服务器 ——
	// 真正的落地是玩家点右键时那一次 ServerPlaceCloveSmokes。
	//
	// 满了（已选数 == 剩余充能）就**直接忽略这次点击**，不覆盖已选的点：
	// 覆盖要玩家自己猜"顶掉了哪一个"，而且界面上那些圈和右键实际放出去的点会不一致。
	// 想重选就按 E 关图再开（关图会把已选的点清空）。
	void SetSmokePendingLocation(const FVector& WorldLocation);

	// 这次齐放到底还剩几层充能用（也就是最多还能再选几个点）。
	// 界面拿它画 "(1/2)" 这种计数，也拿它决定满了就不再收点击。
	int32 GetSmokePendingCapacity() const;

	bool HasSmokePendingLocation() const { return SmokePendingLocations.Num() > 0; }
	const TArray<FVector>& GetSmokePendingLocations() const { return SmokePendingLocations; }

	// --- Tab 记分板（敌我信息叠加层，UMG Widget，不是每帧 Canvas 绘制）---
	// 按住 Tab 显示、松开隐藏。键绑在 PC 自己的 InputComponent 上（不是角色）：
	// 阵亡后角色那套输入组件会被 DisableInput 从栈里摘掉，PC 这个仍在栈内
	// （见 APlayerController::BuildInputStack —— InputEnabled() 时压入自己的组件），
	// 所以躺在地上等回合结束时也能看记分板。
	void ShowScoreboard();
	void HideScoreboard();

	// --- 观战（死亡后观察存活队友）---
	// 本机角色被击杀时由角色 OnRep_Elim 调：等 SpectateDelay 秒的死亡镜头后进观战。
	// 相机：本机 SetViewTarget 到同队存活角色（用其复制的 FollowCamera），切换即时生效。
	// 朝向：Host/权威机天然取被观察者的真实控制器旋转；网络客户端靠
	// ServerSetSpectateTarget 让服务器复制其真实瞄准进来（详见 cpp 注释）。
	void HandleLocalPlayerEliminated();
	// 是否正在观战（BlasterHUD 每帧据此画底部观战栏）
	bool IsSpectating() const { return bSpectating; }
	// 观战栏文案：Main = 玩家名·英雄，OutColor = 英雄强调色；Sub = 操作提示。无目标返回 false。
	bool GetSpectateBarInfo(FString& OutMain, FString& OutSub, FLinearColor& OutColor) const;

	// 观战中返回当前被观察的存活队友（技能条/白闪据此显示他的状态）；非观战返回 nullptr。
	ABlasterCharacter* GetSpectateTargetCharacter() const;

	// 当前是否在 Lobby（选人）地图：按地图名实时判断（跨 ServerTravel 往返 Lobby 也准确）。
	// 技能条隐藏 / 技能键禁用 都用它。
	bool IsInLobby() const;

	// 通知服务器把本 PC（死者）的相机目标切到 Target，让引擎每帧把被观察者的真实瞄准
	// 复制成 TargetViewRotation（OwnerOnly）发给本客户端，远端代理的弹簧臂才能跟着转。
	// Target==nullptr 表示停止观战（服务器切回我自己的 pawn）。仅网络客户端调用。
	UFUNCTION(Server, Reliable)
	void ServerSetSpectateTarget(ABlasterCharacter* Target);

	UFUNCTION(Server, Reliable)
	void ServerBuyWeapon(TSubclassOf<class AWeapon> WeaponClass);

	// 买护甲：客户端只报「买哪一档」（25=轻甲 / 50=重甲）。
	// **价格不带过来** —— 服务器自己查 Buy|Armor 那张表扣钱（客户端报的价格一概不采信，
	// 改过的客户端可以直接报 0）。阶段、钱、是否已经够硬也都在服务器复核。
	UFUNCTION(Server, Reliable)
	void ServerBuyArmor(int32 ArmorAmount);

	virtual void OnPossess(APawn* InPawn) override;
	// 换 pawn / 回合切换 Destroy 角色时都会走这里 —— 暮蝶的地图界面在这一并关掉。
	// 不关的话鼠标光标会永远留在屏幕上、输入模式卡在 GameAndUI，且旧 widget 泄漏。
	virtual void OnUnPossess() override;
	virtual void Tick(float DeltaTime) override;
	virtual void SetupInputComponent() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual float GetServerTime();
	// 当前阶段剩余秒数（顶部比分栏的倒计时用）。分支和 SetHUDTime 里那套完全一致，
	// 抽出来只是为了别在 HUD 里再抄一份；安包后旧回合倒计时失效，这里返回 0。
	float GetPhaseTimeLeft();
	virtual void ReceivedPlayer() override;
	void OnMatchStateSet(FName State);

	// 刚结束的这个回合，赢的是不是本地玩家这一队。PostRound / GameOver 的大字要靠它决定出
	// WON 还是 LOST（公告栏底色也跟着变）。
	bool DidLocalTeamWinRound() const;

	// 本回合赢家。**复制在 PC 自己身上**，服务端在 OnMatchStateSet 里写（见那个函数）。
	//
	// 为什么不直接读 GameState 上那份：客户端的 HandlePostRound 是**本 PC** 的 MatchState
	// RepNotify 触发的，而 MatchState 在 PC 上、GS->RoundWinnerTeam 在 GameState 上 —— 两个
	// Actor 各走各的通道，同一帧里谁先落地没人保证。实测网络客户端常常是 PC 这包先到，
	// 于是读到的赢家还是本回合开始时清掉的 ET_None，一路兜底兜成 LOST（杀光对面也判输）。
	// 和 MatchState 放进同一个 Actor 就没这个问题：同一个 bunch 里的属性是**全部应用完之后**
	// 才统一调 RepNotify 的（FObjectReplicator::PostReceivedBunch → CallRepNotifies，
	// DataReplication.cpp:1497），读它一定已经是最新的。
	UPROPERTY(Replicated)
	ETeam RoundWinnerTeam = ETeam::ET_None;

	// 当前阶段。HUD 自己判阶段用这个，别去够 GameMode（客户端上没有 GameMode）。
	// 返回的是 MatchState 命名空间里那几个常量（MatchState::BuyPhase 等）。
	FName GetMatchStateName() const { return MatchState; }

	void HandleMatchHasStarted();
	void HandleBuyPhase();
	void HandlePostRound();
	void HandleGameOver();

	UFUNCTION(Client, Reliable)
	void ClientSyncPhaseTime(float NewLevelStartingTime);

protected:
	virtual void BeginPlay() override;


	UFUNCTION(Server,Reliable)
	void ServerRequestServerTime(float TimeOfClientRequest);

	UFUNCTION(Client,Reliable)
	void ClientReportServerTime(float TimeOfClientRequest,float TimeServerReceiveClientRequest);

	float ClientServerDelta = 0.f;

	UPROPERTY(EditAnywhere,Category = Time)
	float TimeSyncFrequency = 5.f;

	float TimeSyncRunningTime = 0.f;
	void CheckTimeSync(float DeltaTime);

	UFUNCTION(Server,Reliable)
	void ServerCheckMatchState();

	UFUNCTION(Client,Reliable)
	void ClientJoinMidGame(FName StateOfMatch, float InWarmupDuration, float BuyPhase, float Match, float StartingTime, float PostRound, float GameOver);
private:
	class ABlasterHUD* BlasterHUD;

	class ABlasterGameMode* BlasterGameMode;
	float WarmupDuration = 5.f;
	float MatchTime = 0.f;
	float WarmupTime = 0.f;
	float CooldownTime = 0.f;
	float GameOverTime = 0.f;
	float LevelStartingTime = 0.f;
	uint32 CountdownInt = 0;

	UPROPERTY(ReplicatedUsing=OnRep_MatchState)
	FName MatchState;

	UFUNCTION()
	void OnRep_MatchState();

	UPROPERTY()
	class UCharacterOverlay* CharacterOverlay;

	void PollInit();
	void LobbyPollInit();
	bool bInitializeCharacterOverlay = false;
	bool bLobbyCheckDone = false;
	bool bInLobby = false;

	// --- 玩家 ID（启动菜单里填的那个） ---
	//
	// 链路：菜单（插件 UMenu）把 ID 写进 UMultiplayerSessionsSubsystem::PlayerId
	//   → 换图后本机 PC 每帧检查一次，PlayerState 一到就 Server RPC 推上去
	//   → 服务器 APlayerState::SetPlayerName（这个名字会自动复制给所有人）
	//   → 选人界面 / Tab 计分板 / 击杀提示 / 观战栏全都读 PlayerState 的名字，一次全中。
	//
	// 为什么非得绕服务器一圈：名字存在 PlayerState 上，而 PlayerState 只有服务器能改
	// （客户端改了会被复制覆盖回去）。而且服务器上**代表远端玩家的那个 PC** 读不到对方的 ID
	// —— 那份 ID 在对方的进程里，所以只有本机 PC 自己推才作数。
	//
	// 每张地图的 PC 都是新对象（ServerTravel/ClientTravel 非无缝旅行会重建全部 Actor），
	// 所以 bPlayerIdPushed 也跟着重置 —— 换图会自动重推一次，不用额外处理。
	void ApplyPlayerIdIfNeeded();

	UFUNCTION(Server, Reliable)
	void ServerSetPlayerId(const FString& NewPlayerId);

	bool bPlayerIdPushed = false;

	// 安包后旧回合倒计时被隐藏（由 spike 爆炸倒计时接管），
	// 进入 PostRound 再恢复。用于避免每帧重复 SetVisibility。
	bool bMatchCountdownHidden = false;

	UPROPERTY(EditDefaultsOnly, Category = "Lobby")
	TSubclassOf<class ULobbyOverlay> LobbyOverlayClass;

	UPROPERTY()
	class ULobbyOverlay* LobbyOverlay;

	UPROPERTY(EditDefaultsOnly, Category = "Buy")
	TSubclassOf<class UBuyMenu> BuyMenuClass;

	// 护甲两档（Valorant：轻甲 25 点 400、重甲 50 点 1000）。
	// HeavyArmorAmount 应该 == ABlasterCharacter::MaxArmor，否则封顶会把差价吃掉。
	// 服务器按这里的表扣钱，客户端只是拿它显示价格文本 —— 两边不一致以服务器为准。
	UPROPERTY(EditDefaultsOnly, Category = "Buy|Armor")
	int32 LightArmorAmount = 25;

	UPROPERTY(EditDefaultsOnly, Category = "Buy|Armor")
	int32 LightArmorCost = 400;

	UPROPERTY(EditDefaultsOnly, Category = "Buy|Armor")
	int32 HeavyArmorAmount = 50;

	UPROPERTY(EditDefaultsOnly, Category = "Buy|Armor")
	int32 HeavyArmorCost = 1000;

	UPROPERTY()
	class UBuyMenu* BuyMenu;

	// --- Clove 暮蝶：封烟选点界面（私有实现）---
	void OpenSmokeMap();
	void CloseSmokeMap();

	// 清空已选落点。开图（重新选）和关图（放弃）都要清 —— 两处都调它，别各写一遍。
	void ClearSmokePendingLocations();

	// PC 自己绑的 E 键回调。**只在角色收不到输入时（阵亡）才干活** ——
	// 活着的时候角色的 Dash() 已经转发过同一次按键，这里再走一遍会把刚开的地图立刻关掉。
	void OnSmokeKeyPressed();

	UPROPERTY(EditDefaultsOnly, Category = "Abilities|CloveSmoke")
	TSubclassOf<class UCloveSmokeMapWidget> SmokeMapWidgetClass;

	UPROPERTY()
	class UCloveSmokeMapWidget* SmokeMap;

	// 地图界面是否开着（纯本地状态，不复制 —— 每个客户端各开各的）
	bool bSmokeMapOpen = false;

	// 开图时拥有本 PC 的那个 pawn。Tick 里拿它和当前 pawn 比对，用来发现
	// 「pawn 被换掉/销毁了但没走 OnUnPossess」（客户端换 pawn 就是这条路径）。
	TWeakObjectPtr<APawn> SmokeMapOwnerPawn;

	// 已选中的封烟落点，按点击先后排列（世界坐标，Z 是服务器最终打射线之前的临时值）。
	// 用数组而不是单个 FVector：暮蝶有 2 层充能时可以连着点两个球、一次右键齐放。
	// 元素个数天然就是"选了几个"，所以不再需要单独一个 bool。
	TArray<FVector> SmokePendingLocations;

	// Tab 记分板：留空则用原生 UScoreboardWidget（C++ 默认布局），可指派 WBP 换皮
	UPROPERTY(EditDefaultsOnly, Category = "Scoreboard")
	TSubclassOf<class UScoreboardWidget> ScoreboardClass;

	UPROPERTY()
	class UScoreboardWidget* Scoreboard;

	float HUDHealth;
	float HUDMaxHealth;
	float HUDArmor;
	float HUDMaxArmor;
	float HUDScore;
	int32 HUDDefeats;

	UPROPERTY()
	class UMinimapComponent* MinimapComponent;

	// --- 敌方勾边（纯本地渲染，只有本机视角看得到）---
	//
	// 原理：给敌方角色的身体网格挂**覆层材质**（UMeshComponent::OverlayMaterial，
	// 引擎会给该网格多跑一遍绘制，不带 Replicated 标记）。材质本身是一支反壳描边：
	// Masked + Unlit + 双面，Opacity Mask 接 OneMinus(TwoSideSign) 只留背面 —— 背面
	// 只在轮廓外侧露出来，看起来就是一圈边。"谁是敌人"由本机 PC 自己算（读本机
	// PlayerState 的队伍），所以各端看到的是各自视角里的敌人。
	//
	// 0.2s 节流遍历一次：进视野/阵亡/换队不需要逐帧精度；SetOverlayMaterial 内部
	// 值没变就直接返回（MeshComponent.cpp:290），所以重复调用不会重建渲染状态。
	UPROPERTY(EditDefaultsOnly, Category = "Highlight")
	class UMaterialInterface* EnemyOutlineMaterial;

	// 刷新间隔（秒）
	UPROPERTY(EditDefaultsOnly, Category = "Highlight")
	float EnemyOutlineRefreshInterval = 0.2f;
	float EnemyOutlineRefreshTimer = 0.f;

	// 每帧（节流后）重算：敌人挂覆层材质，其他人一律清掉
	void UpdateEnemyOutlines(float DeltaTime);

	// --- 观战（私有实现）---
	void EnterSpectate();
	void StopSpectate();
	void SpectateTick(float DeltaTime);
	void SpectateStartTimerFinished();
	void RefreshSpectateCandidates();
	void SelectSpectateTarget(int32 Index, bool bBlend);
	void AutoAdvanceSpectateTarget();
	void CycleSpectateTarget(int32 Delta);
	int32 FindSpectateIndex(const ABlasterCharacter* Target) const;
	bool IsValidSpectateTarget(const ABlasterCharacter* Target) const;

	// 死亡后等多久再进观战（秒）：留出看清自己怎么死的/溶解的时间
	float SpectateDelay = 1.5f;

	bool bSpectating = false;
	// 已阵亡、正在等 SpectateDelay 的死亡镜头
	bool bSpectatePending = false;
	FTimerHandle SpectateStartTimer;

	// 当前被观察目标 + 存活同队候选（每次刷新按 PlayerId 排序，顺序稳定）
	TWeakObjectPtr<ABlasterCharacter> SpectateTarget;
	TArray<TWeakObjectPtr<ABlasterCharacter>> SpectateCandidates;
};
