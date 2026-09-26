#include "Spike.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Sound/SoundAttenuation.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"
#include "Kismet/GameplayStatics.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/EngineTypes.h"
#include "CollisionQueryParams.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"

ASpike::ASpike()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	// 根组件 = 方块碰撞体（不是网格！）：物理掉落要求「模拟物理的组件必须是根组件」，
	// 骨骼网格没有物理资产、也没法当物理体，所以根换成 UBoxComponent，网格降为它的子件。
	// 碰撞状态随状态机切换：NoCollision（携带/安放）→ QueryOnly+Pawn重叠（拾取/拆包判定）
	// → QueryAndPhysics（掉包真模拟物理，见 EnablePhysicsDrop）
	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	SetRootComponent(CollisionBox);
	// 占位尺寸，PostInitializeComponents 里按网格导入包围盒重设（见 SetupCollisionBoxFromMesh）
	CollisionBox->SetBoxExtent(FVector(25.f, 25.f, 40.f), false);
	CollisionBox->SetCollisionObjectType(ECC_PhysicsBody);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionBox->SetGenerateOverlapEvents(true);
	CollisionBox->SetIsReplicated(true);

	// 换成 Valorant spike 骨骼网格（EQ_Bomb_S0_Mesh/Skeleton）。动画机 ABP_Spike 在 BP_Spike 里指定。
	SpikeMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("SpikeMesh"));
	SpikeMesh->SetupAttachment(CollisionBox);
	// ⚠️ 不再在构造器里 FObjectFinder 同步加载网格/动画类——那会让编辑器启动阶段的类加载
	// 触发异步 skinned 资产编译并死锁（启动卡 "Waiting for skinned assets to be ready ..."）。
	// 只设软引用默认值（不加载），真正加载推迟到 PostInitializeComponents。
	SpikeMeshAsset = TSoftObjectPtr<USkeletalMesh>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/EQ_Bomb_S0_Mesh.EQ_Bomb_S0_Mesh")));
	SpikeAnimClass = TSoftClassPtr<UAnimInstance>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/Anims/ABP_Spike.ABP_Spike")));
	// 网格只做可见/附带（自身不参与碰撞，碰撞全交给 CollisionBox）。
	// 相对位移在 PostInitializeComponents 里按导入包围盒设（让模型落在方块中心，底面与方块底对齐）。
	// 相对位移是常驻的，不需要逐帧复制：网格跟随根组件移动，关掉组件复制省流量。
	SpikeMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetIsReplicated(false);

	PlantedAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("PlantedAudio"));
	PlantedAudio->SetupAttachment(RootComponent);
	PlantedAudio->bAutoActivate = false;

	CountdownAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("CountdownAudio"));
	CountdownAudio->SetupAttachment(RootComponent);
	CountdownAudio->bAutoActivate = false;

	// 倒计时 beep 默认素材软引用（不加载；UpdateCountdownBeep 首次响前懒加载）。
	// BP 若在 Details 里给 CountdownBeepSound 配了其它素材则以其覆盖为准。
	CountdownBeepSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/Sounds/S_SpikeCountdown_Beep.S_SpikeCountdown_Beep")));

	ExplosionAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("ExplosionAudio"));
	ExplosionAudio->SetupAttachment(RootComponent);
	ExplosionAudio->bAutoActivate = false;

	// 爆炸音效默认素材软引用（不加载；MulticastExplode 首次响前懒加载）
	ExplosionSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/Sounds/S_SpikeExplosion.S_SpikeExplosion")));

	/*
	 * 爆炸球：爆炸后从爆点胀开的纯黑球（视觉）+ 范围伤害的判定形状。
	 *
	 * 网格用引擎自带球体（同 CloveSmoke）：纯视觉、无碰撞、不投影的装饰件。
	 * 挂在根组件（CollisionBox）上、相对位置为零 → 球心 = spike 原点，贴地摆放后就是爆点那个点。
	 *
	 * 为什么不用 USphereComponent 的 Overlap 做伤害判定：要的是「**当前**半径」——
	 * 球一直在胀，Overlap 只在"进入/离开"时给事件，球胀过一个人身上时不会有任何回调。
	 * 服务器每 ExplosionDamageTickInterval 自己遍历一遍角色按距离判，简单，而且永远和看得见的球一致。
	 */
	ExplosionSphere = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ExplosionSphere"));
	ExplosionSphere->SetupAttachment(RootComponent);
	ExplosionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ExplosionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	ExplosionSphere->SetGenerateOverlapEvents(false);
	// 20m 的球投出来的影子会盖住一整个包点，比球本身还碍事
	ExplosionSphere->SetCastShadow(false);
	ExplosionSphere->SetVisibility(false);
	// 纯本端视觉：各端按 MulticastExplode 的时刻自己推演大小，不需要复制
	ExplosionSphere->SetIsReplicated(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> ExplosionSphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (ExplosionSphereMesh.Succeeded())
	{
		ExplosionSphere->SetStaticMesh(ExplosionSphereMesh.Object);
	}

	// 材质：只存软引用（不加载），首次爆炸真要显示时才 LoadSynchronous —— 同 ExplosionSound 的规矩
	ExplosionSphereMaterialAsset = TSoftObjectPtr<UMaterialInterface>(
		FSoftObjectPath(TEXT("/Game/Materials/M_SpikeExplosionSphere.M_SpikeExplosionSphere")));

	// 安包音组件：平时不激活，安包开始(MulticastStartPlant)播一次，松开/取消即 Stop 掐断
	PlantArmAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("PlantArmAudio"));
	PlantArmAudio->SetupAttachment(RootComponent);
	PlantArmAudio->bAutoActivate = false;

	// 拆包音组件：平时不激活，拆包开始(MulticastStartDefuse)响一次、拆完/被打断即 Stop 掐断。
	DefuseArmAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("DefuseArmAudio"));
	DefuseArmAudio->SetupAttachment(RootComponent);
	DefuseArmAudio->bAutoActivate = false;

	// 拆包音默认素材软引用（不加载；MulticastPlayDefuseArmSound 首次响前懒加载）。
	// BP 若在 Details 里给 DefuseArmSound 配了其它素材则以其覆盖为准。
	DefuseArmSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/Sounds/S_SpikeDefuse.S_SpikeDefuse")));

	// 拆包器可见网格：默认隐藏、无碰撞，作为 spike 的子组件存在；
	// 拆包开始(MulticastStartDefuse)才改挂到拆包者手部 socket 并显示（第三人称那份）
	DefuserKitMeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("DefuserKitMeshComp"));
	DefuserKitMeshComp->SetupAttachment(RootComponent);
	DefuserKitMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DefuserKitMeshComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	DefuserKitMeshComp->SetGenerateOverlapEvents(false);
	DefuserKitMeshComp->SetVisibility(false);

	// 第一人称那份拆包器：同一个套路再多建一个组件，拆包时挂到拆包者本人的手模上
	//（哪些机器上才真的挂+显示见 ShowDefuserKit —— 是代码判 IsLocallyControlled，
	// 不是 SetOnlyOwnerSee，理由见头文件里这个成员的注释）。
	DefuserKitFPMeshComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("DefuserKitFPMeshComp"));
	DefuserKitFPMeshComp->SetupAttachment(RootComponent);
	DefuserKitFPMeshComp->bCastDynamicShadow = false;
	DefuserKitFPMeshComp->CastShadow = false;
	DefuserKitFPMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DefuserKitFPMeshComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	DefuserKitFPMeshComp->SetGenerateOverlapEvents(false);
	DefuserKitFPMeshComp->SetVisibility(false);

	// 拆包器默认外观（软引用，懒加载；模式同 SpikeMeshAsset，避开构造器加载 SkeletalMesh 死锁）
	DefuserKitMesh = TSoftObjectPtr<USkeletalMesh>(
		FSoftObjectPath(TEXT("/Game/ValorantAssets/Bomb/Defuser/EQ_Bomb_Defuser_S0_Mesh.EQ_Bomb_Defuser_S0_Mesh")));
}

void ASpike::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// SpikeMesh 的默认网格/动画类推迟到这里加载（而非构造器）：
	// 实例化阶段已经过了类加载，此时按需加载 SkeletalMesh 不会触发编辑器启动期的
	// 异步 skinned 资产编译死锁。BP_Spike 若在 Details 里给 SpikeMesh 配了网格/动画类
	// （组件默认值），这里检测到非空就跳过，以 BP 覆盖为准。
	if (SpikeMesh && SpikeMesh->GetSkeletalMeshAsset() == nullptr && !SpikeMeshAsset.IsNull())
	{
		if (USkeletalMesh* Mesh = SpikeMeshAsset.LoadSynchronous())
		{
			SpikeMesh->SetSkeletalMesh(Mesh);
		}
	}
	if (SpikeMesh && SpikeMesh->AnimClass == nullptr && !SpikeAnimClass.IsNull())
	{
		if (UClass* AnimClass = SpikeAnimClass.LoadSynchronous())
		{
			SpikeMesh->SetAnimInstanceClass(AnimClass);
		}
	}

	// 网格就位后再定方块尺寸、摆网格子件的位置（两者都依赖导入包围盒）
	SetupCollisionBoxFromMesh();
}

void ASpike::SetupCollisionBoxFromMesh()
{
	if (!SpikeMesh || !CollisionBox) return;

	const USkeletalMesh* Mesh = SpikeMesh->GetSkeletalMeshAsset();
	if (!Mesh) return;

	// 导入包围盒：原点不在模型中心（origin.Z≈31.5，底面 Z≈-8.95）。
	// 网格按 -中心 平移，让「包围盒中心」和「actor 原点 = 方块中心」重合 →
	// 方块底 = 网格底，物理落地时模型正好站在地面上，手上/关卡里的摆放也能反向补回来。
	const FBoxSphereBounds& Bounds = Mesh->GetImportedBounds();
	const FVector MeshScale = SpikeMesh->GetRelativeScale3D();
	MeshRelativeOffset = -(Bounds.Origin * MeshScale);
	MeshAttachPivotOffset = -MeshRelativeOffset;
	SpikeMesh->SetRelativeLocation(MeshRelativeOffset);

	// 网格最底点在 actor 坐标系下的 Z（neg）：贴地摆放用（FreezeAtLocation）。
	// 与方块尺寸无关 —— 方块尺寸被 Override 改过时，贴地仍以视觉网格为准。
	MeshBottomInActorZ = MeshRelativeOffset.Z + (Bounds.Origin.Z - Bounds.BoxExtent.Z) * MeshScale.Z;

	// 自动尺寸（Override 填零时生效的那个值）：只读暴露到 Details 面板，方便照着它微调
	AutoCollisionBoxExtent = Bounds.BoxExtent * MeshScale;

	// 方块尺寸：默认紧贴模型外观；Override 非零时以 Override 为准（想严格正立方体就填三个相等值）
	CollisionBox->SetBoxExtent(ComputeCollisionBoxExtent(), false);
}

