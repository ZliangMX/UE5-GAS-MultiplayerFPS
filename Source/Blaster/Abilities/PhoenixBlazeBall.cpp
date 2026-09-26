#include "Blaster/Abilities/PhoenixBlazeBall.h"

#include "Blaster/Abilities/PhoenixFlameWall.h"
#include "Blaster/Blaster.h"               // ActivateNiagaraFX
#include "Blaster/Character/BlasterCharacter.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"

APhoenixBlazeBall::APhoenixBlazeBall()
{
	// 要 tick 才能每帧推进 + 跟准心（只在服务器上真开，见 BeginPlay）
	PrimaryActorTick.bCanEverTick = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	/*
	 * 根组件是个**光秃秃的 SceneComponent**：这颗球没有任何碰撞体。
	 *
	 * 用户要求"无碰撞且不可见" —— 所以既不生成碰撞球（云那颗有，因为它要判"撞到东西"），
	 * 也不留兜底网格（云那颗有，怕暗屏上看不见飞行物；这颗恰恰是**要求**看不见的）。
	 * 没有碰撞体也就意味着没有任何东西能拦住它：这就是"碰到墙不会炸开"的实现方式。
	 */
	BallFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("BallFX"));
	BallFXComp->SetupAttachment(SceneRoot);
	BallFXComp->bAutoActivate = false;
	BallFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	/*
	 * 墙类的兜底：**默认就是 C++ 那个 APhoenixFlameWall**。
	 *
	 * 和"球类留空就用 C++ 的"是同一个考虑（见 UPhoenixBlazeAbility）：整套火墙不依赖任何
	 * BP 资产也能跑起来 —— 连 BP 都没建的时候按 C 也该长出墙来（引擎默认的灰板子，
	 * 不透明、照样挡视野）。要换成带材质/特效的版本，就在 BP_PhoenixBlazeBall 里
	 * 把 WallClass 填成 BP_PhoenixFlameWall。
	 */
	WallClass = APhoenixFlameWall::StaticClass();

	// 服务器生成 → 复制到所有客户端，位移走引擎的移动复制（球是手推位置的，见 Tick）。
	// 客户端不看球（隐形），复制是为了将来加飞行特效时别人也看得见。
	bReplicates = true;
	SetReplicateMovement(true);
}

void APhoenixBlazeBall::BeginPlay()
{
	Super::BeginPlay();

	// 特效资产取 BallEffect，没配就退回组件 Asset 槽里那个（见 Blaster.h ActivateNiagaraFX 的注释）
	ActivateNiagaraFX(BallFXComp, BallEffect);

	if (HasAuthority())
	{
		SpawnWall();

		/*
		 * 飞行的推进、转向、到期全在服务器算 —— 只有服务器知道"射手还按着左键吗"的权威答案
		 *（客户端那台是预测，见 ABlasterCharacter::IsBlazeFiring 的注释）。
		 */
		GetWorldTimerManager().SetTimer(FlyTimer, this, &APhoenixBlazeBall::EndFlight, FlyDuration, false);
	}

	// 客户端不 tick：位置是复制过来的，它只需要跟着走
	SetActorTickEnabled(HasAuthority());
}

void APhoenixBlazeBall::SpawnWall()
{
	if (!WallClass)
	{
		// 没配墙类：球照样飞（虽然看不见），但地上什么都不会长出来。这条日志是唯一的线索。
		UE_LOG(LogTemp, Warning,
			TEXT("[火墙] %s 没有填 WallClass，球飞完地上不会有墙（默认值被清掉了吗？）"),
			*GetName());
		return;
	}

	FActorSpawnParameters SpawnParams;
	// ★ Owner 必须是施法者：墙的"敌人掉血 / 自己回血"全靠 Cast<ABlasterCharacter>(GetOwner()) 找施法者
	SpawnParams.Owner = GetOwner();
	SpawnParams.Instigator = GetInstigator();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	APhoenixFlameWall* Wall = GetWorld()->SpawnActor<APhoenixFlameWall>(
		WallClass, GetActorLocation(), GetActorRotation(), SpawnParams);
	if (Wall == nullptr) return;

	ActiveWall = Wall;
	// 开伤害结算（球还在飞的时候，已经长出来的那几块就该烧人了）
	Wall->BeginWall();
	// 第一个点：球现在的位置。墙从**这里**开始，不是从世界原点
	FeedWall();
}

void APhoenixBlazeBall::InitBall(const FVector& InDirection)
{
	// 方向由能力算好（角色视角，含俯仰）。兜一层：全零输入会让球原地不动，
	// 表现是"按了左键、地上只长了一块板"，很难往"方向是零"上想。
	FlightDir = InDirection;
	if (!FlightDir.Normalize())
	{
		FlightDir = GetActorForwardVector();
		if (!FlightDir.Normalize())
		{
			FlightDir = FVector::ForwardVector;
		}
	}

	SetActorRotation(FlightDir.Rotation());
}

void APhoenixBlazeBall::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bFlightEnded) return;
	if (!HasAuthority()) return;

	SteerTowardsCrosshair(DeltaSeconds);

	// 手推位置：**不做扫描**（无碰撞 = 碰到什么都是穿过去）。
	// 速度大小恒为 FlySpeed，只有方向会被上面那次转向改写。
	SetActorLocation(GetActorLocation() + FlightDir * FlySpeed * DeltaSeconds, /*bSweep=*/false);
	SetActorRotation(FlightDir.Rotation());

	PathSampleAccumulator += DeltaSeconds;
	if (PathSampleAccumulator >= FMath::Max(PathSampleInterval, 0.f))
	{
		PathSampleAccumulator = 0.f;
		FeedWall();
	}
}

