#include "Blaster/Weapon/JettCloudburst.h"

#include "Blaster/Abilities/CloveSmoke.h"
#include "Blaster/Blaster.h"   // ECC_SkeletalMesh（工程自定义的骨骼网格通道）
#include "Blaster/Character/BlasterCharacter.h"	// 按住状态 / 视角（跟准心要读）

#include "Components/AudioComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

AJettCloudburst::AJettCloudburst()
{
	// 要 tick 才能每帧跟准心（只在服务器上真开，见 BeginPlay）。
	PrimaryActorTick.bCanEverTick = true;

	// 碰撞球当根：飞行扫描、撞墙判定都挂在它身上。
	// 半径不用大 —— 判定的是"云的中心撞到墙"，云本身多大是特效的事，不该影响手感。
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

	// 飞行特效载体：挂根上跟着走。bAutoActivate=false —— 特效要等 InitCloud
	// 拿到方向之后再开，生成瞬间就开会在指尖先闪一下原地喷的粒子。
	CloudFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("CloudFX"));
	CloudFXComp->SetupAttachment(CollisionSphere);
	CloudFXComp->bAutoActivate = false;
	CloudFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 兜底可见物（没有 Niagara 资产时的球）
	CloudMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CloudMesh"));
	CloudMesh->SetupAttachment(CollisionSphere);
	CloudMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CloudMesh->SetGenerateOverlapEvents(false);
	CloudMesh->SetCanEverAffectNavigation(false);
	CloudMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		CloudMesh->SetStaticMesh(SphereMesh.Object);
	}
	// 没有飞行特效时它就是唯一可见的东西；配了特效（BeginPlay 里查到）就把它藏了。
	CloudMesh->SetVisibility(false);

	FlightSoundComp = CreateDefaultSubobject<UAudioComponent>(TEXT("FlightSound"));
	FlightSoundComp->SetupAttachment(CollisionSphere);
	FlightSoundComp->bAutoActivate = false;

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->SetIsReplicated(true);
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
	// 直线飞：不受重力，速度全部由 InitCloud 给的初速度决定（方向 = 角色视角，含俯仰）
	ProjectileMovement->ProjectileGravityScale = 0.f;
	ProjectileMovement->InitialSpeed = 0.f;
	ProjectileMovement->MaxSpeed = 0.f;

	// 服务器生成 → 复制到所有客户端，位移由 ProjectileMovementComponent 复制
	bReplicates = true;
	SetReplicateMovement(true);
}

void AJettCloudburst::BeginPlay()
{
	Super::BeginPlay();

	if (CloudEffect && CloudFXComp)
	{
		CloudFXComp->SetAsset(CloudEffect);
		CloudFXComp->Activate(true);
		// 有真特效了，兜底球就没必要再露出来
		if (CloudMesh) CloudMesh->SetVisibility(false);
	}
	else if (CloudMesh)
	{
		// 兜底球的缩放放在这里而不是构造函数里：BP 子类里改的 CloudVisualScale
		// 构造函数阶段读不到（那时 CDO 的 UPROPERTY 还是 C++ 默认值）。
		CloudMesh->SetRelativeScale3D(FVector(CloudVisualScale));
		CloudMesh->SetVisibility(true);
	}

	if (FlightSound && FlightSoundComp)
	{
		FlightSoundComp->SetSound(FlightSound);
		FlightSoundComp->Play();
	}

	// 飞行 + 碰撞 + 到期全在服务器算；客户端只跟随复制过来的位置。
	// 碰撞不在客户端绑 OnComponentHit 的话，客户端会"穿过墙继续飞"——正是想要的，
	// 它只是跟随，命运由服务器决定。
	if (HasAuthority())
	{
		CollisionSphere->OnComponentHit.AddDynamic(this, &AJettCloudburst::OnCloudHit);
		GetWorld()->GetTimerManager().SetTimer(FlyTimer, this, &AJettCloudburst::OnFlyTimerExpired, FlyDuration, false);
	}

	// 跟准心是服务器的事（方向=位移，本来就是服务器权威），客户端一律关掉 tick。
	// 在这里关而不是留到 Tick 里判：这朵云寿命只有 1.5 秒，但全场每人一朵的话
	// 一个 tick 函数空跑也是白跑。
	SetActorTickEnabled(HasAuthority());
}

void AJettCloudburst::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bBloomed) return;			// 已经绽放过（服务器上退场前还有 0.5 秒）
	if (!HasAuthority()) return;	// 客户端不 tick（BeginPlay 里关了），这行是给安全性兜的

	SteerTowardsCrosshair(DeltaSeconds);
}