FVector ASpike::ComputeCollisionBoxExtent() const
{
	// 自动值优先从网格现算 —— 编辑器里这个函数会被可视化器每帧调用，那时
	// SetupCollisionBoxFromMesh 可能还没跑过（AutoCollisionBoxExtent 仍是 0）
	FVector Auto = AutoCollisionBoxExtent;
	if (SpikeMesh)
	{
		if (const USkeletalMesh* Mesh = SpikeMesh->GetSkeletalMeshAsset())
		{
			Auto = Mesh->GetImportedBounds().BoxExtent * SpikeMesh->GetRelativeScale3D();
		}
	}

	if (!CollisionBoxExtentOverride.IsNearlyZero())
	{
		return CollisionBoxExtentOverride;
	}

	// 网格也没加载出来就沿用组件当前尺寸（构造器里的占位值），绝不把盒子设成零
	return (Auto.IsNearlyZero() && CollisionBox) ? CollisionBox->GetUnscaledBoxExtent() : Auto;
}

#if WITH_EDITOR
void ASpike::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 改尺寸/换网格都立刻反映到真正的组件上（视口线框直接读组件尺寸，所以也就实时跟手了）
	const FName Changed = PropertyChangedEvent.GetPropertyName();
	if (Changed == GET_MEMBER_NAME_CHECKED(ASpike, CollisionBoxExtentOverride)
		|| Changed == GET_MEMBER_NAME_CHECKED(ASpike, SpikeMeshAsset))
	{
		SetupCollisionBoxFromMesh();
	}
}
#endif

void ASpike::BeginPlay()
{
	Super::BeginPlay();

	// 关卡里这个 spike 是按「网格原点」摆的：网格子件现在相对 actor 原点有位移（见
	// SetupCollisionBoxFromMesh），这里补一次反向偏移，让出生外观和改之前一模一样；
	// 之后的复位以补偿后的坐标为基准（InitialSpawn*）。
	ApplyMeshPivotCompensation();

	InitialSpawnLocation = GetActorLocation();
	InitialSpawnRotation = GetActorRotation();

	if (HasAuthority())
	{
		// 拾取/拆包判定：QueryOnly + 只和 Pawn 重叠（角色能穿过去，踩上来触发拾取）
		CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		CollisionBox->OnComponentBeginOverlap.AddDynamic(this, &ASpike::OnPickupBeginOverlap);
		CollisionBox->OnComponentEndOverlap.AddDynamic(this, &ASpike::OnPickupEndOverlap);
	}

	// 找出使用发光材质的槽，建动态材质实例，供安包后按阶段拉红。
	// 出生/回合开始是未安状态：光条关（光条只在 Planted 亮）
	SetupGlowMaterials();
	TurnOffGlow();

	// spike 自带的 3D 音（安包音/持续音/beep/爆炸）统一套空间衰减：能听出方位
	ApplySpatialAttenuation(PlantedAudio);
	ApplySpatialAttenuation(CountdownAudio);
	ApplySpatialAttenuation(PlantArmAudio);
	ApplySpatialAttenuation(DefuseArmAudio);
	ApplySpatialAttenuation(ExplosionAudio);
}

void ASpike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ASpike, CurrentState);
	// 被掏到手上没有（显隐要用）。只由服务器在 Draw()/Holster() 里写，复制给所有客户端。
	DOREPLIFETIME(ASpike, bCarrierDrawn);
	DOREPLIFETIME(ASpike, PlantStartTime);
	DOREPLIFETIME(ASpike, PlantedTransform);
	DOREPLIFETIME(ASpike, DroppedTransform);
	DOREPLIFETIME(ASpike, DefuseProgress);
}

void ASpike::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 调方块尺寸用：橙框 = 真正的碰撞体（拾取/物理/拆包都按它算），青扁框 = 网格底面。
	// 两个底面对不上就说明网格缩放有问题；橙框明显够不到模型就是尺寸调小了。
	if (bDrawDebugCollisionBox && CollisionBox)
	{
		DrawDebugBox(GetWorld(), CollisionBox->GetComponentLocation(), CollisionBox->GetUnscaledBoxExtent(),
			CollisionBox->GetComponentQuat(), FColor::Orange, false, -1.f, 0, 2.f);

		const float MeshBottomZ = GetActorLocation().Z + GetMeshBottomLocalZ();
		const FVector BottomCenter(GetActorLocation().X, GetActorLocation().Y, MeshBottomZ);
		DrawDebugBox(GetWorld(), BottomCenter, FVector(CollisionBox->GetUnscaledBoxExtent().X, CollisionBox->GetUnscaledBoxExtent().Y, 0.f),
			CollisionBox->GetComponentQuat(), FColor::Cyan, false, -1.f, 0, 1.f);
	}

	// 安包后服务器/客户端各自按剩余时间推演 Core 转角 + 发光阶段（视觉，不依赖服务器逐帧）
	if (CurrentState == ESpikeState::ESS_Planted)
	{
		UpdatePlantedVisual(DeltaTime);
		UpdateCountdownBeep(DeltaTime);
	}

	// 爆炸球：各端按自己的时钟胀大 → 保持 → 收。
	// 和上面那两行同一套路数 —— 视觉各端自己推演，不复制；伤害是服务器另起的定时器。
	if (bExplosionSphereActive)
	{
		UpdateExplosionSphere(DeltaTime);
	}

	if (HasAuthority())
	{
		// 掉包：方块碰撞体自己跑物理（重力/碰撞/翻滚全交给物理引擎），这里只判「停稳」。
		// 停稳后记一次落点（客户端 OnRep_DroppedTransform 兜底吸附），物理保持开启 ——
		// 刚体会自己休眠，不需要我们关；拾取/安包/复位时才 StopPhysicsDrop。
		if (bPhysicsDropping)
		{
			PhysicsDropElapsed += DeltaTime;

			if (CollisionBox && CollisionBox->IsSimulatingPhysics())
			{
				const bool bSlowEnough =
					CollisionBox->GetPhysicsLinearVelocity().SizeSquared() < FMath::Square(DropSettleLinearSpeed) &&
					CollisionBox->GetPhysicsAngularVelocityInDegrees().SizeSquared() < FMath::Square(DropSettleAngularSpeed);

				// 至少要落一会儿再判速度：刚松手那几帧速度还没上来，会瞬间误判成"已停稳"
				const bool bAsleep = !CollisionBox->IsAnyRigidBodyAwake();
				// 兜底：掉进地图外/无地面时永远停不下来，超时也当落稳收掉标记（物理继续由引擎管）
				const bool bTimedOut = PhysicsDropElapsed > DropMaxDuration;
				if (bAsleep || bTimedOut || (bSlowEnough && PhysicsDropElapsed > 0.3f))
				{
					bPhysicsDropping = false;
					DroppedTransform = GetActorTransform();

					// 落地停稳：丢弃者若已经不在重叠范围内（下落途中可能被碰撞抖动提前清过保护），
					// 这里正式解除对他的拾取保护；他必须走开再回来才能重新捡起自己丢的包
					if (JustDroppedCharacter && !IsOverlappingActor(JustDroppedCharacter))
					{
						JustDroppedCharacter = nullptr;
					}
				}
			}
		}

		if (bIsPlanting)
		{
			// 移动或跳跃会打断安包（站定才能安，Valorant 式）
			if (CurrentPlanter && (CurrentPlanter->GetVelocity().Size2D() > MovementCancelSpeed ||
				CurrentPlanter->GetCharacterMovement()->IsFalling()))
			{
				ServerCancelPlant();
			}
			else
			{
				PlantProgress += DeltaTime / PlantDuration;

				// 把安包进度推给下包者自己的 HUD
				if (PlanterController)
				{
					PlanterController->ClientUpdateSpikeProgress(TEXT("Planting"), PlantProgress);
				}

				if (PlantProgress >= 1.f)
				{
					PlantProgress = 1.f;
					bIsPlanting = false;

					if (CurrentPlanter)
					{
						CurrentPlanter->SetCarriedSpike(nullptr);
						CurrentPlanter->bCarryingSpike = false;
						CurrentPlanter->SetSpikeDrawn(false);
						CurrentPlanter->SetCombatState(ECombatState::ECS_Unoccupied);
						CurrentPlanter->MulticastPlantAnimation(false);
						// 安包完成 → 站起来
						CurrentPlanter->MulticastSpikeAutoCrouch(false);
						/*
						 * 安包完成：自动掏回等级最高的武器（主武器优先，无主武器才掏副武器）。
						 *
						 * 必须有这一句：把尖刺掏出来的时候（UCombatComponent::DrawSpike）手上的枪
						 * 已经被收回挂点、EquippedWeapon 也清空了 —— 空手是"掏包"的固有状态。
						 * 包一旦离手（上面那句 SetCarriedSpike(nullptr)），不主动掏枪的话，
						 * 安包者会举着两只空手站在原地，直到自己按 1/2/3。和拆包取消
						 *（ServerCancelDefuse 尾巴上那条）是同一件事，那边一直是有的。
						 *
						 * 顺序不能反 —— EquipBestOwnedWeapon 自己走 CanChangeWeapon()，
						 * 状态还停在 ECS_Planting 的话会被它挡掉，所以必须在上面那句
						 * SetCombatState(ECS_Unoccupied) **之后**（同 CombatState.h 里那条约定）。
						 */
						if (UCombatComponent* PlanterCombat = CurrentPlanter->GetCombatComponent())
						{
							PlanterCombat->EquipBestOwnedWeapon();
						}
					}

					// 安包完成，隐藏进度条
					if (PlanterController)
					{
						PlanterController->ClientUpdateSpikeProgress(TEXT(""), -1.f);
					}

					// 大招点：安包 +1，给安包者。这里在服务器的 Tick 分支里（权威机才走到），
					// 不能写进 MulticastPlantComplete —— 那是各端都跑的，会各加一遍。
					if (ABlasterPlayerState* PlanterPS = CurrentPlanter ? CurrentPlanter->GetPlayerState<ABlasterPlayerState>() : nullptr)
					{
						PlanterPS->AddUltPoints(1);
					}

					SetSpikeState(ESpikeState::ESS_Planted);
					// 必须在 MulticastPlantComplete() 之前抓：那句会把下包 montage 停掉，
					// 停完挂点就不再是"放下包"那一帧的姿态了（见 CapturePlantPlacement）
					CapturePlantPlacement();
					MulticastPlantComplete();
					FreezeAtLocation();
					StartExplodeTimer();
					MulticastPlayPlantedSound();
					// 成功安包：全局广播音（2D 不分方位），BP 填素材后所有玩家同响
					MulticastPlayPlantSuccessSound();

					if (ABlasterGameState* GS = GetBlasterGameState())
					{
						GS->SpikeState = ESpikeState::ESS_Planted;
						GS->SpikeCarrier = nullptr;
						GS->bSpikePlanted = true;
					}

					CurrentPlanter = nullptr;
					PlanterController = nullptr;
				}
			}
		}

		if (bIsDefusing)
		{
			// 移动或跳跃会打断拆包
			if (CurrentDefuser && (CurrentDefuser->GetVelocity().Size2D() > MovementCancelSpeed ||
				CurrentDefuser->GetCharacterMovement()->IsFalling()))
			{
				ServerCancelDefuse();
			}
			else
			{
				DefuseProgress += (DeltaTime / DefuseDuration) * 100.f;

				// 把拆包进度推给拆包者自己的 HUD（ClientUpdateSpikeProgress 用 0-1 比例）
				if (DefuserController)
				{
					DefuserController->ClientUpdateSpikeProgress(TEXT("Defusing"), DefuseProgress / 100.f);
				}

				if (DefuseProgress >= 100.f)
				{
					DefuseProgress = 100.f;
					bIsDefusing = false;

					// 大招点：拆包 +1，给拆包者。同样只能在服务器这条分支上加
					// （MulticastDefuseComplete 是各端都跑的）
					if (ABlasterPlayerState* DefuserPS = CurrentDefuser ? CurrentDefuser->GetPlayerState<ABlasterPlayerState>() : nullptr)
					{
						DefuserPS->AddUltPoints(1);
					}

					// 拆包完成，清空所有玩家的 spike HUD（拆包者进度条 + 其他人的倒计时）
					ClearSpikeUIForAll();
					MulticastDefuseComplete();
					GetWorldTimerManager().ClearTimer(ExplodeTimerHandle);
					MulticastStopPlantedSound();

					if (ABlasterGameState* GS = GetBlasterGameState())
					{
						GS->SpikeState = ESpikeState::ESS_Defused;
						GS->bSpikePlanted = false;
					}

					// 拆包成功：结束本回合，防守方获胜
					if (ABlasterGameMode* GM = GetWorld()->GetAuthGameMode<ABlasterGameMode>())
					{
						GM->CheckRoundEnd();
					}

					// 配置了拆包完成 montage：先播（spike 保持可见、DefuseProgress=100 会停转/停广播），播完再进 Defused 隐藏
					if (DefuseCompleteMontage)
					{
						MulticastPlayDefuseCompleteMontage();
						GetWorldTimerManager().SetTimer(DefuseMontageHideTimer, this, &ASpike::HideAfterDefuseMontage,
							DefuseCompleteMontage->GetPlayLength(), false);
					}
					else
					{
						SetSpikeState(ESpikeState::ESS_Defused);
					}

					// 拆包成功：退出 Defusing 战斗状态，收起那两条动画、站起来，并把枪掏回手上
					if (CurrentDefuser)
					{
						CurrentDefuser->SetCombatState(ECombatState::ECS_Unoccupied);
						// 拆完了也播收尾那一段（用户："拆包打断或者拆完了再播stop"），并站起来
						CurrentDefuser->MulticastDefuseAnimation(false);
						CurrentDefuser->MulticastSpikeAutoCrouch(false);

						/*
						 * 拆包成功也要**自动掏回等级最高的武器**（主武器优先，无主武器才掏副武器）——
						 * 和安包完成（本文件 Tick 里那条）、拆包被打断（ServerCancelDefuse 尾巴上那条）
						 * 对齐。用户 2026-09-22 把三种收尾摆在一起提的要求：
						 * "安包完和拆包打断或者拆包完会自动掏最高级武器"。
						 *
						 * 这里原来是**刻意不掏**的（旧注释："回合结束，不重新掏枪"）。为什么不掏不合适：
						 * 掏出爆能器那一刻手上的枪就已经挂回闲置 socket、EquippedWeapon 也清空了
						 *（UCombatComponent::DrawSpike），包一离手不主动掏的话，拆包者会举着两只空手
						 * 站在原地直到自己按 1/2/3 —— 回合确实结束了，但这一下是玩家看得见的画面。
						 *
						 * 顺序不能反：EquipBestOwnedWeapon 自己走 CanChangeWeapon()，
						 * 状态还停在 ECS_Defusing 的话会被它挡掉（同 CombatState.h 里那条约定），
						 * 所以必须在上面那句 SetCombatState(ECS_Unoccupied) **之后**。
						 */
						if (UCombatComponent* DefuseCombat = CurrentDefuser->GetCombatComponent())
						{
							DefuseCombat->EquipBestOwnedWeapon();
						}
					}

					CurrentDefuser = nullptr;
					DefuserController = nullptr;
				}
			}
		}

		// 安包后给所有玩家广播 spike 爆炸倒计时（拆包完成 montage 期间不再广播）
		if (CurrentState == ESpikeState::ESS_Planted && DefuseProgress < 100.f)
		{
			BroadcastExplodeCountdown();
		}
	}

}