void APhoenixBlazeBall::FeedWall()
{
	if (ActiveWall.IsValid())
	{
		// 喂的是**球的世界坐标**：贴地、投影到 XY 都是墙那边的事（见 SnapToGround）
		ActiveWall->AddPathPoint(GetActorLocation());
	}

	if (bDrawDebugPath)
	{
		// 球是隐形的，这是唯一能看出"它到底飞哪去了"的手段
		DrawDebugSphere(GetWorld(), GetActorLocation(), 25.f, 8, FColor::Orange,
			/*bPersistentLines=*/false, FlyDuration + 2.f);
	}
}

void APhoenixBlazeBall::SteerTowardsCrosshair(float DeltaSeconds)
{
	if (!bSteerToCrosshair || DeltaSeconds <= 0.f) return;

	/*
	 * 松手之后就不控了：球保持最后一次被掰到的方向直线飞完（用户的要求 ——
	 * "过程中左键松手了就和 jett 的 c 松手了一样让球自己飞"）。
	 * 飞的终点那边也会把射手的按住状态收掉（见 EndFlight），所以不会出现"飞完了还受控"。
	 */
	const ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetOwner());
	if (Char == nullptr || !Char->IsBlazeFiring()) return;

	const AController* Controller = Char->GetController();
	if (Controller == nullptr) return;

	/*
	 * —— 以下整段和 AJettCloudburst::SteerTowardsCrosshair 是同一套算法（有意重复，理由见头文件）——
	 * 全程在**视角基底**里算，不碰世界坐标：把当前航向和"准心那根轴"都转进视角基底，
	 * 在那儿转动，再转回世界。这样"偏了多少"和视口大小 / FOV / 宽高比都无关，
	 * 服务器上没有视口也能算。
	 */
	const FRotator AimRot = Controller->GetControlRotation();
	const FVector CamLoc = Char->GetPawnViewLocation();

	const FVector ToBall = GetActorLocation() - CamLoc;
	if (ToBall.IsNearlyZero()) return;
	// 球已经在镜头后面/贴脸 → 这一帧算出来的夹角没有意义
	if (AimRot.UnrotateVector(ToBall).X <= 1.f) return;

	const FVector CurrentLocalDir = AimRot.UnrotateVector(FlightDir).GetSafeNormal();
	const FVector TargetLocalDir = FVector::ForwardVector;

	const FVector Cross = FVector::CrossProduct(CurrentLocalDir, TargetLocalDir);
	const float SinAngle = Cross.Size();
	const float CosAngle = FVector::DotProduct(CurrentLocalDir, TargetLocalDir);
	const float AngleDegrees = FMath::RadiansToDegrees(FMath::Atan2(SinAngle, CosAngle));

	// 已经对准了 / 在死区里 → 方向不动
	if (SinAngle <= KINDA_SMALL_NUMBER || AngleDegrees <= FMath::Max(0.f, SteeringDeadZone))
	{
		return;
	}

	// 一阶控制率 + 转速封顶
	const float RateDegrees = FMath::Min(AngleDegrees * FMath::Max(0.f, SteeringGain),
		FMath::Max(0.f, MaxSteeringRate));
	// 这一帧最多把角度差走完，不做过冲
	const float StepDegrees = FMath::Min(RateDegrees * DeltaSeconds, AngleDegrees);

	const FQuat Delta(Cross / SinAngle, FMath::DegreesToRadians(StepDegrees));
	FVector NewDir = AimRot.RotateVector(Delta.RotateVector(CurrentLocalDir)).GetSafeNormal();

	// 俯仰夹一层（不夹的话球能被掰到垂直往上，墙就原地长不出来）
	FRotator NewRot = NewDir.Rotation();
	const float PitchLimit = FMath::Clamp(MaxSteeringPitch, 0.f, 89.f);
	NewRot.Pitch = FMath::Clamp(NewRot.Pitch, -PitchLimit, PitchLimit);
	NewDir = NewRot.Vector();

	// 只换方向、大小恒为 FlySpeed
	FlightDir = NewDir;
}

void APhoenixBlazeBall::EndFlight()
{
	// 到期定时器和"球被销毁"两条路都可能进来 → 门禁
	if (bFlightEnded) return;
	bFlightEnded = true;

	if (!HasAuthority()) return;

	GetWorldTimerManager().ClearTimer(FlyTimer);

	// 墙从这一刻起算存活时长（球没飞完不该开始倒计时）
	if (ActiveWall.IsValid())
	{
		ActiveWall->FinishWall();
	}

	/*
	 * 收掉射手那一段"按住控球"。
	 *
	 * 规则和逐风云那条一样：飞完了还按着也没有东西可控了，所以主动收尾
	 *（清标志 + 掏枪；枪会等"发射那一下"的动画播完再出来，见 ABlasterCharacter::StopBlazeFire）。
	 * 玩家不用为了掏枪还得记着松手。
	 *
	 * ⚠ 松手那条路（FireEnd → StopBlazeFire）和这条会先后都到 —— StopBlazeFire 自带幂等门禁。
	 */
	if (ABlasterCharacter* Caster = Cast<ABlasterCharacter>(GetOwner()))
	{
		Caster->StopBlazeFire();
	}

	// 自己退场。留 0.2 秒而不是立刻 Destroy：球是被复制的 actor，位置最后那一跳要发出去，
	// 免得客户端上（将来加了飞行特效的话）停在半路。
	SetLifeSpan(0.2f);
}