void AJettCloudburst::SteerTowardsCrosshair(float DeltaSeconds)
{
	if (!bSteerToCrosshair || DeltaSeconds <= 0.f) return;

	// 松手之后就不控了：云保持最后一次被掰到的方向直线飞到绽放（用户的要求 ——
	// 松手只是"不控了"，不是"把云停住"）。云自己绽放那一下也会把按住状态收掉
	//（见 Bloom 末尾），所以"飞完了还一直受控"这种事不会发生。
	const ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetOwner());
	if (Char == nullptr || !Char->IsCloudburstHoldActive()) return;

	const AController* Controller = Char->GetController();
	if (Controller == nullptr) return;

	FVector Dir = ProjectileMovement ? ProjectileMovement->Velocity : FVector::ZeroVector;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
	}

	/*
	 * 全程在**视角基底**里算，不碰世界坐标：把当前航向和"准心那根轴"都转进视角基底，
	 * 在那儿转动，再转回世界。
	 *
	 * ★ 为什么不用"云在屏幕上的像素坐标"：服务器上没有视口，也拿不到 FOV。
	 *   视角基底里"云的偏移量"（后面那两行 Offset）是屏幕偏移的 tan 值，
	 *   和视口大小 / FOV / 宽高比都无关 —— 同一个算法在独立服务器和房主机器上结果一致。
	 */
	const FRotator AimRot = Controller->GetControlRotation();
	const FVector CamLoc = Char->GetPawnViewLocation();

	const FVector ToCloud = GetActorLocation() - CamLoc;
	// 云已经在镜头后面/贴脸 → 这次算出来的夹角没有意义，这一帧不控。
	// （正常飞行出不去这个分支：1.5 秒内云一直在往前飞。）
	if (ToCloud.IsNearlyZero()) return;

	// 云在不在镜头**前方**（视角基底里的 X 分量）。转到视角基底 = 前 / 右 / 上三个分量。
	// 顺带说明这个算法为什么和视口无关：云在屏幕上的偏移量就是 Local.Y/Local.X 与
	// Local.Z/Local.X（两个 tan 值），它们和视口尺寸 / FOV / 宽高比都无关 ——
	// 服务器上没有视口也能算。这里只用 X 判一次"在不在前面"。
	if (AimRot.UnrotateVector(ToCloud).X <= 1.f) return;

	/*
	 * 目标航向 = 准心那根轴（视角基底里的 +X）。
	 *
	 * 为什么是"对准轴"而不是"对准准心所在的点"：屏幕上看到的就是"云在准心的哪个方位"，
	 * 把航向掰到和轴平行，云在屏幕上就朝准心收敛（beam-rider）—— 偏得越远收敛越快，
	 * 到了准心上就自然保持。这条正好也是用户描述的那个"按偏移施加一个力"。
	 */
	const FVector CurrentLocalDir = AimRot.UnrotateVector(Dir).GetSafeNormal();
	const FVector TargetLocalDir = FVector::ForwardVector;

	// 当前航向离目标航向差多少度（两个单位向量的夹角）
	const FVector Cross = FVector::CrossProduct(CurrentLocalDir, TargetLocalDir);
	const float SinAngle = Cross.Size();
	const float CosAngle = FVector::DotProduct(CurrentLocalDir, TargetLocalDir);
	const float AngleDegrees = FMath::RadiansToDegrees(FMath::Atan2(SinAngle, CosAngle));

	// 已经对准了 / 在死区里 → 方向不动，速度也不动（速度本来就只有大小守恒那一行在写）
	if (SinAngle <= KINDA_SMALL_NUMBER || AngleDegrees <= FMath::Max(0.f, SteeringDeadZone))
	{
		return;
	}

	// 一阶控制率：转多少 ∝ 差多少，再封顶（见头文件里那两个参数的说明）
	const float RateDegrees = FMath::Min(AngleDegrees * FMath::Max(0.f, SteeringGain), FMath::Max(0.f, MaxSteeringRate));

	// 这一帧最多把角度差走完，不做过冲（dt 很大时也不会转过头）
	const float StepDegrees = FMath::Min(RateDegrees * DeltaSeconds, AngleDegrees);

	const FQuat Delta(Cross / SinAngle, FMath::DegreesToRadians(StepDegrees));
	FVector NewDir = AimRot.RotateVector(Delta.RotateVector(CurrentLocalDir)).GetSafeNormal();

	// 俯仰夹一层（理由见头文件 MaxSteeringPitch）
	FRotator NewRot = NewDir.Rotation();
	const float PitchLimit = FMath::Clamp(MaxSteeringPitch, 0.f, 89.f);
	NewRot.Pitch = FMath::Clamp(NewRot.Pitch, -PitchLimit, PitchLimit);
	NewDir = NewRot.Vector();

	// ★ 只换方向、大小恒为 FlySpeed —— 用户的要求："不影响向前的速度"。
	//   （InitCloud 里 MaxSpeed 也设成了 FlySpeed，所以这里不会被引擎再夹一次。）
	if (ProjectileMovement)
	{
		ProjectileMovement->Velocity = NewDir * FlySpeed;
		ProjectileMovement->UpdateComponentVelocity();
	}
}

