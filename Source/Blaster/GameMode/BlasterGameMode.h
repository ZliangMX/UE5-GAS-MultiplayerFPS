#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "Blaster/BlasterTypes/Team.h"
#include "BlasterGameMode.generated.h"

class AWeapon;

namespace MatchState
{
	extern BLASTER_API const FName Cooldown;
	extern BLASTER_API const FName BuyPhase;
	extern BLASTER_API const FName PreRound;   // legacy alias, kept for compat
	extern BLASTER_API const FName PostRound;
	extern BLASTER_API const FName GameOver;
}

UCLASS()
class BLASTER_API ABlasterGameMode : public AGameMode
{
	GENERATED_BODY()
public:
	ABlasterGameMode();
	virtual void Tick(float DeltaTime) override;
	virtual void OnPostLogin(AController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	void PlayerEliminated(class ABlasterCharacter* ElimmedCharacter,
		class ABlasterPlayerController* VictimController,
		ABlasterPlayerController* AttackerController);

	void RequestRespawn(class ACharacter* ElimmedCharacter, AController* ElimmedController);

	// --- 测试机器人（靶子，测试击杀标记用）---
	// 摆在场内当靶子：无 Controller/PlayerState，命中/击杀按测试规则算
	//（任意一方击杀都算成玩家自己的击杀 → RoundKills+1 → 击杀标记弹档位），
	// 被击杀后自动复活，不参与回合胜负/正式计分。
	void HandleTestBotEliminated(class ABlasterCharacter* Bot, class ABlasterPlayerController* AttackerController);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestBots")
	int32 NumTestBots = 4;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestBots")
	float TestBotRespawnDelay = 3.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestBots")
	TSubclassOf<class ABlasterCharacter> TestBotCharacterClass;

	// --- F9 调试切换（Jett → Sage → Phoenix → Clove 循环）---
	// 本机按 F9 把角色在四个之间循环：服务器按 PC 上的 EBlasterAgent 选择出生角色。
	// 四个英雄各有自己的角色 BP，父类都是对应的英雄 C++ 类（见 Character/Agents/）：
	// Jett→BP_JettCharacter、Sage→BP_SageCharacter、Phoenix→BP_PhoenixCharacter、
	// Clove→BP_CloveCharacter。**四个都要显式填**，任何一个留空就落到默认角色类上
	//（DefaultPawnClass = BP_BlasterCharacter，一个不带任何英雄技能的基类实例）。
	//
	// ⚠️ 加了新英雄只改 C++ 是不够的 —— 这里每个 UPROPERTY 的实际值存在
	//    GM_BlasterGameMode.uasset 里，必须在编辑器里把它填成对应的 BP。漏填的表现是
	//    「选了 Clove，出生却是 Jett」，不报错。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TSubclassOf<class ABlasterCharacter> JettCharacterClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TSubclassOf<class ABlasterCharacter> SageCharacterClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TSubclassOf<class ABlasterCharacter> PhoenixCharacterClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TSubclassOf<class ABlasterCharacter> CloveCharacterClass;

	// 服务器选择出生角色：按 PC 的 AgentSelection 返回对应角色类，否则用默认
	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;

	// ==================== 出生点（按攻守分开）====================
	/*
	 * 出生点按队伍挑：**TeamA = 进攻、TeamB = 防守**（和 AssignSpikeToRandomAttacker /
	 * CheckRoundEnd 一致），各自只从打了对应 tag 的 PlayerStart 里选。
	 *
	 * 引擎默认的 ChoosePlayerStart 是从**全场** PlayerStart 里随便挑一个，
	 * 所以不覆写这个函数的话，防守方会被扔进攻方的出生广场（Lotus 里就是 x≈10228 的人
	 * 出生在 x≈1318 的位置）。
	 *
	 * 挑不到带 tag 的点就退回引擎默认行为（并吼一声）—— 这样别的关卡/没打 tag 的关卡照常用。
	 */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	/*
	 * Lotus 里那 10 个出生点是用 **PlayerStartTag** 区分的（细节面板里那个 "Player Start Tag"），
	 * 不是 Actor 的 Tags 数组（那个是空的，已 headless 核实过）。
	 * 不过下面这个匹配同时也认 Actor Tags —— 万一以后改成用标签面板打 tag 也不用回来改代码。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning")
	FName AttackSpawnTag = FName(TEXT("Attack"));

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning")
	FName DefendSpawnTag = FName(TEXT("Defend"));

	// 出生点被占的判定半径（厘米，只算水平距离）：这么近已经有别的角色站着，就换一个点。
	// Lotus 的防守方 5 个点彼此只差 130cm 左右，所以别调太大，不然会一直判成"全占着"。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawning")
	float MinSpawnSeparation = 120.f;

	// Spike 拆包完成/爆炸时调用，结束当前回合
	void CheckRoundEnd();

	// 对局结束（GameOver 倒计时走完）→ 服务器带所有人回 MenuMapPath（默认启动菜单图）
	void TravelToMenuMap();

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing")
	float BuyPhaseTime = 30.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing")
	float RoundTime = 100.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing")
	float PostRoundTime = 5.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing")
	float WarmupDuration = 5.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timing")
	float GameOverTime = 15.f;

	// --- 关卡流程：一局打完之后去哪 ---
	//
	// 一方先到 TotalRoundsToWin（GameState 里，13 分）→ MatchState=GameOver →
	// 上面的 GameOverTime 走完 → 服务器带着所有人 ServerTravel 到这张图（默认回启动菜单）。
	//
	// 为什么不是 RestartGame()：AGameMode::RestartGame 是 ServerTravel("?Restart")，
	// **只是把当前这张图重开一局**，永远回不到菜单。
	//
	// 留空 = 回到旧行为（原地重开一局），想快速反复测对局就把这行清掉。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Levels")
	FString MenuMapPath = TEXT("/Game/Maps/GameStartupMap");

	// 回菜单时走不走无缝旅行。
	//
	// **默认 false（硬旅行）**：回菜单就是"这局结束了、从头来"，让世界彻底重建 ——
	// 分数/回合/队伍/spike 全部归零，客户端重新连一次。
	// 无缝的话 PlayerState 会带过去、菜单图那套（引擎默认 GameMode）还得处理这些残留，不值。
	//
	// 玩家 ID 不受影响：它在 BlasterGameInstance 里，GameInstance 跨旅行不重建。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Levels")
	bool bSeamlessTravelToMenu = false;

	// 出生自带的副武器（手枪）；上回合有副武器则继承它
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapons")
	TSubclassOf<AWeapon> DefaultPistolClass;

	/*
	 * 出生自带的**近战武器**（3 号槽的刀）—— 角色天生就有、不能丢、不能被捡，
	 * 所以它和手枪不一样：**不继承、也不买**，每次重生都原地发一把新的
	 *（角色销毁时旧的那把会一起销毁，见 ABlasterCharacter::Destroyed）。
	 *
	 * 默认指到 /Game/Blueprints/Weapon/BP_Melee（要在 GameMode 蓝图里覆盖也行）。
	 * 注意它是**蒙太奇的宿主**：刀的挥砍动画挂在 BP_Melee 的 Melee|LightCombo / HeavyAttack 上，
	 * 这里配空的话玩家手上就没有刀（按 3 什么都不发生）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapons")
	TSubclassOf<AWeapon> DefaultMeleeClass;

	// --- 经济系统 ---
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy")
	int32 KillReward = 300;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy")
	int32 WinRoundReward = 3000;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Economy")
	int32 LossRoundReward = 1900;

	float LevelStartingTime = 0.f;

	FORCEINLINE float GetCountDownTime() const { return CountDownTime; }

	class ASpike* GetCurrentSpike() const { return CurrentSpike; }

protected:
	virtual void BeginPlay() override;
	virtual void OnMatchStateSet() override;

private:
	float CountDownTime = 0.f;

	// 回菜单的 ServerTravel 只能发起一次。
	// GameOver 倒计时一旦 <= 0 就一直是负数，不挡的话每帧都会再调一次 ServerTravel
	//（真正换图在帧末，这中间还有好几帧）。
	bool bMatchEndTravelStarted = false;

	class ASpike* CurrentSpike = nullptr;

	void AssignTeams();
	void AssignSpikeToRandomAttacker();
	void RespawnAllPlayers();

	// 把场上**所有** ABlastBarrier 一起点亮/收掉（购买阶段的出生屏障）。
	// 每次现场扫一遍而不是 BeginPlay 缓存：关卡 actor 可能来自延迟加载的关卡流，
	// 而这个函数一回合只调两次，扫一遍 actor 表的开销可以忽略。
	void SetBarrierWallsActive(bool bActive);

	// 跨回合武器继承：销毁角色前记录到 PlayerState，重生后恢复
	void SavePlayerWeapons(class ABlasterPlayerController* PC);
	void RestorePlayerWeapons(class ABlasterPlayerController* PC);
	AWeapon* SpawnInheritedWeapon(TSubclassOf<AWeapon> WeaponClass, int32 Ammo, class ABlasterCharacter* Character);
	void StartNewRound();
	void EndRound(ETeam WinningTeam);
	void AwardRoundCredits(ETeam WinningTeam);
	void SwapTeams();
	void SyncPhaseTimeToClients();
	void CacheInitialWeaponSpawns();
	void RespawnMapWeapons();

	// 新回合把所有大招球打回"可拾取"（球的位置是关卡数据，不用重摆，只重置可用性状态）
	void ResetAllUltOrbs();

	// 登录序号（对齐 Lobby 里的 PlayerOrder，PlayerId 跨 ServerTravel 会变；
	// 英雄 + 队伍都按它从 GameInstance 恢复，跟分队同一套逻辑）
	int32 NextOrder = 0;

	ETeam RoundWinnerTeam = ETeam::ET_None;

	struct FInitialWeaponSpawn
	{
		TSubclassOf<class AWeapon> WeaponClass;
		FTransform SpawnTransform;
	};
	TArray<FInitialWeaponSpawn> InitialWeaponSpawns;

	// 测试机器人：每格一个出生点 + 当前机器人 + 复活定时器
	struct FTestBotSlot
	{
		FVector SpawnLocation;
		class ABlasterCharacter* Bot = nullptr;
		class ABlasterCharacter* DeadBody = nullptr;
		FTimerHandle RespawnTimer;
	};
	TArray<FTestBotSlot> TestBotSlots;
	bool bTestBotsSpawned = false;

	void SpawnTestBots();
	void SpawnTestBot(int32 Index);
	void RespawnTestBot(int32 Index);

	class ABlasterGameState* GetBlasterGameState() const;
};
