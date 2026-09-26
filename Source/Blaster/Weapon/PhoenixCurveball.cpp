#include "PhoenixCurveball.h"
#include "Blaster/Blaster.h"                    // ActivateNiagaraFX
#include "Blaster/Interfaces/VisionBlockerInterface.h"
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
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
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

	/*
	 * 飞行特效载体。挂**根**上而不是挂 CurveballMesh 上：配了特效时会把占位球藏掉
	 *（CurveballMesh 的显隐会传给子组件），挂在它下面的话特效会跟着一起消失。
	 * 挂根上既不受影响，也照样随 actor 复制到所有客户端（组件本身不是复制的，位置来自根）。
	 *
	 * bAutoActivate=false：等 BeginPlay 里拿到 BP 指派的 FlightEffect 再开 ——
	 * 构造函数阶段读不到 BP 的值，直接开会在生成那一瞬间先按 CDO 的空资产空转一次。
	 */
	FlightFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("FlightFX"));
	FlightFXComp->SetupAttachment(RootComponent);
	FlightFXComp->bAutoActivate = false;
	FlightFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void APhoenixCurveball::BeginPlay()
{
	Super::BeginPlay();
	// 客户端纯跟随复制位置，Tick 只在服务器推进物理
	SetActorTickEnabled(GetLocalRole() == ROLE_Authority);

	/*
	 * 飞行特效：资产取 FlightEffect（规范位置），没配就退回组件 Asset 槽里那个 —— 见 Blaster.h
	 * 里那段长注释（"配了 Niagara 局内看不见"就是只认属性、资产却填在组件上）。
	 * 开成功就把占位球藏掉；**点光留着** —— 它那圈照亮墙面的光是环境感，和特效不冲突，
	 * 而且它挂在占位球下面，所以这里只藏网格自己（bPropagateToChildren=false）。
	 */
	if (ActivateNiagaraFX(FlightFXComp, FlightEffect) && CurveballMesh)
	{
		CurveballMesh->SetVisibility(false, /*bPropagateToChildren=*/false);
	}

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
		Velocity += CurveRight * Sign * CurveAccel * dt;
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

	// ★ 侧向方向必须相对**抛掷方向**算，不能直接用世界的 `FVector::RightVector`：
	//   世界右向恒为 (0,1,0)，玩家朝 -X 抛时那正好是玩家的"左" ⇒ 左右键整体反掉；
	//   朝 ±Y 抛时更是推到弹道前后去（球忽快忽慢不拐弯）。
	//   Up × Forward 得到的才是"站在抛掷者视角的右手边"（朝 +X ⇒ +Y，朝 -X ⇒ -Y）。
	//   抛掷方向在技能那边已经把 Z 归零，这里再兜一手竖直输入的退化情况。
	CurveRight = FVector::CrossProduct(FVector::UpVector, Forward);
	if (!CurveRight.Normalize())
	{
		CurveRight = FVector::RightVector;
	}

	const float Sign = bCurveLeft ? -1.f : 1.f;
	const float InitialLateral = InitialSpeed * 0.15f;
	Velocity = Forward * InitialSpeed + CurveRight * Sign * InitialLateral;
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

	/*
	 * 挡视线的东西先收集一遍（全场 actor 里实现了 IVisionBlockerInterface 的那些）：
	 * 火墙、烟。收集放在受害者循环外面 —— 每颗闪光爆炸只需要遍历一次世界，
	 * 而不是"每个受害者再遍历一遍"。
	 *
	 * 为什么它们不参与上面那条 LineTrace：它们全是 NoCollision 的（子弹/人/闪光弹都要能穿过去），
	 * 所以 trace 撞不到。改用"逐个问一句"的方式，几何形状（火墙的一排板子 / 烟的球）
	 * 由各自实现，见 IVisionBlockerInterface 的注释。
	 */
	TArray<const IVisionBlockerInterface*> VisionBlockers;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (const IVisionBlockerInterface* Blocker = Cast<IVisionBlockerInterface>(*It))
		{
			VisionBlockers.Add(Blocker);
		}
	}

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

		// 第二道遮挡：火墙 / 烟。⚠ 只管判定 —— 它们本身仍是 NoCollision，
		// 闪光球从它们中间飞过去、子弹穿过、人走进去，一样都不受影响。
		bool bBlockedByAbility = false;
		for (const IVisionBlockerInterface* Blocker : VisionBlockers)
		{
			if (Blocker && Blocker->BlocksVisionSegment(BlastPos, TargetLoc))
			{
				bBlockedByAbility = true;
				break;
			}
		}
		if (bBlockedByAbility) continue;

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

	UE_LOG(LogTemp, Log, TEXT("[Curveball] DETONATE at %s inRange=%d blinded=%d blockers=%d"),
		*BlastPos.ToString(), CountInRange, CountBlinded, VisionBlockers.Num());

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