void ASpike::OnPickupBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	ABlasterCharacter* Character = Cast<ABlasterCharacter>(OtherActor);
	if (!Character) return;

	Character->OverlappingSpike = this;

	if (!HasAuthority()) return;
	if (Character->IsElimmed()) return;

	ABlasterPlayerState* PS = Character->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// Auto-pickup: attacker walks over dropped spike
	if (CurrentState == ESpikeState::ESS_Dropped && Character != JustDroppedCharacter && PS->Team == ETeam::ET_TeamA)
	{
		PickUp(Character);
	}
}

void ASpike::OnPickupEndOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	ABlasterCharacter* Character = Cast<ABlasterCharacter>(OtherActor);
	if (Character)
	{
		Character->OverlappingSpike = nullptr;

		// 掉包物理下落途中不解保护：方块在丢弃者脚下弹跳/翻滚时反复进出碰撞，
		// 每次都清一次保护就等于没有保护（下一帧再进来就直接捡走了）。
		// 落稳那一刻统一判定（见 Tick），之后正常靠离开范围解除。
		if (Character == JustDroppedCharacter && !bPhysicsDropping)
		{
			JustDroppedCharacter = nullptr;
		}
	}
}

void ASpike::PickUp(ABlasterCharacter* Character)
{
	if (!HasAuthority() || CurrentState != ESpikeState::ESS_Dropped) return;

	ABlasterPlayerState* PS = Character->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->Team != ETeam::ET_TeamA) return;

	AttachToCharacter(Character);
	Character->SetCarriedSpike(this);
	Character->bCarryingSpike = true;
	SetSpikeState(ESpikeState::ESS_Carried);

	if (ABlasterGameState* GS = GetBlasterGameState())
	{
		GS->SpikeState = ESpikeState::ESS_Carried;
		GS->SpikeCarrier = PS;
	}
}

void ASpike::Drop()
{
	if (!HasAuthority()) return;

	// 掉包会打断进行中的安包/拆包（含被击杀时掉包）
	if (bIsPlanting) ServerCancelPlant();
	if (bIsDefusing) ServerCancelDefuse();

	// Clear the carrier's reference before detaching (DetachFromCharacter clears CurrentCarrier)
	if (CurrentCarrier)
	{
		JustDroppedCharacter = CurrentCarrier;
		CurrentCarrier->SetCarriedSpike(nullptr);
		CurrentCarrier->bCarryingSpike = false;
		CurrentCarrier->SetSpikeDrawn(false);
	}

	DetachFromCharacter();
	SetSpikeState(ESpikeState::ESS_Dropped);

	// 掉包：方块碰撞体真模拟物理（自由落体 + 翻滚 + 落稳），服务器权威、落包期间把运动复制给客户端
	EnablePhysicsDrop();

	if (ABlasterGameState* GS = GetBlasterGameState())
	{
		GS->SpikeState = ESpikeState::ESS_Dropped;
		GS->SpikeCarrier = nullptr;
	}
}

void ASpike::ResetSpike()
{
	if (!HasAuthority()) return;

	/*
	 * 爆炸球的伤害必须在这里掐掉。
	 *
	 * 场景：spike 爆炸 → 回合结束 → 新回合复位。如果回合结束的延迟比球的 5 秒还短，
	 * 复位时球还在、伤害定时器也还在跑 —— 新回合刚重生的玩家会被上一回合那个球继续打，
	 * 而且是"看不见来源的持续掉血"，极难查。
	 *
	 * 只停伤害、**不动可见性**：球的显示是各端按自己时钟推演的、没复制，
	 * 服务器这边单方面 SetVisibility(false) 会和客户端看到的对不上（host 没球了、client 还有）。
	 * 视觉上让它自己胀完那 5 秒收掉即可，各端天然一致。
	 */
	GetWorldTimerManager().ClearTimer(ExplosionSphereDamageTimer);

	// 清空下包/拆包/携带等中间状态
	// 回合切换不再移除 HUD，这里必须主动隐藏各自的进度条
	if (PlanterController)
	{
		PlanterController->ClientUpdateSpikeProgress(TEXT(""), -1.f);
	}
	if (DefuserController)
	{
		DefuserController->ClientUpdateSpikeProgress(TEXT(""), -1.f);
	}

	bIsPlanting = false;
	bIsDefusing = false;
	PlantProgress = 0.f;
	DefuseProgress = 0.f;
	bCarrierDrawn = false;

	// 若回合在拆包进行中被复位（如倒计时结束）：退出 Defusing，并把武器掏回来，
	// 避免角色卡在空手/Defusing 状态（角色若被重生销毁则此调用无害）
	if (CurrentDefuser)
	{
		CurrentDefuser->SetCombatState(ECombatState::ECS_Unoccupied);
		// 回合在拆包进行中被复位（不是玩家松手）：拆包那串蒙太奇也要跟着收尾并让他站起来，
		// 否则新回合开始手上还挂着待命循环那个姿势。
		CurrentDefuser->MulticastDefuseAnimation(false);
		CurrentDefuser->MulticastSpikeAutoCrouch(false);
		if (UCombatComponent* DefuseCombat = CurrentDefuser->GetCombatComponent())
		{
			DefuseCombat->EquipBestOwnedWeapon();
		}
	}

	CurrentPlanter = nullptr;
	CurrentDefuser = nullptr;
	CurrentCarrier = nullptr;
	JustDroppedCharacter = nullptr;
	PlanterController = nullptr;
	DefuserController = nullptr;

	// 停止爆炸倒计时、拆包 montage 延迟隐藏与下包音频
	GetWorldTimerManager().ClearTimer(ExplodeTimerHandle);
	GetWorldTimerManager().ClearTimer(DefuseMontageHideTimer);
	MulticastStopPlantedSound();
	// 回合复位：掐掉可能还在响的安包音，摘掉手上可能还挂着的拆包器
	MulticastStopPlantArmSound();
	MulticastStopDefuseKit();

	// 中止可能还在进行的物理掉落
	StopPhysicsDrop();

	// 先解除附着并关掉物理，干净地传送回出生点
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	if (CollisionBox)
	{
		CollisionBox->SetSimulatePhysics(false);
		CollisionBox->SetEnableGravity(false);
	}
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	// 物理掉落期间可能翻滚成任意角度：复位要回到关卡的竖直初始朝向，否则下一回合开始是歪的
	SetActorRotation(InitialSpawnRotation);
	SetActorLocation(InitialSpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);

	// 恢复掉落状态（SetSpikeState 会重新开启物理掉落）
	SetSpikeState(ESpikeState::ESS_Dropped);

	// 不再模拟物理，把复位后的落点复制给客户端
	DroppedTransform = GetActorTransform();

	// 兜底：确保拾取盒可重叠（防止之前某种状态把碰撞关掉导致下一回合无法拾取）
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CollisionBox->SetGenerateOverlapEvents(true);

	if (ABlasterGameState* GS = GetBlasterGameState())
	{
		GS->SpikeState = ESpikeState::ESS_Dropped;
		GS->SpikeCarrier = nullptr;
		GS->bSpikePlanted = false;
	}
}

