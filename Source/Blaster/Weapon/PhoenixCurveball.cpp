#include "PhoenixCurveball.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "GameFramework/Controller.h"
#include "GameplayEffect.h"
#include "AbilitySystemComponent.h"
#include "Blaster/Character/BlasterCharacter.h"

APhoenixCurveball::APhoenixCurveball()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);
	NetUpdateFrequency = 100.f;

	// 组件都构造在 CDO：客户端实例用同一套（根变换随 SetReplicateMovement 复制，网格/灯跟随根）
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	CurveballMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	CurveballMesh->SetupAttachment(RootComponent);
	CurveballMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); // 爆炸判定用 Tick 里的 sweep，不开物理碰撞
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		CurveballMesh->SetStaticMesh(SphereMesh.Object);
	}
	CurveballMesh->SetRelativeScale3D(FVector(0.18f));

	CurveballLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	CurveballLight->SetupAttachment(CurveballMesh);
	CurveballLight->SetLightColor(FLinearColor(1.f, 0.92f, 0.68f));
	CurveballLight->SetIntensity(9000.f);
	CurveballLight->SetAttenuationRadius(450.f);

	FlightSoundComp = CreateDefaultSubobject<UAudioComponent>(TEXT("FlightSound"));
	FlightSoundComp->SetupAttachment(RootComponent);
	FlightSoundComp->bAutoActivate = false; // BeginPlay 由服务器/客户端各自启动（声音本地渲染，无复制）
	FlightSoundComp->SetAutoActivate(false);
}

void APhoenixCurveball::BeginPlay()
{
	Super::BeginPlay();
	// 客户端纯跟随复制位置，Tick 只在服务器推进物理
	SetActorTickEnabled(GetLocalRole() == ROLE_Authority);

	// 飞行呼啸音效：服务器/客户端各自本地播放（跟随球位置；无音频设备的服务器静默无害）
	if (FlightSoundComp && FlightSound)
	{
		FlightSoundComp->SetSound(FlightSound);
		FlightSoundComp->Play();
	}
}

void APhoenixCurveball::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (GetLocalRole() != ROLE_Authority) return;

	// 固定步长上限，防止物理穿透（高帧率下 dt 极小）
	const float dt = FMath::Min(DeltaTime, 0.05f);
	Age += dt;

	// 弧线段：施加侧向加速度让轨迹拐弯（bCurveLeft=左 / 否则右），之后改直线平飞
	if (Age < CurveTime)
	{
		const float Sign = bCurveLeft ? -1.f : 1.f;
		Velocity += FVector::RightVector * Sign * CurveAccel * dt;
	}
	// 闪光弹不吃重力，纯水平弧线飞行（用户要求）

	// 前进碰撞检测：球体从当前位置扫到目标位置（墙/人，ECC_Visibility 命中角色 mesh），
	// 命中即停在障碍表面并爆炸（Valorant 式：受墙壁和人的碰撞）
	const FVector CurrentLoc = GetActorLocation();
	const FVector TargetLoc = CurrentLoc + Velocity * dt;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	if (const AActor* Inst = GetInstigator()) Params.AddIgnoredActor(Inst); // 不因自己/投掷者贴身误爆
	FHitResult MoveHit;
	if (GetWorld()->SweepSingleByChannel(MoveHit, CurrentLoc, TargetLoc, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(20.f), Params))
	{
		SetActorLocation(MoveHit.ImpactPoint);
		Detonate();
		return;
	}

	if (Age >= Lifetime || !SetActorLocation(TargetLoc))
	{
		Detonate();
	}
}

void APhoenixCurveball::InitCurveball(const FVector& InDirection, bool bCurveLeftIn)
{
	bCurveLeft = bCurveLeftIn;
	Age = 0.f;
	bDetonated = false;

	// 出生即带一小段侧向速度（≈8° 偏角）→ 弧线从一开始就可见（用户要求"拐弯更早"），
	// 之后由 Tick 的 CurveAccel 持续平缓侧向加速（弧小、拉长）
	const FVector Forward = InDirection.GetSafeNormal();
	const float Sign = bCurveLeft ? -1.f : 1.f;
	const float InitialLateral = InitialSpeed * 0.15f;
	Velocity = Forward * InitialSpeed + FVector::RightVector * Sign * InitialLateral;
}

