// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterHUD.h"
#include "MinimapWidget.h"
#include "SkillBarWidget.h"
#include "WindEffectWidget.h"
#include "FlashEffectWidget.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "CharacterOverlay.h"
#include "Announcement.h"
#include "LobbyOverlay.h"
#include "Components/TextBlock.h"
#include "Components/ProgressBar.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterTypes/Team.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/Canvas.h"
#include "Components/CapsuleComponent.h"

void ABlasterHUD::AddMinimapWidget()
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController() || MinimapWidget) return;

	// 若蓝图上挂了 WBP_Minimap 用蓝图类，否则退回纯 C++ 类（自绘逻辑相同）
	const TSubclassOf<UMinimapWidget> WidgetClass = MinimapWidgetClass ? MinimapWidgetClass : TSubclassOf<UMinimapWidget>(UMinimapWidget::StaticClass());
	MinimapWidget = CreateWidget<UMinimapWidget>(PC, WidgetClass);
	if (MinimapWidget)
	{
		MinimapWidget->AddToViewport();
	}
}

void ABlasterHUD::AddSkillBarWidget()
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController() || SkillBarWidget) return;

	// 若蓝图上挂了 WBP_SkillBar 用蓝图类，否则退回纯 C++ 类（自绘逻辑相同）
	const TSubclassOf<USkillBarWidget> WidgetClass = SkillBarWidgetClass ? SkillBarWidgetClass : TSubclassOf<USkillBarWidget>(USkillBarWidget::StaticClass());
	SkillBarWidget = CreateWidget<USkillBarWidget>(PC, WidgetClass);
	if (SkillBarWidget)
	{
		SkillBarWidget->AddToViewport();
	}
}

void ABlasterHUD::AddWindEffectWidget()
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController() || WindEffectWidget) return;

	const TSubclassOf<UWindEffectWidget> WidgetClass = WindEffectWidgetClass ? WindEffectWidgetClass : TSubclassOf<UWindEffectWidget>(UWindEffectWidget::StaticClass());
	WindEffectWidget = CreateWidget<UWindEffectWidget>(PC, WidgetClass);
	if (WindEffectWidget)
	{
		WindEffectWidget->AddToViewport();
	}
}

void ABlasterHUD::AddFlashEffectWidget()
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController() || FlashEffectWidget) return;

	const TSubclassOf<UFlashEffectWidget> WidgetClass = FlashEffectWidgetClass ? FlashEffectWidgetClass : TSubclassOf<UFlashEffectWidget>(UFlashEffectWidget::StaticClass());
	FlashEffectWidget = CreateWidget<UFlashEffectWidget>(PC, WidgetClass);
	if (FlashEffectWidget)
	{
		// 压在所有 UI 之上（高 ZOrder）：被闪时白屏应盖住准星/技能条/血条等一切
		FlashEffectWidget->AddToViewport(10);
	}
}