void ASpike::StartPlant(ABlasterCharacter* Character)
{
	if (!HasAuthority() || CurrentState != ESpikeState::ESS_Carried) return;
	if (Character != CurrentCarrier) return;
	if (Character->IsElimmed()) return;
	if (!Character->IsInPlantZone()) return;

	ServerStartPlant(Character);
}

void ASpike::ServerStartPlant_Implementation(ABlasterCharacter* Character)
{
	if (CurrentState != ESpikeState::ESS_Carried) return;
	if (Character != CurrentCarrier) return;
	// 空手蒙太奇那段不可打断：安包不许插进去。
	// 客户端那条路已经在 ABlasterCharacter::SpikePressed 挡住了（长按计时器根本不会起），
	// 这里是服务器侧的兜底 —— 下面那句 SetCombatState(ECS_Planting) 会把空手状态直接顶掉，
	// 而空手那边还揣着"我才是当前状态"的计时器，两边收尾会打架。
	if (Character->IsEmptyHandLocked()) return;

	bIsPlanting = true;
	PlantProgress = 0.f;
	CurrentPlanter = Character;
	PlanterController = Cast<ABlasterPlayerController>(Character->GetController());
	MulticastStartPlant();

	// 下包中换到动画锚点那套挂点：这套骨架的 SpikeSocket 挂在被下包动画驱动的武器锚点上，
	// 动画会把它一路压到地面（见 Spike.h 里 PlantAttachSocket 的注释），跟着它包才不会吊在半空。
	MulticastSetAttachSlot(Character, ESpikeAttachSlot::ESAS_Plant);

	// 进入下包状态：播放安装动画 + 阻止切枪/切技能等其它操作
	Character->SetCombatState(ECombatState::ECS_Planting);
	Character->MulticastPlantAnimation(true);

	// 下包自动蹲下（用户要求）。多播而不是只在这台服务器上 Crouch()：蹲下是客户端预测状态，
	// 本人那台不一起蹲的话他下一次移动上报就会把蹲下顶回去（理由见 MulticastSpikeAutoCrouch 的注释）。
	Character->MulticastSpikeAutoCrouch(true);
}

void ASpike::ServerCancelPlant_Implementation()
{
	// 隐藏安包进度条
	if (PlanterController)
	{
		PlanterController->ClientUpdateSpikeProgress(TEXT(""), -1.f);
	}

	if (CurrentPlanter)
	{
		CurrentPlanter->SetCombatState(ECombatState::ECS_Unoccupied);
		CurrentPlanter->MulticastPlantAnimation(false);
		// 安包中断 → 自动蹲下的那位也该起来了（玩家自己蹲着进来的话这一句不动他）
		CurrentPlanter->MulticastSpikeAutoCrouch(false);
		// 取消下包 → 换回「掏在手上」那套挂点。不换的话包还挂在武器锚点上，
		// 而站姿下那条锚点在脚底（地板下），表现是「一取消下包，手里的包就没了」。
		// 掉包路径也会走到这里：Drop() 是 ServerCancelPlant() 之后才 SetSpikeDrawn(false)，
		// 所以这里 bCarrierDrawn 还是 true、这句会执行一次 —— 恰好在手上停一帧再去自由落体，
		// 落点就从手里开始（比从地面锚点开始自然）。真被闸挡掉的是「先收包再取消」那种次序。
		MulticastSetAttachSlot(CurrentPlanter, ESpikeAttachSlot::ESAS_Draw);
	}

	bIsPlanting = false;
	PlantProgress = 0.f;
	CurrentPlanter = nullptr;
	PlanterController = nullptr;

	// 松开/打断安包：各端掐断安包音
	MulticastStopPlantArmSound();

	// 松开/打断安包：各端把展开 montage 停下 —— 不然半展开的包会僵在手上，
	// 等它自己播完才回 Idle（ABP 的 Idle 是收拢状态）
	MulticastStopPlantStartMontage();
}

void ASpike::MulticastStartPlant_Implementation()
{
	bIsPlanting = true;
	PlantProgress = 0.f;

	// 安包音：开始安包即播一次（单发）。从 spike（安包者手上）位置发声，能听出方位；
	// 松开/取消/完成时由 MulticastStopPlantArmSound / MulticastPlantComplete 掐断
	if (PlantArmAudio && PlantArmSound)
	{
		PlantArmAudio->SetSound(PlantArmSound);
		PlantArmAudio->Play();
	}

	// 开始下包：spike 自己在手上播展开 montage（长度一般 = PlantDuration，结束时正好展开成型）。
	// 各端都跑这里，所以服务器和客户端一致；本人第一人称手里那份包是照搬这条的姿势显示的。
	PlayPlantStartMontageLocal();
}

void ASpike::MulticastStopPlantArmSound_Implementation()
{
	// 松开安包/取消/安包完成：掐断正在响的安包音
	if (PlantArmAudio)
	{
		PlantArmAudio->Stop();
	}
}

void ASpike::MulticastPlayPlantSuccessSound_Implementation()
{
	// 成功安包：全局广播、不分方位、无距离衰减（PlaySound2D）
	if (PlantSuccessSound)
	{
		UGameplayStatics::PlaySound2D(this, PlantSuccessSound);
	}
}

void ASpike::MulticastPlantComplete_Implementation()
{
	PlantProgress = 1.f;
	bIsPlanting = false;

	// 安包完成：安包音到此为止
	if (PlantArmAudio)
	{
		PlantArmAudio->Stop();
	}

	/*
	 * 安包完成：把"开始下包"那条展开 montage 停下，让位给后面落地后那条（PlantUnfoldMontage）。
	 *
	 * 必须在这里停、而不是等它自己播完：下面（服务器 FreezeAtLocation / 客户端 OnRep_PlantedTransform）
	 * 就要在同一个槽上播 PlantUnfoldMontage 了，两条都占着 DefaultSlot 会互相抢权重。
	 * 带 0.15s 混合时间收，接得上 ABP 的 Planted 状态，不会硬切。
	 */
	StopPlantStartMontageLocal();
}

void ASpike::StartDefuse(ABlasterCharacter* Character)
{
	if (!HasAuthority() || CurrentState != ESpikeState::ESS_Planted) return;

	ABlasterPlayerState* PS = Character->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->Team != ETeam::ET_TeamB) return;

	ServerStartDefuse(Character);
}

void ASpike::ServerStartDefuse_Implementation(ABlasterCharacter* Character)
{
	if (CurrentState != ESpikeState::ESS_Planted) return;
	// 空手蒙太奇那段不可打断（同 ServerStartPlant 那条）。下面会 SetCombatState(ECS_Defusing)
	// 把空手状态顶掉，两个"我才是当前状态"的收尾逻辑会打架。
	if (Character->IsEmptyHandLocked()) return;

	/*
	 * 已经在拆了就别再"开一次"。
	 *
	 * 这个函数是**状态入口**（下面会 SetCombatState(ECS_Defusing) + 播整串拆包动画），
	 * 必须有幂等门禁：任何一次重复触发都会把"掏出→待命"整串从头再播一遍
	 *（表现就是"拆到一半又掏了一遍defuse"），进度还会被下面那句 DefuseProgress = 0 清掉。
	 *
	 * 拦不住的情况：换个人按 —— `IsEmptyHandLocked()` 只认 ECS_EmptyHand，拆包期间角色是
	 * ECS_Defusing，所以第二个人按下去会走到这里，CurrentDefuser 和拆包器都被抢过去，
	 * 前一个人的动画却没人收（他手上就一直挂着掏出那个姿势）。
	 *
	 * 松手再按不受影响：那条路会先走 ServerCancelDefuse（把 bIsDefusing 置回 false）再进来，
	 * 掏出动画面因此照常重播。回合复位（ResetSpike）也清了 bIsDefusing，不会卡死。
	 */
	if (bIsDefusing) return;

	// 两段式拆包：拆满第一段（≥50）则从 50 继续，否则从头开始（进度单位 0-100）
	const float SegmentPercent = FirstDefuseSegmentFraction * 100.f;
	if (DefuseProgress < SegmentPercent)
	{
		DefuseProgress = 0.f;
	}

	bIsDefusing = true;
	CurrentDefuser = Character;
	DefuserController = Cast<ABlasterPlayerController>(Character->GetController());
	MulticastStartDefuse(Character);

	// 拆包时不能持枪：把手上的枪收回挂点，角色变空手（动画机走空手），
	// 并进入 ECS_Defusing 阻止切枪/开火/换弹（都要求 Unoccupied）
	Character->SetCombatState(ECombatState::ECS_Defusing);
	if (UCombatComponent* DefuseCombat = Character->GetCombatComponent())
	{
		DefuseCombat->HolsterEquippedWeapon();
	}

	/*
	 * 拆包那串蒙太奇：掏出 defuse → 待命（循环）→（被打断/拆完时）收起。
	 * 多播给所有客户端 —— 第三人称身体是所有人眼里的他，本人那台另外还要播手模那份
	 *（ABlasterCharacter::MulticastDefuseAnimation 内部会分）。
	 *
	 * ★ **从掏出 defuse 开始就算正在拆包**（用户要求）：进度在上面那几行就已经起走了，
	 *   和动画播到哪一段无关 —— 这里只是画面。
	 */
	Character->MulticastDefuseAnimation(true);

	// 拆包自动蹲下（理由同 ServerStartPlant 那条）
	Character->MulticastSpikeAutoCrouch(true);
}

void ASpike::ServerCancelDefuse_Implementation()
{
	// 两段式拆包：拆满第一段（≥50%）松手 → 进度掉回 50（下次从半程继续）；
	// 未满 50% 松手 → 清零重来（进度单位 0-100）
	const float SegmentPercent = FirstDefuseSegmentFraction * 100.f;
	if (DefuseProgress >= SegmentPercent)
	{
		DefuseProgress = SegmentPercent;
	}
	else
	{
		DefuseProgress = 0.f;
	}

	// 隐藏拆包进度条
	if (DefuserController)
	{
		DefuserController->ClientUpdateSpikeProgress(TEXT(""), -1.f);
	}

	ABlasterCharacter* Defuser = CurrentDefuser;

	bIsDefusing = false;
	CurrentDefuser = nullptr;
	DefuserController = nullptr;

	// 松开/打断拆包：各端摘掉并隐藏手上正在显示的拆包器
	MulticastStopDefuseKit();

	// 拆包被打断：恢复战斗状态，并自动掏枪（主武器优先，无主武器才掏副武器）
	if (Defuser)
	{
		Defuser->SetCombatState(ECombatState::ECS_Unoccupied);
		// 收起 defuse 那串蒙太奇（待命循环 → 收尾那一段），并站起来
		Defuser->MulticastDefuseAnimation(false);
		Defuser->MulticastSpikeAutoCrouch(false);
		if (UCombatComponent* DefuseCombat = Defuser->GetCombatComponent())
		{
			DefuseCombat->EquipBestOwnedWeapon();
		}
	}
}

