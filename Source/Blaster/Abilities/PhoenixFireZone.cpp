// Phoenix Q 火圈。判定思路见头文件，这里只补实现上的取舍：
// 结算走"每 0.25 秒遍历全场角色"这种最笨也最好查的写法 —— 一个火圈最多 16 跳，
// 每跳遍历几十个角色，开销可以忽略；换成重叠体/事件驱动反而要处理
// "人已经站在里面时火才生成"这类边缘情况。

#include "Blaster/Abilities/PhoenixFireZone.h"

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
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

APhoenixFireZone::APhoenixFireZone()
{
	// 不需要 tick：表现全交给 Niagara/材质，判定走定时器。
	PrimaryActorTick.bCanEverTick = false;

	FireMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FireMesh"));
	SetRootComponent(FireMesh);

	// 纯视觉：碰撞、overlap、导航全关。
	// 火圈**不挡路也不挡子弹** —— 它是"踩上去持续掉血"的地面效果，不是实体障碍。
	// 判定完全由 ApplyFireTick 的距离比较完成，和碰撞系统无关。
	FireMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FireMesh->SetGenerateOverlapEvents(false);
	FireMesh->SetCanEverAffectNavigation(false);
	// 贴地的圆盘投影没有意义（而且会让地面出现一圈奇怪的暗边）
	FireMesh->SetCastShadow(false);

	FireFXComp = CreateDefaultSubobject<UNiagaraComponent>(TEXT("FireFX"));
	FireFXComp->SetupAttachment(FireMesh);
	FireFXComp->bAutoActivate = false;	// 资产在 BeginPlay 里 SetAsset 之后再开
	FireFXComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	LoopSoundComp = CreateDefaultSubobject<UAudioComponent>(TEXT("LoopSound"));
	LoopSoundComp->SetupAttachment(FireMesh);
	LoopSoundComp->bAutoActivate = false;

	// 服务器生成 → 复制到所有客户端。位置固定不动，所以不需要移动复制。
	// 不做 bAlwaysRelevant：距离远的火圈玩家根本看不到，没必要占带宽。
	bReplicates = true;
	SetReplicateMovement(false);

	// 默认网格：引擎自带圆柱。它是"以原点为中心、半径 50、高 100"的圆盘坯子，
	// 压扁再按 FireRadius 缩放就是一圈贴地的火（见 OnConstruction）。
	// 用 ConstructorHelpers 是为了"C++ 类裸建出来就能看"—— 不然忘了在 BP 里填网格，
	// 表现是"踩上去掉血但地上什么都没有"，很难往"没配网格"上想。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		FireMesh->SetStaticMesh(CylinderMesh.Object);
	}
}

void APhoenixFireZone::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!FireMesh) return;

	// 半径 → 缩放的换算从**网格资产自己的包围盒**来（和 ACloveSmoke 同一套做法）：
	// BP 里换成别的圆盘模型时，只要它以原点为中心，FireRadius 照样是对的数值。
	FVector HalfExtent(50.f, 50.f, 50.f); // 引擎 BasicShapes/Cylinder 的半尺寸
	if (const UStaticMesh* Mesh = FireMesh->GetStaticMesh())
	{
		const FVector Extent = Mesh->GetBounds().BoxExtent;
		if (Extent.X > KINDA_SMALL_NUMBER && Extent.Y > KINDA_SMALL_NUMBER)
		{
			HalfExtent = Extent;
		}
	}

	// Z 不跟着半径走：XY 决定火有多大，Z 是"这层火有多厚"，由 ThicknessScale 单独给。
	FireMesh->SetRelativeScale3D(FVector(
		FireRadius / HalfExtent.X,
		FireRadius / HalfExtent.Y,
		ThicknessScale));

	// 材质在 OnConstruction 里换而不是在构造函数里：构造函数阶段 BP 子类指派的
	// FireMaterial 还没被序列化上去（CDO 的 UPROPERTY 值是资产加载之后才生效的），
	// 在构造函数里读会永远拿到 nullptr。OnConstruction 在 SpawnActor 时必跑。
	if (FireMaterial)
	{
		FireMesh->SetMaterial(0, FireMaterial);
	}
}

