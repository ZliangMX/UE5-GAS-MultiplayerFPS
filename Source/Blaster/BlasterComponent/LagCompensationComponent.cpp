#include "LagCompensationComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Weapon/Weapon.h"

namespace
{
	// 命中盒命中后画出来的颜色（调试用）：头=红、胸=黄、胯=绿
	const FColor HeadBoxColor(255, 60, 60);
	const FColor ChestBoxColor(255, 220, 60);
	const FColor PelvisBoxColor(60, 255, 120);
	const FColor LegsBoxColor(120, 160, 255);

	// 没打进这个盒子时的颜色 —— 「一个盒子都没看到」要能区分「没生成盒子」和「生成了但没打中」
	const FColor BoxMissColor(130, 130, 130);    // 射线几何上就没碰到这个盒子
	const FColor BoxBlockedColor(150, 90, 0);    // 碰到了，但被墙/更近的命中挡在前面
}

ULagCompensationComponent::ULagCompensationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 纯服务器本地数据，不需要复制
	SetIsReplicatedByDefault(false);
}

void ULagCompensationComponent::BeginPlay()
{
	Super::BeginPlay();

	Character = Cast<ABlasterCharacter>(GetOwner());

	/*
	 *骨骼名解析：不同骨架叫法不同，先按配置试，试不到再试常见别名，都不行就 NAME_None 走偏移兜底。
	 *FName 比较不区分大小写，所以 "head" 能命中 "Head"。
	 *
	 *胸骨这条别名链出过事：当前骨架（TP_Wushu_S0_Skeleton，Valorant 那批导入时把 spine_01/02/03
	 *改成了 Spine1~4）里 `spine_02` 一根都不存在，于是胸盒从 2026-09-11 起一直在走兜底 ——
	 *钉在「角色位置 + 固定偏移」上，不跟脊柱走、弯腰下蹲也不跟，上报的 BoneName 还是 None。
	 *现在把实际存在的名字（Spine2/Spine3）排到前面，老的别名留在后面兼容别的骨架。
	 */
	ResolvedHeadBone = ResolveFirstValidBone({ HeadBoneName, TEXT("head"), TEXT("Head") });
	ResolvedChestBone = ResolveFirstValidBone({ ChestBoneName, TEXT("Spine2"), TEXT("Spine3"),
		TEXT("spine_02"), TEXT("spine_01"), TEXT("spine_03"), TEXT("spine") });
	ResolvedPelvisBone = ResolveFirstValidBone({ PelvisBoneName, TEXT("pelvis"), TEXT("hips"), TEXT("root") });

	// 环形缓冲：装得下整个回溯窗口，再多留两帧给插值
	const float Interval = FMath::Max(RecordInterval, 1.f / 120.f);
	const int32 BufferSize = FMath::Max(2, FMath::CeilToInt(MaxRecordTime / Interval) + 2);
	FrameHistory.SetNum(BufferSize);
	NextFrameIndex = 0;
	NumFramesRecorded = 0;
}

void ULagCompensationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (Character == nullptr) return;

	// 常驻命中盒可视化放在权威判断**之前**：客户端也有这个组件、骨骼位置也是现成的，
	// 调盒子位置时多半是在客户端窗口里对着别人看，把它挡在 HasAuthority 后面就白开了。
	// 它不读任何历史数据，跟回溯本身无关，纯粹是画框。
	if (bDrawDebugHitboxes) DrawDebugHitboxes();

	// 只有权威机记录历史（客户端记了也没人用），阵亡的角色不需要再被回溯
	if (!Character->HasAuthority() || Character->IsElimmed()) return;

	// 诊断输出和记录解耦：记录是 30Hz，这个按自己的间隔打一行
	if (bLogLagDiagnostics)
	{
		LagLogAccumulator += DeltaTime;
		if (LagLogAccumulator >= LagLogInterval)
		{
			LagLogAccumulator = 0.f;
			LogInputLagDiagnostics();
		}
	}

	RecordAccumulator += DeltaTime;
	if (RecordAccumulator < RecordInterval) return;
	RecordAccumulator = 0.f;

	SaveFramePackage();
}