void ASpike::MulticastStartDefuse_Implementation(ABlasterCharacter* Character)
{
	bIsDefusing = true;
	const float SegmentPercent = FirstDefuseSegmentFraction * 100.f;
	if (DefuseProgress < SegmentPercent)
	{
		DefuseProgress = 0.f;
	}

	// 把拆包器挂到拆包者手上显示（各端本地执行）
	ShowDefuserKit(Character);

	// 拆包音：开始拆即起（各端各响各的，3D 从包的位置发声），拆完/被打断掐断
	MulticastPlayDefuseArmSound();
}

void ASpike::MulticastDefuseComplete_Implementation()
{
	DefuseProgress = 100.f;
	bIsDefusing = false;

	// 拆包完成：拆包器摘除
	HideDefuserKit();

	// 拆包完成：掐断拆包音（拆完了不该还响着）
	StopDefuseArmSoundLocal();
}

void ASpike::MulticastStopDefuseKit_Implementation()
{
	// 拆包被中断：各端摘除并隐藏拆包器
	HideDefuserKit();

	// 拆包被中断（松开/移动打断/死亡/回合复位）：掐断拆包音
	StopDefuseArmSoundLocal();
}

void ASpike::MulticastPlayDefuseArmSound_Implementation()
{
	/*
	 * 拆包音：开始拆那一刻响**一次**，之后不再重播（素材 1.008s 单发，播完就静）。
	 *
	 * ★ 这条 multicast 是在 HasAuthority() 之外发的：拆包进度是权威机独占的，
	 *   但声音每一端都要响（客户端也得听见别人在拆）。
	 *   不需要 Tick 续播，所以客户端也不需要在 Tick 里做任何事。
	 */
	if (!DefuseArmAudio) return;

	// 首次响前按需加载素材（软引用懒加载，避开类加载期同步加载音频 —— 同 UpdateCountdownBeep）
	if (!bDefuseArmSoundLoaded)
	{
		bDefuseArmSoundLoaded = true;
		if (!DefuseArmSound.IsNull())
		{
			if (USoundBase* Snd = DefuseArmSound.LoadSynchronous())
			{
				DefuseArmAudio->SetSound(Snd);
			}
		}
	}
	if (DefuseArmAudio->Sound == nullptr) return;

	DefuseArmAudio->Play();
}

void ASpike::MulticastStopDefuseArmSound_Implementation()
{
	StopDefuseArmSoundLocal();
}

void ASpike::StopDefuseArmSoundLocal()
{
	// 拆完/被打断/回合复位：掐断正在响的拆包音。没在响时 Stop 是空操作，不用先判 IsPlaying。
	if (DefuseArmAudio)
	{
		DefuseArmAudio->Stop();
	}
}

void ASpike::ShowDefuserKit(ABlasterCharacter* Character)
{
	if (!Character || !Character->GetMesh() || !DefuserKitMeshComp) return;

	// 若拆包器还挂在上一个拆包者身上，先摘除
	HideDefuserKit();

	// 首次附加前按需加载拆包器网格（软引用懒加载，避开启动期类加载死锁）
	if (DefuserKitMeshComp->GetSkeletalMeshAsset() == nullptr && !DefuserKitMesh.IsNull())
	{
		if (USkeletalMesh* KitMesh = DefuserKitMesh.LoadSynchronous())
		{
			DefuserKitMeshComp->SetSkeletalMesh(KitMesh);
		}
	}

	USkeletalMesh* KitMesh = DefuserKitMeshComp->GetSkeletalMeshAsset();
	if (KitMesh == nullptr)
	{
		// 没配置网格就什么都不显示
		return;
	}

	// 和包本体走同一套解析：名字无效就退到 FallbackAttachPoint 并警告，
	// 不兜底的话 AttachToComponent 会静默落到网格原点（拆包器贴在脚底）。
	const FAttachmentTransformRules Rules(EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, EAttachmentRule::KeepWorld, false);

	/*
	 * ---- 3P 那份和 1P 那份**互斥**，同一台机器上只显示一个 ----
	 *
	 * 判的是 IsLocallyControlled，不是给组件设 SetOnlyOwnerSee/SetOwnerNoSee ——
	 * 那两个标志的判据是"组件的 Owner 是不是观察者"，而这个 actor 的 Owner 是关卡，
	 * 对所有人都成立，一设就成了"谁都看不见"（头文件 DefuserKitFPMeshComp 那条注释）。
	 *
	 * ⚠ 3P 那份必须**跳过**给本人这台机器，不能只靠"本人看不见自己的身体"：
	 *   本人看自己的身体确实是关掉的（GetMesh()->SetOwnerNoSee），但 OwnerNoSee 只是
	 *   **那一个组件**的渲染标志 —— 既不给子组件，更管不到另一个附着上来的组件
	 *   （ASpike::ApplyMeshVisibility 里那条注释踩的是同一个坑，那是包本体，这里是拆包器）。
	 *   不跳过的话，本人第一人称画面里会同时出现"挂在 3P 身上的那个"和下面那个 FP 的
	 *   —— 就是"拆包时看得见两个拆包器"。别人那台机器上 IsLocallyControlled 为假，
	 *   照常挂 3P，远端看拆包的人身上有拆包器。
	 */
	const bool bLocalDefuser = Character->IsLocallyControlled();
	if (!bLocalDefuser)
	{
		const FName KitPointTP = ResolveAttachPoint(Character->GetMesh(), DefuseKitSocketTP, TEXT("拆包器(3P)"));
		DefuserKitMeshComp->AttachToComponent(Character->GetMesh(), Rules, KitPointTP);
		DefuserKitMeshComp->SetRelativeScale3D(FVector::OneVector);
		DefuserKitMeshComp->SetVisibility(true);
		LocalKitAttachedCharacter = Character;
	}

	// ---- 第一人称那份：只在本人那台机器上挂 ----
	USkeletalMeshComponent* FPArms = Character->GetFPArmsMesh();
	if (!bLocalDefuser || FPArms == nullptr || DefuserKitFPMeshComp == nullptr)
	{
		return;
	}

	LocalKitAttachedCharacter = Character;

	if (DefuserKitFPMeshComp->GetSkeletalMeshAsset() != KitMesh)
	{
		DefuserKitFPMeshComp->SetSkeletalMesh(KitMesh);
	}

	const FName KitPointFP = ResolveAttachPoint(FPArms, DefuseKitSocketFP, TEXT("拆包器(1P)"));
	DefuserKitFPMeshComp->AttachToComponent(FPArms, Rules, KitPointFP);

	/*
	 * ★ 用**世界缩放**归一大小：手模那条链的根骨带 100 倍（瓦的米→厘米），挂上去继承下来
	 *   就是 100 倍大的拆包器。SetWorldScale3D 按父链反算相对缩放，于是这里填 1 就是
	 *   "和第三人称那个一样大"（和 FPSpikeMesh 补世界缩放是同一件事）。
	 *   ⚠ 顺序：先挂点再设缩放 —— 上面 Rules 的缩放规则是 KeepWorld，挂上去那一刻会保持
	 *     世界缩放并反算相对缩放；反过来先设缩放、再挂点的话，刚设好的值会被那次反算盖掉。
	 */
	DefuserKitFPMeshComp->SetWorldScale3D(FVector::OneVector);

	// 挂点上的微调（默认零 = 网格原点正落在挂点上）。只用位移和旋转 ——
	// 缩放上面那行说了算，这里再乘一次会两边打架。
	DefuserKitFPMeshComp->SetRelativeLocationAndRotation(
		DefuserKitFPMeshOffset.GetLocation(), DefuserKitFPMeshOffset.GetRotation());

	DefuserKitFPMeshComp->SetVisibility(true);
}

void ASpike::HideDefuserKit()
{
	if (DefuserKitMeshComp)
	{
		DefuserKitMeshComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		DefuserKitMeshComp->SetVisibility(false);
	}
	if (DefuserKitFPMeshComp)
	{
		DefuserKitFPMeshComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		DefuserKitFPMeshComp->SetVisibility(false);
	}
	LocalKitAttachedCharacter = nullptr;
}

void ASpike::CancelAction()
{
	if (!HasAuthority()) return;

	if (bIsPlanting)
	{
		ServerCancelPlant();
	}
	else if (bIsDefusing)
	{
		ServerCancelDefuse();
	}
}

void ASpike::StartExplodeTimer()
{
	GetWorldTimerManager().SetTimer(ExplodeTimerHandle, this, &ASpike::OnExplode, ExplodeCountdown, false);
	PlantStartTime = GetWorld()->GetTimeSeconds();
}

float ASpike::GetRemainingTimer() const
{
	if (CurrentState != ESpikeState::ESS_Planted) return 0.f;
	if (HasAuthority()) return GetWorldTimerManager().GetTimerRemaining(ExplodeTimerHandle);
	return FMath::Max(0.f, ExplodeCountdown - (GetWorld()->GetTimeSeconds() - PlantStartTime));
}

void ASpike::OnExplode()
{
	SetSpikeState(ESpikeState::ESS_Exploded);
	MulticastExplode();
	MulticastStopPlantedSound();

	// 清空所有玩家的倒计时 UI
	ClearSpikeUIForAll();

	if (ABlasterGameState* GS = GetBlasterGameState())
	{
		GS->SpikeState = ESpikeState::ESS_Exploded;
		GS->bSpikePlanted = false;
	}

	// 爆炸成功：结束本回合，进攻方获胜
	if (ABlasterGameMode* GM = GetWorld()->GetAuthGameMode<ABlasterGameMode>())
	{
		GM->CheckRoundEnd();
	}
}

void ASpike::BroadcastExplodeCountdown()
{
	if (!HasAuthority()) return;

	const float Remaining = GetRemainingTimer();
	const int32 Seconds = FMath::CeilToInt(Remaining);
	const FString CountdownText = FString::Printf(TEXT("%02d:%02d"), Seconds / 60, Seconds % 60);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(*It);
		if (!PC) continue;

		// 正在拆包的人继续看自己的拆包进度，不被爆炸倒计时覆盖
		if (DefuserController && PC == DefuserController) continue;

		// Progress 传 -1：只显示倒计时文字，不显示进度条
		PC->ClientUpdateSpikeProgress(CountdownText, -1.f);
	}
}

void ASpike::ClearSpikeUIForAll()
{
	if (!HasAuthority()) return;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(*It))
		{
			PC->ClientUpdateSpikeProgress(TEXT(""), -1.f);
		}
	}
}

void ASpike::MulticastExplode_Implementation()
{
	// 爆炸瞬间若还有人在拆包：摘掉他手上的拆包器
	HideDefuserKit();

	/*
	 * 爆炸球：各端本地开始胀，服务器同时起伤害结算。
	 *
	 * ★ 必须放在下面 ExplosionAudio 那两个 early-return **之前**：
	 *   音效素材没配好（组件为空 / Sound 加载不出来）不该把整个爆炸球一起吞掉 ——
	 *   那是纯视觉 + 伤害，和音效之间没有任何依赖。
	 */
	StartExplosionSphere();

	// 各端在 spike 位置播一次爆炸 boom（独立组件，完整播完不被 StopPlantedSound 截断）
	if (!ExplosionAudio) return;

	if (!bExplosionSoundLoaded)
	{
		bExplosionSoundLoaded = true;
		if (!ExplosionSound.IsNull())
		{
			if (USoundBase* Snd = ExplosionSound.LoadSynchronous())
			{
				ExplosionAudio->SetSound(Snd);
			}
		}
	}
	if (ExplosionAudio->Sound == nullptr) return;

	ExplosionAudio->Stop();
	ExplosionAudio->Play();
}

