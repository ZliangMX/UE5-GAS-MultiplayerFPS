// Fill out your copyright notice in the Description page of Project Settings.


#include "MinimapComponent.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/PlantZone/PlantZone.h"
#include "Blaster/BlasterTypes/Team.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

UMinimapComponent::UMinimapComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UMinimapComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(UMinimapComponent, VisibleEnemyInfo, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UMinimapComponent, DeadMarkers, COND_OwnerOnly);
	DOREPLIFETIME(UMinimapComponent, SpikeMinimapData);
}

void UMinimapComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	APlayerController* PC = GetOwner<APlayerController>();
	if (!PC || !PC->HasAuthority()) return;

	ServerAccumulator += DeltaTime;
	if (ServerAccumulator >= MinimapUpdateInterval)
	{
		ServerAccumulator = 0.f;
		UpdateMinimapServerData();
	}
}

// --- OnRep：HUD 每帧直接读数据，无需额外处理 ---
void UMinimapComponent::OnRep_VisibleEnemyInfo() {}
void UMinimapComponent::OnRep_DeadMarkers() {}
void UMinimapComponent::OnRep_SpikeMinimapData() {}

// --- 地图坐标换算（固定地图） ---
float UMinimapComponent::GetMapSizePx() const
{
	return 2.f * MapHalfSizeCm * PixelsPerCM;
}

FVector2D UMinimapComponent::WorldToMap(const FVector& WorldPos) const
{
	const float HalfPx = GetMapSizePx() * 0.5f;

	// 世界 +X → 地图右、+Y → 地图上（北朝上，翻转 Y）
	FVector2D Offset(
	(WorldPos.Y - MapOrigin.Y) * PixelsPerCM,
		-(WorldPos.X - MapOrigin.X) * PixelsPerCM
		
	);

	// 越界 clamp 到地图中心 ±HalfPx，再加 HalfPx 转成相对左上角
	Offset.X = FMath::Clamp(Offset.X, -HalfPx, HalfPx);
	Offset.Y = FMath::Clamp(Offset.Y, -HalfPx, HalfPx);
	return Offset + FVector2D(HalfPx, HalfPx);
}

FVector UMinimapComponent::GetOwnLocation() const
{
	const APlayerController* PC = GetOwner<APlayerController>();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	return Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
}

float UMinimapComponent::GetOwnYaw() const
{
	const APlayerController* PC = GetOwner<APlayerController>();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	return Pawn ? Pawn->GetControlRotation().Yaw : 0.f;
}

void UMinimapComponent::GatherTeammateLocations(TArray<FVector>& OutLocations) const
{
	OutLocations.Reset();

	const APlayerController* PC = GetOwner<APlayerController>();
	if (!PC) return;
	const ABlasterCharacter* Own = Cast<ABlasterCharacter>(PC->GetPawn());
	if (!Own) return;

	const ETeam OwnTeam = Own->GetPlayerState<ABlasterPlayerState>()
		? Own->GetPlayerState<ABlasterPlayerState>()->Team : ETeam::ET_None;

	const AGameStateBase* GS = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	if (!GS) return;

	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS) continue;

		ABlasterCharacter* Ally = Cast<ABlasterCharacter>(BPS->GetPawn());
		if (!Ally || Ally == Own || Ally->IsElimmed()) continue;
		if (BPS->Team == ETeam::ET_None || BPS->Team != OwnTeam) continue;

		OutLocations.Add(Ally->GetActorLocation());
	}
}

// --- 安装区轮廓（客户端扫描关卡中所有 APlantZone，首次调用缓存） ---
const TArray<TArray<FVector>>& UMinimapComponent::GetPlantZoneOutlines()
{
	if (!bPlantZonesCached) CachePlantZoneOutlines();
	return CachedPlantZoneOutlines;
}

void UMinimapComponent::CachePlantZoneOutlines()
{
	CachedPlantZoneOutlines.Reset();
	const UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<APlantZone> It(World); It; ++It)
		{
			const TArray<FVector> Corners = It->GetDisplayCorners();
			if (Corners.Num() >= 3)
			{
				CachedPlantZoneOutlines.Add(Corners);
			}
		}
	}
	bPlantZonesCached = true;
}

