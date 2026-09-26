// Phoenix C 火墙。形状/判定/复制的取舍见头文件，这里只补实现上的细节：
//   · 贴地：每记一个点就往下打一条线，打不到就用球自己的 Z（见 SnapToGround）
//   · 摆板子：两端都在地面上，但高度不一样时让板子**顺着地面斜过来**（MakeFromXZ），
//     台阶/斜坡上的墙因此在视觉上是连着的
//   · 结算：和 APhoenixFireZone 一个套路（每 0.25 秒遍历全场角色做距离比较），
//     只是把"到圆心的距离"换成"到折线的距离"

#include "Blaster/Abilities/PhoenixFlameWall.h"

#include "Blaster/Blaster.h"               // ActivateNiagaraFX
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"			// TActorIterator
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// 板子网格的半尺寸（/Engine/BasicShapes 下的 Plane 和 Cube 都是 ±50 的 100 单位网格），
	// 板子的缩放就是拿它换算的 —— 和 APhoenixFireZone 按网格包围盒换算同一个路子。
	constexpr float BasicMeshHalfExtent = 50.f;

	/*
	 * 默认火焰材质：竖直流动的火。
	 *
	 * 走构造函数而不是"建个 BP_PhoenixFlameWall 在 BP 里指派"：墙现在压根没有 BP
	 * （APhoenixBlazeBall::WallClass 的默认值就是 C++ 这个类），所以默认值必须写在 C++ 里才生效。
	 * 想换材质：填 WallMaterial 属性即可（它优先）。
	 */
	const TCHAR* WallDefaultMaterialPath = TEXT("/Game/Blueprints/Abilities/M_PhoenixFlameWall_Flow.M_PhoenixFlameWall_Flow");
}

APhoenixFlameWall::APhoenixFlameWall()
{
	// 不需要 tick：墙的形状由球"喂"进来，判定走定时器。
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	// ★ 面片，不是方块：墙要的是**一个面**（配合那套双面火焰材质）。
	// 它的世界包围盒在厚度方向上是 0，所以判视线不能再用组件的 AABB（见 BlocksVisionSegment）。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultWallMaterial(WallDefaultMaterialPath);

	Segments.Reserve(FMath::Max(MaxSegments, 1));
	for (int32 i = 0; i < FMath::Max(MaxSegments, 1); ++i)
	{
		UStaticMeshComponent* Segment = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("Segment%d"), i));
		Segment->SetupAttachment(SceneRoot);
		if (PlaneMesh.Succeeded())
		{
			Segment->SetStaticMesh(PlaneMesh.Object);
		}
		if (DefaultWallMaterial.Succeeded())
		{
			Segment->SetMaterial(0, DefaultWallMaterial.Object);
		}
		// 火墙**不是障碍物**：人可以直接走进去（然后挨烧），所以碰撞全关。
		// 判定完全由 ApplyWallTick 的距离比较完成，和碰撞系统无关 —— 和火圈同一套。
		Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Segment->SetGenerateOverlapEvents(false);
		Segment->SetCanEverAffectNavigation(false);
		// 一开始全藏着，长到哪亮到哪（见 SyncSegments）
		Segment->SetVisibility(false);
		Segments.Add(Segment);
	}

	WallFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("WallFX"));
	WallFXComp->SetupAttachment(SceneRoot);
	WallFXComp->bAutoActivate = false;
	WallFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	LoopSoundComp = CreateDefaultSubobject<UAudioComponent>(TEXT("LoopSound"));
	LoopSoundComp->SetupAttachment(SceneRoot);
	LoopSoundComp->bAutoActivate = false;

	// 服务器生成 → 复制到所有客户端。墙自己不动（形状全在 PathPoints 里），
	// 所以不需要移动复制 —— 且**故意**让 actor 的位移复制关掉：板子的位置是各端自己摆的。
	bReplicates = true;
	SetReplicateMovement(false);
}