void ULagCompensationComponent::SaveFramePackage()
{
	if (FrameHistory.Num() == 0) return;

	FBlasterRewindFrame& Frame = FrameHistory[NextFrameIndex];
	FillFrame(Frame);

	NextFrameIndex = (NextFrameIndex + 1) % FrameHistory.Num();
	NumFramesRecorded = FMath::Min(NumFramesRecorded + 1, FrameHistory.Num());
}

void ULagCompensationComponent::FillFrame(FBlasterRewindFrame& OutFrame)
{
	if (Character == nullptr) return;

	OutFrame.Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	OutFrame.RootLocation = Character->GetActorLocation();
	// 命中盒只绕 Z 转：角色会前倾/后仰，但那点误差远小于不回溯的误差
	OutFrame.RootRotation = FRotator(0.f, Character->GetActorRotation().Yaw, 0.f);

	OutFrame.HeadLocation = BoneWorldLocation(ResolvedHeadBone, HeadFallbackOffset);
	OutFrame.ChestLocation = BoneWorldLocation(ResolvedChestBone, ChestFallbackOffset);
	OutFrame.PelvisLocation = BoneWorldLocation(ResolvedPelvisBone, PelvisFallbackOffset);
	// 射手侧的眼位 = 那一枪的射线起点。任何机器上都能算（远端角色走「身体变换 × 固定眼高」那条路），
	// 所以服务器不需要客户端上报任何起点参数 —— 见 ABlasterCharacter::GetFPEyeWorldLocation。
	OutFrame.EyeLocation = Character->GetFPEyeWorldLocation();
}

void ULagCompensationComponent::DrawDebugHitboxes()
{
#if ENABLE_DRAW_DEBUG
	UWorld* World = GetWorld();
	if (World == nullptr || Character == nullptr) return;

	// 本地控制的那一个是第一人称视角，自己的框正好糊在屏幕中央，什么都看不见 —— 跳过。
	// （想看自己的框：临时把这一行注掉，或者退到第三人称/旁观视角再看。）
	if (Character->IsLocallyControlled()) return;

	/*
	 *画的是「此刻」的活姿势，不是历史帧。
	 *历史帧最多差一个 RecordInterval（1/30s，跑动时约 20cm），拿它对着角色拉框会越拉越偏。
	 *姿势来源和记录用的完全一样（同一个 FillFrame），所以四个盒子的中心和尺寸与判定用的那份数据
	 *是同一套算法 —— 看见的框就是服务器会拿来打判定的框。
	 */
	FBlasterRewindFrame Frame;
	FillFrame(Frame);

	const FQuat BoxRotation(FRotator(0.f, Frame.RootRotation.Yaw, 0.f));

	auto DrawOne = [&](const FVector& BoneLocation, const FVector& Offset, const FVector& Extent, const FColor& Color)
	{
		// LifeTime = -1（且 bPersistentLines=false）= 只画这一帧。每帧重画 → 看上去是常驻的。
		// 别改成持久线：那是往场景里叠加、只增不减，几秒钟就糊成一片，而且不会随角色移动。
		DrawDebugBox(World, BoneLocation + BoxRotation.RotateVector(Offset), Extent, BoxRotation,
			Color, false, -1.f, 0, 1.5f);
	};

	DrawOne(Frame.HeadLocation, HeadBoxOffset, HeadExtent, HeadBoxColor);
	DrawOne(Frame.ChestLocation, ChestBoxOffset, ChestExtent, ChestBoxColor);
	DrawOne(Frame.PelvisLocation, PelvisBoxOffset, PelvisExtent, PelvisBoxColor);
	// 腿盒没有对应骨骼（BoneName = None → 不算爆头），挂在角色根上，和判定那边一致
	DrawOne(Frame.RootLocation, LegsBoxOffset, LegsExtent, LegsBoxColor);
#endif
}