// --- 服务器数据刷新 ---
void UMinimapComponent::UpdateMinimapServerData()
{
	APlayerController* PC = GetOwner<APlayerController>();
	AGameStateBase* GS = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	if (!PC || !GS)
	{
		VisibleEnemyInfo.Empty();
		return;
	}

	const float Now = GetWorld()->GetTimeSeconds();
	const ETeam OwnTeam = PC->GetPlayerState<ABlasterPlayerState>() ? PC->GetPlayerState<ABlasterPlayerState>()->Team : ETeam::ET_None;

	// 收集同队存活"观察者"（含自己）。Valorant 红点语义：敌人被任一存活队友看到 → 全队小地图显示。
	// 持枪时 bUseControllerRotationYaw=true，角色的复制 Actor Yaw ≈ 视角 Yaw，服务器可用它近似队友朝向。
	TArray<ABlasterCharacter*> Observers;
	Observers.Reserve(GS->PlayerArray.Num());
	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None || BPS->Team != OwnTeam) continue;

		ABlasterCharacter* Ally = Cast<ABlasterCharacter>(BPS->GetPawn());
		if (!Ally || Ally->IsElimmed()) continue;

		Observers.Add(Ally);
	}

	TArray<FMinimapEnemyInfo> NewVisible;	// 全队无人存活 → Observers 空，下面自然不亮任何敌人
	NewVisible.Reserve(GS->PlayerArray.Num());

	for (APlayerState* PS : GS->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->Team == ETeam::ET_None || BPS->Team == OwnTeam) continue;

		ABlasterCharacter* Enemy = Cast<ABlasterCharacter>(BPS->GetPawn());
		if (!Enemy) continue;

		// 死亡敌人：只在死亡瞬间记一次红叉（按角色指针去重，防止尸体位移每帧刷新导致残留）
		if (Enemy->IsElimmed())
		{
			const TWeakObjectPtr<AActor> EnemyPtr(Enemy);
			if (!TrackedDeadCharacters.Contains(EnemyPtr))
			{
				FMinimapDeadMarker Marker;
				Marker.Location = Enemy->GetActorLocation();
				Marker.DeathTime = Now;
				DeadMarkers.Add(Marker);
				TrackedDeadCharacters.Add(EnemyPtr);
			}
			continue;
		}

		// 任一存活队友看到即加入（全队共享红点），全都没看到就不亮
		for (ABlasterCharacter* Observer : Observers)
		{
			if (!IsEnemyVisibleFrom(Observer, Enemy)) continue;

			FMinimapEnemyInfo EnemyInfo;
			EnemyInfo.Location = Enemy->GetActorLocation();
			EnemyInfo.Yaw = Enemy->GetControlRotation().Yaw;
			NewVisible.Add(EnemyInfo);
			break;
		}
	}

	// 剪掉已销毁的角色记录；红叉超过显示时长（留 0.5s 余量）后移除
	TrackedDeadCharacters.RemoveAll([](const TWeakObjectPtr<AActor>& Ptr) { return !Ptr.IsValid(); });
	const float MarkerPruneTime = MinimapDeadMarkerLifetime + 0.5f;
	DeadMarkers.RemoveAll([&](const FMinimapDeadMarker& M) { return Now - M.DeathTime > MarkerPruneTime; });

	// Spike：掉落/已安放才在小地图显示（携带中隐藏，避免暴露携带者位置）
	FSpikeMinimapData NewSpikeData;
	ABlasterGameMode* GM = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if (GM)
	{
		ASpike* Spike = GM->GetCurrentSpike();
		if (Spike)
		{
			const ESpikeState SpikeState = Spike->GetSpikeState();
			if (SpikeState == ESpikeState::ESS_Planted || SpikeState == ESpikeState::ESS_Dropped)
			{
				NewSpikeData.Location = Spike->GetActorLocation();
				NewSpikeData.bVisible = true;
			}
		}
	}
	SpikeMinimapData = NewSpikeData;

	VisibleEnemyInfo = NewVisible;
}

// 单个敌人能否被"观察者"（自己或任一存活队友）看到 —— 距离 + 视野锥 + 无遮挡三关
bool UMinimapComponent::IsEnemyVisibleFrom(const ABlasterCharacter* Observer, const ABlasterCharacter* Enemy) const
{
	if (!Observer || !Enemy || !GetWorld()) return false;

	// 距离（水平面，俯视判定不看高差）
	FVector ToEnemy = Enemy->GetActorLocation() - Observer->GetActorLocation();
	ToEnemy.Z = 0.f;
	if (ToEnemy.Size() > MinimapVisibilityRange) return false;

	// 视野锥：观察者角色朝前方向（持枪时 bUseControllerRotationYaw=true，复制 Actor Yaw ≈ 视角 Yaw）
	const FVector Forward2D = FRotationMatrix(FRotator(0.f, Observer->GetActorRotation().Yaw, 0.f)).GetUnitAxis(EAxis::X);
	const float CosHalf = FMath::Cos(FMath::DegreesToRadians(MinimapViewConeHalfAngle));
	const FVector Dir = ToEnemy.GetSafeNormal();
	if (FVector::DotProduct(Dir, Forward2D) < CosHalf) return false;

	// 遮挡：从观察者眼睛打到敌人身上；命中不是敌人本体（墙/其他人）= 看不见
	// 必须忽略观察者自身，否则从自己胶囊体内打出的 trace 会立刻打到自己
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Observer);

	FHitResult Hit;
	const bool bBlocked = GetWorld()->LineTraceSingleByChannel(
		Hit, Observer->GetPawnViewLocation(), Enemy->GetActorLocation() + FVector(0.f, 0.f, 60.f), ECC_Visibility, QueryParams);
	return !(bBlocked && Hit.GetActor() != Enemy);
}