void APhoenixCurveball::Detonate()
{
	if (bDetonated) return;
	bDetonated = true;

	if (GetLocalRole() != ROLE_Authority)
	{
		Destroy();
		return;
	}

	const FVector BlastPos = GetActorLocation();
	const float HalfAngleRad = FMath::DegreesToRadians(FlashFOVDegrees * 0.5f);
	const float CosHalf = FMath::Cos(HalfAngleRad);

	// 盲效果总时长：从 GE CDO 的 Duration 读（兜底 1.75s）。
	// 白闪快照 ServerReceiveFlashBlind 需要绝对结束服务器时间 = Now + Duration。
	float BlindDuration = 1.75f;
	if (FlashBlindEffectClass)
	{
		const UGameplayEffect* Def = FlashBlindEffectClass->GetDefaultObject<UGameplayEffect>();
		if (Def && Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
		{
			float Dur = 1.75f;
			if (Def->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Dur) && Dur > 0.f)
			{
				BlindDuration = Dur;
			}
		}
	}

	int32 CountInRange = 0;
	int32 CountBlinded = 0;

	// 作用判定：距离 + 视线（爆炸点→受害者头，不被世界几何遮挡）+ 朝向（受害者视角方向与
	// "受害者→爆炸点"水平夹角 < 半角）。Valorant 式：队友/自己只要看着爆炸也会被闪。
	for (TActorIterator<ABlasterCharacter> It(GetWorld()); It; ++It)
	{
		ABlasterCharacter* Victim = *It;
		if (!Victim || Victim->IsElimmed()) continue;

		const FVector VictimLoc = Victim->GetActorLocation();
		if (FVector::DistSquared(VictimLoc, BlastPos) > FMath::Square(FlashRadius)) continue;
		++CountInRange;

		// LOS：从爆炸点 trace 到受害者头部（忽略曲线球自身与受害者本体，避免自己挡自己）
		const FVector TargetLoc = VictimLoc + FVector::UpVector * 80.f;
		FHitResult Hit;
		FCollisionQueryParams Params;
		Params.AddIgnoredActor(this);
		Params.AddIgnoredActor(Victim);
		if (GetWorld()->LineTraceSingleByChannel(Hit, BlastPos, TargetLoc, ECC_Visibility, Params))
		{
			continue; // 视线被世界几何/其他角色遮挡
		}

		// 朝向：优先控制器视角（真人玩家的准心方向），测试机器人/无控制器的角色回退到面朝方向
		FVector Facing = Victim->GetActorForwardVector();
		if (const AController* C = Victim->GetController())
		{
			Facing = C->GetControlRotation().Vector();
		}
		Facing.Z = 0.f;
		if (!Facing.Normalize()) continue;

		FVector ToBlast = BlastPos - VictimLoc;
		ToBlast.Z = 0.f;
		if (!ToBlast.Normalize()) continue;

		if (FVector::DotProduct(Facing, ToBlast) < CosHalf) continue; // 没看向爆炸点

		// 施加盲效果 GE（服务器执行；GE 复制到受害者客户端 → HUD 白屏读剩余时间）
		if (UAbilitySystemComponent* ASC = Victim->GetAbilitySystemComponent())
		{
			if (FlashBlindEffectClass)
			{
				FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
				// EffectCauser = 闪光弹本体 → 被闪客户端白闪 widget 读 Causer 位置 = 爆炸点，
				// 投影到屏幕画爆炸点亮斑（不是固定屏幕中心）
				Ctx.AddInstigator(GetInstigator(), this);
				const FActiveGameplayEffectHandle Applied = ASC->ApplyGameplayEffectToSelf(FlashBlindEffectClass->GetDefaultObject<UGameplayEffect>(), 1.f, Ctx);
				UE_LOG(LogTemp, Log, TEXT("[Curveball] BLIND victim=%s instigator=%s applied=%d"),
					*Victim->GetName(),
					GetInstigator() ? *GetInstigator()->GetName() : TEXT("none"),
					Applied.IsValid() ? 1 : 0);
				++CountBlinded;

					// 白闪快照写给所有客户端：盲 GE 只复制给受害者本人，阵亡的观战者读的是"被观察
					// 队友"的 ASC，GAS 对非本机客户端不暴露其活跃效果 → 服务器在此把 结束服务器
					// 时间+爆炸点 复制到受害者角色上，观战者白闪 widget 据此显示。
					// ServerReceiveFlashBlind 只在更新的结束时间到来时覆盖（同帧多颗闪光取最长）。
					Victim->ServerReceiveFlashBlind(BlastPos, BlindDuration);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[Curveball] DETONATE at %s inRange=%d blinded=%d"),
		*BlastPos.ToString(), CountInRange, CountBlinded);

	// 隐藏本体 + 延迟销毁。延迟要盖过白闪时长（GE_FlashBlind 1.75s）：
	// 被闪客户端白闪 widget 需要一直能读到 EffectCauser（本球）的位置来投影爆炸点亮斑。
	SetActorHiddenInGame(true);
	SetActorTickEnabled(false);
	GetWorldTimerManager().SetTimer(DestroyTimer, this, &APhoenixCurveball::DestroyTimerFinished, 2.0f, false);
}

void APhoenixCurveball::DestroyTimerFinished()
{
	Destroy();
}