bool APhoenixFlameWall::BlocksVisionSegment(const FVector& From, const FVector& To) const
{
	// 判定的半尺寸固定这三样：长（从组件的世界缩放反推，见下）、墙高、判定厚度。
	// 这些在循环外算一次就够 —— 整面墙的板子高度/厚度都一样。
	const float HalfHeight = FMath::Max(WallHeight, 1.f) * 0.5f;
	const float HalfThickness = FMath::Max(WallThickness, 1.f) * 0.5f;

	for (const UStaticMeshComponent* Segment : Segments)
	{
		// 跳过还没立起来 / 已经用不上的板子：它们要么还停在原点（没摆过），要么是上一段墙的残留 ——
		// 不跳的话原点附近会凭空多出一块"挡视线"的判定（墙在场上别处，判定却在原点）。
		if (Segment == nullptr || !Segment->IsVisible()) continue;

		/*
		 * 判据是**我们自己造的一个跟着板子转的盒子**，不是组件的包围盒。
		 *
		 * 为什么必须自己造：板子现在是一张**零厚度的面片**，它的世界包围盒在法线方向上是压扁的，
		 * 直接拿去做线段相交会漏判（表现就是"明明躲在墙后还是被闪"）。
		 * 换成自己造的盒子还有一个好处：形状就是**这块板本身**（长 × 判定厚度 × 墙高），
		 * 斜着的板子不再像以前那样因为世界 AABB 变胖而多挡一圈。
		 *
		 * 做法：把线段两端换算到"以板心为原点、跟着板子转"的坐标系里（只转不平移不缩放），
		 * 再和一个轴对齐盒子比 —— 于是"斜的盒子 vs 直的线段"就变成了"直的盒子 vs 斜的线段"。
		 * 板子的局部轴：X = 沿墙、Y = 向上、Z = 法线（见 PlaceSegment）。
		 */
		const FTransform& T = Segment->GetComponentTransform();
		const FQuat InvRot = T.GetRotation().Inverse();
		const FVector LocalFrom = InvRot.RotateVector(From - T.GetLocation());
		const FVector LocalTo = InvRot.RotateVector(To - T.GetLocation());

		// 板长：网格是 1 米见方的，X 缩放的 100 倍就是板长，除半就是半长（PlaceSegment 摆的，这里反推）
		const float HalfLength = FMath::Abs(Segment->GetComponentScale().X) * BasicMeshHalfExtent;

		const FVector HalfExtent(HalfLength, HalfHeight, HalfThickness);
		if (FMath::LineBoxIntersection(FBox(-HalfExtent, HalfExtent), LocalFrom, LocalTo, LocalTo - LocalFrom))
		{
			return true;
		}
	}

	return false;
}

void APhoenixFlameWall::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 普通（非推送式）复制：这个数组就是这面墙的全部状态，改了就该发。
	DOREPLIFETIME(APhoenixFlameWall, PathPoints);
}

void APhoenixFlameWall::BeginPlay()
{
	Super::BeginPlay();

	// 特效资产取 WallEffect，没配就退回组件 Asset 槽里那个（见 Blaster.h ActivateNiagaraFX 的注释）
	ActivateNiagaraFX(WallFXComp, WallEffect);

	if (LoopSound && LoopSoundComp)
	{
		LoopSoundComp->SetSound(LoopSound);
		LoopSoundComp->Play();
	}

	// 材质在 BeginPlay 里换而不是构造函数里：构造函数阶段 BP 子类指派的 WallMaterial
	// 还没序列化上来（那时读到的永远是 nullptr）。和 APhoenixFireZone::OnConstruction 同一个理由。
	if (WallMaterial)
	{
		for (UStaticMeshComponent* Segment : Segments)
		{
			if (Segment) Segment->SetMaterial(0, WallMaterial);
		}
	}

	// 客户端第一次收到这个 actor 时 PathPoints 可能已经有值（中途加入/丢包重传后的完整状态），
	// 那一下 RepNotify 不一定响（初始复制不走 RepNotify）—— 所以这里自己摆一次。
	SyncSegments();
}

bool APhoenixFlameWall::AddPathPoint(const FVector& InWorldPoint)
{
	// 形状只在服务器算（它就是那个被复制的数据本身）
	if (!HasAuthority()) return false;

	const FVector Grounded = SnapToGround(InWorldPoint);

	if (PathPoints.Num() > 0)
	{
		const FVector& Last = PathPoints.Last();
		// 只比水平距离：墙是"投影到 XY 平面上"的，球往上飞/往下扎都不该多长一块板
		if (FVector::DistSquared2D(Grounded, Last) < FMath::Square(FMath::Max(SegmentSpacing, 1.f)))
		{
			return false;
		}
		// N 个点只能长出 N-1 块板，板子用完了就不再记了（墙到头了）
		// ★ 上限取 min(MaxSegments, 组件数)：BP 里把 MaxSegments 改大了也不会凭空多出组件，
		//   按 MaxSegments 放行的话会记下一堆**摆不出来**的点（墙无声地断掉一截）。
		if (PathPoints.Num() >= GetEffectiveMaxSegments() + 1)
		{
			return false;
		}
	}

	PathPoints.Add(Grounded);
	SyncSegments();
	return true;
}