/*
 * ==================== 爆炸球 ====================
 *
 * 分工总纲：**视觉各端自己推演，伤害只在服务器结算**（和倒计时视觉/beep 同一套路数）。
 * 所有端判断"球现在多大"用的是同一个公式：Elapsed / GrowTime 夹到 0~1，再乘最大半径，
 * 所以两边看到的球一样大；起点取 MulticastExplode 到达的那一帧，天然同步。
 */

void ASpike::StartExplosionSphere()
{
	if (!ExplosionSphere) return;

	bExplosionSphereActive = true;
	ExplosionSphereElapsed = 0.f;

	// 换算基准取网格资产自己的包围球半径（同 CloveSmoke）：BP 里把球换成别的模型时，
	// 只要模型以原点为中心，ExplosionSphereRadius 照样是真实世界半径，不用回来改代码。
	ExplosionSphereAssetRadius = 50.f;	// /Engine/BasicShapes/Sphere 的半径
	if (const UStaticMesh* Mesh = ExplosionSphere->GetStaticMesh())
	{
		const float AssetRadius = Mesh->GetBounds().SphereRadius;
		if (AssetRadius > KINDA_SMALL_NUMBER)
		{
			ExplosionSphereAssetRadius = AssetRadius;
		}
	}

	// 材质：BP 里填了就直接用；否则首次爆炸时把默认软引用加载出来（同 ExplosionSound 的懒加载规矩）
	if (ExplosionSphereMaterial)
	{
		ExplosionSphere->SetMaterial(0, ExplosionSphereMaterial);
	}
	else if (!bExplosionSphereMatLoaded)
	{
		bExplosionSphereMatLoaded = true;
		if (!ExplosionSphereMaterialAsset.IsNull())
		{
			if (UMaterialInterface* Mat = ExplosionSphereMaterialAsset.LoadSynchronous())
			{
				ExplosionSphere->SetMaterial(0, Mat);
			}
		}
	}

	// 从 0 起胀。缩放不能真给 0：某些情况下零缩放会让变换求逆出 NaN 并刷警告，给个极小值即可。
	ExplosionSphere->SetWorldScale3D(FVector(KINDA_SMALL_NUMBER));
	ExplosionSphere->SetVisibility(true);

	// 伤害只在服务器结算。FirstDelay = 0：进圈的人当场就开始掉血，不用先白等一个间隔。
	if (HasAuthority())
	{
		const float Interval = FMath::Max(ExplosionDamageTickInterval, 0.02f);
		GetWorldTimerManager().SetTimer(
			ExplosionSphereDamageTimer, this, &ASpike::ApplyExplosionSphereDamage,
			Interval, /*bLoop=*/true, /*FirstDelay=*/0.f);
	}
}

void ASpike::UpdateExplosionSphere(float DeltaTime)
{
	if (!ExplosionSphere)
	{
		bExplosionSphereActive = false;
		return;
	}

	ExplosionSphereElapsed += DeltaTime;

	const float GrowTime = FMath::Max(ExplosionSphereGrowTime, KINDA_SMALL_NUMBER);
	const float GrowProgress = FMath::Clamp(ExplosionSphereElapsed / GrowTime, 0.f, 1.f);

	// 半径 → 缩放：资产半径对应缩放 1，所以比值就是要设的缩放
	ExplosionSphere->SetWorldScale3D(FVector(ExplosionSphereRadius * GrowProgress / ExplosionSphereAssetRadius));

	// 胀满 → 保持 HoldTime → 收
	if (ExplosionSphereElapsed >= GrowTime + FMath::Max(ExplosionSphereHoldTime, 0.f))
	{
		StopExplosionSphere();
	}
}

void ASpike::StopExplosionSphere()
{
	bExplosionSphereActive = false;

	if (ExplosionSphere)
	{
		ExplosionSphere->SetVisibility(false);
	}

	if (HasAuthority())
	{
		GetWorldTimerManager().ClearTimer(ExplosionSphereDamageTimer);
	}
}

void ASpike::ApplyExplosionSphereDamage()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// 用**当前**半径判定：球还在胀的时候只有已经被罩住的人吃这一跳。
	// （两边用的是同一个公式，所以"打到的"和"看到的"永远一致。）
	const float GrowTime = FMath::Max(ExplosionSphereGrowTime, KINDA_SMALL_NUMBER);
	const float GrowProgress = FMath::Clamp(ExplosionSphereElapsed / GrowTime, 0.f, 1.f);
	const float RadiusSq = FMath::Square(ExplosionSphereRadius * GrowProgress);

	const FVector Origin = GetActorLocation();
	const float PerTick = ExplosionDamagePerSecond * FMath::Max(ExplosionDamageTickInterval, 0.02f);

	for (TActorIterator<ABlasterCharacter> It(World); It; ++It)
	{
		ABlasterCharacter* Victim = *It;
		if (!Victim || Victim->IsElimmed()) continue;

		// 球形判定（不是火圈那种"水平距离 + 竖直容差"的圆柱）：
		// 爆点在脚下，20m 的球会把上下楼层一起罩住，"被球盖到"按直线距离算才和看到的一致。
		if (FVector::DistSquared(Victim->GetActorLocation(), Origin) > RadiusSq) continue;

		/*
		 * 伤害打**所有人，含进攻方自己** —— 原作行为：安完包不撤，会被自己的爆能器炸死。
		 * 所以这里**刻意没有任何比队逻辑**，和火圈(APhoenixFireZone)那边正好相反，别照抄错了。
		 *
		 * InstigatorController 传 nullptr：爆能器是地图物件，没有"是谁杀的"。
		 * 这条链已验证过是安全的：ABlasterCharacter::ReceiveDamage 对 Instigator 有判空，
		 * ABlasterGameMode::PlayerEliminated 里 AttackerPS 为空时击杀信息打成 "???"，
		 * 也不会给任何人加击杀数 / 弹击杀标记。
		 */
		UGameplayStatics::ApplyDamage(Victim, PerTick, nullptr, this, UDamageType::StaticClass());
	}
}

void ASpike::MulticastPlayPlantedSound_Implementation()
{
	if (PlantedAudio && PlantedSound)
	{
		PlantedAudio->SetSound(PlantedSound);
		PlantedAudio->Play();
	}
}

void ASpike::MulticastStopPlantedSound_Implementation()
{
	if (PlantedAudio)
	{
		PlantedAudio->Stop();
	}
	if (CountdownAudio)
	{
		CountdownAudio->Stop();
	}
}

void ASpike::ApplySpatialAttenuation(UAudioComponent* Comp) const
{
	if (!Comp) return;

	FSoundAttenuationSettings Settings;
	if (SpikeSpatialAttenuation)
	{
		Settings = SpikeSpatialAttenuation->Attenuation;
	}
	else
	{
		// 代码默认：球 400cm 内满音量、之后 4000cm 渐弱，并开启 3D 定位（能听出方位）。
		// FSoundAttenuationSettings 默认已是 Linear + Sphere + bSpatialize，这里只显式补关键项。
		Settings.bAttenuate = true;
		Settings.bSpatialize = true;
		Settings.FalloffDistance = 4000.f;
	}

	Comp->bOverrideAttenuation = true;
	Comp->AttenuationOverrides = Settings;
}

void ASpike::UpdateCountdownBeep(float DeltaTime)
{
	// 拆包完成 montage 播放中/已拆：不再响
	if (DefuseProgress >= 100.f) return;
	if (!CountdownAudio) return;

	const float Remaining = GetRemainingTimer();
	if (Remaining <= 0.f || Remaining > ExplodeCountdown) return;

	// 首次响前按需加载 beep 素材（软引用懒加载，避开类加载期同步加载音频）
	if (!bCountdownSoundLoaded)
	{
		bCountdownSoundLoaded = true;
		if (!CountdownBeepSound.IsNull())
		{
			if (USoundBase* Snd = CountdownBeepSound.LoadSynchronous())
			{
				CountdownAudio->SetSound(Snd);
			}
		}
	}
	if (CountdownAudio->Sound == nullptr) return;

	// Valorant 阶梯节奏：剩余时间越少响得越密
	// >35s：每秒 1 响 → >25s：每秒 2 响 → >10s：每秒 4 响 → ≤10s：每秒 8 响
	float Interval = 1.0f;
	if (Remaining <= 35.f) Interval = 0.5f;
	if (Remaining <= 25.f) Interval = 0.25f;
	if (Remaining <= 10.f) Interval = 0.125f;

	BeepAccumulator += DeltaTime;
	while (BeepAccumulator >= Interval)
	{
		BeepAccumulator -= Interval;
		// 停掉再播：每响都是独立一声（8 响/s 时 ~0.15s 的样本会重叠成糊音）
		CountdownAudio->Stop();
		CountdownAudio->Play();
	}
}

FName ASpike::ResolveAttachPoint(USkeletalMeshComponent* Mesh, FName Desired, const TCHAR* What) const
{
	if (!Mesh) return NAME_None;

	// socket 和骨骼都算"能用"：往下走的都是 AttachToComponent，它两者都吃。
	auto Usable = [Mesh](FName Name)
	{
		if (Name == NAME_None) return false;
		return Mesh->GetSocketByName(Name) != nullptr || Mesh->GetBoneIndex(Name) != INDEX_NONE;
	};

	if (Usable(Desired)) return Desired;

	if (Desired != NAME_None)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Spike] %s挂点 '%s' 在网格 '%s' 上既不是 socket 也不是骨骼，退到 '%s'。"
				 "（想挂别处就改 BP_Spike 里的挂点名，或在 FallbackAttachPoint 里改兜底名）"),
			What, *Desired.ToString(),
			Mesh->GetSkeletalMeshAsset() ? *Mesh->GetSkeletalMeshAsset()->GetName() : TEXT("?"),
			*FallbackAttachPoint.ToString());
	}

	return Usable(FallbackAttachPoint) ? FallbackAttachPoint : NAME_None;
}