void APhoenixFireZone::BeginPlay()
{
	Super::BeginPlay();

	// 特效资产取 FireEffect，没配就退回组件 Asset 槽里那个（见 Blaster.h ActivateNiagaraFX 的注释）
	ActivateNiagaraFX(FireFXComp, FireEffect);

	if (LoopSound && LoopSoundComp)
	{
		LoopSoundComp->SetSound(LoopSound);
		LoopSoundComp->Play();
	}

	// 判定和寿命都只在服务器：客户端也起定时器的话，同一圈火在两端会各自扣一次血
	//（伤害虽然由服务器权威写，但客户端会白跑一遍遍历、还可能打出本地伤害数字）。
	if (HasAuthority())
	{
		// 只在服务器定寿命。客户端也 SetLifeSpan 的话，两边计时长短一旦有差异
		//（网络延迟/帧率），就会出现"火在客户端提前消失"或者"服务器没了客户端还留着"。
		SetLifeSpan(FireDuration);

		// 第一跳延后一个间隔：落地的当帧就扣血会显得"这火是瞬发的"，
		// 而且玩家连"看到火然后抬脚"的机会都没有。
		GetWorldTimerManager().SetTimer(
			DamageTimer, this, &APhoenixFireZone::ApplyFireTick,
			FMath::Max(DamageTickInterval, 0.02f), /*bLoop=*/true,
			/*FirstDelay=*/FMath::Max(DamageTickInterval, 0.02f));
	}
}

void APhoenixFireZone::ApplyFireTick()
{
	UWorld* World = GetWorld();
	if (!World) return;

	ABlasterCharacter* Caster = Cast<ABlasterCharacter>(GetOwner());
	AController* CasterController = Caster ? Caster->GetController() : nullptr;
	const ABlasterPlayerState* CasterPS = Caster ? Caster->GetPlayerState<ABlasterPlayerState>() : nullptr;

	const FVector Origin = GetActorLocation();
	const float RadiusSq = FMath::Square(FireRadius);
	const float ZTol = FMath::Max(VerticalTolerance, 0.f);

	for (TActorIterator<ABlasterCharacter> It(World); It; ++It)
	{
		ABlasterCharacter* Victim = *It;
		if (!Victim || Victim->IsElimmed()) continue;

		// 竖直：别把楼上/楼下的人一起烧了（角色位置是胶囊中心，站立时天然比地面高半个身位）
		if (FMath::Abs(Victim->GetActorLocation().Z - Origin.Z) > ZTol) continue;

		// 水平：只看 XY 距离，火圈是个正圆
		if (FVector::DistSquared2D(Victim->GetActorLocation(), Origin) > RadiusSq) continue;

		// —— 施法者本人：踩自己的火回血（Valorant 原版），不走伤害那条路 ——
		if (Victim == Caster)
		{
			if (bHealCaster)
			{
				Victim->HealByAbility(HealPerTick);
			}
			continue;
		}

		if (!bDamageEnemies) continue;

		/*
		 * 只伤敌人。
		 *
		 * 工程里的伤害链路本身没有队别门槛（武器那边比队只是为了画命中反馈），
		 * 所以"只伤敌人"必须在这里做。判据用的是 ABlasterPlayerState::Team ——
		 * 和武器命中反馈同一份数据，不会出现"枪打中算敌人、火烧到算队友"这种不一致。
		 *
		 * 拿不到双方 PlayerState 时按"敌我未定"处理：照常造成伤害。
		 * 理由是宁可多打一次也不要静默失效（大厅/测试机器人没有 PS，
		 * 而"火圈对某些人完全不起作用"是最难查的那类 bug）；测试机器人按惯例
		 * 一直是可以打的（见武器那边的 IsTestBot 用法）。
		 */
		const ABlasterPlayerState* VictimPS = Victim->GetPlayerState<ABlasterPlayerState>();
		if (!Victim->IsTestBot() && VictimPS && CasterPS && VictimPS->Team == CasterPS->Team)
		{
			continue;
		}

		// DamageCauser 传火圈自己（不是火球——火球在地落地的当帧就已经退场了）
		UGameplayStatics::ApplyDamage(Victim, DamagePerTick, CasterController, this, UDamageType::StaticClass());
	}
}
