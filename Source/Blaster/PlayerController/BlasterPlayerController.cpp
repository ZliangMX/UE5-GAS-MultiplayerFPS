#include "BlasterPlayerController.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/HUD/CharacterOverlay.h"
#include "Blaster/HUD/Announcement.h"
#include "Blaster/HUD/LobbyOverlay.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "GameFramework/GameMode.h"
#include "Net/UnrealNetwork.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/GameMode/LobbyGameMode.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Kismet/GameplayStatics.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/HUD/BuyMenu.h"
#include "Blaster/HUD/CloveSmokeMapWidget.h"
#include "Blaster/HUD/ScoreboardWidget.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/Weapon/WeaponKillIconSet.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/BlasterComponent/MinimapComponent.h"
#include "Materials/MaterialInterface.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sound/SoundBase.h"
#include "EngineUtils.h"
#include "InputCoreTypes.h"
#include "GameFramework/PlayerInput.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerState.h"
#include "MultiplayerSessionsSubsystem.h"

ABlasterPlayerController::ABlasterPlayerController()
{
	MinimapComponent = CreateDefaultSubobject<UMinimapComponent>(TEXT("MinimapComponent"));
}

void ABlasterPlayerController::BeginPlay()
{
	Super::BeginPlay();
	BlasterHUD = Cast<ABlasterHUD>(GetHUD());
	ServerCheckMatchState();
}

void ABlasterPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABlasterPlayerController, MatchState);
	DOREPLIFETIME(ABlasterPlayerController, RoundWinnerTeam);
}

void ABlasterPlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 玩家 ID 得在 bInLobby 的提前 return **之前**推 —— Lobby（选人）地图正是最需要它的地方，
	// 选人界面上的名字就是 PlayerState 的名字
	ApplyPlayerIdIfNeeded();

	LobbyPollInit();
	if (bInLobby) return;

	SetHUDTime();
	CheckTimeSync(DeltaTime);
	PollInit();

	// 观战：只在真实本机运行（服务器上对远端 PC 的 Tick 也会到这里，用 IsLocalController 挡掉）
	if (IsLocalController())
	{
		// 敌方勾边：按 0.2s 节流重算"该勾谁"
		UpdateEnemyOutlines(DeltaTime);

		SpectateTick(DeltaTime);

		// 封烟地图的自愈检查。**不能只靠 OnUnPossess**：客户端换 pawn 走的是
		// APlayerController::ClientRestart_Implementation，它直接 SetPawn 而不调 UnPossess
		//（PlayerController.cpp:790），所以回合切换后 OnUnPossess 根本不会触发，
		// 鼠标光标会永远留在屏幕上。这里每帧比一次指针，代价可忽略，
		// 换来「任何路径丢 pawn 都能自动关图」。
		if (bSmokeMapOpen && SmokeMapOwnerPawn.Get() != GetPawn())
		{
			CloseSmokeMap();
		}
	}
}

void ABlasterPlayerController::UpdateEnemyOutlines(float DeltaTime)
{
	// 没配材质 = 整个功能关着，这时连世界都别遍历。
	// 材质在 PC 蓝图的 Class Defaults 里指定（Highlight 分类），C++ 不写死路径。
	if (!EnemyOutlineMaterial) return;

	EnemyOutlineRefreshTimer += DeltaTime;
	if (EnemyOutlineRefreshTimer < EnemyOutlineRefreshInterval) return;
	EnemyOutlineRefreshTimer = 0.f;

	// ★ 判敌我必须看**本机的 PlayerState**，不能用 GameMode / 队伍数组：
	//   客户端上那些东西要么不存在，要么是自己的视角之外的
	const ABlasterPlayerState* MyPS = GetPlayerState<ABlasterPlayerState>();
	const ETeam MyTeam = MyPS ? MyPS->Team : ETeam::ET_None;
	APawn* MyPawn = GetPawn();

	for (TActorIterator<ABlasterCharacter> It(GetWorld()); It; ++It)
	{
		ABlasterCharacter* Other = *It;
		if (!Other || Other == MyPawn) continue;

		// 只勾角色本体（CharacterMesh0）。手里的枪是另一个网格组件，不碰。
		USkeletalMeshComponent* Body = Other->GetMesh();
		if (!Body) continue;

		// 四条都满足才勾：双方都分好队（ET_None = 还没选边）、不是本队、还活着。
		// 阵亡的尸体会被这里清掉 —— 死了就不该再顶着边躺在地上。
		const ABlasterPlayerState* OtherPS = Other->GetPlayerState<ABlasterPlayerState>();
		const bool bOutline = MyTeam != ETeam::ET_None
			&& OtherPS != nullptr
			&& OtherPS->Team != ETeam::ET_None
			&& OtherPS->Team != MyTeam
			&& !Other->IsElimmed();

		// MeshComponent.cpp:290 —— 值没变就直接返回，所以这里不用自己比：
		// 状态稳定的敌人每 0.2s 调一次也不会重建渲染状态。
		Body->SetOverlayMaterial(bOutline ? EnemyOutlineMaterial : nullptr);
	}
}


void ABlasterPlayerController::CheckTimeSync(float DeltaTime)
{
	TimeSyncRunningTime += DeltaTime;
	if (IsLocalController() && TimeSyncRunningTime > TimeSyncFrequency)
	{
		ServerRequestServerTime(GetWorld()->GetTimeSeconds());
		TimeSyncRunningTime = 0.f;
	}
}

void ABlasterPlayerController::ServerCheckMatchState_Implementation()
{
	ABlasterGameMode* GameMode = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if (GameMode)
	{
		WarmupDuration = GameMode->WarmupDuration;
		WarmupTime = GameMode->BuyPhaseTime;
		MatchTime = GameMode->RoundTime;
		CooldownTime = GameMode->PostRoundTime;
		GameOverTime = GameMode->GameOverTime;
		LevelStartingTime = GameMode->LevelStartingTime;
		MatchState = GameMode->GetMatchState();
		ClientJoinMidGame(MatchState, WarmupDuration, WarmupTime, MatchTime, LevelStartingTime, CooldownTime, GameOverTime);

		if (BlasterHUD && (MatchState == MatchState::WaitingToStart || MatchState == MatchState::BuyPhase))
		{
			BlasterHUD->AddAnnouncement();
		}
	}
}

void ABlasterPlayerController::ClientJoinMidGame_Implementation(FName StateOfMatch, float InWarmupDuration, float BuyPhase, float Match, float StartingTime, float PostRound, float GameOver)
{
	WarmupDuration = InWarmupDuration;
	WarmupTime = BuyPhase;
	MatchTime = Match;
	CooldownTime = PostRound;
	GameOverTime = GameOver;
	LevelStartingTime = StartingTime;
	MatchState = StateOfMatch;
	OnMatchStateSet(MatchState);
	if (BlasterHUD && (MatchState == MatchState::WaitingToStart || MatchState == MatchState::BuyPhase))
	{
		BlasterHUD->AddAnnouncement();
	}
}

void ABlasterPlayerController::ClientReportServerTime_Implementation(float TimeOfClientRequest,
																	 float TimeServerReceiveClientRequest)
{
	float RoundTripTime = GetWorld()->GetTimeSeconds() - TimeOfClientRequest;
	float CurrentServerTime = TimeServerReceiveClientRequest + (0.5f * RoundTripTime);
	ClientServerDelta = CurrentServerTime - GetWorld()->GetTimeSeconds();
}

void ABlasterPlayerController::ServerRequestServerTime_Implementation(float TimeOfClientRequest)
{
	float ServerTimeOfReceipt = GetWorld()->GetTimeSeconds();
	ClientReportServerTime(TimeOfClientRequest, ServerTimeOfReceipt);
}