void ABlasterHUD::AddCharacterOverlay()
{
	APlayerController* PlayerController = GetOwningPlayerController();
	if (PlayerController && CharacterOverlayClass)
	{
		CharacterOverlay = CreateWidget<UCharacterOverlay>(PlayerController, CharacterOverlayClass);
		CharacterOverlay->AddToViewport();

		// Init new team fields to avoid "TextBlock" defaults
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

void ABlasterHUD::AddAnnouncement()
{
	APlayerController* PlayerController = GetOwningPlayerController();
	if (PlayerController && AnnouncementClass && !Announcement)
	{
		Announcement = CreateWidget<UAnnouncement>(PlayerController, AnnouncementClass);
		Announcement->AddToViewport();

		// Init new round result fields
		if (Announcement->RoundResultText)
			Announcement->RoundResultText->SetText(FText());
		if (Announcement->TeamSwapText)
			Announcement->TeamSwapText->SetText(FText());
	}
}

void ABlasterHUD::AddLobbyOverlay()
{
	APlayerController* PlayerController = GetOwningPlayerController();
	if (PlayerController && LobbyOverlayClass && !LobbyOverlay)
	{
		LobbyOverlay = CreateWidget<ULobbyOverlay>(PlayerController, LobbyOverlayClass);
		LobbyOverlay->AddToViewport();
	}
}

void ABlasterHUD::RemoveLobbyOverlay()
{
	if (LobbyOverlay)
	{
		LobbyOverlay->RemoveFromParent();
		LobbyOverlay = nullptr;
	}
}

void ABlasterHUD::DrawHUD()
{
	Super::DrawHUD();

	// 击杀信息：懒绑定 GameState 的 KillFeed 多播（数据经 RPC 进来，只绑一次）
	if (!bKillFeedBound)
	{
		ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
		if (GS)
		{
			GS->OnKillFeedEntry.AddDynamic(this, &ABlasterHUD::OnKillFeedEntry);
			bKillFeedBound = true;
		}
	}

	FVector2D ViewportSize;
	if (GEngine)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		const FVector2D ViewportCenter = FVector2D(ViewportSize.X / 2, ViewportSize.Y / 2);

		// 观战中：画面是队友第三人称，自己的准星/Sage 治疗选中都不画
		ABlasterPlayerController* SpectateOwningPC = Cast<ABlasterPlayerController>(GetOwningPlayerController());
		const bool bSpectating = SpectateOwningPC && SpectateOwningPC->IsSpectating();

		float SpreadScaled = CrosshairSpreadMax * HUDPackage.CrosshairSpread;

		if (!bSpectating && HUDPackage.CrosshairCenter)
		{
			FVector2D Spread(0.f,0.f);
			DrawCrosshair(HUDPackage.CrosshairCenter,ViewportCenter,Spread,HUDPackage.CrosshairsColor);
		}
		if (!bSpectating && HUDPackage.CrosshairLeft)
		{
			FVector2D Spread(-SpreadScaled,0.f);
			DrawCrosshair(HUDPackage.CrosshairLeft,ViewportCenter,Spread,HUDPackage.CrosshairsColor);
		}
		if (!bSpectating && HUDPackage.CrosshairRight)
		{
			FVector2D Spread(SpreadScaled,0.f);
			DrawCrosshair(HUDPackage.CrosshairRight,ViewportCenter,Spread,HUDPackage.CrosshairsColor);
		}
		if (!bSpectating && HUDPackage.CrosshairTop)
		{
			FVector2D Spread(0.f,-SpreadScaled);
			DrawCrosshair(HUDPackage.CrosshairTop,ViewportCenter,Spread,HUDPackage.CrosshairsColor);
		}
		if (!bSpectating && HUDPackage.CrosshairBottom)
		{
			FVector2D Spread(0.f,SpreadScaled);
			DrawCrosshair(HUDPackage.CrosshairBottom,ViewportCenter,Spread,HUDPackage.CrosshairsColor);
		}

		// 射击反馈：命中标记 / 漂浮伤害数字 / 受击方向指示 / 击杀信息 / 击杀确认标记
		DrawHitMarker(ViewportCenter);
		DrawDamageNumbers();
		DrawDamageDirections(ViewportSize);
		DrawKillFeed(ViewportSize);
		DrawKillMarker(ViewportSize);

		// Sage 治疗选中：准心下的队友血条 + 选中态小十字（观战中不画）
		if (!bSpectating)
		{
			DrawSageHealOverlay(ViewportCenter);
		}

		// 观战栏（阵亡后观察队友）：底部中央 玩家名·英雄 + 操作提示
		DrawSpectateBar(ViewportSize);
	}

	// 小地图已解耦为 UMinimapWidget（首帧懒创建，之后由 widget 自绘）
	if (!MinimapWidget)
	{
		AddMinimapWidget();
	}

	// 技能条（技能充能/冷却，首帧懒创建，之后由 widget 自绘）
	if (!SkillBarWidget)
	{
		AddSkillBarWidget();
	}

	// 视角风特效（技能武装逐风时全屏风丝，首帧懒创建，武装时自动显示）
	if (!WindEffectWidget)
	{
		AddWindEffectWidget();
	}

	// 被闪白屏（收到 GE_FlashBlind 期间全屏白闪，首帧懒创建，被闪时自动显示）
	if (!FlashEffectWidget)
	{
		AddFlashEffectWidget();
	}

	// 技能条在 Lobby（选人）地图整块隐藏：widget 跨地图常驻（只懒创建一次），
	// 回到对局地图时由实时地图名判断恢复显示。
	if (SkillBarWidget)
	{
		ABlasterPlayerController* HudPC = Cast<ABlasterPlayerController>(GetOwningPlayerController());
		const bool bLobby = HudPC && HudPC->IsInLobby();
		SkillBarWidget->SetVisibility(bLobby ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
}

// --- 射击反馈 ---

void ABlasterHUD::ShowHitMarker()
{
	HitMarkerTime = GetWorld()->GetTimeSeconds();
}

void ABlasterHUD::AddDamageNumber(float Damage, const FVector& WorldLocation)
{
	FDamageNumberEntry Entry;
	Entry.Damage = Damage;
	Entry.WorldLocation = WorldLocation;
	Entry.SpawnTime = GetWorld()->GetTimeSeconds();
	DamageNumbers.Add(Entry);
	if (DamageNumbers.Num() > 30) DamageNumbers.RemoveAt(0);
}

void ABlasterHUD::AddDamageDirection(const FVector& DamageOrigin)
{
	FDamageDirectionEntry Entry;
	Entry.DamageOrigin = DamageOrigin;
	Entry.SpawnTime = GetWorld()->GetTimeSeconds();
	DamageDirections.Add(Entry);
	if (DamageDirections.Num() > 5) DamageDirections.RemoveAt(0);
}

void ABlasterHUD::AddKillFeedEntry(const FString& KillerName, const FString& VictimName)
{
	FKillFeedEntry Entry;
	Entry.KillerName = KillerName;
	Entry.VictimName = VictimName;
	Entry.SpawnTime = GetWorld()->GetTimeSeconds();
	KillFeedEntries.Add(Entry);
	if (KillFeedEntries.Num() > 10) KillFeedEntries.RemoveAt(0);
}

void ABlasterHUD::OnKillFeedEntry(const FString& KillerName, const FString& VictimName)
{
	AddKillFeedEntry(KillerName, VictimName);
}

void ABlasterHUD::ShowKillMarker(int32 RoundKills)
{
	KillMarkerTier = FMath::Clamp(RoundKills, 1, 5);
	KillMarkerTime = GetWorld()->GetTimeSeconds();
}

void ABlasterHUD::HideKillMarker()
{
	KillMarkerTier = 0;
}

void ABlasterHUD::DrawKillMarker(const FVector2D& ViewportSize)
{
	if (KillMarkerTier <= 0) return;

	const float Now = GetWorld()->GetTimeSeconds();
	const float Elapsed = Now - KillMarkerTime;
	if (Elapsed >= KillMarkerPopDuration + KillMarkerHoldTime + KillMarkerFadeTime)
	{
		KillMarkerTier = 0; // 动画播完自动清零
		return;
	}

	// 弹出缩放：PopDuration 内 1.0 → 1.28 → 1.0（overshoot）
	float Scale = 1.f;
	if (Elapsed < KillMarkerPopDuration)
	{
		const float P = Elapsed / KillMarkerPopDuration;
		Scale = 1.f + 0.28f * FMath::Sin(P * PI);
	}
	// 停留后渐隐
	float Alpha = 1.f;
	if (Elapsed >= KillMarkerPopDuration + KillMarkerHoldTime)
	{
		Alpha = 1.f - (Elapsed - KillMarkerPopDuration - KillMarkerHoldTime) / KillMarkerFadeTime;
	}
	Alpha = FMath::Clamp(Alpha, 0.f, 1.f);

	// 屏幕中下方（距底边约 90px）
	const FVector2D Center(ViewportSize.X * 0.5f, ViewportSize.Y - 90.f);
	const float Size = 30.f * Scale;

	FLinearColor Color;
	switch (KillMarkerTier)
	{
	case 1: Color = FLinearColor(1.f, 1.f, 1.f, Alpha); break;
	case 2: Color = FLinearColor(1.f, 1.f, 1.f, Alpha); break;
	case 3: Color = FLinearColor(1.f, 0.90f, 0.70f, Alpha); break;
	case 4: Color = FLinearColor(1.f, 0.85f, 0.50f, Alpha); break;
	default: Color = FLinearColor(1.f, 0.78f, 0.25f, Alpha); break;
	}
	const float Thickness = 3.f;

	DrawKillMarkerShape(Center, Size, KillMarkerTier, Color, Thickness);
}

void ABlasterHUD::DrawKillLine(const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness)
{
	DrawLine(A.X, A.Y, B.X, B.Y, Color, Thickness);
}

void ABlasterHUD::DrawKillMarkerShape(const FVector2D& Center, float Size, int32 Tier, const FLinearColor& Color, float Thickness)
{
	const float S = Size;
	switch (Tier)
	{
	case 1: // 1杀：X
	{
		DrawKillLine(Center + FVector2D(-S * 0.8f, -S * 0.8f), Center + FVector2D(S * 0.8f, S * 0.8f), Color, Thickness);
		DrawKillLine(Center + FVector2D(S * 0.8f, -S * 0.8f), Center + FVector2D(-S * 0.8f, S * 0.8f), Color, Thickness);
		break;
	}
	case 2: // 2杀：菱形
	{
		const FVector2D A(Center.X, Center.Y - S);
		const FVector2D B(Center.X + S, Center.Y);
		const FVector2D C(Center.X, Center.Y + S);
		const FVector2D D(Center.X - S, Center.Y);
		DrawKillLine(A, B, Color, Thickness);
		DrawKillLine(B, C, Color, Thickness);
		DrawKillLine(C, D, Color, Thickness);
		DrawKillLine(D, A, Color, Thickness);
		break;
	}
	case 3: // 3杀：菱形 + 内三角
	{
		const FVector2D A(Center.X, Center.Y - S);
		const FVector2D B(Center.X + S, Center.Y);
		const FVector2D C(Center.X, Center.Y + S);
		const FVector2D D(Center.X - S, Center.Y);
		DrawKillLine(A, B, Color, Thickness);
		DrawKillLine(B, C, Color, Thickness);
		DrawKillLine(C, D, Color, Thickness);
		DrawKillLine(D, A, Color, Thickness);
		const FVector2D T1(Center.X, Center.Y - S * 0.45f);
		const FVector2D T2(Center.X + S * 0.45f, Center.Y + S * 0.45f);
		const FVector2D T3(Center.X - S * 0.45f, Center.Y + S * 0.45f);
		DrawKillLine(T1, T2, Color, Thickness);
		DrawKillLine(T2, T3, Color, Thickness);
		DrawKillLine(T3, T1, Color, Thickness);
		break;
	}
	case 4: // 4杀：菱形 + 左右小三角翼
	{
		const FVector2D A(Center.X, Center.Y - S);
		const FVector2D B(Center.X + S, Center.Y);
		const FVector2D C(Center.X, Center.Y + S);
		const FVector2D D(Center.X - S, Center.Y);
		DrawKillLine(A, B, Color, Thickness);
		DrawKillLine(B, C, Color, Thickness);
		DrawKillLine(C, D, Color, Thickness);
		DrawKillLine(D, A, Color, Thickness);
		const FVector2D LW1(Center.X - S * 1.25f, Center.Y - S * 0.15f);
		const FVector2D LW2(Center.X - S * 0.72f, Center.Y);
		const FVector2D LW3(Center.X - S * 1.25f, Center.Y + S * 0.35f);
		DrawKillLine(LW1, LW2, Color, Thickness);
		DrawKillLine(LW2, LW3, Color, Thickness);
		DrawKillLine(LW3, LW1, Color, Thickness);
		const FVector2D RW1(Center.X + S * 1.25f, Center.Y - S * 0.15f);
		const FVector2D RW2(Center.X + S * 0.72f, Center.Y);
		const FVector2D RW3(Center.X + S * 1.25f, Center.Y + S * 0.35f);
		DrawKillLine(RW1, RW2, Color, Thickness);
		DrawKillLine(RW2, RW3, Color, Thickness);
		DrawKillLine(RW3, RW1, Color, Thickness);
		break;
	}
	default: // 5杀+：金色五星 + 外六边形
	{
		// 五星（外顶点 5 个 + 内顶点 5 个交替）
		FVector2D Star[10];
		const float OuterR = S * 1.05f;
		const float InnerR = S * 0.42f;
		for (int32 i = 0; i < 10; ++i)
		{
			const float R = (i % 2 == 0) ? OuterR : InnerR;
			const float Angle = -PI / 2.f + i * PI / 5.f;
			Star[i] = Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * R;
		}
		for (int32 i = 0; i < 10; ++i)
		{
			DrawKillLine(Star[i], Star[(i + 1) % 10], Color, Thickness + 0.5f);
		}
		// 外六边形
		FVector2D Hex[6];
		for (int32 i = 0; i < 6; ++i)
		{
			const float Angle = -PI / 2.f + i * 2.f * PI / 6.f;
			Hex[i] = Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * (S * 1.45f);
		}
		for (int32 i = 0; i < 6; ++i)
		{
			DrawKillLine(Hex[i], Hex[(i + 1) % 6], Color, Thickness);
		}
		break;
	}
	}
}

void ABlasterHUD::DrawSageHealOverlay(const FVector2D& ViewportCenter)
{
	APlayerController* PC = GetOwningPlayerController();
	ABlasterCharacter* Char = PC ? Cast<ABlasterCharacter>(PC->GetPawn()) : nullptr;
	if (!Char) return;
	const bool bHealSelect = Char->IsSageHealSelecting();
	const bool bHoldFlash = Char->IsCurveballHolding();
	if (!bHealSelect && !bHoldFlash) return;

	// 选中态/持闪光态准星：无枪械准星，画屏幕中央小十字；Sage 指向可治疗目标时变绿
	const ABlasterCharacter* Target = bHealSelect ? Char->GetSageHealTarget() : nullptr;
	const FLinearColor CrossColor = Target
		? FLinearColor(0.30f, 1.f, 0.50f, 1.f)
		: FLinearColor(1.f, 1.f, 1.f, 0.90f);
	const float CS = 11.f;
	const float CT = 2.f;
	DrawLine(ViewportCenter.X - CS, ViewportCenter.Y, ViewportCenter.X - 5.f, ViewportCenter.Y, CrossColor, CT);
	DrawLine(ViewportCenter.X + 5.f, ViewportCenter.Y, ViewportCenter.X + CS, ViewportCenter.Y, CrossColor, CT);
	DrawLine(ViewportCenter.X, ViewportCenter.Y - CS, ViewportCenter.X, ViewportCenter.Y - 5.f, CrossColor, CT);
	DrawLine(ViewportCenter.X, ViewportCenter.Y + 5.f, ViewportCenter.X, ViewportCenter.Y + CS, CrossColor, CT);

	if (!Target) return;

	// 队友头顶血条：世界坐标（头顶）→ 屏幕
	const float HeadZ = Target->GetCapsuleComponent() ? Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 15.f : 110.f;
	const FVector Projected = Project(Target->GetActorLocation() + FVector(0.f, 0.f, HeadZ));
	if (Projected.Z < 0.f) return; // 在相机背后

	const float BarW = 140.f;
	const float BarH = 13.f;
	const float X = Projected.X - BarW * 0.5f;
	const float Y = Projected.Y;
	const float Frac = FMath::Clamp(Target->GetHealth() / FMath::Max(1.f, Target->GetMaxHealth()), 0.f, 1.f);

	// 背景 + 血量填充 + 白色细边框
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.60f), X, Y, BarW, BarH);
	if (Frac > 0.01f)
	{
		const FLinearColor FillColor = Frac > 0.35f
			? FLinearColor(0.30f, 0.95f, 0.45f, 0.95f)
			: FLinearColor(1.f, 0.40f, 0.30f, 0.95f);
		DrawRect(FillColor, X, Y, BarW * Frac, BarH);
	}
	const FLinearColor Border(1.f, 1.f, 1.f, 0.70f);
	DrawLine(X, Y, X + BarW, Y, Border, 1.5f);
	DrawLine(X, Y, X, Y + BarH, Border, 1.5f);
	DrawLine(X + BarW, Y, X + BarW, Y + BarH, Border, 1.5f);
	DrawLine(X, Y + BarH, X + BarW, Y + BarH, Border, 1.5f);

	// 数值（条上方居中）
	if (Canvas)
	{
		const FString Txt = FString::Printf(TEXT("%d / %d"),
			(int32)FMath::CeilToFloat(Target->GetHealth()), (int32)Target->GetMaxHealth());
		UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
		if (Font)
		{
			float TxtW = 0.f, TxtH = 0.f;
			Canvas->TextSize(Font, Txt, TxtW, TxtH);
			Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, 0.95f));
			Canvas->DrawText(Font, Txt, X + (BarW - TxtW) * 0.5f, Y - TxtH - 2.f, 1.f, 1.f);
		}
	}
}

