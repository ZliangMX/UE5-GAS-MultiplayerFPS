// Phoenix Q「火球」的实现说明见头文件。这里只补一条容易踩的：
// 下坠段**不动手写速度**，而是把 ProjectileGravityScale 打开交给 ProjectileMovement ——
// 这样"撞地"仍然由引擎发 OnComponentHit，不用自己再做一套落地检测。

#include "Blaster/Abilities/PhoenixFireball.h"

#include "Blaster/Abilities/PhoenixFireZone.h"
#include "Blaster/Blaster.h"   // ECC_SkeletalMesh（工程自定义的骨骼网格通道）

#include "Components/AudioComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"		// ResolveGroundPoint 里判"撞到的是不是人"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

APhoenixFireball::APhoenixFireball()
{
	// 要 tick 才能累计飞行距离（只在服务器上真开，见 BeginPlay）。
	PrimaryActorTick.bCanEverTick = true;

	// 碰撞球当根：飞行扫描、撞墙/撞人判定都挂在它身上。
	// 半径不用大 —— 判定的是"球心碰到东西"，球本身多大是特效的事，不该影响手感。
	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	SetRootComponent(CollisionSphere);
	CollisionSphere->InitSphereRadius(16.f);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	// 只挡"实心的东西"：地形/建筑、别的 actor、人。命中的就是这三个通道。
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_SkeletalMesh, ECR_Block);

	// 飞行特效载体：挂根上跟着走。bAutoActivate=false —— 特效要等 InitFireball
	// 拿到方向之后再开，生成瞬间就开会在指尖先闪一下原地喷的粒子。
	FlightFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("FlightFX"));
	FlightFXComp->SetupAttachment(CollisionSphere);
	FlightFXComp->bAutoActivate = false;
	FlightFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 兜底可见物（没有 Niagara 资产时的发光球）
	FireMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FireMesh"));
	FireMesh->SetupAttachment(CollisionSphere);
	FireMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FireMesh->SetGenerateOverlapEvents(false);
	FireMesh->SetCanEverAffectNavigation(false);
	FireMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		FireMesh->SetStaticMesh(SphereMesh.Object);
	}

	FlightSoundComp = CreateDefaultSubobject<UAudioComponent>(TEXT("FlightSound"));
	FlightSoundComp->SetupAttachment(CollisionSphere);
	FlightSoundComp->bAutoActivate = false;

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->SetIsReplicated(true);
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
	// 直线段不受重力：速度全部由 InitFireball 给的初速度决定（方向 = 角色视角，含俯仰）。
	// 下坠段由 BeginDrop 把 ProjectileGravityScale 打开 —— 不是两套代码，是同一个组件的两种用法。
	ProjectileMovement->ProjectileGravityScale = 0.f;
	ProjectileMovement->InitialSpeed = 0.f;
	ProjectileMovement->MaxSpeed = 0.f;

	// 服务器生成 → 复制到所有客户端，位移由 ProjectileMovementComponent 复制
	bReplicates = true;
	SetReplicateMovement(true);
}

void APhoenixFireball::BeginPlay()
{
	Super::BeginPlay();

	// 特效资产取 FlightEffect（规范位置），没配就退回组件 Asset 槽里那个 —— 见 Blaster.h 的长注释：
	// 原来只认属性，资产填在组件上时这里整段进不去、组件（bAutoActivate=false）永远不激活，
	// 表现就是"配了 Niagara、局内看不见"。
	if (ActivateNiagaraFX(FlightFXComp, FlightEffect))
	{
		// 有真特效了，兜底球就没必要再露出来
		if (FireMesh) FireMesh->SetVisibility(false);
	}
	else if (FireMesh)
	{
		// 兜底球的缩放放在这里而不是构造函数里：BP 子类里改的 VisualScale
		// 构造函数阶段读不到（那时 CDO 的 UPROPERTY 还是 C++ 默认值）。
		FireMesh->SetRelativeScale3D(FVector(VisualScale));
		if (FireMaterial)
		{
			FireMesh->SetMaterial(0, FireMaterial);
		}
		FireMesh->SetVisibility(true);
	}

	if (FlightSound && FlightSoundComp)
	{
		FlightSoundComp->SetSound(FlightSound);
		FlightSoundComp->Play();
	}

	// 飞行 + 碰撞 + 下坠判定全在服务器算；客户端只跟随复制过来的位置。
	// 客户端不绑 OnComponentHit 的话，它会"穿过墙继续飞"——正是想要的，
	// 它只是跟随，命运由服务器决定。
	if (HasAuthority())
	{
		CollisionSphere->OnComponentHit.AddDynamic(this, &APhoenixFireball::OnFireballHit);
		GetWorld()->GetTimerManager().SetTimer(FlightFuseTimer, this, &APhoenixFireball::OnFlightFuseExpired, MaxFlightTime, false);
	}

	// 距离累计只有服务器做，客户端一律关掉 tick。
	SetActorTickEnabled(HasAuthority());
}