void AJettCloudburst::InitCloud(const FVector& InDirection)
{
	// 方向由能力算好（角色视角，含俯仰）。这里只兜一层：全零输入会让
	// ProjectileMovement 保持静止，表现为"云停在指尖不动"，很难查。
	FVector Dir = InDirection;
	if (!Dir.Normalize())
	{
		Dir = GetActorForwardVector();
	}

	SetActorRotation(Dir.Rotation());

	if (ProjectileMovement)
	{
		ProjectileMovement->bRotationFollowsVelocity = true;
		ProjectileMovement->InitialSpeed = FlySpeed;
		ProjectileMovement->MaxSpeed = FlySpeed;
		ProjectileMovement->Velocity = Dir * FlySpeed;
		// 立刻推一帧，免得生成的当帧还停在指尖
		ProjectileMovement->UpdateComponentVelocity();
	}

	// 出指尖就撞到射手自己（云从手边生成，半径 16 的球已经在他身体里）→
	// 那会把云当场绽放在脸上。ProjectileMovement 的扫描移动会读这张表，
	// 所以云会直接穿过射手飞出去（和 Jett 飞刀同样的处理，见 AProjectile::BeginPlay）。
	if (AActor* MyOwner = GetOwner())
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

void AJettCloudburst::OnCloudHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	Bloom();
}

void AJettCloudburst::OnFlyTimerExpired()
{
	Bloom();
}

void AJettCloudburst::Bloom()
{
	// 碰撞和到期定时器可能在同一帧先后进来 → 没有这个守卫会生成两个烟球。
	// 另外 Destroy 之后客户端的复制回调也可能再打一次，所以守卫放在最前面。
	if (bBloomed) return;
	bBloomed = true;

	// 客户端不生成任何东西 —— 烟球是服务器生成的复制 actor
	if (!HasAuthority()) return;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlyTimer);
	}

	// 停下来：飞行特效/音效立刻收掉，碰撞关掉（不然这 0.5 秒里它还可能再撞一次）
	if (ProjectileMovement) ProjectileMovement->StopMovementImmediately();
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (CloudFXComp) CloudFXComp->Deactivate();
	if (FlightSoundComp) FlightSoundComp->Stop();

	const FVector BloomLocation = GetActorLocation();
	const FRotator BloomRotation = GetActorRotation();

	// —— 1) 广播绽放特效（Niagara 不复制，必须自己通知所有端）——
	MulticastBloomFX(BloomLocation, BloomRotation);

	// —— 2) 生成烟球 ——
	if (!SmokeClass)
	{
		// 没配烟雾类：云飞出去了、撞到了、然后什么都没有。这条日志是唯一的线索。
		UE_LOG(LogTemp, Warning,
			TEXT("[逐风云] %s 没有填 SmokeClass，云绽放后不会有烟球（请在 BP_JettCloudburst 里填 BP_CloveSmoke 或另建的白色烟球）"),
			*GetName());
	}
	else if (UWorld* World = GetWorld())
	{
		// ★ 必须走 Deferred spawn：SmokeRadius / SmokeDuration 要在 BeginPlay **之前**写进去。
		//   ACloveSmoke::BeginPlay 里直接 SetLifeSpan(SmokeDuration)，SpawnActor 返回之后再改
		//   已经晚了 —— 那一发会按暮蝶的 15 秒活，而且半径是 BP 里那个 250。
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = GetOwner();
		SpawnParams.Instigator = GetInstigator();
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ACloveSmoke* Smoke = World->SpawnActorDeferred<ACloveSmoke>(
			SmokeClass, FTransform(BloomRotation, BloomLocation), GetOwner(), GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Smoke)
		{
			Smoke->SmokeRadius = SmokeRadius;
			Smoke->SmokeDuration = SmokeDuration;
			// FinishSpawning 才触发 OnConstruction（半径→缩放的换算）和复制注册
			Smoke->FinishSpawning(FTransform(BloomRotation, BloomLocation));
		}
	}

	/*
	 * —— 3) 收掉射手那一段"按住" ——
	 *
	 * 规则（用户定的）：按住控云这段**到云绽放为止**。云飞完了还按着 C 也没有东西可控了，
	 * 所以这里主动收尾：清掉按住标志 + 播 Outro + 到点掏枪（见 ABlasterCharacter::EndCloudburstHold）。
	 * 玩家不用为了掏枪还得记着松手。
	 *
	 * ⚠ 松手那条路（SkillCReleased）和这条会先后都到 —— EndCloudburstHold 自带幂等门禁，
	 *   谁先到都一样，不会播两遍 Outro。
	 */
	if (ABlasterCharacter* Caster = Cast<ABlasterCharacter>(GetOwner()))
	{
		Caster->EndCloudburstHold();
	}

	// —— 4) 自己退场 ——
	// 不立刻 Destroy：MulticastBloomFX 是 Unreliable 的普通 RPC，和"actor 被销毁"这条
	// 复制走的是不同通道，同一帧里谁先到不保证。留 0.5 秒让特效 RPC 稳稳送到。
	// （这 0.5 秒里它已经不渲染、不碰撞、不移动，只是个空壳。）
	SetLifeSpan(0.5f);
}

void AJettCloudburst::MulticastBloomFX_Implementation(FVector Location, FRotator Rotation)
{
	// NetMulticast = 服务器自己也执行一遍，所以这条在每台机器（含 Host）上各播一次，
	// 一次不多一次不少。不要在里面再判 HasAuthority。
	if (BloomEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			this, BloomEffect, Location, Rotation, FVector::OneVector, /*bAutoDestroy=*/true);
	}
}