void APhoenixFlameWall::BeginWall()
{
	if (!HasAuthority()) return;

	/*
	 * 硬保险丝：正常情况下这条永远不会响 —— 球飞完会调 FinishWall 把它换成 WallDuration。
	 *
	 * 留着是因为"球没了、墙还在"这种事没法从这边发现（球是另一个 actor，被销毁时不会通知我们）。
	 * 没有它的话，一次异常的球销毁 = 一面永久的墙，而且它每 0.25 秒还在烧人 —— 很难查。
	 */
	SetLifeSpan(WallDuration + 30.f);

	// 第一跳延后一个间隔（理由同火圈：生成当帧就扣血显得像瞬发，也让人没有"看到火抬脚"的机会）
	GetWorldTimerManager().SetTimer(DamageTimer, this, &APhoenixFlameWall::ApplyWallTick,
		FMath::Max(DamageTickInterval, 0.02f), /*bLoop=*/true,
		/*FirstDelay=*/FMath::Max(DamageTickInterval, 0.02f));
}

void APhoenixFlameWall::FinishWall()
{
	if (!HasAuthority()) return;

	// 从"球飞完"这一刻开始算存活时长（球还在飞的时候墙就在烧人了，不该从生成那刻算）
	SetLifeSpan(WallDuration);
}

void APhoenixFlameWall::OnRep_PathPoints()
{
	SyncSegments();
}

void APhoenixFlameWall::SyncSegments()
{
	// N 个点 → N-1 块板；点数不足 2（墙还没开始长）时是 0，不是负的
	const int32 SegmentCount = FMath::Clamp(PathPoints.Num() - 1, 0, GetEffectiveMaxSegments());

	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		UStaticMeshComponent* Segment = Segments[i];
		if (!Segment) continue;

		if (i < SegmentCount)
		{
			PlaceSegment(Segment, PathPoints[i], PathPoints[i + 1]);
			Segment->SetVisibility(true);
		}
		else
		{
			Segment->SetVisibility(false);
		}
	}
}

void APhoenixFlameWall::PlaceSegment(UStaticMeshComponent* Segment, const FVector& A, const FVector& B)
{
	if (!Segment) return;

	const FVector Delta = B - A;
	const float Length = Delta.Size();
	if (Length <= KINDA_SMALL_NUMBER) return;

	/*
	 * 一块板 = 一段墙体：局部 X 沿着 A→B，局部 Y 尽量保持世界向上（MakeFromXY），
	 * 于是法线（局部 Z）自然垂直于墙面 —— 这张**面片**就竖着立在墙线上了。
	 * 用 MakeFromXY 而不是 (B-A).Rotation()：后者会把板子按方向**滚一圈**，
	 * 平地上看不出来，一旦这段路有点坡，板子就会歪成一个斜方块。
	 */
	const FQuat Rot = FRotationMatrix::MakeFromXY(Delta / Length, FVector::UpVector).ToQuat();

	// 中心 = 两端中点再往上抬半个墙高（局部 Y 就是墙的"向上"，坡上它会跟着倾斜）
	const FVector Center = (A + B) * 0.5f + Rot.GetAxisY() * (WallHeight * 0.5f);

	// Z 缩放留 1：面片在法线方向没有尺寸（厚度只活在判定里，见 WallThickness）。
	// X 多给一点点（1.5%）：相邻板子之间留缝的话，斜着看会透光 —— 一面"看得穿"的墙就不叫墙了。
	Segment->SetWorldLocationAndRotation(Center, Rot);
	Segment->SetWorldScale3D(FVector(
		(Length / (BasicMeshHalfExtent * 2.f)) * 1.015f,
		WallHeight / (BasicMeshHalfExtent * 2.f),
		1.f));
}