void ASpike::AttachToCharacterAtPoint(ABlasterCharacter* Character, FName Desired, const TCHAR* What)
{
	if (!Character || !Character->GetMesh()) return;

	const FName Point = ResolveAttachPoint(Character->GetMesh(), Desired, What);
	// 统一走 AttachToComponent（socket / 骨骼 / NAME_None 三种都吃）。
	// 换成它的原因见引擎 USkeletalMeshSocket::AttachActor（SkeletalMesh.cpp:6013）：
	// 它内部就是 root->AttachToComponent(SkelComp, SnapToTargetNotIncludingScale, SocketName)，
	// 但外面套了 `if (GetSocketMatrix(...))` —— 名字查不到时整段不执行、也不报错。
	AttachToComponent(Character->GetMesh(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, Point);
	// 挂点上的位置按「网格原点」算，补回网格子件的位移，让外观和以前一致
	ApplyMeshPivotCompensation();
}

void ASpike::MulticastSetAttachSlot_Implementation(ABlasterCharacter* Character, ESpikeAttachSlot Slot)
{
	if (!Character || !Character->GetMesh()) return;

	/*
	 * 两道闸都是防「迟到的 RPC 把已经结束的状态又摆回去」：
	 *   · 下包那一下：安包完成/取消后 bIsPlanting 已经落回 false，此时再挂到锚点上会把
	 *     FreezeAtLocation 摆好的落地位置顶掉。
	 *   · 换回手那一下：掉包路径（Drop → ServerCancelPlant）也会走这里，而那之后网格本来
	 *     就该是收起来的（bCarrierDrawn = false），别在身上留一个幽灵。
	 */
	if (Slot == ESpikeAttachSlot::ESAS_Plant && !bIsPlanting) return;
	if (Slot == ESpikeAttachSlot::ESAS_Draw && !bCarrierDrawn) return;

	switch (Slot)
	{
	case ESpikeAttachSlot::ESAS_Carry:
		AttachToCharacterAtPoint(Character, CarryAttachSocket, TEXT("携带"));
		break;
	case ESpikeAttachSlot::ESAS_Plant:
		AttachToCharacterAtPoint(Character, PlantAttachSocket, TEXT("下包"));
		break;
	case ESpikeAttachSlot::ESAS_Draw:
	default:
		AttachToCharacterAtPoint(Character, DrawAttachSocket, TEXT("掏出"));
		break;
	}
}

void ASpike::AttachToCharacter(ABlasterCharacter* Character)
{
	// 拾起时若还在物理掉落中则停掉（否则重新附着会和复制移动打架）
	StopPhysicsDrop();

	CurrentCarrier = Character;
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 收回背上 = 又变回"常态隐藏"（重新掏出来要再按一次 4）
	bCarrierDrawn = false;
	ApplyMeshVisibility();
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	AttachToCharacterAtPoint(Character, CarryAttachSocket, TEXT("携带"));

	// 兜底：所有客户端（尤其刚重生的远端携带者代理）都重新附着一次
	if (HasAuthority())
	{
		MulticastCarrierAttach(Character);
	}
}

void ASpike::MulticastCarrierAttach_Implementation(ABlasterCharacter* Character)
{
	if (!Character || !Character->GetMesh()) return;

	StopPhysicsDrop();
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 显隐不在这里写死 true：这一条只是"重新挂一次"的兜底，可能在"已经掏在手上"的时候跑
	//（新回合复位后立刻交给随机攻击者那条路），写死 true 会把"常态隐藏"顶掉。
	// 摆正显隐一律走 ApplyMeshVisibility（它读复制过来的 bCarrierDrawn）。
	ApplyMeshVisibility();

	// 和 AttachToCharacter 一致：补回网格子件的位移（否则远端看到的携带位置会偏 31.5cm）
	AttachToCharacterAtPoint(Character, CarryAttachSocket, TEXT("携带"));
}

void ASpike::Draw(ABlasterCharacter* Character)
{
	if (!Character) return;

	StopPhysicsDrop();

	CurrentCarrier = Character;
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 掏到手上 = 显形。这一位是复制的 —— 各端（包括本人）都靠它把网格显示出来，
	// 本人手上另有一个只本人可见的副本（ABlasterCharacter::FPSpikeMesh），
	// 所以本人这台机器上这个 actor 反过来还要藏起来（见 ApplyMeshVisibility）。
	bCarrierDrawn = true;
	ApplyMeshVisibility();

	// 这里以前是 `if (找到 socket) { 重挂 }`，没有 else —— 名字写错就整段跳过，
	// 表现是「按 4 掏出包，包一动不动」而且日志里什么都不打。现在无条件重挂。
	AttachToCharacterAtPoint(Character, DrawAttachSocket, TEXT("掏出"));
}

void ASpike::Holster(ABlasterCharacter* Character)
{
	AttachToCharacter(Character);
}

void ASpike::DetachFromCharacter()
{
	CurrentCarrier = nullptr;
	// 摘下来了 → "被掏在手上"这一位跟着清掉（掉在地上要显形，靠的是这一位为假 + 状态是 Dropped）
	bCarrierDrawn = false;
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	// 网格自身不参与任何碰撞/物理；掉包落地由根组件 CollisionBox 模拟（Drop 里紧接着 EnablePhysicsDrop）
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}

void ASpike::CapturePlantPlacement()
{
	/*
	 * 只从「安包完成」那一处调用（服务器），此刻包还挂在 PlantAttachSocket 上、
	 * 姿态就是下包动画把它放下的那一帧 —— 直接读 actor 自己的世界变换即可：
	 * 它已经含了挂点变换 + MeshAttachPivotOffset 的补偿，和玩家眼里看到的完全一致，
	 * 不用再自己拼 socket 变换（那种拼法漏掉 pivot 补偿，反而会差十几厘米）。
	 *
	 * 停止下包 montage（MulticastPlantComplete）之后 pose 会回落，所以调用点必须在它前面。
	 */
	bHasPlantPlacement = false;
	if (!bIsPlanting || CurrentCarrier == nullptr) return;

	PlantPlacementTransform = GetActorTransform();
	bHasPlantPlacement = true;
}

void ASpike::FreezeAtLocation()
{
	CurrentCarrier = nullptr;
	bCarrierDrawn = false;
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	/*
	 * 安放位置 = **下包动画把包放下的那一帧的位置**（CapturePlantPlacement 抓的快照），
	 * 不是下包者脚下 —— 动画里包是被放到身前 83.7cm 的地上的，按脚下摆会和动画对不上（会瞬移一下）。
	 * 快照拿不到（理论上不会走到：服务器每次安包完成前都抓）才退回脚底。
	 */
	FVector DropPoint = GetActorLocation();
	if (bHasPlantPlacement)
	{
		DropPoint = PlantPlacementTransform.GetLocation();
	}
	else if (CurrentPlanter)
	{
		DropPoint = CurrentPlanter->GetActorLocation();
	}

	// 在落点正上/下方 trace 找地面高度（斜坡、箱子顶上安包一样贴地）
	FVector GroundPoint = DropPoint;
	const FVector TraceStart = DropPoint + FVector(0.f, 0.f, 200.f);
	const FVector TraceEnd = DropPoint - FVector(0.f, 0.f, 500.f);
	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	if (CurrentPlanter) Params.AddIgnoredActor(CurrentPlanter);
	if (GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params))
	{
		GroundPoint = Hit.ImpactPoint;
	}

	/*
	 * 朝向：跟动画那一帧的 yaw，俯仰/翻滚归零（包始终直立）。
	 * 实测放下那一帧挂点自身就是直立的（up = (0,0,1)），所以"只取 yaw"不会和动画的朝向打架；
	 * 顺带免疫 PlantDuration 被改小、快照抓在动画中段（那时挂点是斜的）的情况。
	 * 没快照就沿用初始朝向。
	 */
	const FRotator PlacementRotation = bHasPlantPlacement
		? FRotator(0.f, PlantPlacementTransform.Rotator().Yaw, 0.f)
		: InitialSpawnRotation;
	SetActorRotation(PlacementRotation);

	// 底部贴紧地面：origin 落点 = 地面Z - 网格最底点(actor 坐标系，负值)，让网格最底点正好贴地
	const FVector LocalBottom(0.f, 0.f, GetMeshBottomLocalZ());
	const FVector WorldBottomShift = GetActorRotation().RotateVector(LocalBottom);
	SetActorLocation(FVector(DropPoint.X, DropPoint.Y, GroundPoint.Z - WorldBottomShift.Z + PlantedGroundZOffset),
		false, nullptr, ETeleportType::TeleportPhysics);

	// 快照用完即弃：下次安包重新抓
	bHasPlantPlacement = false;

	// 记录安包后的最终 transform，复制到客户端让所有人看到同一摆放
	PlantedTransform = GetActorTransform();

	// 安包完成：spike 自己在原地播放"展开"montage（服务器直接播；客户端在 OnRep_PlantedTransform 里播）
	PlayPlantUnfoldMontageLocal();

	// 安放后不再模拟物理（万一它是掉包落地后被捡起来安包的）：方块只保留拾取/拆包重叠
	if (CollisionBox)
	{
		CollisionBox->SetSimulatePhysics(false);
		CollisionBox->SetEnableGravity(false);
	}
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	SpikeMesh->SetSimulatePhysics(false);
	SpikeMesh->SetEnableGravity(false);
	SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SpikeMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
}

void ASpike::SetSpikeState(ESpikeState NewState)
{
	CurrentState = NewState;
	OnRep_SpikeState();
}

void ASpike::OnRep_CarrierDrawn()
{
	ApplyMeshVisibility();
}

const ABlasterCharacter* ASpike::GetCarryingCharacter() const
{
	// 服务器上有 CurrentCarrier；客户端上那个指针**不复制**，退回"看它现在挂在谁身上" ——
	// 附着本身是复制的（attachment replication），两边的答案一致。
	if (CurrentCarrier) return CurrentCarrier;
	return Cast<ABlasterCharacter>(GetAttachParentActor());
}

void ASpike::ApplyMeshVisibility()
{
	if (!SpikeMesh) return;

	/*
	 * 常态隐藏：
	 *   背在携带者身上（Carried）→ 藏，除非他按 4 掏到手上（bCarrierDrawn）；
	 *   其它任何状态（掉在地上 / 已安放 / 已拆除 / 已爆炸）→ 显形。
	 */
	const bool bCarried = (CurrentState == ESpikeState::ESS_Carried);
	const bool bVisible = !bCarried || bCarrierDrawn;

	/*
	 * 携带者本人那台再压一层。
	 *
	 * 这个 actor 挂在第三人称身体上，而**本人看自己的身体是关掉的**
	 *（ABlasterCharacter::RefreshFPRig 里的 GetMesh()->SetOwnerNoSee(true)）——
	 * 但 OwnerNoSee 是组件的渲染标志，既不传给子组件、更管不到附着上来的 actor，
	 * 所以不压这一层的话，掏出包的人会在自己第一人称画面里额外看到一个包飘在身上。
	 * 本人手上那个是角色上**另一个**组件（FPSpikeMesh，只本人可见），这里让位给它。
	 *
	 * 注意不能用 SpikeMesh->SetOwnerNoSee()：那个标志比的是"组件的 Owner 是不是观察者"，
	 * 而这个 actor 的 Owner 是关卡，对**所有人**都成立 —— 一设就把别人看到的包也一起藏了。
	 */
	const ABlasterCharacter* Carrier = GetCarryingCharacter();
	const bool bHideFromLocalCarrier = bCarried && bCarrierDrawn && Carrier && Carrier->IsLocallyControlled();

	SpikeMesh->SetVisibility(bVisible && !bHideFromLocalCarrier, /*bPropagateToChildren=*/true);
}