void ABlasterHUD::DrawHitMarker(FVector2D ViewportCenter)
{
	const float Now = GetWorld()->GetTimeSeconds();
	const float Elapsed = Now - HitMarkerTime;
	if (HitMarkerTime <= 0.f || Elapsed >= HitMarkerDuration) return;

	// 命中瞬间准星中央闪现白色 X，随持续时间渐隐
	const float Alpha = 1.f - Elapsed / HitMarkerDuration;
	const float Size = 14.f;
	const float Thickness = 2.f;
	const FLinearColor Color(1.f, 1.f, 1.f, Alpha);

	DrawLine(ViewportCenter.X - Size, ViewportCenter.Y - Size, ViewportCenter.X + Size, ViewportCenter.Y + Size, Color, Thickness);
	DrawLine(ViewportCenter.X + Size, ViewportCenter.Y - Size, ViewportCenter.X - Size, ViewportCenter.Y + Size, Color, Thickness);
}

void ABlasterHUD::DrawDamageNumbers()
{
	if (DamageNumbers.Num() == 0) return;

	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->GetPawn()) return;

	const float Now = GetWorld()->GetTimeSeconds();
	const float Lifetime = 1.f;
	for (int32 i = DamageNumbers.Num() - 1; i >= 0; --i)
	{
		FDamageNumberEntry& Entry = DamageNumbers[i];
		const float Elapsed = Now - Entry.SpawnTime;
		if (Elapsed >= Lifetime)
		{
			DamageNumbers.RemoveAt(i);
			continue;
		}

		// 世界坐标 → 屏幕，1s 内上飘 60px + 渐隐（Project 返回 Z<0 表示在相机背后，跳过）
		const FVector Projected = Project(Entry.WorldLocation);
		if (Projected.Z >= 0.f)
		{
			const float Progress = Elapsed / Lifetime;
			FVector2D ScreenPos(Projected.X, Projected.Y);
			ScreenPos.Y -= Progress * 60.f;
			const float Alpha = 1.f - Progress;
			const FString Text = FString::Printf(TEXT("-%.0f"), Entry.Damage);
			if (Canvas)
			{
				Canvas->SetLinearDrawColor(FLinearColor(1.f, 0.25f, 0.25f, Alpha));
				UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
				Canvas->DrawText(Font, Text, ScreenPos.X, ScreenPos.Y, 1.f, 1.f);
			}
		}
	}
}