void ULagCompensationComponent::LogInputLagDiagnostics()
{
	UWorld* World = GetWorld();
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	const FNetworkPredictionData_Server_Character* ServerData =
		MoveComp ? MoveComp->GetPredictionData_Server_Character() : nullptr;
	if (World == nullptr || ServerData == nullptr) return;

	// 「服务器现在墙钟」减「最近一次收到客户端 Move 的服务器墙钟」= 服务器手里的输入有多旧。
	// 模型 A 成立时，服务器权威位置落后的量正好等于它。
	const float InputAge = World->GetTimeSeconds() - ServerData->ServerTimeStampLastServerMove;

	// 没在收 Move 的角色（没动过的 bot、还没进来的、断线的）只会刷屏，跳过。
	// ServerTimeStampLastServerMove 初值是 0 → InputAge 会是个巨大的数，这里一并挡掉。
	if (InputAge > MaxRecordTime) return;

	const APlayerState* PS = Character->GetPlayerState();
	// 服务器上 GetPingInMilliseconds() 返回 ExactPing，正是我们要的往返 RTT
	const float OneWayLatency = PS ? PS->GetPingInMilliseconds() * 0.5f * 0.001f : 0.f;

	const FVector ServerLoc = Character->GetActorLocation();
	const float Speed = Character->GetVelocity().Size();

	UE_LOG(LogTemp, Warning,
		TEXT("[LagComp|输入年龄] %s 速度=%.0fcm/s 位置=(%.0f,%.0f,%.0f) | 距上次收到Move=%.1fms 单向延迟=%.1fms"),
		*Character->GetName(), Speed, ServerLoc.X, ServerLoc.Y, ServerLoc.Z,
		InputAge * 1000.f, OneWayLatency * 1000.f);
}

FName ULagCompensationComponent::ResolveFirstValidBone(const TArray<FName>& Candidates) const
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (Mesh == nullptr) return NAME_None;

	for (const FName& Candidate : Candidates)
	{
		if (Candidate != NAME_None && Mesh->GetBoneIndex(Candidate) != INDEX_NONE)
		{
			return Candidate;
		}
	}
	return NAME_None;
}

FVector ULagCompensationComponent::BoneWorldLocation(FName Bone, const FVector& FallbackOffset) const
{
	if (Bone != NAME_None && Character && Character->GetMesh())
	{
		return Character->GetMesh()->GetBoneLocation(Bone, EBoneSpaces::WorldSpace);
	}
	// 骨架里找不到这根骨骼：退回「角色位置 + 固定偏移」
	return Character ? Character->GetActorLocation() + FallbackOffset : FVector::ZeroVector;
}

bool ULagCompensationComponent::GetRewoundFrame(float Time, FBlasterRewindFrame& OutFrame)
{
	if (Character == nullptr) return false;

	// 一点历史都没有（刚出生 / 刚进观战范围）：先就地记一帧，避免「明明打中却判空」
	if (NumFramesRecorded == 0)
	{
		SaveFramePackage();
		if (NumFramesRecorded == 0) return false;
	}

	// 找 Time 前后夹住它的两帧：帧数没写满时有效帧是 [0, NumFramesRecorded)，
	// 写满后绕圈但仍然是这前 NumFramesRecorded 个槽（最多几十帧，直接扫）
	const FBlasterRewindFrame* Older = nullptr;
	const FBlasterRewindFrame* Newer = nullptr;
	for (int32 Index = 0; Index < NumFramesRecorded; ++Index)
	{
		const FBlasterRewindFrame& Frame = FrameHistory[Index];
		if (Frame.Time <= Time && (Older == nullptr || Frame.Time > Older->Time)) Older = &Frame;
		if (Frame.Time >= Time && (Newer == nullptr || Frame.Time < Newer->Time)) Newer = &Frame;
	}

	if (Older == nullptr && Newer == nullptr) return false;

	// 比历史还老：夹到最老的一帧（再往回没有数据了）
	if (Older == nullptr) { OutFrame = *Newer; return true; }
	// 比最新记录帧还新（典型：本地玩家 / 时钟估计把 RewindTime 顶到 Now，最新帧永远是过去的）：
	// 夹到旧帧 = 让角色凭空后退最多一个 RecordInterval（600cm/s → 20cm），
	// 而且这偏差和目标有没有延迟完全无关，会把要测的信号淹掉 —— 直接用「现在」的活姿势。
	if (Newer == nullptr) { FillFrame(OutFrame); return true; }
	if (Older == Newer) { OutFrame = *Older; return true; }

	const float Span = Newer->Time - Older->Time;
	const float Alpha = Span > KINDA_SMALL_NUMBER ? (Time - Older->Time) / Span : 0.f;

	OutFrame.Time = Time;
	OutFrame.RootLocation = FMath::Lerp(Older->RootLocation, Newer->RootLocation, Alpha);
	OutFrame.RootRotation = FMath::Lerp(Older->RootRotation, Newer->RootRotation, Alpha);
	OutFrame.HeadLocation = FMath::Lerp(Older->HeadLocation, Newer->HeadLocation, Alpha);
	OutFrame.ChestLocation = FMath::Lerp(Older->ChestLocation, Newer->ChestLocation, Alpha);
	OutFrame.PelvisLocation = FMath::Lerp(Older->PelvisLocation, Newer->PelvisLocation, Alpha);
	OutFrame.EyeLocation = FMath::Lerp(Older->EyeLocation, Newer->EyeLocation, Alpha);
	return true;
}

