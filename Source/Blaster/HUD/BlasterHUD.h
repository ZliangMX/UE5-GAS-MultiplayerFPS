// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BlasterHUD.generated.h"

USTRUCT(BlueprintType)
struct FHUDPackage
{
	GENERATED_BODY()
public:
	class UTexture2D* CrosshairCenter;
	UTexture2D* CrosshairLeft;
	UTexture2D* CrosshairRight;
	UTexture2D* CrosshairTop;
	UTexture2D* CrosshairBottom;
	float CrosshairSpread;
	FLinearColor CrosshairsColor;
};
/**
 * 
 */
UCLASS()
class BLASTER_API ABlasterHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	// --- 小地图（已解耦为 UMinimapWidget，纯 NativePaint 自绘）---
	UPROPERTY(EditAnywhere, Category = "Minimap")
	TSubclassOf<class UMinimapWidget> MinimapWidgetClass;

	UPROPERTY()
	class UMinimapWidget* MinimapWidget;

	void AddMinimapWidget();

	// --- 技能条（技能充能/冷却，USkillBarWidget NativePaint 自绘）---
	UPROPERTY(EditAnywhere, Category = "Abilities")
	TSubclassOf<class USkillBarWidget> SkillBarWidgetClass;

	UPROPERTY()
	class USkillBarWidget* SkillBarWidget;

	void AddSkillBarWidget();

	// --- 视角风特效（技能武装逐风时全屏风丝，UWindEffectWidget NativePaint 自绘）---
	UPROPERTY(EditAnywhere, Category = "Abilities")
	TSubclassOf<class UWindEffectWidget> WindEffectWidgetClass;

	UPROPERTY()
	class UWindEffectWidget* WindEffectWidget;

	void AddWindEffectWidget();

	// --- 被闪白屏（收到 GE_FlashBlind 期间全屏白闪，UFlashEffectWidget NativePaint 自绘）---
	UPROPERTY(EditAnywhere, Category = "Abilities")
	TSubclassOf<class UFlashEffectWidget> FlashEffectWidgetClass;

	UPROPERTY()
	class UFlashEffectWidget* FlashEffectWidget;

	void AddFlashEffectWidget();

	UPROPERTY(EditAnywhere,Category="Player Stats")
	TSubclassOf<class UUserWidget> CharacterOverlayClass;

	UPROPERTY()
	class UCharacterOverlay* CharacterOverlay;

	void AddCharacterOverlay();

	UPROPERTY(EditAnywhere,Category="Announcements")
	TSubclassOf<UUserWidget> AnnouncementClass;

	UPROPERTY()
	class UAnnouncement* Announcement;

	void AddAnnouncement();

	UPROPERTY(EditAnywhere, Category = "Lobby")
	TSubclassOf<class ULobbyOverlay> LobbyOverlayClass;

	UPROPERTY()
	class ULobbyOverlay* LobbyOverlay;

	void AddLobbyOverlay();
	void RemoveLobbyOverlay();

	// --- 射击反馈（HUD Canvas 绘制，零资源）---
	void ShowHitMarker();
	void AddDamageNumber(float Damage, const FVector& WorldLocation);
	// 受击方向指示：记录一次伤害来源位置，屏幕边缘画红色弧指向它（1.5s 渐隐）
	void AddDamageDirection(const FVector& DamageOrigin);
	void AddKillFeedEntry(const FString& KillerName, const FString& VictimName);

	// 击杀确认标记：屏幕中下方弹出矢量造型，RoundKills = 本回合击杀数（决定造型档位）
	void ShowKillMarker(int32 RoundKills);
	void HideKillMarker();

	// GameState OnKillFeedEntry 委托绑定入口（动态多播委托要求 UFUNCTION）
	UFUNCTION()
	void OnKillFeedEntry(const FString& KillerName, const FString& VictimName);
private:
	FHUDPackage HUDPackage;
	void DrawCrosshair(UTexture2D* Texture,FVector2D ViewportCenter,FVector2D Spread,FLinearColor CrosshairColor);

	UPROPERTY(EditAnywhere)
	float CrosshairSpreadMax=16.f;

	// 命中标记：HitMarkerTime 记录最近一次命中时刻，超过 HitMarkerDuration 秒后消失
	float HitMarkerTime = 0.f;
	float HitMarkerDuration = 0.12f;
	void DrawHitMarker(FVector2D ViewportCenter);

	// 漂浮伤害数字（世界坐标 → 屏幕，1s 上飘渐隐）
	struct FDamageNumberEntry
	{
		float Damage;
		FVector WorldLocation;
		float SpawnTime;
	};
	TArray<FDamageNumberEntry> DamageNumbers;
	void DrawDamageNumbers();

	// 受击方向指示：屏幕边缘红色弧指向伤害来源（1.5s 渐隐，超过 0.9s 开始淡出）
	struct FDamageDirectionEntry
	{
		FVector DamageOrigin;
		float SpawnTime;
	};
	TArray<FDamageDirectionEntry> DamageDirections;
	void DrawDamageDirections(const FVector2D& ViewportSize);

	// 受击方向红弧外观（可在 HUD 类默认值/子类里调）
	UPROPERTY(EditAnywhere, Category = "Damage Direction")
	float DamageArcRadius = 56.f;       // 弧半径（越大越显眼）
	UPROPERTY(EditAnywhere, Category = "Damage Direction")
	float DamageArcMargin = 16.f;       // 弧最外缘离屏幕边的空隙（越小越贴边）
	UPROPERTY(EditAnywhere, Category = "Damage Direction")
	float DamageArcHalfAngle = 40.f;    // 半张开角（整段弧 = 2×）
	UPROPERTY(EditAnywhere, Category = "Damage Direction")
	float DamageArcThickness = 5.f;     // 线宽

	// 击杀信息（右上角，4s 渐隐）。数据经 GameState MulticastKillFeed → OnKillFeedEntry 广播进来。
	struct FKillFeedEntry
	{
		FString KillerName;
		FString VictimName;
		float SpawnTime;
	};
	TArray<FKillFeedEntry> KillFeedEntries;
	void DrawKillFeed(const FVector2D& ViewportSize);

	// 击杀确认标记：Tier 1..5（0=未激活），KillMarkerTime 记录最近一次击杀时刻。
	// 弹出缩放（overshoot）→ 停留 → 渐隐，到点自动清零。
	int32 KillMarkerTier = 0;
	float KillMarkerTime = 0.f;
	float KillMarkerPopDuration = 0.25f;
	float KillMarkerHoldTime = 2.5f;
	float KillMarkerFadeTime = 1.f;
	void DrawKillMarker(const FVector2D& ViewportSize);
	// 画一条连线段（命中 X 同款 DrawLine 手法）
	void DrawKillLine(const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness);
	// 按档位画造型（Center 为屏幕坐标中心，Size 为半边长）
	void DrawKillMarkerShape(const FVector2D& Center, float Size, int32 Tier, const FLinearColor& Color, float Thickness);

	// Sage 治疗选中：准心下的队友血条 + 选中态小十字（无枪械准星时指示瞄准点）
	void DrawSageHealOverlay(const FVector2D& ViewportCenter);

	// 观战栏（阵亡观察队友）：底部中央两行字。每帧查 PC 观战状态；非观战不画
	void DrawSpectateBar(const FVector2D& ViewportSize);

	// 击杀信息懒绑定：首帧拿到 GameState 后绑一次
	bool bKillFeedBound = false;
public:
	FORCEINLINE void SetHUDPackage(const FHUDPackage& Package){HUDPackage=Package;};
};