void ABlasterHUD::DrawDamageDirections(const FVector2D& ViewportSize)
{
	if (DamageDirections.Num() == 0) return;

	APlayerController* PC = GetOwningPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!PC || !Pawn) return;

	const FVector2D Center = ViewportSize / 2.f;
	const float Now = GetWorld()->GetTimeSeconds();
	const float Lifetime = 1.5f;
	const float FadeStart = 0.9f;

	for (int32 i = DamageDirections.Num() - 1; i >= 0; --i)
	{
		FDamageDirectionEntry& Entry = DamageDirections[i];
		const float Elapsed = Now - Entry.SpawnTime;
		if (Elapsed >= Lifetime)
		{
			DamageDirections.RemoveAt(i);
			continue;
		}
		float Alpha = 1.f;
		if (Elapsed >= FadeStart)
		{
			Alpha = 1.f - (Elapsed - FadeStart) / (Lifetime - FadeStart);
		}

		// 世界方向（水平）→ 相对相机 yaw 的角度（正 = 攻击者在右侧）
		FVector ToAttacker = Entry.DamageOrigin - Pawn->GetActorLocation();
		ToAttacker.Z = 0.f;
		if (ToAttacker.IsNearlyZero()) continue;
		ToAttacker.Normalize();
		const float CamYaw = Pawn->GetControlRotation().Yaw;
		const float AttackerYaw = FMath::RadiansToDegrees(FMath::Atan2(ToAttacker.Y, ToAttacker.X));
		const float RelYaw = FRotator::NormalizeAxis(AttackerYaw - CamYaw);

		// 屏幕方向：+X=右、+Y=下，正相对角（右）→ +X 方向
		const float Rad = FMath::DegreesToRadians(RelYaw);
		const FVector2D Dir(FMath::Sin(Rad), -FMath::Cos(Rad));

		// 沿该方向把弧推到屏幕边缘：从中心发一条射线，求它首次触到
		// "内缩 (弧半径+边距) 的四条边" 的距离 t，取最小者 → 整段弧完整露出在边缘。
		// ⚠️ 曾用 Scale=1 起步再 Min（MaxX/|Dir.X| 是几百），恒停在 1 → 弧永远画在屏幕中心。
		const float ArcRadius = DamageArcRadius;
		const float Lead = ArcRadius + DamageArcMargin;   // 弧最外缘离屏幕边的空隙
		float Dist = 0.f;                                  // 中心→边界线的单位方向参数距离
		if (FMath::Abs(Dir.X) > KINDA_SMALL_NUMBER)
		{
			const float ReachX = (Dir.X > 0.f)
				? (ViewportSize.X - Lead - Center.X) / Dir.X
				: (Center.X - Lead) / (-Dir.X);
			Dist = (Dist > 0.f) ? FMath::Min(Dist, ReachX) : ReachX;
		}
		if (FMath::Abs(Dir.Y) > KINDA_SMALL_NUMBER)
		{
			const float ReachY = (Dir.Y > 0.f)
				? (ViewportSize.Y - Lead - Center.Y) / Dir.Y
				: (Center.Y - Lead) / (-Dir.Y);
			Dist = (Dist > 0.f) ? FMath::Min(Dist, ReachY) : ReachY;
		}
		const FVector2D Pos = Center + Dir * Dist;

		// 红色弧段（朝向伤害来源那侧的半圆弧），半径/张开角/粗细可调
		const FLinearColor Color(1.f, 0.2f, 0.2f, Alpha);
		const float HalfArc = FMath::DegreesToRadians(DamageArcHalfAngle);
		const int32 Segments = 22;
		const float BaseAngle = FMath::Atan2(Dir.Y, Dir.X);

		FVector2D Prev = Pos + FVector2D(FMath::Cos(BaseAngle - HalfArc), FMath::Sin(BaseAngle - HalfArc)) * ArcRadius;
		for (int32 s = 1; s <= Segments; ++s)
		{
			const float A = BaseAngle - HalfArc + (2.f * HalfArc) * s / Segments;
			const FVector2D P = Pos + FVector2D(FMath::Cos(A), FMath::Sin(A)) * ArcRadius;
			DrawLine(Prev.X, Prev.Y, P.X, P.Y, Color, DamageArcThickness);
			Prev = P;
		}
		// 弧中央一小段指针，指明精确方向
		const float Tick = 14.f;
		DrawLine(Pos.X + Dir.X * (ArcRadius - Tick), Pos.Y + Dir.Y * (ArcRadius - Tick),
			Pos.X + Dir.X * (ArcRadius + 4.f), Pos.Y + Dir.Y * (ArcRadius + 4.f), Color, DamageArcThickness + 1.f);
	}
}