void ULagCompensationComponent::CollectCandidates(ABlasterCharacter* Shooter, TArray<ABlasterCharacter*>& OutCandidates) const
{
	UWorld* World = GetWorld();
	if (World == nullptr || Shooter == nullptr) return;

	const ABlasterPlayerState* ShooterPS = Shooter->GetPlayerState<ABlasterPlayerState>();
	const FVector ShooterLocation = Shooter->GetActorLocation();
	const float MaxDistSq = FMath::Square(MaxRewindDistance);

	for (TActorIterator<ABlasterCharacter> It(World); It; ++It)
	{
		ABlasterCharacter* Other = *It;
		if (Other == nullptr || Other == Shooter) continue;

		// 阵亡的不参与（尸体不该被打中）
		if (Other->IsElimmed()) continue;

		if (FVector::DistSquared(Other->GetActorLocation(), ShooterLocation) > MaxDistSq) continue;

		// 队友不参与命中判定（测试 bot 例外，和命中反馈那边的规则保持一致）
		const ABlasterPlayerState* OtherPS = Other->GetPlayerState<ABlasterPlayerState>();
		if (ShooterPS && OtherPS && OtherPS->Team == ShooterPS->Team && !Other->IsTestBot()) continue;

		OutCandidates.Add(Other);
	}
}

bool ULagCompensationComponent::RayIntersectsOrientedBox(const FVector& Start, const FVector& End, const FVector& Center,
	const FQuat& Rotation, const FVector& Extent, float& OutTime)
{
	// 变换到盒子局部空间 → 变成「射线 vs 轴对齐盒」的 slab 测试
	const FVector LocalStart = Rotation.UnrotateVector(Start - Center);
	const FVector LocalEnd = Rotation.UnrotateVector(End - Center);
	const FVector Dir = LocalEnd - LocalStart;

	double TMin = 0.0;
	double TMax = 1.0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double S = LocalStart[Axis];
		const double D = Dir[Axis];
		const double E = Extent[Axis];

		if (FMath::IsNearlyZero(D))
		{
			// 和这对平面平行：起点不在这条轴的范围内就永远进不来
			if (S < -E || S > E) return false;
			continue;
		}

		double T1 = (-E - S) / D;
		double T2 = (E - S) / D;
		if (T1 > T2) { Swap(T1, T2); }

		TMin = FMath::Max(TMin, T1);
		TMax = FMath::Min(TMax, T2);
		if (TMin > TMax) return false;
	}

	OutTime = static_cast<float>(TMin);
	return true;
}