void ABlasterPlayerController::SetHUDHealth(float Health, float MaxHealth, float Armor, float MaxArmor)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;

	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->HealthBar && BlasterHUD->CharacterOverlay->HealthText;
	if (bHUDValid)
	{
		UCharacterOverlay* Overlay = BlasterHUD->CharacterOverlay;

		const float HealthPercent = MaxHealth > 0.f ? Health / MaxHealth : 0.f;
		Overlay->HealthBar->SetPercent(HealthPercent);
		FString HealthText = FString::Printf(TEXT("%d/%d"), FMath::CeilToInt(Health), FMath::CeilToInt(MaxHealth));

		// 护甲优先走专属控件（Valorant 那样血条旁边一条甲条 + 一个甲数字）。
		// WBP_CharacterOverlay 里**还没加**这两控件时，退回「把甲并进血量文字」——
		// 这样护甲系统不改 WBP 就能看见效果，等加了控件自动切回正经显示。
		if (Overlay->ArmorText)
		{
			Overlay->ArmorText->SetText(FText::FromString(FString::Printf(TEXT("%d"), FMath::CeilToInt(Armor))));
			Overlay->ArmorText->SetVisibility(Armor > 0.f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}
		else if (Armor > 0.f)
		{
			HealthText += FString::Printf(TEXT("  (+%d)"), FMath::CeilToInt(Armor));
		}

		if (Overlay->ArmorBar)
		{
			Overlay->ArmorBar->SetPercent(MaxArmor > 0.f ? Armor / MaxArmor : 0.f);
			Overlay->ArmorBar->SetVisibility(Armor > 0.f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		}

		Overlay->HealthText->SetText(FText::FromString(HealthText));
	}
	else
	{
		bInitializeCharacterOverlay = true;
		HUDHealth = Health;
		HUDMaxHealth = MaxHealth;
		HUDArmor = Armor;
		HUDMaxArmor = MaxArmor;
	}
}


void ABlasterPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// 复活 / 新回合：拥有新角色就退出观战回自己视角（服务器与各客户端各自触发，
	// 由 StopSpectate 里 !HasAuthority 的 RPC 兜底把服务器相机目标也切回来）
	if (bSpectating)
	{
		StopSpectate();
	}

	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(InPawn);
	if (BlasterCharacter)
	{
		SetHUDHealth(BlasterCharacter->GetHealth(), BlasterCharacter->GetMaxHealth(),
			BlasterCharacter->GetArmor(), BlasterCharacter->GetMaxArmor());
	}
}

void ABlasterPlayerController::OnUnPossess()
{
	// 暮蝶的封烟地图开在视口上、还占着鼠标光标和输入模式。塔下丢 pawn 的路径有两条
	// （回合切换直接 Destroy 角色 / F9 换英雄），都会经过这里 —— 在这关一次就够，
	// 否则鼠标光标会永远停在屏幕上、输入模式卡在 GameAndUI，玩家连枪都开不了。
	CloseSmokeMap();

	// 同理兜一层：狙击镜也是视口上的 widget，换 pawn 时一起收掉。
	// **必须在 Super::OnUnPossess() 之前** —— Pawn 是在 Super 里被清空的，之后再 GetPawn() 就没了。
	if (ABlasterCharacter* LeavingPawn = Cast<ABlasterCharacter>(GetPawn()))
	{
		LeavingPawn->HideSniperScopeIfAiming();
	}

	Super::OnUnPossess();
}


float ABlasterPlayerController::GetServerTime()
{
	if (HasAuthority()) return GetWorld()->GetTimeSeconds();
	return GetWorld()->GetTimeSeconds() + ClientServerDelta;
}

void ABlasterPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();
	if (IsLocalController())
	{
		ServerRequestServerTime(GetWorld()->GetTimeSeconds());
	}
}


void ABlasterPlayerController::SetHUDScore(float Score)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->ScoreAmount;
	if (bHUDValid)
	{
		FString ScoreText = FString::Printf(TEXT("%d"), FMath::FloorToInt(Score));
		BlasterHUD->CharacterOverlay->ScoreAmount->SetText(FText::FromString(ScoreText));
	}
	else
	{
		bInitializeCharacterOverlay = true;
		HUDScore = Score;
	}
}

void ABlasterPlayerController::SetHUDDefeats(int32 Defeats)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->DefeatsAmount;
	if (bHUDValid)
	{
		FString DefeatsText = FString::Printf(TEXT("%d"), Defeats);
		BlasterHUD->CharacterOverlay->DefeatsAmount->SetText(FText::FromString(DefeatsText));
	}
	else
	{
		bInitializeCharacterOverlay = true;
		HUDDefeats = Defeats;
	}
}

void ABlasterPlayerController::SetHUDWeaponAmmo(int32 Ammo)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->WeaponAmmoAmount;
	if (bHUDValid)
	{
		// 负数 = "这件武器没有弹药这个概念"，把数字清空而不是显示 0：
		// 近战武器（3 号槽的刀）走这里，见 AMeleeWeapon::SetHUDAmmo。
		// 显示 0 会让人以为"弹匣空了、要换弹"，而刀根本没有换弹这回事。
		FString AmmoText = Ammo < 0 ? FString() : FString::Printf(TEXT("%d"), Ammo);
		BlasterHUD->CharacterOverlay->WeaponAmmoAmount->SetText(FText::FromString(AmmoText));
	}
}

void ABlasterPlayerController::SetHUDCarriedAmmo(int32 Ammo)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->CarriedAmmoAmount;
	if (bHUDValid)
	{
		FString AmmoText = FString::Printf(TEXT("%d"), Ammo);
		BlasterHUD->CharacterOverlay->CarriedAmmoAmount->SetText(FText::FromString(AmmoText));
	}
}

// --- 射击反馈 ---

void ABlasterPlayerController::ShowHitMarker()
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		BlasterHUD->ShowHitMarker();
	}
}

void ABlasterPlayerController::ClientShowHitMarker_Implementation()
{
	ShowHitMarker();
}

void ABlasterPlayerController::ShowDamageNumber(float Damage, const FVector& DamageOrigin)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		BlasterHUD->AddDamageNumber(Damage, DamageOrigin);
	}
}

void ABlasterPlayerController::ClientShowDamageNumber_Implementation(float Damage, const FVector& DamageOrigin)
{
	ShowDamageNumber(Damage, DamageOrigin);
}

void ABlasterPlayerController::ShowDamageDirection(const FVector& DamageOrigin)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		BlasterHUD->AddDamageDirection(DamageOrigin);
	}
}

void ABlasterPlayerController::ClientShowDamageDirection_Implementation(const FVector& DamageOrigin)
{
	ShowDamageDirection(DamageOrigin);
}

void ABlasterPlayerController::ShowKillMarker(int32 RoundKills, UWeaponKillIconSet* KillIconSet)
{
	// HUD 只能绑定本地玩家控制器；服务器对远端 PC 也会走到这里，必须跳过
	if (!IsLocalController()) return;

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		BlasterHUD->ShowKillMarker(RoundKills, KillIconSet);
	}
}

void ABlasterPlayerController::ClientShowKillMarker_Implementation(int32 RoundKills, UWeaponKillIconSet* KillIconSet)
{
	ShowKillMarker(RoundKills, KillIconSet);
}

void ABlasterPlayerController::HideKillMarker()
{
	if (!IsLocalController()) return;
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		BlasterHUD->HideKillMarker();
	}
}

void ABlasterPlayerController::PlayKillSound(bool bHeadshot, UWeaponKillIconSet* KillSet, int32 KillIndex)
{
	// 优先级：武器资产里这一档的音效 →（爆头 / 普通）击杀确认音 → 项目自带兜底。
	// 后两级是加这套东西**之前**的老路，所以没填资产的枪听起来和以前一模一样。
	//
	// KillIndex 由武器在服务器上算好传来（见 AWeapon::ComputeThisKillIndex）—— 不在这儿自己读
	// PlayerState：远端客户端收到这个 RPC 时，它那份计数复制到第几了说不准，
	// 会出现"图标显示第 3 杀、音效却响第 2 段"。
	USoundBase* Sound = KillSet ? KillSet->GetKillSound(KillIndex) : nullptr;
	if (!Sound)
	{
		Sound = bHeadshot && HeadshotKillSound ? HeadshotKillSound : KillConfirmSound;
	}
	if (!Sound)
	{
		// 兜底：没配任何击杀音就用项目自带的"拾取提示"音（界面提示音，两段短音）
		Sound = LoadObject<USoundBase>(nullptr,
			TEXT("/Game/Assets/InterfaceAndItemSounds/Cues/Special_Powerup_08_wav_Cue.Special_Powerup_08_wav_Cue"));
	}
	if (Sound)
	{
		UGameplayStatics::PlaySound2D(this, Sound);
	}
}

void ABlasterPlayerController::ClientPlayKillSound_Implementation(bool bHeadshot, UWeaponKillIconSet* KillSet, int32 KillIndex)
{
	PlayKillSound(bHeadshot, KillSet, KillIndex);
}

// --- 射击反馈 end ---

