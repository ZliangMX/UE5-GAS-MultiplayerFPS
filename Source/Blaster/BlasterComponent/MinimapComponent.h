// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MinimapComponent.generated.h"

/** 小地图死亡红叉条目：服务器算好后复制给 owner，客户端控制渐隐 */
USTRUCT(BlueprintType)
struct FMinimapDeadMarker
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	float DeathTime = 0.f;
};

/** 小地图 spike 显示数据：服务器算好后复制给所有客户端（双方都能看到安放/掉落的爆能器） */
USTRUCT(BlueprintType)
struct FSpikeMinimapData
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	bool bVisible = false;
};

/** 小地图可见敌人条目：位置 + 朝向（朝向用于画红色方向箭头） */
USTRUCT(BlueprintType)
struct FMinimapEnemyInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	float Yaw = 0.f;
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BLASTER_API UMinimapComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMinimapComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// --- 固定地图：显示世界 [MapOrigin ± MapHalfSizeCm] 方形区域 ---
	//
	// ★ 2026-09-24 重标定（底图从「BlasterMap 绿岛俯拍截图」换成 Lotus 官方小地图）。
	//   换底图**必须**连带改这三个数，否则箭头/队友点全跑偏 —— 它们和贴图是一对，
	//   不是各自独立的。这次的来源是 valorant-api 给的 Lotus 官方小地图参数：
	//       u = worldY * 7.2e-05 + 0.454789      (u = 归一化横坐标)
	//       v = worldX * (-7.2e-05) + 0.917752   (v = 归一化纵坐标)
	//   把它代进 WorldToMap 的两条式子，用「贴图 996x996 正好铺满整个世界范围」解出来，
	//   就是下面这三个值（注意 PixelsPerCM 的语义 = 世界厘米 → **贴图像素**）。
	//
	//   自检（换任何图/关卡后都该重跑）：A/B/C 三个包点的世界坐标换算后应落在底图上
	//   三块米黄色区里 —— A→(851,359)  B→(501,457)  C→(147,435)（贴图 996 坐标系）。
	//   这套映射成立的前提是 **Lotus 关卡保留着 Valorant 原始世界坐标**（已核实：
	//   三个包点的实测质心与官方坐标一致）。
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	FVector MapOrigin = FVector(5802.4f, 627.9f, -1.f);

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MapHalfSizeCm = 6944.5f;

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float PixelsPerCM = 0.071712f;

	// 底图贴图 → 屏幕像素的缩放。**只影响小地图画多大，不影响世界↔地图的对应**
	// （WorldToMap / MapToWorld 在内部把它一起乘进去，所以调用方拿到的还是
	//  「0..GetMapSizePx() 的地图内像素」，一处都不用改）。
	//
	// 为什么需要它：贴图 996px 对应整个世界范围（2*MapHalfSizeCm*PixelsPerCM 恰好 = 996），
	// 1:1 画出来就是 996px —— 在 1920 宽的屏幕上占一半，太大。
	// 0.3 → 约 299px（换图前的原大小）；0.4 → 约 398px（2026-09-24 用户实跑后定的大小）。
	// 贴图分辨率有富余，想再大就往上调（0.5 → 498px）；这就是唯一的"小地图多大"旋钮。
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MapDisplayScale = 0.4f;

	// --- 服务器可见性判定 ---
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MinimapVisibilityRange = 5000.f;

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MinimapViewConeHalfAngle = 70.f;

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MinimapUpdateInterval = 0.1f;

	// 死亡红叉显示时长（客户端渐隐用，服务器按此剪枝）
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MinimapDeadMarkerLifetime = 2.f;

	// --- 数据（服务器算好后复制给 owner / 所有人） ---
	UPROPERTY(ReplicatedUsing = OnRep_VisibleEnemyInfo)
	TArray<FMinimapEnemyInfo> VisibleEnemyInfo;

	UPROPERTY(ReplicatedUsing = OnRep_DeadMarkers)
	TArray<FMinimapDeadMarker> DeadMarkers;

	UPROPERTY(ReplicatedUsing = OnRep_SpikeMinimapData)
	FSpikeMinimapData SpikeMinimapData;

	UFUNCTION()
	void OnRep_VisibleEnemyInfo();

	UFUNCTION()
	void OnRep_DeadMarkers();

	UFUNCTION()
	void OnRep_SpikeMinimapData();

	// --- 客户端读取 API（HUD 每帧调用） ---

	// 地图边长（像素）= 2 × MapHalfSizeCm × PixelsPerCM
	float GetMapSizePx() const;

	// 世界坐标 → 地图内像素（相对地图左上角，越界 clamp 到边）
	FVector2D WorldToMap(const FVector& WorldPos) const;

	// WorldToMap 的逆：地图内像素 → 世界坐标 XY（Z 原样返回 0，由调用方自己决定高度）。
	// 暮蝶的封烟界面用它把"玩家点在地图上的哪一格"翻译回世界坐标。
	//
	// ⚠️ 只接受**地图内部**的像素（0..GetMapSizePx()）。世界坐标**不再 clamp** ——
	//    WorldToMap 会 clamp 是因为画图标时越界就等于贴在边上，而这里 clamp 会把
	//    "点到地图外"悄悄变成"贴边放烟"。所以本函数不含 clamp，越界判定留给调用方。
	FVector MapToWorld(const FVector2D& MapPos) const;

	const TArray<FMinimapEnemyInfo>& GetVisibleEnemies() const { return VisibleEnemyInfo; }
	const TArray<FMinimapDeadMarker>& GetDeadMarkers() const { return DeadMarkers; }
	const FSpikeMinimapData& GetSpikeData() const { return SpikeMinimapData; }
	float GetDeadMarkerLifetime() const { return MinimapDeadMarkerLifetime; }

	// 自己当前位置 / 朝向（HUD 画自己箭头用）
	FVector GetOwnLocation() const;
	float GetOwnYaw() const;

	// 队友位置聚合（客户端零带宽：从 GameState->PlayerArray 读引擎复制 transform）
	void GatherTeammateLocations(TArray<FVector>& OutLocations) const;

	// 安装区轮廓（关卡中所有 APlantZone 的显示多边形，世界坐标俯视 2D）。首次调用时扫描并缓存
	const TArray<TArray<FVector>>& GetPlantZoneOutlines();

private:
	// 扫描关卡中所有 APlantZone 并缓存其显示多边形
	void CachePlantZoneOutlines();

	// 安装区轮廓缓存（世界坐标角点）
	TArray<TArray<FVector>> CachedPlantZoneOutlines;
	bool bPlantZonesCached = false;

	// 服务器：按 MinimapUpdateInterval 刷新可见敌人 / 死亡红叉 / spike
	void UpdateMinimapServerData();

	// 服务器：单个敌人能否被"观察者"（自己或任一存活队友）看到 —— 距离 + 视野锥 + 无遮挡
	bool IsEnemyVisibleFrom(const class ABlasterCharacter* Observer, const class ABlasterCharacter* Enemy) const;

	float ServerAccumulator = 0.f;

	// 已记录过死亡红叉的角色（防止尸体位移导致每帧刷新叉子残留）
	TArray<TWeakObjectPtr<AActor>> TrackedDeadCharacters;
};
