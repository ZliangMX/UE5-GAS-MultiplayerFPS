#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "BlasterPlayerState.generated.h"

class AWeapon;

UCLASS()
class BLASTER_API ABlasterPlayerState : public APlayerState
{
	GENERATED_BODY()
public:

	// 无缝旅行时新 PlayerState 只复制引擎基础字段，这里把 Lobby 选好的队伍带过去
	virtual void SeamlessTravelTo(APlayerState* NewPlayerState) override;

	virtual void OnRep_Score() override;

	UFUNCTION()
	virtual void OnRep_Defeats();

	UFUNCTION()
	virtual void OnRep_Team();

	void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OurLifetimeProps) const override;
	void AddToScore(float ScoreAmount);
	void AddToDefeats(int32 DefeatsAmount);

	void SetTeam(ETeam NewTeam);

	// --- 大招点数 ---
	// 攒够 `BlasterAgent::GetUltPointsRequired(Agent)` 点才能放大招（大招本身是 Stage 3 的事，
	// 这里只负责攒点 + 复制给 UI）。
	//
	// 点数来源（**全部只在服务器加**，和 Valorant 一致）：
	//   · 击杀 +1（BlasterGameMode 结算击杀处）    · 阵亡 +1（同上，死也涨）
	//   · 安包 +1 / 拆包 +1（ASpike 完成时，给安包者/拆包者）
	//   · 大招球 +1（放在地图上，见 AUltOrb —— 还没做）
	// **跨回合保留**：和护甲/武器不同，大招点是整场比赛累积的，换回合不清零。
	UFUNCTION()
	void OnRep_UltPoints();

	// 服务器：加/扣大招点。自动夹在 [0, Required]，封顶后不再涨（多出来的击杀不浪费存储）
	void AddUltPoints(int32 Amount);

	// 服务器：扣光大招点（放大招的消费口）。夹到 0，不会变负。
	// ⚠️ 扣点时机**由具体大招自己决定**，不是统一的 —— 绝大多数大招按下即扣，
	// 但 Phoenix「再来一次」要等"回到标记点"才扣（用户明确要求）。
	// 所以别在某个公共入口（比如技能激活）里无条件调它，见 UPhoenixRunItBackAbility。
	void SpendAllUltPoints();

	FORCEINLINE int32 GetUltPoints() const { return UltPoints; }
	// 当前英雄攒满需要多少点
	int32 GetUltPointsRequired() const;
	// 攒满了（UI 画高亮 / 大招门控都用它）
	bool IsUltReady() const;

	UPROPERTY(ReplicatedUsing = OnRep_UltPoints)
	int32 UltPoints = 0;

	// --- 选英雄 ---
	// Lobby 里选的英雄，复制到所有人（两侧 UI 读它展示）。None=未选(占位"随机")；
	// 全就绪时服务器给未选者随机补位，进对局也按它决定出生角色类。
	void SetAgent(EBlasterAgent NewAgent);
	FORCEINLINE EBlasterAgent GetAgent() const { return Agent; }
	FORCEINLINE bool HasSelectedAgent() const { return Agent >= EBlasterAgent::Jett && Agent < EBlasterAgent::MAX; }

	// --- 本回合击杀数（击杀确认标记用） ---
	// 仅服务器读写：每击杀 +1，回合开始归零，决定击杀标记造型档位。
	void AddRoundKill();
	void ResetRoundKills();
	FORCEINLINE int32 GetRoundKills() const { return RoundKills; }

	// 阵亡数（击杀数走 APlayerState::GetScore）：Tab 记分板读它显示 D 列
	FORCEINLINE int32 GetDefeats() const { return Defeats; }

	UPROPERTY(ReplicatedUsing = OnRep_Team)
	ETeam Team = ETeam::ET_None;

	// 选英雄：服务器写入、全客户端可见（Lobby 两侧槽位/对局出生类）
	UPROPERTY(Replicated)
	EBlasterAgent Agent = EBlasterAgent::None;

	UPROPERTY(Replicated)
	bool bIsReady = false;

	UPROPERTY(Replicated)
	int32 Credits = 800;

	void SetReady(bool bReady);
	void AddCredits(int32 Amount);
	bool SpendCredits(int32 Amount);

	// --- 跨回合武器继承（仅服务器读写，不复制） ---
	// 回合切换销毁角色前记录身上的武器，重生后恢复，实现枪械继承
	void SaveWeaponsForInheritance(AWeapon* PrimaryWeapon, AWeapon* SecondaryWeapon);

	TSubclassOf<AWeapon> StoredPrimaryWeaponClass;
	int32 StoredPrimaryAmmo = 0;

	TSubclassOf<AWeapon> StoredSecondaryWeaponClass;
	int32 StoredSecondaryAmmo = 0;

	// --- 跨回合护甲继承（仅服务器读写，不复制）---
	// 护甲跟武器一样跨回合保留：血量每回合回满，甲不回（Valorant 语义）。
	// 换回合销毁角色前由 GameMode 存进来，重生后写回新角色（同一对钩子函数，见 SavePlayerWeapons）。
	int32 StoredArmor = 0;

private:
	UPROPERTY()
	class ABlasterCharacter* Character;
	UPROPERTY()
	class ABlasterPlayerController* Controller;

	// 本回合击杀数（仅服务器读写，不复制）
	int32 RoundKills = 0;

	UPROPERTY(ReplicatedUsing = OnRep_Defeats)
	int32 Defeats = 0;
};