void ABlasterPlayerController::SetHUDMatchCountDown(float CountDownTime)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->MatchCountDownText;
	if (bHUDValid)
	{
		if (CountDownTime < 0)
		{
			BlasterHUD->CharacterOverlay->MatchCountDownText->SetText(FText::FromString(""));
			return;
		}

		int32 Minutes = FMath::FloorToInt(CountDownTime / 60.0f);
		int32 Seconds = CountDownTime - Minutes * 60;

		FString CountDownText = FString::Printf(TEXT("%02d:%02d"), Minutes, Seconds);
		BlasterHUD->CharacterOverlay->MatchCountDownText->SetText(FText::FromString(CountDownText));
	}
}

void ABlasterPlayerController::SetHUDAnnouncementCountdown(float CountDownTime)
{
	BlasterHUD = BlasterHUD ==  nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->Announcement && BlasterHUD->Announcement->WarmupTime;
	if (bHUDValid)
	{
		if (CountDownTime < 0.f)
		{
			BlasterHUD->Announcement->WarmupTime->SetText(FText::FromString(""));
			return;
		}
		int32 Minutes = FMath::FloorToInt(CountDownTime / 60.0f);
		int32 Seconds = CountDownTime - Minutes * 60;

		FString CountDownText = FString::Printf(TEXT("%02d:%02d"), Minutes, Seconds);
		BlasterHUD->Announcement->WarmupTime->SetText(FText::FromString(CountDownText));
	}
}

// --- Team / Round HUD setters ---

void ABlasterPlayerController::SetHUDTeam(ETeam Team)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->TeamText;
	if (bHUDValid)
	{
		FString TeamString;
		switch (Team)
		{
		case ETeam::ET_TeamA: TeamString = TEXT("Attack"); break;
		case ETeam::ET_TeamB: TeamString = TEXT("Defend"); break;
		default: TeamString = TEXT(""); break;
		}
		BlasterHUD->CharacterOverlay->TeamText->SetText(FText::FromString(TeamString));
	}
}

void ABlasterPlayerController::SetHUDTeamScore(int32 ScoreA, int32 ScoreB)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->TeamScoreText;
	if (bHUDValid)
	{
		FString ScoreText = FString::Printf(TEXT("A %d : %d B"), ScoreA, ScoreB);
		BlasterHUD->CharacterOverlay->TeamScoreText->SetText(FText::FromString(ScoreText));
	}
}

void ABlasterPlayerController::SetHUDAliveCount(int32 AliveA, int32 AliveB)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->AliveCountText;
	if (bHUDValid)
	{
		FString AliveText = FString::Printf(TEXT("%d v %d"), AliveA, AliveB);
		BlasterHUD->CharacterOverlay->AliveCountText->SetText(FText::FromString(AliveText));
	}
}

void ABlasterPlayerController::SetHUDSpikeStatus(const FString& Status, float Progress)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->SpikeStatusText;
	if (bHUDValid)
	{
		BlasterHUD->CharacterOverlay->SpikeStatusText->SetText(FText::FromString(Status));
	}

	bool bBarValid = BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->SpikeTimerBar;
	if (bBarValid)
	{
		if (Progress >= 0.f)
		{
			BlasterHUD->CharacterOverlay->SpikeTimerBar->SetPercent(Progress);
			BlasterHUD->CharacterOverlay->SpikeTimerBar->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			BlasterHUD->CharacterOverlay->SpikeTimerBar->SetVisibility(ESlateVisibility::Hidden);
		}
	}

	// 上面那块是旧 HUD 的（bValorantHUDOnly 打开时 CharacterOverlay 根本不建，等于空转）。
	// 新那套把同一份数据画在公告位置上 —— 基类 UAnnouncement::SetSpikeStatus 是空实现，
	// 只有 UValorantAnnouncement 会接。见 Announcement.h。
	if (BlasterHUD && BlasterHUD->Announcement)
	{
		BlasterHUD->Announcement->SetSpikeStatus(Status, Progress);
	}
}

void ABlasterPlayerController::ClientUpdateSpikeProgress_Implementation(const FString& Status, float Progress)
{
	SetHUDSpikeStatus(Status, Progress);
}

void ABlasterPlayerController::SetHUDRoundResult(const FString& ResultText)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->Announcement && BlasterHUD->Announcement->RoundResultText;
	if (bHUDValid)
	{
		BlasterHUD->Announcement->RoundResultText->SetText(FText::FromString(ResultText));
		BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
	}
}

void ABlasterPlayerController::SetHUDTeamSwap(bool bShowSwap)
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDValid = BlasterHUD && BlasterHUD->Announcement && BlasterHUD->Announcement->TeamSwapText;
	if (bHUDValid)
	{
		if (bShowSwap)
		{
			BlasterHUD->Announcement->TeamSwapText->SetText(FText::FromString(TEXT("Sides Swapped!")));
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			BlasterHUD->Announcement->TeamSwapText->SetText(FText());
		}
	}
}

// --- Time / MatchState ---

float ABlasterPlayerController::GetPhaseTimeLeft()
{
	if (MatchState == MatchState::WaitingToStart) return WarmupDuration - GetServerTime() + LevelStartingTime;
	if (MatchState == MatchState::BuyPhase)       return WarmupTime - GetServerTime() + LevelStartingTime;
	if (MatchState == MatchState::InProgress)     return MatchTime - GetServerTime() + LevelStartingTime;
	if (MatchState == MatchState::PostRound)      return CooldownTime - GetServerTime() + LevelStartingTime;
	if (MatchState == MatchState::GameOver)       return GameOverTime - GetServerTime() + LevelStartingTime;
	return 0.f;
}

void ABlasterPlayerController::SetHUDTime()
{
	const float TimeLeft = GetPhaseTimeLeft();

	// 安包后旧回合倒计时不再显示（由 spike 爆炸倒计时接管），
	// 直到拆包/爆炸进入 PostRound 才恢复。旧倒计时安包后就没用了。
	ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
	const bool bSpikePlanted = GS && GS->bSpikePlanted;
	const bool bHideMatchCountdown = (MatchState == MatchState::InProgress) && bSpikePlanted;

	if (bHideMatchCountdown)
	{
		// 只在状态切换时隐藏一次
		if (!bMatchCountdownHidden)
		{
			BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
			if (BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->MatchCountDownText)
			{
				BlasterHUD->CharacterOverlay->MatchCountDownText->SetVisibility(ESlateVisibility::Hidden);
				bMatchCountdownHidden = true;
			}
		}
		return;
	}

	uint32 SecondsLeft = FMath::CeilToInt(TimeLeft);

	if (HasAuthority())
	{
		BlasterGameMode = BlasterGameMode == nullptr ? Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this)) : BlasterGameMode;
		if (BlasterGameMode)
		{
			SecondsLeft = FMath::CeilToInt(BlasterGameMode->GetCountDownTime() + LevelStartingTime);
		}
	}

	// 从安包状态恢复（拆包/爆炸进入 PostRound）：重新显示角落倒计时
	if (bMatchCountdownHidden)
	{
		bMatchCountdownHidden = false;
		BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
		if (BlasterHUD && BlasterHUD->CharacterOverlay && BlasterHUD->CharacterOverlay->MatchCountDownText)
		{
			BlasterHUD->CharacterOverlay->MatchCountDownText->SetVisibility(ESlateVisibility::Visible);
		}
	}

	if (CountdownInt != SecondsLeft)
	{
		// Announcement 的大字倒计时只用于最开始的准备阶段，其余阶段一律用 HUD 角落倒计时
		if (MatchState == MatchState::WaitingToStart)
		{
			SetHUDAnnouncementCountdown(TimeLeft);
			SetHUDMatchCountDown(-1.f);
		}
		else
		{
			SetHUDMatchCountDown(TimeLeft);
			SetHUDAnnouncementCountdown(-1.f);
		}
		CountdownInt = SecondsLeft;
	}
}

