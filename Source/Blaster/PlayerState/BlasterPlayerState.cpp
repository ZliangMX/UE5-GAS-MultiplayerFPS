#include "BlasterPlayerState.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/Weapon/Weapon.h"
#include "Net/UnrealNetwork.h"


void ABlasterPlayerState::SeamlessTravelTo(APlayerState* NewPlayerState)
{
	Super::SeamlessTravelTo(NewPlayerState);

	if (ABlasterPlayerState* NewBlasterPS = Cast<ABlasterPlayerState>(NewPlayerState))
	{
		// 把 Lobby 里选的队伍带进对局，避免被重新平衡分配覆盖
		NewBlasterPS->Team = Team;
		// 把 Lobby 里选的英雄也带过去（None 也会在对局出生时被随机补位）
		NewBlasterPS->Agent = Agent;
	}
}

void ABlasterPlayerState::AddToScore(float ScoreAmount)
{
	SetScore(GetScore() + ScoreAmount);
	Character = Character==nullptr? Cast<ABlasterCharacter>(GetPawn()) : Character;
	if (Character)
	{
		Controller = Controller==nullptr? Cast<ABlasterPlayerController>(Character->Controller) : Controller;
		if (Controller)
		{
			Controller->SetHUDScore(GetScore());
		}
	}
}

void ABlasterPlayerState::AddToDefeats(int32 DefeatsAmount)
{
	Defeats+=DefeatsAmount;
	Character = Character==nullptr? Cast<ABlasterCharacter>(GetPawn()) : Character;
	if (Character)
	{
		Controller = Controller==nullptr? Cast<ABlasterPlayerController>(Character->Controller) : Controller;
		if (Controller)
		{
			Controller->SetHUDDefeats(Defeats);
		}
	}
}

int32 ABlasterPlayerState::GetUltPointsRequired() const
{
	return BlasterAgent::GetUltPointsRequired(Agent);
}

bool ABlasterPlayerState::IsUltReady() const
{
	return UltPoints >= GetUltPointsRequired();
}

void ABlasterPlayerState::AddUltPoints(int32 Amount)
{
	if (Amount == 0) return;

	// 封顶：攒满之后继续击杀不再累积（否则 8/7 这种数字会让 UI 和门控都难写）。
	// 注意英雄是进对局才 roll 的，Agent 变化时 Required 会变 —— 夹在 [0, Required] 每回合都成立。
	UltPoints = FMath::Clamp(UltPoints + Amount, 0, GetUltPointsRequired());

	// 权威机改自己的复制属性不会触发 OnRep，手动刷一次 UI（客户端走 OnRep_UltPoints）
	OnRep_UltPoints();
}

void ABlasterPlayerState::SpendAllUltPoints()
{
	if (UltPoints == 0) return;

	// 直接清零，而不是 AddUltPoints(-Required)：Required 随英雄走（换英雄后可能变小），
	// 拿它做减法容易残留个位数点数。扣点的语义就是"清空"。
	UltPoints = 0;

	// 权威机改自己的复制属性不会触发 OnRep，手动刷一次 UI（客户端走 OnRep_UltPoints）
	OnRep_UltPoints();
}

void ABlasterPlayerState::OnRep_UltPoints()
{
	// 大招点数的消费方（技能条大招槽 / 记分板）都从 PS 读，这里不用推给 HUD。
	// 留这个空实现是为了：① 复制属性必须有 RepNotify 才能被 UI 感知；
	// ② 以后要加"攒满时播个提示音"之类的东西，挂这里最自然。
}

void ABlasterPlayerState::SetTeam(ETeam NewTeam)
{
	Team = NewTeam;
	if (HasAuthority())
	{
		OnRep_Team();
	}
}

void ABlasterPlayerState::SetAgent(EBlasterAgent NewAgent)
{
	Agent = NewAgent;
	// 仅服务器调用（LobbyGameMode / 对局出生）。复制属性：客户端靠 DOREPLIFETIME 收到，
	// Lobby 的 UI 走 PlayerArray 轮询刷新，无需 OnRep。
}

void ABlasterPlayerState::OnRep_Score()
{
	Super::OnRep_Score();

	Character = Character==nullptr? Cast<ABlasterCharacter>(GetPawn()) : Character;
	if (Character)
	{
		Controller = Controller==nullptr? Cast<ABlasterPlayerController>(Character->Controller) : Controller;
		if (Controller)
		{
			Controller->SetHUDScore(GetScore());
		}
	}
}

void ABlasterPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABlasterPlayerState, Defeats);
	DOREPLIFETIME(ABlasterPlayerState, Team);
	DOREPLIFETIME(ABlasterPlayerState, Agent);
	DOREPLIFETIME(ABlasterPlayerState, bIsReady);
	DOREPLIFETIME(ABlasterPlayerState, Credits);
	// 大招点数：复制给所有端。Valorant 里只有队友的大招状态可见，但本项目记分板是
	// "只显示本队"，所以复制给所有人 + UI 只画本队 = 实际信息面和 Valorant 一致，
	// 比为了藏敌人那点信息去搞队伍条件复制省事得多。
	DOREPLIFETIME(ABlasterPlayerState, UltPoints);
}

void ABlasterPlayerState::SetReady(bool bReady)
{
	bIsReady = bReady;
}

void ABlasterPlayerState::AddCredits(int32 Amount)
{
	Credits += Amount;
}

bool ABlasterPlayerState::SpendCredits(int32 Amount)
{
	if (Credits >= Amount)
	{
		Credits -= Amount;
		return true;
	}
	return false;
}

void ABlasterPlayerState::SaveWeaponsForInheritance(AWeapon* PrimaryWeapon, AWeapon* SecondaryWeapon)
{
	StoredPrimaryWeaponClass = PrimaryWeapon ? PrimaryWeapon->GetClass() : nullptr;
	StoredPrimaryAmmo = PrimaryWeapon ? PrimaryWeapon->GetAmmo() : 0;

	StoredSecondaryWeaponClass = SecondaryWeapon ? SecondaryWeapon->GetClass() : nullptr;
	StoredSecondaryAmmo = SecondaryWeapon ? SecondaryWeapon->GetAmmo() : 0;
}

void ABlasterPlayerState::AddRoundKill()
{
	RoundKills++;
}

void ABlasterPlayerState::ResetRoundKills()
{
	RoundKills = 0;
}

void ABlasterPlayerState::OnRep_Defeats()
{
	Character = Character==nullptr? Cast<ABlasterCharacter>(GetPawn()) : Character;
	if (Character)
	{
		Controller = Controller==nullptr? Cast<ABlasterPlayerController>(Character->Controller) : Controller;
		if (Controller)
		{
			Controller->SetHUDDefeats(Defeats);
		}
	}
}

void ABlasterPlayerState::OnRep_Team()
{
	Character = Character==nullptr ? Cast<ABlasterCharacter>(GetPawn()) : Character;
	if (Character)
	{
		Controller = Controller==nullptr ? Cast<ABlasterPlayerController>(Character->Controller) : Controller;
		if (Controller)
		{
			Controller->SetHUDTeam(Team);
		}
	}
}
