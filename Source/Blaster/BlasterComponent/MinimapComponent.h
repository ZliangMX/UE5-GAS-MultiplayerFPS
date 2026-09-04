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
	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	FVector MapOrigin = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float MapHalfSizeCm = 20000.f;

	UPROPERTY(EditDefaultsOnly, Category = "Minimap")
	float PixelsPerCM = 0.0075f;

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