void ABlasterPlayerController::OnMatchStateSet(FName State)
{
	// 只有服务端走这里（客户端靠 OnRep_MatchState）。趁切阶段这一下把"本回合谁赢"一起抄进
	// 本 PC 的复制属性 —— 必须写在下面那几个 Handle* **之前**：宿主机就在下一行同步执行
	// HandlePostRound，而两个属性是同一帧、同一个 Actor 写进去的，必然同批发出去。
	// GameState 上那份在 EndRound 里已经先写好了（见 ABlasterGameMode::EndRound），这里读得到。
	if (HasAuthority())
	{
		// 只有结算那两个阶段要"谁赢了"，其余阶段一律清空。不清的话会留下一个坑：
		// GameMode 是先给所有 PC 调这个、**再** StartNewRound 去清 GS 上那份的，所以刚进
		// BuyPhase 时 GS 上还挂着上一回合的赢家 —— 抄过来就成了旧值常驻，万一哪次 EndRound
		// 没写赢家（GM 上那个已经是 None 就直接 return），底层读到的会是上回合的结论。
		const bool bResultPhase = (State == MatchState::PostRound || State == MatchState::GameOver);
		const UWorld* World = GetWorld();
		const ABlasterGameState* GS = World ? World->GetGameState<ABlasterGameState>() : nullptr;
		RoundWinnerTeam = (bResultPhase && GS) ? GS->RoundWinnerTeam : ETeam::ET_None;
	}

	MatchState = State;

	if (MatchState == MatchState::BuyPhase)
	{
		HandleBuyPhase();
	}
	else if (MatchState == MatchState::InProgress)
	{
		HandleMatchHasStarted();
	}
	else if (MatchState == MatchState::PostRound)
	{
		HandlePostRound();
	}
	else if (MatchState == MatchState::GameOver)
	{
		HandleGameOver();
	}
}


void ABlasterPlayerController::OnRep_MatchState()
{
	if (MatchState == MatchState::BuyPhase)
	{
		HandleBuyPhase();
	}
	else if (MatchState == MatchState::InProgress)
	{
		HandleMatchHasStarted();
	}
	else if (MatchState == MatchState::PostRound)
	{
		HandlePostRound();
	}
	else if (MatchState == MatchState::GameOver)
	{
		HandleGameOver();
	}
}

void ABlasterPlayerController::HandleBuyPhase()
{
	// 新回合开始：清掉上一回合的击杀标记
	HideKillMarker();

	AddBuyMenu();

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		if (BlasterHUD->CharacterOverlay == nullptr) BlasterHUD->AddCharacterOverlay();
		if (BlasterHUD->Announcement)
		{
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
			if (BlasterHUD->Announcement->AnnouncementText)
				BlasterHUD->Announcement->AnnouncementText->SetText(FText::FromString(TEXT("Buy Phase")));
			// 这里只给**后半句**：UValorantAnnouncement 那个面板的提示行长这样 ——
			// "PRESS" 和中间那个 [B] 键帽是版面里写死的，InfoText 接的是右边的后缀。
			// （旧的 WBP_Announcement 上会只显示 "TO BUY"，但那时 bValorantHUDOnly 是关的。）
			if (BlasterHUD->Announcement->InfoText)
				BlasterHUD->Announcement->InfoText->SetText(FText::FromString(TEXT("TO BUY")));
			if (BlasterHUD->Announcement->RoundResultText)
				BlasterHUD->Announcement->RoundResultText->SetText(FText());
		}
	}

	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (BlasterCharacter)
	{
		// 新回合：上回合开着的狙击镜是视口上的 widget，不随角色销毁，在这儿收一次。
		// 放在最前 —— 这段跑完 GameMode 才会 Destroy 旧角色重开一局（StartNewRound），
		// 那之后再想收就找不到原来那个角色实例了（widget 是它建在自己身上的）。
		BlasterCharacter->HideSniperScopeIfAiming();

		BlasterCharacter->bDisableGameplay = false;
		BlasterCharacter->ResetHealth();
	}

	ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
	if (GS)
	{
		SetHUDTeamScore(GS->TeamAScore, GS->TeamBScore);
		SetHUDAliveCount(GS->AliveCountTeamA, GS->AliveCountTeamB);
	}
}

void ABlasterPlayerController::HandleMatchHasStarted()
{
	CloseBuyMenu();

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		if (BlasterHUD->CharacterOverlay == nullptr) BlasterHUD->AddCharacterOverlay();
		if (BlasterHUD->Announcement)
		{
			// ★ 这里**不能**无条件 SetVisibility(Hidden)：安包/拆包进度条画在这个 widget 里面，
			//   而进 InProgress 是每回合都来的一道 —— 藏一块等于常规阶段整局看不到那条进度条。
			//   基类默认还是 Hidden（旧的 WBP_Announcement 走那条），UValorantAnnouncement
			//   覆写成"只清字、不藏块"，面板自己按内容收放。见 Announcement.h::OnRoundStarted。
			BlasterHUD->Announcement->OnRoundStarted();
		}
	}
}

bool ABlasterPlayerController::DidLocalTeamWinRound() const
{
	const UWorld* World = GetWorld();
	const ABlasterGameState* GS = World ? World->GetGameState<ABlasterGameState>() : nullptr;
	const ABlasterPlayerState* PS = Cast<ABlasterPlayerState>(PlayerState);
	if (PS == nullptr || PS->Team == ETeam::ET_None)
	{
		// 对局里 Team 一定有值，走到这儿说明有别的毛病（PS 还没到 / 分边没同步）。
		// 症状是"不管怎么打都判 LOST"，所以留一行日志，省得下次再从头猜一遍。
		UE_LOG(LogTemp, Warning, TEXT("[RoundResult] %s 判本轮输赢时本地队伍是 %s —— 这一局只会判成 LOST"),
			*GetName(), PS ? TEXT("ET_None") : TEXT("没有 PlayerState"));
		return false;
	}

	// 第一顺位是本 PC 自己那份：和 MatchState 同一个 Actor、服务端同一行写的，客户端必然
	// 和 MatchState 一起到达（见头文件 RoundWinnerTeam 的注释，别改成只读 GameState）。
	// 第二顺位才是 GameState 上那份（比如 PC 还没来得及写就被别的路径调了），
	// 最后按总分兜底 —— 那手只在 GameOver 那一场是对的，PostRound 上真走到这儿会判错。
	ETeam Winner = RoundWinnerTeam;
	if (Winner == ETeam::ET_None && GS)
	{
		Winner = GS->RoundWinnerTeam;
	}
	if (Winner == ETeam::ET_None && GS)
	{
		Winner = (GS->TeamAScore >= GS->TeamBScore) ? ETeam::ET_TeamA : ETeam::ET_TeamB;
		UE_LOG(LogTemp, Warning, TEXT("[RoundResult] %s 判本轮输赢时两个赢家来源都是 ET_None，退回比总分（%d:%d）—— "
			"如果这行出现在 PostRound，说明赢家没抄到 PC 上，去查 OnMatchStateSet 里那次赋值"),
			*GetName(), GS->TeamAScore, GS->TeamBScore);
	}

	return PS->Team == Winner;
}

void ABlasterPlayerController::HandlePostRound()
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		// 不移除 CharacterOverlay，让安包/拆包进度条在回合结束阶段照常显示；
		// 公告文字结构与购买阶段一致（阶段名 + 提示）
		if (BlasterHUD->Announcement)
		{
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
			// 大字直接出 WON / LOST（Valorant 结算就是这个），不再写阶段名 "Post Round"。
			// UValorantAnnouncement 按这两个字自动换底色（青 / 红），见 EValorantAnnPanelTone。
			if (BlasterHUD->Announcement->AnnouncementText)
				BlasterHUD->Announcement->AnnouncementText->SetText(FText::FromString(
					DidLocalTeamWinRound() ? TEXT("WON") : TEXT("LOST")));

			if (BlasterHUD->Announcement->InfoText)
				BlasterHUD->Announcement->InfoText->SetText(FText());
		}
	}

	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (BlasterCharacter && BlasterCharacter->GetCombatComponent())
	{
		BlasterCharacter->GetCombatComponent()->FireButtonPressed(false);
	}
}

void ABlasterPlayerController::HandleGameOver()
{
	// 对局结束：退出观战回自己视角（下一段读秒由 GameMode 处理）
	if (bSpectating)
	{
		StopSpectate();
	}

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		if (BlasterHUD->CharacterOverlay) BlasterHUD->CharacterOverlay->RemoveFromParent();
		if (BlasterHUD->Announcement)
		{
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
			// 和 PostRound 同一套：大字 WON / LOST（底色跟着换），下面的小字报是哪边赢的。
			// "Game Over" 这个阶段名不再用了。
			if (BlasterHUD->Announcement->AnnouncementText)
				BlasterHUD->Announcement->AnnouncementText->SetText(FText::FromString(
					DidLocalTeamWinRound() ? TEXT("WON") : TEXT("LOST")));

			// 这行要的是**整场**谁赢（比分高的那队），不是刚结束那回合 —— 和上面那个大字语义不同，
			// 别顺手改成 RoundWinnerTeam。这里读 GameState 的比分是安全的：GameOver 是在
			// PostRound 读秒走完之后才切的，比分早复制过来了，不存在上面那种"同帧抢跑"。
			FString WinnerText;
			const ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
			if (GS)
			{
				WinnerText = GS->TeamAScore > GS->TeamBScore ? TEXT("Attackers Win!") : TEXT("Defenders Win!");
			}
			if (BlasterHUD->Announcement->RoundResultText)
				BlasterHUD->Announcement->RoundResultText->SetText(FText::FromString(WinnerText));
		}
	}
}