bool ULagCompensationComponent::ServerSideRewind(ABlasterCharacter* Shooter, const FVector& HitTarget,
	float HitTime, FHitResult& OutHit)
{
	UWorld* World = GetWorld();
	if (World == nullptr || Shooter == nullptr) return false;

	// 只在权威机上算，而且只能由射手自己的组件发起
	if (!Shooter->HasAuthority() || Shooter != Character) return false;

	// 客户端的时间戳可能因时钟估计有偏差：夹进「回溯窗口」里（顺便限制高延迟玩家能往回拉的幅度）
	const float Now = World->GetTimeSeconds();
	const float RewindTime = FMath::Clamp(HitTime, Now - MaxRecordTime, Now);

	// 射手侧的时间：客户端上报的是「服务器此刻」，所以就是 Now。他自己的起点就用这个时刻的姿势。
	const APlayerState* ShooterPS = Shooter->GetPlayerState();
	const float OneWayLatency = ShooterPS ? ShooterPS->GetPingInMilliseconds() * 0.5f * 0.001f : 0.f;

	/*
	 *目标侧的时间：射手屏幕上看到的目标，其实是他「单向延迟 + 客户端插值缓冲」之前的样子。
	 *不减去这一块，回溯窗口等于 0（RewindTime 已经被夹到 Now），目标原地不动。
	 *本地玩家单向延迟 = 0，这条对 host 是恒等变换。
	 */
	const float ViewLatency = bCompensateShooterLatency
		? FMath::Min(OneWayLatency + TargetInterpDelay, MaxRecordTime) : 0.f;
	const float TargetRewindTime = FMath::Clamp(RewindTime - ViewLatency, Now - MaxRecordTime, Now);

	FBlasterRewindFrame ShooterFrame;
	if (!GetRewoundFrame(RewindTime, ShooterFrame)) return false;

	/*
	 *起点：**服务器自己记下的、射手那一刻的眼位**，客户端在起点上零发言权。
	 *
	 *为什么是眼睛而不是枪口：第一人称下玩家瞄准的是「眼睛看到的那条线」，准星就是屏幕中心，
	 *开火时客户端也是从眼位沿准星射线打的（见 UCombatComponent::Fire / UCombatComponent::TraceUnderCrosshairs）。
	 *用枪口当起点的话，服务器算的是另一条线 —— 近处贴脸时两条线差出枪口到眼睛那 ~30cm，
	 *「准星明明在敌人身上」会算成擦边未命中。
	 *
	 *为什么不再要客户端上报起点：起点的位置信息全是服务器自己算的（GetFPEyeWorldLocation 在任何机器上
	 *都成立），客户端只提供**方向**——HitTarget 是准星射线上的一个点，方向由它决定。
	 *以前那套「客户端报枪口 + 服务器拿手骨比长度」的校验连同它的全部参数一起删了：
	 *起点不再是客户端给的，就不需要校验收窄它的自由度。作弊者现在最多能把方向指向别处，
	 *那和「他本来就瞄那里」没有区别，起点依然牢牢长在眼睛里。
	 */
	const FVector Start = ShooterFrame.EyeLocation.IsNearlyZero() ? ShooterFrame.HeadLocation : ShooterFrame.EyeLocation;

	if (FVector::DistSquared(Start, HitTarget) < 1.f)
	{
		if (bLogLagDiagnostics)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[LagComp|未命中] %s 起点和目标点重合（退化射线）起=%.0f,%.0f,%.0f 终=%.0f,%.0f,%.0f"),
				*Shooter->GetName(), Start.X, Start.Y, Start.Z, HitTarget.X, HitTarget.Y, HitTarget.Z);
		}
		return false;
	}

	TArray<ABlasterCharacter*> Candidates;
	CollectCandidates(Shooter, Candidates);
	if (Candidates.Num() == 0)
	{
		// 候选会被四件事剔掉：自己、已阵亡、队友（测试 bot 除外）、超出 MaxRewindDistance。
		// 打自己队的 bot 是「明明打中了却不掉血」最常见的原因，所以把队伍号一起打出来。
		if (bLogLagDiagnostics)
		{
			int32 TotalChars = 0;
			for (TActorIterator<ABlasterCharacter> It(World); It; ++It) { ++TotalChars; }

			const ABlasterPlayerState* TeamPS = Shooter->GetPlayerState<ABlasterPlayerState>();
			const int32 ShooterTeam = TeamPS ? static_cast<int32>(TeamPS->Team) : -1;
			UE_LOG(LogTemp, Warning,
				TEXT("[LagComp|未命中] %s 候选目标=0 || 场上角色数=%d 射手队伍=%d 回溯距离上限=%.0f"
					 " —— 其余的人要么已阵亡、要么是队友、要么太远"),
				*Shooter->GetName(), TotalChars, ShooterTeam, MaxRewindDistance);
		}
		return false;
	}

	// 世界几何挡不挡这一枪：忽略所有角色的射线，拿到遮挡点在 Start→HitTarget 上的比例。
	// 墙不会动 → 用「现在」的墙去判定「过去的一枪」是成立的。
	float BlockRatio = 1.f;
	FString OccluderDesc;
	{
		FCollisionQueryParams Params(TEXT("RewindOcclusion"), false, Shooter);
		// 手上那把枪（网格有碰撞）就在射线起点边上，别让它把自己这一枪挡了
		if (const AWeapon* EquippedWeapon = Shooter->GetEquippedWeapon())
		{
			Params.AddIgnoredActor(EquippedWeapon);
		}
		/*
		 *射手和候选**身上挂着的其它 actor**（手上的爆能器、副武器、地上的枪……）也要一并忽略。
		 *
		 *AddIgnoredActor 只按 actor 忽略它自己的组件，挂载链上的独立 actor 一个都带不上。
		 *而客户端开火那条射线（AHitScanWeapon::WeaponTraceHit）忽略的是整棵挂载树，
		 *两边不对称 → 服务器这边单方面把「客户端明明打得中」的枪判成被挡。
		 */
		auto IgnoreActorAndAttached = [&Params](AActor* Owner)
		{
			if (Owner == nullptr) return;
			TArray<AActor*> Attached;
			Owner->GetAttachedActors(Attached, true, true);
			Params.AddIgnoredActors(Attached);
		};
		IgnoreActorAndAttached(Shooter);
		for (ABlasterCharacter* Candidate : Candidates)
		{
			Params.AddIgnoredActor(Candidate);
			IgnoreActorAndAttached(Candidate);
		}

		/*
		 *用 Multi 而不是 Single：只看第一个命中时，「谁在挡」和「后面还有没有别人在挡」是混在一起的。
		 *这里把前几个命中全打出来（第一个仍然等于 Single 的结果，逻辑不变），
		 *一次日志就能看清整摞挡路的东西 —— 尤其能区分「关卡几何挡的」和「某个角色的部件挡的」。
		 */
		TArray<FHitResult> WorldHits;
		World->LineTraceMultiByChannel(WorldHits, Start, HitTarget, ECC_Visibility, Params);

		// Multi 会把「只重叠不阻挡」的也收进来，而 Single 只认阻挡 —— 只看 bBlockingHit 的第一个才是等价结果
		auto IsBlocking = [](const FHitResult& H) { return H.bBlockingHit; };
		const int32 FirstBlocking = WorldHits.IndexOfByPredicate(IsBlocking);

		if (FirstBlocking != INDEX_NONE)
		{
			const FHitResult& WorldHit = WorldHits[FirstBlocking];
			BlockRatio = WorldHit.Time;

			const UPrimitiveComponent* HitComp = WorldHit.GetComponent();
			OccluderDesc = FString::Printf(
				TEXT(" || 遮挡者=%s 组件=%s(%s) 类别=%s 预设=%s 物体类型=%d 离线眼=%.0fcm 起点在其体内=%s"),
				*GetNameSafe(WorldHit.GetActor()),
				*GetNameSafe(HitComp),
				HitComp ? *HitComp->GetClass()->GetName() : TEXT("?"),
				WorldHit.GetActor() ? *WorldHit.GetActor()->GetClass()->GetName() : TEXT("?"),
				HitComp ? *HitComp->GetCollisionProfileName().ToString() : TEXT("?"),
				HitComp ? static_cast<int32>(HitComp->GetCollisionObjectType()) : -1,
				FVector::Dist(Start, WorldHit.ImpactPoint),
				WorldHit.bStartPenetrating ? TEXT("是") : TEXT("否"));

			// 后面还摞着什么（最多再看 2 层阻挡）——用来判断「挡路的只有一层还是要穿好几层」
			int32 Listed = 0;
			for (int32 Idx = FirstBlocking + 1; Idx < WorldHits.Num() && Listed < 2; ++Idx)
			{
				if (!WorldHits[Idx].bBlockingHit) continue;
				++Listed;
				OccluderDesc += FString::Printf(TEXT(" || 其后第%d层=%s.%s 离线眼=%.0fcm"),
					Listed, *GetNameSafe(WorldHits[Idx].GetActor()),
					*GetNameSafe(WorldHits[Idx].GetComponent()),
					FVector::Dist(Start, WorldHits[Idx].ImpactPoint));
			}

			// 比例≈0 说明起点本身就埋在几何体里（眼睛贴墙/出生点异常），这一枪必然打不中任何人
			if (BlockRatio < 0.01f && bLogLagDiagnostics)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[LagComp|未命中] %s 起点被几何体包住（起点=%s 撞到=%s 比例=%.3f）—— 眼睛贴进墙里了？"),
					*Shooter->GetName(), *Start.ToCompactString(),
					*GetNameSafe(WorldHit.GetActor()), BlockRatio);
			}
		}
	}

	// 逐个候选目标，在自己那一时刻的命中盒上取「最近命中」；比墙更远的命中不算数
	float BestTime = BlockRatio;
	ABlasterCharacter* BestCharacter = nullptr;
	FName BestBone = NAME_None;
	FVector BestPoint = FVector::ZeroVector;
	bool bFound = false;
	int32 NumBoxesTested = 0;

	for (ABlasterCharacter* Other : Candidates)
	{
		ULagCompensationComponent* OtherComp = Other->GetLagCompensation();
		if (OtherComp == nullptr) continue;

		FBlasterRewindFrame Frame;
		// 用 TargetRewindTime（比射手自己的时刻再往回一个「看世界的延迟」），不是 RewindTime
		if (!OtherComp->GetRewoundFrame(TargetRewindTime, Frame))
		{
			if (bLogLagDiagnostics)
			{
				UE_LOG(LogTemp, Warning, TEXT("[LagComp|未命中] %s 拿不到目标 %s 在 %.3f 的历史帧"),
					*Shooter->GetName(), *Other->GetName(), TargetRewindTime);
			}
			continue;
		}

		const FQuat BoxRotation(FRotator(0.f, Frame.RootRotation.Yaw, 0.f));

		auto TestBox = [&](const FVector& Center, const FVector& Extent, FName Bone, const FColor& HitColor)
		{
			++NumBoxesTested;

			float HitTimeOnRay = 0.f;
			const bool bGeomHit = RayIntersectsOrientedBox(Start, HitTarget, Center, BoxRotation, Extent, HitTimeOnRay);
			const bool bWins = bGeomHit && HitTimeOnRay < BestTime;

#if ENABLE_DRAW_DEBUG
			// 无条件画：赢的用部位颜色、被挡的用橙色、几何上就没碰到的用灰色。
			// 全都不画 → 盒子压根没生成（帧/骨骼解析问题）；画了全是灰 → 射线偏了；有橙色 → 被墙挡住。
			if (bDrawDebugRewind)
			{
				const FColor Color = bWins ? HitColor : (bGeomHit ? BoxBlockedColor : BoxMissColor);
				DrawDebugBox(World, Center, Extent, BoxRotation, Color, false, 2.f, 0, bWins ? 1.5f : 0.8f);
			}
#endif

			if (!bWins) return;

			BestTime = HitTimeOnRay;
			BestCharacter = Other;
			BestBone = Bone;
			BestPoint = FMath::Lerp(Start, HitTarget, HitTimeOnRay);
			bFound = true;
		};

		// 盒子中心 = 骨骼位置 + 偏移。偏移是角色局部空间的（跟朝向一起转），
		// 因为骨骼原点和那块几何的中心并不重合 —— head 骨骼原点在脖子根，头还在上面一截。
		auto BoxCenter = [&](const FVector& BoneLocation, const FVector& Offset)
		{
			return BoneLocation + BoxRotation.RotateVector(Offset);
		};

		// 头 / 胸 / 胯 / 腿：谁在这条射线上更近就算谁。
		// 腿盒没有对应骨骼（BoneName = None → 不算爆头），挂在角色根上盖住下半身。
		TestBox(BoxCenter(Frame.HeadLocation, HeadBoxOffset), HeadExtent, ResolvedHeadBone, HeadBoxColor);
		TestBox(BoxCenter(Frame.ChestLocation, ChestBoxOffset), ChestExtent, ResolvedChestBone, ChestBoxColor);
		TestBox(BoxCenter(Frame.PelvisLocation, PelvisBoxOffset), PelvisExtent, ResolvedPelvisBone, PelvisBoxColor);
		TestBox(BoxCenter(Frame.RootLocation, LegsBoxOffset), LegsExtent, NAME_None, LegsBoxColor);
	}