void ASpike::OnRep_SpikeState()
{
	switch (CurrentState)
	{
	case ESpikeState::ESS_Dropped:
	case ESpikeState::ESS_Carried:
		// 显隐不写死：掉在地上是显形的，背在身上是藏着的（掏到手上才显形）——
		// 这一句同时覆盖"被丢到地上时显形"和"常态隐藏"两条。
		ApplyMeshVisibility();
		// 网格只做视觉：碰撞/物理全在根组件 CollisionBox 上（掉包落地由它模拟，见 EnablePhysicsDrop）
		SpikeMesh->SetSimulatePhysics(false);
		SpikeMesh->SetEnableGravity(false);
		SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// 回到“未安/常态”：清掉上一回合残留的 Core 转角与红色发光（回合切换/掉落/拾起都走到这里）
		ResetPlantedVisualState();
		// 若上一回合的爆炸 boom 还在响（~6s 长），回合复位时掐掉
		if (ExplosionAudio)
		{
			ExplosionAudio->Stop();
		}
		break;
	case ESpikeState::ESS_Planted:
		ApplyMeshVisibility();
		SpikeMesh->SetSimulatePhysics(false);
		SpikeMesh->SetEnableGravity(false);
		SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// 每次安包都从 0 度重新开始累计 Core 转角，并强制应用当前阶段的发光色
		CoreSpinDegrees = 0.f;
		UrgencyStage = 0;
		LastAppliedGlowStage = -1;
		// beep 节奏也从 0 累积：第一响发生在安包后约一个节拍间隔
		BeepAccumulator = 0.f;
		break;
	case ESpikeState::ESS_Defused:
	case ESpikeState::ESS_Exploded:
		// 拆除/爆炸后不销毁 spike，且网格留在原地可见（拆完=收拢外观、爆炸=残骸）。
		// Tick 只在 Planted 态累计 Core 转角，这里不再转动；光条也在这两态关掉（只在 Planted 亮）
		ApplyMeshVisibility();
		SpikeMesh->SetSimulatePhysics(false);
		SpikeMesh->SetEnableGravity(false);
		SpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		TurnOffGlow();
		break;
	}
}

void ASpike::ResetPlantedVisualState()
{
	CoreSpinDegrees = 0.f;
	UrgencyStage = 0;
	LastAppliedGlowStage = -1;
	TurnOffGlow();
}

void ASpike::OnRep_PlantedTransform()
{
	// 客户端收到安包后的 transform：恢复 spike 在地面的摆放（位置 + 竖直旋转）
	SetActorLocation(PlantedTransform.GetLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(PlantedTransform.GetRotation());

	// spike 已被放到地面正确位置，此时才播展开 montage
	PlayPlantUnfoldMontageLocal();
}

void ASpike::OnRep_DroppedTransform()
{
	// 客户端收到掉包落稳后的落点（服务器物理模拟的结果），吸附到同一位置
	SetActorLocation(DroppedTransform.GetLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(DroppedTransform.GetRotation());
}

void ASpike::EnablePhysicsDrop()
{
	if (!HasAuthority() || !CollisionBox) return;

	// 掉包 = 让根组件（方块碰撞体）真跑物理：自由落体 + 翻滚 + 自己撞地停稳。
	// 骨骼网格没有物理资产跑不了物理，方块碰撞体就是为这一步换的。
	// 先关模拟再改碰撞响应：改一个正在模拟的刚体的响应会触发重建物理状态、丢掉速度
	CollisionBox->SetSimulatePhysics(false);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetCollisionObjectType(ECC_PhysicsBody);
	// ⚠️ 不要用 SetCollisionResponseToAllChannels(Ignore) 清场再逐条设回来：那会让
	// ECC_Pawn 瞬间变成 Ignore → 触发一次 EndOverlap（OnPickupEndOverlap 会清掉
	// JustDroppedCharacter 保护）→ 紧接着恢复 Overlap 又触发 BeginOverlap，保护已经没了
	// → 刚扔下就被自己秒捡。只改需要改的通道，Pawn 全程保持 Overlap。
	// 世界静态/动态：阻挡（能落地、能被墙挡、能压在别的东西上）
	CollisionBox->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionBox->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	// 角色：保持重叠不阻挡（人能穿过去踩上来触发自动拾取），和原来拾取球行为一致
	CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CollisionBox->SetGenerateOverlapEvents(true);

	CollisionBox->SetSimulatePhysics(true);
	CollisionBox->SetEnableGravity(true);
	// 轻微阻尼：落地后不一直滑、不一直微颤（真空中否则会滑很久）
	CollisionBox->SetLinearDamping(0.1f);
	CollisionBox->SetAngularDamping(0.5f);

	bPhysicsDropping = true;
	PhysicsDropElapsed = 0.f;

	// 下落/翻滚期间把物理运动复制给客户端（各端都能看到它掉下去）；落稳后由 Tick 记录落点
	SetReplicateMovement(true);
}

void ASpike::StopPhysicsDrop()
{
	bPhysicsDropping = false;
	SetReplicateMovement(false);

	if (CollisionBox)
	{
		CollisionBox->SetSimulatePhysics(false);
		CollisionBox->SetEnableGravity(false);
	}
}

void ASpike::ApplyMeshPivotCompensation()
{
	// 网格子件相对 actor 原点有位移（见 SetupCollisionBoxFromMesh），而 socket / 关卡摆放给的
	// 位置是「网格原点」的位置 → 沿 actor 局部轴偏移补回来。附着时 actor 旋转 = socket 旋转，
	// 所以局部偏移正好等于世界空间中需要的那一段。
	if (!MeshAttachPivotOffset.IsNearlyZero())
	{
		AddActorLocalOffset(MeshAttachPivotOffset, false);
	}
}

float ASpike::GetMeshBottomLocalZ() const
{
	// 网格最底点在 actor 坐标系下的 Z（负值）：origin 落到 groundZ - 该值，最底点即贴地
	if (SpikeMesh && SpikeMesh->GetSkeletalMeshAsset())
	{
		const FBoxSphereBounds& Bounds = SpikeMesh->GetSkeletalMeshAsset()->GetImportedBounds();
		const FVector MeshScale = SpikeMesh->GetRelativeScale3D();
		return MeshRelativeOffset.Z + (Bounds.Origin.Z - Bounds.BoxExtent.Z) * MeshScale.Z;
	}
	return MeshBottomInActorZ;
}

void ASpike::PlayPlantUnfoldMontageLocal()
{
	if (!PlantUnfoldMontage || !SpikeMesh) return;
	if (UAnimInstance* Anim = SpikeMesh->GetAnimInstance())
	{
		Anim->Montage_Play(PlantUnfoldMontage);
	}
}

void ASpike::PlayPlantStartMontageLocal()
{
	if (!PlantStartMontage || !SpikeMesh) return;
	if (UAnimInstance* Anim = SpikeMesh->GetAnimInstance())
	{
		Anim->Montage_Play(PlantStartMontage);
	}
}

void ASpike::StopPlantStartMontageLocal()
{
	// 留空时直接返回：Montage_Stop 的第二个参数是 nullptr 时会停掉**所有** montage，
	// 那会把正在播的落地展开/拆包 montage 一起打断
	if (!PlantStartMontage || !SpikeMesh) return;
	if (UAnimInstance* Anim = SpikeMesh->GetAnimInstance())
	{
		Anim->Montage_Stop(0.15f, PlantStartMontage);
	}
}

void ASpike::MulticastStopPlantStartMontage_Implementation()
{
	StopPlantStartMontageLocal();
}

void ASpike::PlayDefuseCompleteMontageLocal()
{
	if (!DefuseCompleteMontage || !SpikeMesh) return;
	if (UAnimInstance* Anim = SpikeMesh->GetAnimInstance())
	{
		Anim->Montage_Play(DefuseCompleteMontage);
	}
}

void ASpike::MulticastPlayDefuseCompleteMontage_Implementation()
{
	PlayDefuseCompleteMontageLocal();
}

void ASpike::HideAfterDefuseMontage()
{
	// 拆包完成 montage 播完：进入 Defused 状态隐藏 spike
	if (HasAuthority())
	{
		SetSpikeState(ESpikeState::ESS_Defused);
	}
}

void ASpike::UpdatePlantedVisual(float DeltaTime)
{
	// 每帧按剩余时间推演 Core 累计转角与发光阶段（视觉，各端独立运行）
	float Remaining = GetRemainingTimer();

	// 客户端可能先收到 "已安包" 再收到 PlantStartTime：期间按满时间算，避免刚安包就进急迫/变红
	if (!HasAuthority() && PlantStartTime <= 0.f)
	{
		Remaining = ExplodeCountdown;
	}

	int32 Stage = 0;
	if (Remaining <= CriticalRemainingTime) Stage = 2;
	else if (Remaining <= AlertRemainingTime) Stage = 1;

	const float Speed = (Stage == 2) ? CoreSpinCriticalSpeed
		: (Stage == 1 ? CoreSpinAlertSpeed : CoreSpinSpeed);

	// 拆包完成(montage 期间, DefuseProgress=100) 或 spike 正在播 montage 时不再额外累加转角，
	// 避免与 montage 里自带的 Core 转动叠加成双重转速
	UAnimInstance* Anim = SpikeMesh ? SpikeMesh->GetAnimInstance() : nullptr;
	const bool bSpikeMontagePlaying = Anim &&
		((PlantUnfoldMontage && Anim->Montage_IsPlaying(PlantUnfoldMontage)) ||
		 (DefuseCompleteMontage && Anim->Montage_IsPlaying(DefuseCompleteMontage)));
	if (DefuseProgress < 100.f && !bSpikeMontagePlaying)
	{
		CoreSpinDegrees += Speed * DeltaTime;
	}

	if (Stage != LastAppliedGlowStage)
	{
		LastAppliedGlowStage = Stage;
		UrgencyStage = Stage; // 给蓝图/动画机读当前阶段（0/1/2）
		ApplyGlowColor(Stage);
	}
}

void ASpike::SetupGlowMaterials()
{
	if (!SpikeMesh || !SpikeMesh->GetSkeletalMeshAsset()) return;

	GlowMIDs.Reset();
	const TArray<UMaterialInterface*>& Mats = SpikeMesh->GetMaterials();
	for (int32 i = 0; i < Mats.Num(); ++i)
	{
		if (UMaterialInterface* Mat = Mats[i])
		{
			const FString MatName = Mat->GetName();
			// 只挑核心/临时的自发光槽（M_Bomb_CoreEmissive / M_Bomb_TempEmissive）
			if (MatName.EndsWith(TEXT("CoreEmissive")) || MatName.EndsWith(TEXT("TempEmissive")))
			{
				if (UMaterialInstanceDynamic* MID = SpikeMesh->CreateAndSetMaterialInstanceDynamic(i))
				{
					GlowMIDs.Add(MID);
				}
			}
		}
	}
}

void ASpike::SetGlowColor(const FLinearColor& Color)
{
	for (UMaterialInstanceDynamic* MID : GlowMIDs)
	{
		if (MID)
		{
			MID->SetVectorParameterValue(GlowColorParamName, Color);
		}
	}
}

void ASpike::ApplyGlowColor(int32 Stage)
{
	const FLinearColor& C = (Stage == 2) ? CriticalGlowColor : (Stage == 1 ? AlertGlowColor : NormalGlowColor);
	SetGlowColor(C);
}

void ASpike::TurnOffGlow()
{
	// 黑 = 自发光关闭（光条只在 Planted 态由 ApplyGlowColor 按阶段点亮）
	SetGlowColor(FLinearColor(0.f, 0.f, 0.f, 1.f));
}

ABlasterGameState* ASpike::GetBlasterGameState() const
{
	return GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
}