void ABlasterPlayerController::ClientSyncPhaseTime_Implementation(float NewLevelStartingTime)
{
	LevelStartingTime = NewLevelStartingTime;
}

void ABlasterPlayerController::LobbyPollInit()
{
	if (bLobbyCheckDone) return;

	const FString MapName = GetWorld()->GetMapName();
	bInLobby = MapName.Contains(TEXT("Lobby"));

	if (!bInLobby)
	{
		bLobbyCheckDone = true;
		return;
	}

	// Only create for the local player (server also ticks remote client proxies)
	if (IsLocalController() && LobbyOverlayClass && !LobbyOverlay)
	{
		LobbyOverlay = CreateWidget<ULobbyOverlay>(this, LobbyOverlayClass);
		if (LobbyOverlay)
		{
			LobbyOverlay->AddToViewport();
		}
	}

	bLobbyCheckDone = true;
}

void ABlasterPlayerController::ApplyPlayerIdIfNeeded()
{
	if (bPlayerIdPushed) return;

	// 只由本机玩家推自己的名字。服务器上代表远端玩家的那个 PC 走不到这里 ——
	// 它读的是**服务器进程**的子系统，那里面没有别人的 ID（别人的 ID 在别人机器上）。
	if (!IsLocalController()) return;

	// 客户端这边 PlayerState 是复制过来的，可能晚一两帧；没到就下一帧再试
	APlayerState* PS = GetPlayerState<APlayerState>();
	if (!PS) return;

	bPlayerIdPushed = true;

	const UGameInstance* GameInstance = GetGameInstance();
	const UMultiplayerSessionsSubsystem* Sessions = GameInstance ? GameInstance->GetSubsystem<UMultiplayerSessionsSubsystem>() : nullptr;
	const FString Desired = Sessions ? Sessions->PlayerId : FString();

	// 没填就保持引擎默认名（主机名/Player 之类），不覆盖
	if (Desired.IsEmpty()) return;
	if (PS->GetPlayerName() == Desired) return;

	ServerSetPlayerId(Desired);
}

void ABlasterPlayerController::ServerSetPlayerId_Implementation(const FString& NewPlayerId)
{
	// 服务端再夹一次：RPC 参数是客户端说了算的，不能信
	FString Clean = NewPlayerId;
	Clean.TrimStartAndEndInline();
	Clean = Clean.Left(16);
	if (Clean.IsEmpty()) return;

	if (APlayerState* PS = GetPlayerState<APlayerState>())
	{
		// PlayerName 是 ReplicatedUsing 的，服务器改完会自动同步给所有人
		PS->SetPlayerName(Clean);
	}
}

void ABlasterPlayerController::PollInit()
{
	if (CharacterOverlay == nullptr)
	{
		if (BlasterHUD && BlasterHUD->CharacterOverlay)
		{
			CharacterOverlay = BlasterHUD->CharacterOverlay;
			if (CharacterOverlay)
			{
				if (HUDMaxHealth > 0.f)
				{
					SetHUDHealth(HUDHealth, HUDMaxHealth, HUDArmor, HUDMaxArmor);
				}
				SetHUDScore(HUDScore);
				SetHUDDefeats(HUDDefeats);

				// Init team HUD fields to avoid "TextBlock" defaults
				if (CharacterOverlay->TeamText)
					CharacterOverlay->TeamText->SetText(FText());
				if (CharacterOverlay->TeamScoreText)
					CharacterOverlay->TeamScoreText->SetText(FText());
				if (CharacterOverlay->AliveCountText)
					CharacterOverlay->AliveCountText->SetText(FText());
				if (CharacterOverlay->SpikeStatusText)
					CharacterOverlay->SpikeStatusText->SetText(FText());
				if (CharacterOverlay->SpikeTimerBar)
					CharacterOverlay->SpikeTimerBar->SetVisibility(ESlateVisibility::Hidden);
			}
		}
	}

	// Init announcement text fields
	if (BlasterHUD == nullptr) BlasterHUD = Cast<ABlasterHUD>(GetHUD());
	if (BlasterHUD && BlasterHUD->Announcement)
	{
		if (BlasterHUD->Announcement->RoundResultText && BlasterHUD->Announcement->RoundResultText->GetText().IsEmpty())
			BlasterHUD->Announcement->RoundResultText->SetText(FText());
		if (BlasterHUD->Announcement->TeamSwapText && BlasterHUD->Announcement->TeamSwapText->GetText().IsEmpty())
			BlasterHUD->Announcement->TeamSwapText->SetText(FText());
	}
}

void ABlasterPlayerController::ServerToggleReady_Implementation()
{
	ALobbyGameMode* LobbyGM = Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this));
	if (LobbyGM)
	{
		LobbyGM->TogglePlayerReady(this);
	}
}

void ABlasterPlayerController::ServerToggleAgent_Implementation()
{
	// F9：Jett → Sage → Phoenix → Clove 循环并原地重生。单一数据源 = PlayerState.Agent
	ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	EBlasterAgent Current = PS->GetAgent();
	if (!BlasterAgent::IsPlayable(Current))
	{
		// None（未选）→ 直接给 Jett
		Current = EBlasterAgent::Jett;
	}

	// ⚠️ 这是个手写环形链表，新英雄必须在这里接上 —— 漏了的话 Clove 不是"切不到"，
	//    而是**被静默当成 default 跳过去**（选到 Phoenix 后直接回 Jett），不报错。
	EBlasterAgent Next;
	switch (Current)
	{
	case EBlasterAgent::Sage:		Next = EBlasterAgent::Phoenix; break;
	case EBlasterAgent::Phoenix:	Next = EBlasterAgent::Clove; break;
	case EBlasterAgent::Clove:		Next = EBlasterAgent::Jett; break;
	case EBlasterAgent::Jett:
	default:						Next = EBlasterAgent::Sage; break;
	}
	PS->SetAgent(Next);

	// 这里 Destroy 角色 → PC 会收到 OnUnPossess，暮蝶的地图界面（如果有开）在那关掉。
	// 不关的话鼠标光标会留在屏幕上、输入模式卡在 GameAndUI。
	if (APawn* CurrentPawn = GetPawn())
	{
		CurrentPawn->Reset();
		CurrentPawn->Destroy();
	}

	if (ABlasterGameMode* GM = GetWorld()->GetAuthGameMode<ABlasterGameMode>())
	{
		GM->RestartPlayer(this);
	}
}

EBlasterAgent ABlasterPlayerController::GetAgent() const
{
	const ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
	return PS ? PS->GetAgent() : EBlasterAgent::None;
}

void ABlasterPlayerController::ServerSelectAgent_Implementation(EBlasterAgent NewAgent)
{
	// 校验：None（随机）或真实可玩英雄才接受
	if (!BlasterAgent::IsPlayable(NewAgent) && NewAgent != EBlasterAgent::None) return;

	ALobbyGameMode* LobbyGM = Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this));
	if (LobbyGM)
	{
		LobbyGM->SelectPlayerAgent(this, NewAgent);
	}
}

void ABlasterPlayerController::ServerStartGame_Implementation()
{
	ALobbyGameMode* LobbyGM = Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this));
	if (LobbyGM)
	{
		LobbyGM->StartGame();
	}
}

void ABlasterPlayerController::ServerSwitchTeam_Implementation(ETeam TargetTeam)
{
	ALobbyGameMode* LobbyGM = Cast<ALobbyGameMode>(UGameplayStatics::GetGameMode(this));
	if (LobbyGM)
	{
		LobbyGM->SwitchPlayerTeam(this, TargetTeam);
	}
}

void ABlasterPlayerController::AddBuyMenu()
{
	if (BuyMenu) return;
	if (!BuyMenuClass) return;
	// Widget 只能绑定本地玩家控制器；服务器对远端 PC 也会走到这里，必须跳过
	if (!IsLocalController()) return;

	BuyMenu = CreateWidget<UBuyMenu>(this, BuyMenuClass);
	if (BuyMenu)
	{
		BuyMenu->AddToViewport();
		BuyMenu->SetVisibility(ESlateVisibility::Hidden);
	}
}