void ABlasterHUD::DrawKillFeed(const FVector2D& ViewportSize)
{
	if (KillFeedEntries.Num() == 0) return;

	const float Now = GetWorld()->GetTimeSeconds();
	const float Lifetime = 4.f;
	const float StartX = ViewportSize.X - 280.f;
	const float StartY = 60.f;
	const float LineHeight = 22.f;

	// 最新的在最上面（数组头）
	int32 LineIndex = 0;
	for (int32 i = KillFeedEntries.Num() - 1; i >= 0; --i)
	{
		FKillFeedEntry& Entry = KillFeedEntries[i];
		const float Elapsed = Now - Entry.SpawnTime;
		if (Elapsed >= Lifetime)
		{
			KillFeedEntries.RemoveAt(i);
			continue;
		}
		const float Alpha = FMath::Clamp(1.f - (Elapsed / Lifetime), 0.f, 1.f);
		const FString Text = FString::Printf(TEXT("%s  x  %s"), *Entry.KillerName, *Entry.VictimName);
		if (Canvas)
		{
			Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, Alpha));
			UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
			Canvas->DrawText(Font, Text, StartX, StartY + LineIndex * LineHeight, 1.f, 1.f);
		}
		++LineIndex;
	}
}

void ABlasterHUD::DrawSpectateBar(const FVector2D& ViewportSize)
{
	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetOwningPlayerController());
	if (!BPC || !BPC->IsSpectating()) return;

	FString Main, Sub;
	FLinearColor Color;
	if (!BPC->GetSpectateBarInfo(Main, Sub, Color)) return;

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font) return;

	// 主行（玩家名·英雄）大号、副行（操作提示）小号，堆在底部中央
	float MainW = 0.f, MainH = 0.f;
	float SubW = 0.f, SubH = 0.f;
	GetTextSize(Main, MainW, MainH, Font, 1.35f);
	GetTextSize(Sub, SubW, SubH, Font, 0.8f);

	const float MainX = (ViewportSize.X - MainW) * 0.5f;
	const float MainY = ViewportSize.Y - MainH - 48.f;
	const float SubX = (ViewportSize.X - SubW) * 0.5f;
	const float SubY = MainY - SubH - 6.f;

	DrawText(Sub, FLinearColor(1.f, 1.f, 1.f, 0.7f), SubX, SubY, Font, 0.8f, false);
	DrawText(Main, Color, MainX, MainY, Font, 1.35f, false);
}

// --- 射击反馈 end ---



void ABlasterHUD::DrawCrosshair(UTexture2D* Texture, FVector2D ViewportCenter,FVector2D Spread,FLinearColor CrosshairColor)
{
	const float TextureWidth = Texture->GetSizeX();
	const float TextureHeight = Texture->GetSizeY();
	const FVector2D TextureDrawPoint = FVector2D(
		ViewportCenter.X-(TextureWidth/2.f)+Spread.X,
		ViewportCenter.Y-(TextureHeight/2.f)+Spread.Y
		);
	DrawTexture(
		Texture,
		TextureDrawPoint.X,
		TextureDrawPoint.Y,
		TextureWidth,
		TextureHeight,
		0.f,
		0.f,
		1.f,
		1.f,
		CrosshairColor
		);
}

