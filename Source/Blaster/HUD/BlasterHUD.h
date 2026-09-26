// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BlasterHUD.generated.h"

class UWeaponKillIconSet;

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
	// 瞄准（ADS）时**叠在准星底下**的镜框贴图（武器的 AimTexture，没配就是 nullptr）。
	// 和准星同一套画法（DrawCrosshair，按原始尺寸居中），但**颜色是它自己的** —— 见下面。
	UTexture2D* AimTexture;
	// 镜框单独的颜色。**故意不用 CrosshairsColor**：那个在瞄到可交互物时会变红，
	// 而镜框是个静态的框，跟着准星一起闪红很怪（Valorant 里框也是一直不变的）。
	// （给个默认值：这个结构体没构造函数，不初始化的话是未定义值。虽然只有 AimTexture
	// 非空时才会被用到、而那时必定赋过值，但留个白色默认更稳妥。）
	FLinearColor AimTextureColor = FLinearColor::White;
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

	// --- 局内 HUD 用哪一套 ---
	// true  = 只用 Valorant 那套。课程模板里**纯重复**的两块不再创建：
	//          血甲弹面板 UCharacterOverlay / 通用技能条 USkillBarWidget。
	//         回合公告 UAnnouncement 换成 UValorantAnnouncement（BUY PHASE 那个框），见下面。
	// false = 旧的那套照旧创建（想对比或回退时翻回来，别的都不用动）。
	// ★ 小地图**不受这个开关管**：我们的上下两条里根本没有小地图这个功能，收掉就真没了。
	//
	// ⚠️ 打开期间 PlayerController 那边往 CharacterOverlay 的写入全走空指针保护（不会崩），
	//    但屏幕上会暂时没有：
	//     · 我方/敌方存活人数（AliveCountText）
	//     · 本回合击杀/阵亡（ScoreAmount / DefeatsAmount）
	//     · 安包/拆包进度条（SpikeStatusText / SpikeTimerBar）
	//     前两项 Valorant HUD 里本来也有，之后搬进我们的上下两条即可。
	UPROPERTY(EditAnywhere, Category = "Valorant HUD")
	bool bValorantHUDOnly = true;

	// --- Valorant 局内公告面板（BUY PHASE 那个框，UValorantAnnouncement）---
	// 用户给的截图里的公告位置（面板实测 x1003..1558, y179.5..373，设计分辨率 2559x1439）。
	// ★ UValorantAnnouncement 是 UAnnouncement 的**子类**，所以 PlayerController 那十几处
	//   `BlasterHUD->Announcement->InfoText->SetText(...)` 一个字都不用改，只要这里换类即可。
	// 留空 = 用 C++ 类（纯代码建版面）；想在设计器里改就做个 WBP 派生自它，填到这里。
	UPROPERTY(EditAnywhere, Category = "Valorant HUD")
	TSubclassOf<class UValorantAnnouncement> ValorantAnnouncementClass;

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

	/*
	 * Lobby（选人）地图里把"归 HUD 管"的那两个 widget 收起来 / 回到对局时放回去。
	 *
	 * 为什么需要它：小地图（UMinimapWidget）**自己没判过 Lobby**（顶栏底栏都判了），
	 * 大厅里会照常画一张地图；技能条原来在 DrawHUD 末尾每帧设一次，现在并到这里。
	 *
	 * ⚠️ 只碰这两个，别往里加底栏/顶栏/风丝/被闪 —— 它们各自管自己的可见性，
	 *    两个地方抢着设会互相盖（理由写在 DrawHUD 开头和这个函数的实现里）。
	 */
	void SetGameplayWidgetsCollapsed(bool bCollapsed);

	// --- Valorant 底部 HUD（UValorantBottomHUD：血/甲/弹装饰 + Jett 技能条 C/Q/E/X）---
	// 和 USkillBarWidget 并存（那个是"通用技能条"，这个是照 Jett 素材拼的整套底栏）。
	// 想撤掉只要把这类置空 / 删掉这段和 AddValorantHUDWidget 调用，别的不受影响。
	UPROPERTY(EditAnywhere, Category = "Valorant HUD")
	TSubclassOf<class UValorantBottomHUD> ValorantHUDClass;

	UPROPERTY()
	class UValorantBottomHUD* ValorantHUD;

	void AddValorantHUDWidget();

	// --- Valorant 顶部比分栏（UValorantTopHUD：左右队伍条 + 中央倒计时 + 左右回合数）---
	// 和底部 HUD 各自独立：想撤掉只要把这类置空 / 删掉这段和 AddValorantTopHUDWidget 调用。
	UPROPERTY(EditAnywhere, Category = "Valorant HUD")
	TSubclassOf<class UValorantTopHUD> ValorantTopHUDClass;

	UPROPERTY()
	class UValorantTopHUD* ValorantTopHUD;

	void AddValorantTopHUDWidget();

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

	// 击杀确认标记：屏幕中下方弹出，RoundKills = 本回合击杀数（决定造型档位，1..6）
	// Icons = 击杀时手上那把枪配的图标集，可为空 —— 那一档没填贴图就回落到矢量造型
	void ShowKillMarker(int32 RoundKills, UWeaponKillIconSet* Icons);
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

	// 击杀确认标记：Tier 1..6（0=未激活），KillMarkerTime 记录最近一次击杀时刻。
	// 弹出缩放（overshoot）→ 停留 → 渐隐，到点自动清零。
	// KillMarkerIcons 是这一发击杀时手上武器配的图标集，在 ShowKillMarker 那一刻定下来，
	// 整段动画期间不再变 —— 中途换枪/丢枪都不会把已经弹出来的图标换掉。
	int32 KillMarkerTier = 0;
	float KillMarkerTime = 0.f;
	class UWeaponKillIconSet* KillMarkerIcons = nullptr;
	float KillMarkerPopDuration = 0.25f;
	float KillMarkerHoldTime = 2.5f;
	float KillMarkerFadeTime = 1.f;
	// 有贴图时图标的显示边长（px，正方形）。按 KillIconSize 缩放而不是贴图原始像素 ——
	// 想让图标大一点/小一点改这里就行，不用重新导图。没填贴图的档位不受影响。
	// 2026-09-24 用户实跑反馈「变大一点」：96 → 130。
	UPROPERTY(EditAnywhere, Category = "Kill Marker")
	float KillIconSize = 130.f;

	// 击杀图标**中心**距屏幕底边的距离（px）。调大 = 整块往上走。
	// 原来是写死在 DrawKillMarker 里的 90，配合 96 的图标几乎贴着屏幕底边；
	// 拎出来是为了「往上挪」不用改代码逻辑，只动这一个数。
	UPROPERTY(EditAnywhere, Category = "Kill Marker")
	float KillMarkerBottomOffset = 160.f;
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