void ABlasterPlayerController::ToggleBuyMenu()
{
	if (!BuyMenu) AddBuyMenu();
	if (!BuyMenu) return;

	const bool bIsVisible = BuyMenu->GetVisibility() == ESlateVisibility::Visible;
	if (bIsVisible)
	{
		CloseBuyMenu();
		return;
	}

	// 只在购买阶段允许打开
	if (MatchState != MatchState::BuyPhase) return;

	BuyMenu->SetVisibility(ESlateVisibility::Visible);
	bShowMouseCursor = true;
	SetInputMode(FInputModeGameAndUI());
}

void ABlasterPlayerController::CloseBuyMenu()
{
	if (BuyMenu)
	{
		BuyMenu->SetVisibility(ESlateVisibility::Hidden);
	}
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
}

// ============================ Clove 暮蝶：封烟选点 ============================

void ABlasterPlayerController::SmokeMapKeyPressed()
{
	// Lobby（选人）地图禁用技能键，和 ABlasterCharacter::Dash 同一道门。
	// Dash 那条路径本来就挡住了，但 PC 自己绑的 E（阵亡时走的那条）绕过了 Dash，
	// 这里补一道，省得以后多出一条调用路径时在选人界面弹出封烟地图。
	// 已经开着图时不挡 —— 那种情况下 E 是"关掉"，在哪张地图上都得能关掉。
	if (!bSmokeMapOpen && IsInLobby())
	{
		return;
	}

	// —— 没开图：还有充能才开 ——
	// 局部变量别叫 Character：AController 有个同名成员，会撞 C4458。
	if (!bSmokeMapOpen)
	{
		const ABlasterCharacter* Clove = Cast<ABlasterCharacter>(GetPawn());
		if (!Clove || !Clove->IsCloveSmokeAvailable())
		{
			return;
		}
		OpenSmokeMap();
		return;
	}

	// —— 开着 = 关掉 ——
	// E 只负责开关界面，**不封烟**（用户定义：左键选点、右键封烟）。
	// 已经点好的落点跟着一起丢掉（CloseSmokeMap 里清），下次开图重新选。
	CloseSmokeMap();
}

void ABlasterPlayerController::SmokeMapDeployPressed()
{
	// 右键：把已选的点一次性全放出去。由 UCloveSmokeMapWidget 在右键时回调。
	if (!bSmokeMapOpen) return;

	// 没点过就没得放 —— 右键在这种状态下什么也不做（界面照旧开着，
	// 不关掉：右键不是"退出"，退出只有 E）。事件仍由 widget 吃掉，
	// 不会漏给 viewport 变成 ADS。
	if (SmokePendingLocations.Num() == 0)
	{
		return;
	}

	if (ABlasterCharacter* Clove = Cast<ABlasterCharacter>(GetPawn()))
	{
		// 从开图到按确认之间充能可能已经没了（这期间被打死又复活 / 换了角色）。
		// 这里只是省一次白跑的 RPC，真正的把关在服务器那边 ——
		// 服务端会按**它自己**那一刻的剩余充能再截断一次数组。
		if (Clove->IsCloveSmokeAvailable())
		{
			// 一条 RPC 带上全部落点，不要在这里循环调单点版：
			// 拆成多条时中途丢弃/乱序会变成"只放了一个，另一层充能白扣"。
			Clove->ServerPlaceCloveSmokes(SmokePendingLocations);
		}
	}

	CloseSmokeMap();
}

void ABlasterPlayerController::OpenSmokeMap()
{
	if (bSmokeMapOpen) return;
	if (!IsLocalController()) return;

	if (!SmokeMap)
	{
		// 和 ScoreboardClass 完全同一套路：留空就用原生 C++ 控件。
		// 这个界面的布局全在 UCloveSmokeMapWidget::Initialize() 里用代码搭（根 Border + 自绘
		// 地图），本来就没有 WBP 可指派 —— 所以默认值必须是 StaticClass 而不是 nullptr，
		// 否则 PC 蓝图漏填一个字段的表现是「按 E 毫无反应」。
		const TSubclassOf<UCloveSmokeMapWidget> WidgetClass = SmokeMapWidgetClass
			? SmokeMapWidgetClass
			: TSubclassOf<UCloveSmokeMapWidget>(UCloveSmokeMapWidget::StaticClass());

		SmokeMap = CreateWidget<UCloveSmokeMapWidget>(this, WidgetClass);
	}
	if (!SmokeMap) return;

	// 用 Add/Remove 而不是 SetVisibility 开关：这个 widget 的 NativeTick 每帧都在
	// Invalidate(Paint)（鼠标光圈要跟着动），留在视口里的话即使隐藏也会让整个窗口
	// 每帧重绘 —— 相当于关着地图也在白白烧 GPU。
	SmokeMap->AddToViewport();

	bSmokeMapOpen = true;
	SmokeMapOwnerPawn = GetPawn();
	ClearSmokePendingLocations();

	// 抢鼠标 + 切输入模式（和 BuyMenu 同一套）。
	// GameAndUI 而不是 UIOnly：键盘还得留给游戏 —— 开着地图也能用 WASD 走位，
	// 而且 E 要能被角色/PC 的输入组件收到，UIOnly 会把键盘整个吃掉。
	bShowMouseCursor = true;
	SetInputMode(FInputModeGameAndUI());

	// 冻镜头。引擎里 AddPitchInput / AddYawInput 都带 !IsLookInputIgnored() 门禁
	//（PlayerController.cpp:5602-5610），所以这一句就够，不必去改角色的 Look()。
	SetIgnoreLookInput(true);
}

void ABlasterPlayerController::CloseSmokeMap()
{
	if (!bSmokeMapOpen) return;

	bSmokeMapOpen = false;
	SmokeMapOwnerPawn = nullptr;
	ClearSmokePendingLocations();

	if (SmokeMap)
	{
		SmokeMap->RemoveFromParent();
	}

	// 交还输入模式。若购买界面还开着（玩家开着地图期间按了 B），别把它一脚踢掉 ——
	// 它要的正是 GameAndUI + 光标，一刀切成 GameOnly 会让它点不动。
	if (BuyMenu && BuyMenu->GetVisibility() == ESlateVisibility::Visible)
	{
		bShowMouseCursor = true;
		SetInputMode(FInputModeGameAndUI());
	}
	else
	{
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
	}

	SetIgnoreLookInput(false);
}

void ABlasterPlayerController::OnSmokeKeyPressed()
{
	// 活着的时候，同一个 E 由角色的 Dash() 转发过来（见 ABlasterCharacter::Dash）。
	// 这里必须让路 —— PC 的输入组件压在栈顶且这条绑定是 bConsumeInput=false，
	// 两条路都会收到这次按键，不挡的话「开图」会被立刻执行的第二遍逻辑关掉。
	//
	// 走得通的前提是链路是单向的：角色活着 → 只有角色那条路干活；
	// 角色阵亡 → MulticastElim 里 DisableInput(PC) 把角色的输入组件摘出栈，只剩这里。
	if (const ABlasterCharacter* Clove = Cast<ABlasterCharacter>(GetPawn()))
	{
		if (!Clove->IsElimmed()) return;
	}

	SmokeMapKeyPressed();
}

void ABlasterPlayerController::SetSmokePendingLocation(const FVector& WorldLocation)
{
	if (!bSmokeMapOpen) return;

	// 满了就直接丢弃这次点击。**不替换**已选的点：替换会让界面上的圈和右键实际
	// 放出去的点对不上（玩家看到 2 个圈，以为放的是这两个，实际有 1 个被悄悄换掉了）。
	if (SmokePendingLocations.Num() >= GetSmokePendingCapacity()) return;

	SmokePendingLocations.Add(WorldLocation);
}

void ABlasterPlayerController::ClearSmokePendingLocations()
{
	SmokePendingLocations.Reset();
}

int32 ABlasterPlayerController::GetSmokePendingCapacity() const
{
	// 能选几个点 = 还剩几层充能。这条是**实时**的：充能回复/被别的路径消耗掉
	//（比如开着图的时候又死了一次）都会让这个数变，界面跟着变就行。
	//
	// 客户端这个数只决定"界面让不让你再点"和文案上的计数，不是安全边界 ——
	// 真正的把关在 ServerPlaceCloveSmokes 里（那边按服务器自己的充能数再截一次）。
	const ABlasterCharacter* Clove = Cast<ABlasterCharacter>(GetPawn());
	return Clove ? Clove->GetCloveSmokeCharges() : 0;
}

void ABlasterPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!InputComponent) return;

	// Tab 记分板：按住显示、松开隐藏。
	// 走原始按键绑定（本项目 F9/F10 调试键同款），不新增 Enhanced Input 资产。
	InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &ABlasterPlayerController::ShowScoreboard);
	InputComponent->BindKey(EKeys::Tab, IE_Released, this, &ABlasterPlayerController::HideScoreboard);

	// E：暮蝶封烟的**阵亡那条路**。为什么需要它 —— 阵亡时角色的输入组件被 DisableInput
	// 摘出栈（ABlasterCharacter::MulticastElim），角色的 E 收不到，而「死后放烟」是暮蝶
	// 的招牌。PC 这个绑定不受影响（同 Tab 记分板的道理）。
	//
	// ⚠️ bConsumeInput = false 是必须的。PC 的输入组件压在栈顶
	//（APlayerController::BuildInputStack 最后压入自己的），默认的 true 会把这个 E
	// 整个吞掉 —— 表现是 Sage 治疗 / Jett 冲刺 / Phoenix 曲线球**全部失灵**，
	// 而且不报任何错。
	//
	// 不吞之后两条路都会收到 E，互斥交给 OnSmokeKeyPressed 里的 IsElimmed 判断。
	InputComponent->BindKey(EKeys::E, IE_Pressed, this, &ABlasterPlayerController::OnSmokeKeyPressed).bConsumeInput = false;
}