void APhoenixFireball::InitFireball(const FVector& InDirection)
{
	// 方向由能力算好（角色视角，含俯仰）。这里只兜一层：全零输入会让
	// ProjectileMovement 保持静止，表现为"球停在指尖不动"，很难查。
	FVector Dir = InDirection;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
	}

	MyOwner = GetOwner();

	SetActorRotation(Dir.Rotation());

	if (ProjectileMovement)
	{
		ProjectileMovement->InitialSpeed = FlySpeed;
		ProjectileMovement->MaxSpeed = FlySpeed;
		ProjectileMovement->Velocity = Dir * FlySpeed;
		// 立刻推一帧，免得生成的当帧还停在指尖
		ProjectileMovement->UpdateComponentVelocity();
	}

	// 出指尖就撞到射手自己（球从手边生成，半径 16 的球已经在他身体里）→ 那会当场
	// 铺一圈火在自己脚下。ProjectileMovement 的扫描移动会读这张表，所以球会直接穿过
	// 射手飞出去（和 Jett 云、飞刀同样的处理）。
	if (MyOwner && CollisionSphere)
	{
		CollisionSphere->IgnoreActorWhenMoving(MyOwner, true);

		TArray<AActor*> AttachedToOwner;
		MyOwner->GetAttachedActors(AttachedToOwner, /*bResetArray=*/true, /*bRecursivelyIncludeAttachedActors=*/true);
		for (AActor* Attached : AttachedToOwner)
		{
			if (Attached)
			{
				CollisionSphere->IgnoreActorWhenMoving(Attached, true);
			}
		}
	}
}

void APhoenixFireball::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bLanded || bDropping) return;
	if (!HasAuthority()) return;	// 客户端不 tick（BeginPlay 里关了），这行是给安全性兜的

	// ★ 只累计直线段：下坠段是"已经到射程了"之后的自由落体，再往里加会把距离翻倍 ——
	//   虽然这里已经 return 掉了，但把条件写全比靠 return 顺序更抗改动。
	const float Speed = ProjectileMovement ? ProjectileMovement->Velocity.Size() : 0.f;
	TraveledDistance += Speed * DeltaSeconds;

	if (TraveledDistance >= MaxRange)
	{
		BeginDrop();
	}
}

void APhoenixFireball::BeginDrop()
{
	if (bDropping || bLanded) return;
	bDropping = true;

	if (!ProjectileMovement) return;

	// 丢平：水平速度清零、竖直保留（直线段没受重力，所以这里基本就是 0），
	// 再把重力打开让它自己往下掉。
	// ★ 不手写 Z 速度：交给 ProjectileMovement 才拿得到引擎发的"撞地"事件。
	const float VerticalSpeed = ProjectileMovement->Velocity.Z;
	ProjectileMovement->Velocity = FVector(0.f, 0.f, VerticalSpeed);
	ProjectileMovement->ProjectileGravityScale = FMath::Max(DropGravityScale, 0.f);
	ProjectileMovement->UpdateComponentVelocity();
}

void APhoenixFireball::OnFireballHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	LandAt(Hit.ImpactPoint, OtherActor);
}

void APhoenixFireball::OnFlightFuseExpired()
{
	// 飞了 6 秒还没撞到任何东西（飞出地图边缘/掉进无底洞）—— 就地落地，
	// 宁可火铺在半空往下找到的那块地上，也不要"扔出去了什么都没有"。
	UE_LOG(LogTemp, Warning, TEXT("[火球] %s 飞行超时（%.1fs）还没落地，强制落地生成火圈"), *GetName(), MaxFlightTime);
	LandAt(GetActorLocation(), nullptr);
}

FVector APhoenixFireball::ResolveGroundPoint(const FVector& From, AActor* IgnoredActor) const
{
	const UWorld* World = GetWorld();
	if (!World) return From;

	const FVector Start = From + FVector(0.f, 0.f, GroundTraceUpOffset);
	const FVector End = From - FVector(0.f, 0.f, GroundTraceDownDistance);

	// Visibility 是"找地面"的标准通道（WorldStatic / WorldDynamic / Pawn 默认都挡它）。
	FCollisionQueryParams Params(TEXT("PhoenixFireballGround"), /*bTraceComplex=*/false, this);
	/*
	 * ★ 撞到人身上时不能把他自己当"地面"—— 否则火会铺在他胸口高度，
	 *   而他站在火里却掉不到火的判定范围（火圈自己在 Z 上只有 150cm 容差）。
	 *
	 * ⚠ 但**只能忽略"人"**：球撞地那一拍 OtherActor 就是脚下那块地板，把它一起忽略掉的话，
	 *   射线会**穿过地板继续往下找** —— 地图下层（夹层 / 下层地形）只要还有东西，
	 *   火圈就生成在地板**下面**。表现是"球砸下去了、然后地上什么都没有"，而且一条日志都不会有
	 *   （球确实落地了、也确实生成了火圈，只是生在看不见的地方）—— 最难查的那类。
	 *   原来是无条件 AddIgnoredActor(IgnoredActor)，踩的就是这个坑。
	 *
	 * 施法者自己也一直忽略（球从他身前生成，往下打有可能打到他）。
	 */
	if (IgnoredActor && (IgnoredActor == GetOwner() || IgnoredActor->IsA<APawn>()))
	{
		Params.AddIgnoredActor(IgnoredActor);
	}

	FHitResult GroundHit;
	if (World->LineTraceSingleByChannel(GroundHit, Start, End, ECC_Visibility, Params))
	{
		return GroundHit.ImpactPoint;
	}

	// 脚下什么都没有（悬空处）—— 就用撞击点本身，至少火圈会出现，
	// 而不是"炸了一下什么都没有"这种最难查的表现。
	return From;
}