FVector APhoenixFlameWall::SnapToGround(const FVector& InWorldPoint) const
{
	const UWorld* World = GetWorld();
	if (!World) return InWorldPoint;

	/*
	 * 从球的**上方**起打：球本身可能已经在贴地飞，往上让一点才不会一开场就漏掉地面。
	 *
	 * 往下这一段给得很深（Z 减 100000，比任何地图都深）：目的是"球飞多高都能找回地面" ——
	 * 玩家把准心抬起来打，球会飞到半空（球是照着准心飞的，没碰撞也没重力），
	 * 这时如果探测范围只有两千米，墙就会停在球的高度上**浮在半空**。
	 * 代价只是一条更长的射线（没打中就是一次空查询），比"墙飘着"这种难查的表现划算得多。
	 */
	const FVector Start(InWorldPoint.X, InWorldPoint.Y, InWorldPoint.Z + 200.f);
	const FVector End(InWorldPoint.X, InWorldPoint.Y, InWorldPoint.Z - 100000.f);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PhoenixFlameWallGround), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(this);
	// 别把施法者自己当地面（球从他身前生成，往下打有可能打到他）
	Params.AddIgnoredActor(GetOwner());

	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		return FVector(InWorldPoint.X, InWorldPoint.Y, Hit.ImpactPoint.Z);
	}

	// 底下什么都没有（挂在半空/地图边缘）→ 就用球自己的高度，墙浮在那儿也比沉到地图下面强
	return InWorldPoint;
}

void APhoenixFlameWall::ApplyWallTick()
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (PathPoints.Num() < 2) return;	// 还没长出来：没有可判定的墙

	ABlasterCharacter* Caster = Cast<ABlasterCharacter>(GetOwner());
	AController* CasterController = Caster ? Caster->GetController() : nullptr;
	const ABlasterPlayerState* CasterPS = Caster ? Caster->GetPlayerState<ABlasterPlayerState>() : nullptr;

	const float HalfWidthSq = FMath::Square(FMath::Max(DamageHalfWidth, 0.f));
	const float ZTol = FMath::Max(VerticalTolerance, 0.f);

	for (TActorIterator<ABlasterCharacter> It(World); It; ++It)
	{
		ABlasterCharacter* Victim = *It;
		if (!Victim || Victim->IsElimmed()) continue;

		const FVector Loc = Victim->GetActorLocation();
		const FVector2D Loc2D(Loc.X, Loc.Y);

		// —— 在不在墙里：到折线的**水平**距离（最近的那一段）——
		bool bInside = false;
		for (int32 i = 1; i < PathPoints.Num(); ++i)
		{
			const FVector& A = PathPoints[i - 1];
			const FVector& B = PathPoints[i];
			const FVector2D A2(A.X, A.Y);
			const FVector2D AB(B.X - A.X, B.Y - A.Y);
			const float LenSq = AB.SizeSquared();

			// 角色投影落在这一段的哪个位置（夹在两端之间 = 到线段，不是到直线）
			float T = 0.f;
			if (LenSq > KINDA_SMALL_NUMBER)
			{
				T = FMath::Clamp(FVector2D::DotProduct(Loc2D - A2, AB) / LenSq, 0.f, 1.f);
			}
			if (FVector2D::DistSquared(Loc2D, A2 + AB * T) > HalfWidthSq) continue;

			// 竖直：拿这一段在 T 处的高度比，别把楼上/楼下的人一起烧了（同火圈的容差）
			if (FMath::Abs(Loc.Z - FMath::Lerp(A.Z, B.Z, T)) > ZTol) continue;

			bInside = true;
			break;
		}
		if (!bInside) continue;

		// —— 施法者本人：站在自己的墙里回血（和火球同一条规则）——
		if (Victim == Caster)
		{
			if (bHealCaster)
			{
				Victim->HealByAbility(HealPerTick);
			}
			continue;
		}

		if (!bDamageEnemies) continue;

		// 只伤敌人：判据是 ABlasterPlayerState::Team（理由和取舍见 APhoenixFireZone::ApplyFireTick，
		// 拿不到双方 PlayerState 时按"敌我未定"照常造成伤害）。
		const ABlasterPlayerState* VictimPS = Victim->GetPlayerState<ABlasterPlayerState>();
		if (!Victim->IsTestBot() && VictimPS && CasterPS && VictimPS->Team == CasterPS->Team)
		{
			continue;
		}

		// DamageCauser 传墙自己（不是那个球 —— 球在这面墙活着的时候早就退场了）
		UGameplayStatics::ApplyDamage(Victim, DamagePerTick, CasterController, this, UDamageType::StaticClass());
	}
}