void ABlasterPlayerController::ShowScoreboard()
{
	// Widget 只能绑本地玩家控制器（服务器对远端 PC 也会走到这里）
	if (!IsLocalController()) return;

	// Lobby 选人阶段没有对局记分板
	if (IsInLobby()) return;

	if (!Scoreboard)
	{
		const TSubclassOf<UScoreboardWidget> WidgetClass = ScoreboardClass
			? ScoreboardClass
			: TSubclassOf<UScoreboardWidget>(UScoreboardWidget::StaticClass());

		Scoreboard = CreateWidget<UScoreboardWidget>(this, WidgetClass);
		if (Scoreboard)
		{
			// 压在小地图/技能条/血条之上，被闪白屏(10)之下；自身 SelfHitTestInvisible 不吃输入
			Scoreboard->AddToViewport(5);
			Scoreboard->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	if (Scoreboard && !Scoreboard->IsScoreboardShown())
	{
		Scoreboard->ShowScoreboard();
	}
}

void ABlasterPlayerController::HideScoreboard()
{
	if (Scoreboard && Scoreboard->IsScoreboardShown())
	{
		Scoreboard->HideScoreboard();
	}
}

void ABlasterPlayerController::ServerBuyWeapon_Implementation(TSubclassOf<AWeapon> WeaponClass)
{
	if (!WeaponClass) return;

	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (!BlasterCharacter) return;

	// 只在购买阶段允许购买
	ABlasterGameMode* GM = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if (!GM || GM->GetMatchState() != MatchState::BuyPhase) return;

	// 空手蒙太奇（技能收尾那一段）不可打断，买枪也不例外。
	// ★ 必须挡在**扣钱之前**：下面是先 SpendCredits 再 EquipWeapon，而 EquipWeapon 走
	//   CanChangeWeapon()（不放行 ECS_EmptyHand）—— 挡晚了就是"钱花了、枪没到手"，
	//   而且不退款（只有 SpawnActor 失败才退）。掏枪中（ECS_Equip）那次踩过同一个坑，
	//   区别是那个状态**允许**换枪所以放行了，空手这个状态是彻底不许插进来。
	if (BlasterCharacter->IsEmptyHandLocked()) return;

	ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	AWeapon* DefaultWeapon = WeaponClass->GetDefaultObject<AWeapon>();
	if (!DefaultWeapon) return;

	if (!PS->SpendCredits(DefaultWeapon->GetWeaponCost())) return;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = BlasterCharacter;
	AWeapon* Weapon = GetWorld()->SpawnActor<AWeapon>(WeaponClass, SpawnParams);
	if (!Weapon)
	{
		PS->AddCredits(DefaultWeapon->GetWeaponCost()); // 生成失败退款
		return;
	}

	if (BlasterCharacter->GetCombatComponent())
	{
		BlasterCharacter->GetCombatComponent()->EquipWeapon(Weapon);
	}
}

void ABlasterPlayerController::ServerBuyArmor_Implementation(int32 ArmorAmount)
{
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (!BlasterCharacter) return;

	// 和 ServerBuyWeapon 同样的守卫：只在购买阶段能买
	ABlasterGameMode* GM = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if (!GM || GM->GetMatchState() != MatchState::BuyPhase) return;

	ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// 服务器自己把客户端报的档位**吸附到两档之一**，价格也按服务端表算 —— 不采信客户端那套装
	// （改过的客户端可能报 ArmorAmount=9999 / Cost=0）。注意 MaxArmor 只是封顶，
	// 客户端能买到多少完全由这两档决定。
	const bool bLight = ArmorAmount <= LightArmorAmount;
	const int32 TierArmor = bLight ? LightArmorAmount : HeavyArmorAmount;
	const int32 TierCost = bLight ? LightArmorCost : HeavyArmorCost;

	// 已经不低于这一档就别扣钱（穿着重甲再点一次重甲 = 白花钱）
	if (BlasterCharacter->GetArmor() >= TierArmor) return;

	if (!PS->SpendCredits(TierCost)) return;

	// 直接补到该档满值（Valorant 语义：买甲是把甲补到那一档，不是往上叠加）
	BlasterCharacter->SetArmor(static_cast<float>(FMath::Min(TierArmor, FMath::CeilToInt(BlasterCharacter->GetMaxArmor()))));
}

// --- 观战（死亡后观察存活队友）---
// 观战 = 把本机相机 SetViewTarget 到同队存活角色，用目标角色自带的 FollowCamera（弹簧臂）。
// 视角朝向两条路径：
//  - Host/权威机：被观察角色是权威 pawn、带真实 Controller → SpringArm(bUsePawnControlRotation)
//    取 GetViewRotation = 真实控制旋转，天然跟着目标瞄准转，无需额外处理。
//  - 网络客户端：被观察角色是 SimulatedProxy（无 Controller）→ Pawn::GetViewRotation 走
//    "正在被观战"分支，返回本 PC 的 BlendedTargetViewRotation。它由引擎平滑收敛到服务器
//    复制来的 TargetViewRotation（= 服务器侧目标的真实瞄准）。所以切目标时必须调
//    ServerSetSpectateTarget 让服务器把本 PC 的相机目标也切过去，这条复制链路才成立。

void ABlasterPlayerController::HandleLocalPlayerEliminated()
{
	if (!IsLocalController()) return;
	if (bSpectating || bSpectatePending) return;

	// 先留 SpectateDelay 秒看自己怎么死的/溶解，再进观战
	bSpectatePending = true;
	GetWorldTimerManager().SetTimer(
		SpectateStartTimer,
		this,
		&ABlasterPlayerController::SpectateStartTimerFinished,
		SpectateDelay,
		false);
}

void ABlasterPlayerController::SpectateStartTimerFinished()
{
	bSpectatePending = false;
	EnterSpectate();
}

void ABlasterPlayerController::EnterSpectate()
{
	if (!IsLocalController() || bSpectating) return;

	// 延迟窗口内已经复活（回合恰好结束）就不进
	ABlasterCharacter* MyChar = Cast<ABlasterCharacter>(GetPawn());
	if (!MyChar || !MyChar->IsElimmed()) return;

	bSpectating = true;
	RefreshSpectateCandidates();
	// 有存活队友 → 进观战从第 0 个开始（带个短 blend，从死亡点平滑拉过去）；
	// 全队阵亡 → 相机留在自己尸体，底部只显示"你已阵亡"提示，等复活 Tick 自动退出
	SelectSpectateTarget(0, true);
}

void ABlasterPlayerController::StopSpectate()
{
	// 无论是否正在观战，都先清掉"等待进观战"的延迟状态（跨回合 stale 防护：
	// 若在 1.5s 延迟窗口内回合结束复活，下次再死时必须能重新触发）
	GetWorldTimerManager().ClearTimer(SpectateStartTimer);
	bSpectatePending = false;

	if (!bSpectating) return;
	bSpectating = false;
	SpectateTarget = nullptr;
	SpectateCandidates.Reset();

	// 立刻切回自己（新回合复活后的角色）；不用 blend，避免从观战位飞回
	if (GetPawn())
	{
		SetViewTarget(GetPawn());
	}

	// 网络客户端：同步服务器把本 PC 相机目标切回我自己的 pawn，停掉 TargetViewRotation 复制
	if (!HasAuthority())
	{
		ServerSetSpectateTarget(nullptr);
	}
}

void ABlasterPlayerController::SpectateTick(float DeltaTime)
{
	// 复活 / 新回合：已经拥有未阵亡角色 → 退出观战回自己视角
	ABlasterCharacter* MyChar = Cast<ABlasterCharacter>(GetPawn());
	if (MyChar && !MyChar->IsElimmed())
	{
		if (bSpectating)
		{
			StopSpectate();
		}
		return;
	}

	if (!bSpectating) return;

	// 被观察目标死亡 / 失效（被打死或被销毁）→ 自动切下一个存活队友
	ABlasterCharacter* Target = SpectateTarget.Get();
	if (!Target || !IsValidSpectateTarget(Target))
	{
		AutoAdvanceSpectateTarget();
	}

	// 滚轮切换存活队友（死亡后角色 InputComponent 已被 DisableInput，但 PC 的原始按键
	// 仍在收：WasInputKeyJustPressed 从本帧按键状态读，Enhanced Input 也先喂给 UPlayerInput）
	if (WasInputKeyJustPressed(EKeys::MouseScrollDown))
	{
		CycleSpectateTarget(1);
	}
	else if (WasInputKeyJustPressed(EKeys::MouseScrollUp))
	{
		CycleSpectateTarget(-1);
	}
}

bool ABlasterPlayerController::IsValidSpectateTarget(const ABlasterCharacter* Target) const
{
	if (!Target || Target == GetPawn()) return false;
	if (Target->IsTestBot() || Target->IsElimmed()) return false;

	const ABlasterPlayerState* MyPS = GetPlayerState<ABlasterPlayerState>();
	const ABlasterPlayerState* OtherPS = Target->GetPlayerState<ABlasterPlayerState>();
	if (!MyPS || !OtherPS) return false;
	return MyPS->Team == OtherPS->Team;
}

void ABlasterPlayerController::RefreshSpectateCandidates()
{
	SpectateCandidates.Reset();

	const ABlasterPlayerState* MyPS = GetPlayerState<ABlasterPlayerState>();
	if (!MyPS) return;

	APawn* MyPawn = GetPawn();
	// 只遍历本客户端世界里已复制的角色（含远端代理）；服务器对远端 PC 的 Tick 不走这里
	for (TActorIterator<ABlasterCharacter> It(GetWorld()); It; ++It)
	{
		ABlasterCharacter* Char = *It;
		if (!Char || Char == MyPawn || Char->IsTestBot()) continue;
		if (Char->IsElimmed()) continue;

		const ABlasterPlayerState* OtherPS = Char->GetPlayerState<ABlasterPlayerState>();
		if (!OtherPS || OtherPS->Team != MyPS->Team) continue;

		SpectateCandidates.Add(Char);
	}

	// 按 PlayerId（加入顺序）排序，滚轮切人的顺序稳定
	SpectateCandidates.Sort([](const TWeakObjectPtr<ABlasterCharacter>& A, const TWeakObjectPtr<ABlasterCharacter>& B)
	{
		const ABlasterPlayerState* PSA = A.IsValid() ? A->GetPlayerState<ABlasterPlayerState>() : nullptr;
		const ABlasterPlayerState* PSB = B.IsValid() ? B->GetPlayerState<ABlasterPlayerState>() : nullptr;
		if (!PSA) return false;
		if (!PSB) return true;
		return PSA->GetPlayerId() < PSB->GetPlayerId();
	});
}

int32 ABlasterPlayerController::FindSpectateIndex(const ABlasterCharacter* Target) const
{
	for (int32 i = 0; i < SpectateCandidates.Num(); i++)
	{
		if (SpectateCandidates[i].Get() == Target) return i;
	}
	return -1;
}

void ABlasterPlayerController::SelectSpectateTarget(int32 Index, bool bBlend)
{
	if (!SpectateCandidates.IsValidIndex(Index)) return;

	ABlasterCharacter* NewTarget = SpectateCandidates[Index].Get();
	if (!NewTarget || NewTarget == SpectateTarget.Get()) return;

	SpectateTarget = NewTarget;
	if (bBlend)
	{
		SetViewTargetWithBlend(NewTarget, 0.35f, VTBlend_Cubic, 0.5f, true);
	}
	else
	{
		SetViewTarget(NewTarget);
	}

	// 网络客户端：让服务器把本 PC 相机目标也切到同一人，真实瞄准复制链路才成立
	if (!HasAuthority())
	{
		ServerSetSpectateTarget(NewTarget);
	}
}

void ABlasterPlayerController::AutoAdvanceSpectateTarget()
{
	if (!bSpectating) return;
	RefreshSpectateCandidates();
	if (SpectateCandidates.Num() == 0)
	{
		// 全队阵亡：清掉目标（观战栏回落到"你已阵亡"提示），相机留在原地等回合结束
		SpectateTarget = nullptr;
		return;
	}

	// 从当前目标向后找下一个；Base==-1（没有目标）→ 0
	const int32 Base = FindSpectateIndex(SpectateTarget.Get());
	const int32 Next = (Base + 1) % SpectateCandidates.Num();
	SelectSpectateTarget(Next, false);
}

void ABlasterPlayerController::CycleSpectateTarget(int32 Delta)
{
	if (!bSpectating) return;
	RefreshSpectateCandidates();
	if (SpectateCandidates.Num() == 0) return;

	int32 Base = FindSpectateIndex(SpectateTarget.Get());
	if (Base < 0) Base = 0;
	const int32 N = SpectateCandidates.Num();
	int32 Next = (Base + Delta) % N;
	if (Next < 0) Next += N;
	SelectSpectateTarget(Next, true);
}

bool ABlasterPlayerController::GetSpectateBarInfo(FString& OutMain, FString& OutSub, FLinearColor& OutColor) const
{
	OutMain = FString();
	OutSub = FString();
	OutColor = FLinearColor(1.f, 1.f, 1.f, 1.f);

	ABlasterCharacter* Target = SpectateTarget.Get();
	if (!Target)
	{
		OutMain = TEXT("ELIMINATED");
		OutSub = TEXT("Waiting for the round to end · You respawn as yourself");
		OutColor = FLinearColor(1.f, 1.f, 1.f, 0.8f);
		return true;
	}

	const ABlasterPlayerState* PS = Target->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return false;

	const FString Name = PS->GetPlayerName();
	const FString AgentName = BlasterAgent::ToDisplayName(PS->GetAgent());
	OutMain = Name.IsEmpty()
		? AgentName
		: FString::Printf(TEXT("%s · %s"), *Name, *AgentName);
	OutSub = TEXT("Scroll to switch teammate · You respawn as yourself");
	OutColor = BlasterAgent::GetAccentColor(PS->GetAgent());
	return true;
}

ABlasterCharacter* ABlasterPlayerController::GetSpectateTargetCharacter() const
{
	return SpectateTarget.IsValid() ? SpectateTarget.Get() : nullptr;
}

bool ABlasterPlayerController::IsInLobby() const
{
	// 实时按地图名判断，而不是缓存一次：ServerTravel 往返 Lobby/对局地图后也能正确切换。
	// 与 LobbyPollInit 判 Lobby 用同一规则（地图名含 "Lobby"）。
	const UWorld* World = GetWorld();
	return World && World->GetMapName().Contains(TEXT("Lobby"));
}

void ABlasterPlayerController::ServerSetSpectateTarget_Implementation(ABlasterCharacter* Target)
{
	if (!HasAuthority()) return;

	// 只在被观察者仍然有效时切过去（同队/存活/非机器人/不是自己）；否则切回自己的 pawn
	if (Target && IsValidSpectateTarget(Target))
	{
		// 服务器实体切到被观察者的权威 pawn。此后引擎每帧 TickActor 把
		// TargetPawn->GetViewRotation()（= 服务器侧的真实瞄准，Controller 有效）
		// 复制成 TargetViewRotation 发给死者客户端，其远端弹簧臂跟着转。
		SetViewTarget(Target);
	}
	else if (APawn* MyPawn = GetPawn())
	{
		// 停止观战 / 目标失效：切回我自己的 pawn（尸体，或下一回合已复活的新角色）
		SetViewTarget(MyPawn);
	}
}