void APhoenixFireball::LandAt(const FVector& ImpactPoint, AActor* HitActor)
{
	if (bLanded) return;
	bLanded = true;

	// 只有服务器生成火圈；客户端在这条路径上什么都不做
	if (!HasAuthority()) return;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlightFuseTimer);
	}

	// 停下来：飞行特效/音效立刻收掉，碰撞关掉（不然这 0.5 秒里还可能再撞一次、
	// 再进一次 LandAt —— 虽然被 bLanded 挡住了，但白跑一次扫描）
	if (ProjectileMovement)
	{
		ProjectileMovement->StopMovementImmediately();
		ProjectileMovement->Deactivate();
	}
	if (CollisionSphere) CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (FlightFXComp) FlightFXComp->Deactivate();
	if (FlightSoundComp) FlightSoundComp->Stop();

	const FVector GroundPoint = ResolveGroundPoint(ImpactPoint, HitActor);

	// 广播落地特效：Niagara 不复制，必须自己通知所有端
	MulticastLandFX(GroundPoint);

	APhoenixFireZone* Zone = nullptr;

	if (!FireZoneClass)
	{
		// 没配火圈类：球飞出去了、砸地了、然后地上什么都没有。这条日志是唯一的线索。
		UE_LOG(LogTemp, Warning,
			TEXT("[火球] %s 没有填 FireZoneClass，落地后不会有火圈（请在 BP_PhoenixFireball 里填 BP_PhoenixFireZone）"),
			*GetName());
	}
	else if (UWorld* World = GetWorld())
	{
		// ★ 必须走 Deferred spawn：FireRadius / FireDuration 要在 BeginPlay **之前**写进去。
		//   APhoenixFireZone::BeginPlay 里直接 SetLifeSpan + 起伤害定时器，SpawnActor
		//   返回之后再改已经晚了（不会报错，只是配置静默不生效）。
		const FTransform SpawnXform(FRotator::ZeroRotator, GroundPoint);

		Zone = World->SpawnActorDeferred<APhoenixFireZone>(
			FireZoneClass, SpawnXform, GetOwner(), GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Zone)
		{
			// Owner = 施法者：火圈靠它认"谁踩自己的火要回血"（见 APhoenixFireZone::ApplyFireTick）
			Zone->FinishSpawning(SpawnXform);
		}
		else
		{
			// 配了类却 Spawn 不出来（类没加载 / 蓝图有编译错 / 世界正在关闭）：
			// 不报的话表现和"没配"完全一样 —— 都是地上什么都没有。
			UE_LOG(LogTemp, Warning, TEXT("[火球] %s 火圈生成失败（FireZoneClass=%s 没能 Spawn 出来）"),
				*GetName(), *GetNameSafe(FireZoneClass));
		}
	}

	/*
	 * 落地回执（成功路径也打，和 [火墙] THROW 那条同一个用意）。
	 *
	 * 落地这一拍原本一条日志都没有：出问题时"球飞出去了、然后什么都没有"只能靠猜。
	 * ΔZ = 解析出的地面点比撞击点低多少 —— 正常贴地撞击 ≈0，撞在人身上 ≈ 一个身高；
	 * **明显偏大（几百上千）就是射线穿了地板、火圈生到地板下面去了**（见 ResolveGroundPoint）。
	 */
	UE_LOG(LogTemp, Log, TEXT("[火球] LAND 撞击=%s 地面=%s ΔZ=%.1f 火圈=%s"),
		*ImpactPoint.ToString(), *GroundPoint.ToString(), GroundPoint.Z - ImpactPoint.Z,
		Zone ? *Zone->GetName() : TEXT("未生成"));

	// 自己退场：不立刻 Destroy —— MulticastLandFX 是 Unreliable 的普通 RPC，
	// 和"actor 被销毁"这条复制走的是不同通道，同一帧里谁先到不保证。
	// 留 0.5 秒让特效 RPC 稳稳送到（这 0.5 秒里它已经不渲染、不碰撞、不移动）。
	SetLifeSpan(FMath::Max(DespawnDelay, 0.05f));
}

void APhoenixFireball::MulticastLandFX_Implementation(FVector Location)
{
	// NetMulticast = 服务器自己也执行一遍，所以这条在每台机器（含 Host）上各播一次，
	// 一次不多一次不少。不要在里面再判 HasAuthority。
	if (LandEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			this, LandEffect, Location, FRotator::ZeroRotator, FVector::OneVector, /*bAutoDestroy=*/true);
	}
	if (LandSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, LandSound, Location);
	}
}