#if ENABLE_DRAW_DEBUG
	if (bDrawDebugRewind)
	{
		DrawDebugLine(World, Start, HitTarget, bFound ? FColor::Red : FColor::Blue, false, 2.f, 0, 1.5f);
		// 遮挡点：射线被墙截断的位置，画个黄球 —— 命中盒全灰但这里有球 = 墙的问题
		if (BlockRatio < 1.f)
		{
			DrawDebugSphere(World, FMath::Lerp(Start, HitTarget, BlockRatio), 12.f, 12, FColor::Yellow, false, 2.f);
		}
	}
#endif

	if (!bFound || BestCharacter == nullptr)
	{
		if (bLogLagDiagnostics)
		{
			const FString BlockNote = BlockRatio < 1.f
				? (TEXT("（被挡在前面 → 命中也算超距）") + OccluderDesc)
				: FString(TEXT(""));
			UE_LOG(LogTemp, Warning,
				TEXT("[LagComp|未命中] %s 射线打进 0 个命中盒 || 候选=%d 测试盒数=%d 遮挡比例=%.2f%s"
					 " || 起=%.0f,%.0f,%.0f 终=%.0f,%.0f,%.0f"),
				*Shooter->GetName(), Candidates.Num(), NumBoxesTested, BlockRatio, *BlockNote,
				Start.X, Start.Y, Start.Z, HitTarget.X, HitTarget.Y, HitTarget.Z);
		}
		return false;
	}

	if (bLogLagDiagnostics)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LagComp|命中] %s → %s 骨骼=%s 射线比例=%.2f 距离=%.0fcm"),
			*Shooter->GetName(), *BestCharacter->GetName(),
			BestBone.IsNone() ? TEXT("腿(无骨骼)") : *BestBone.ToString(),
			BestTime, FVector::Dist(Start, BestPoint));
	}

	// 组装成一条「普通射线命中」的结果：让武器侧用同一套代码结算伤害（含爆头 BoneName）
	OutHit = FHitResult();
	OutHit.bBlockingHit = true;
	OutHit.Time = BestTime;
	OutHit.Distance = FVector::Dist(Start, BestPoint);
	OutHit.Location = BestPoint;
	OutHit.ImpactPoint = BestPoint;
	OutHit.ImpactNormal = (Start - HitTarget).GetSafeNormal();
	OutHit.TraceStart = Start;
	OutHit.TraceEnd = HitTarget;
	OutHit.BoneName = BestBone;
	OutHit.Component = BestCharacter->GetMesh();
	OutHit.HitObjectHandle = FActorInstanceHandle(BestCharacter);
	return true;
}
