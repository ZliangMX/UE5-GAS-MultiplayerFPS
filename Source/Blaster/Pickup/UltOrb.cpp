#include "UltOrb.h"

#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

AUltOrb::AUltOrb()
{
	PrimaryActorTick.bCanEverTick = true;

	// Actor 要复制（bAvailable / ChannelProgress / bIsChanneling 要下发给客户端）；
	// 但**位置不复制** —— 球是关卡静态摆放的，出生点由关卡自己同步给各端，
	// SetReplicateMovement 开着纯属浪费带宽。
	bReplicates = true;
	SetReplicateMovement(false);

	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	SetRootComponent(PickupSphere);
	// 占位半径，OnConstruction 里按 PickupRadius 重设
	PickupSphere->InitSphereRadius(60.f);
	PickupSphere->SetCollisionObjectType(ECC_WorldDynamic);
	// 响应在构造期一次设死，之后只开关 SetCollisionEnabled —— 不重复设响应，
	// 避免"清空所有通道再逐条设回"时 ECC_Pawn 瞬间变成 Ignore 而产生多余的 Begin/EndOverlap。
	PickupSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PickupSphere->SetGenerateOverlapEvents(true);
	// 碰撞只在服务器开（和 AWeapon::AreaSphere 完全一样的模式）。
	// "谁站在球里"由服务器写进角色的 OverlappingOrb，客户端不靠本地物理判断 —— 见类注释。
	PickupSphere->SetIsReplicated(false);

	OrbMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("OrbMesh"));
	OrbMesh->SetupAttachment(PickupSphere);
	// 网格纯视觉：自身无碰撞、不跑物理，碰撞全交给 PickupSphere
	OrbMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OrbMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	OrbMesh->SetGenerateOverlapEvents(false);
	OrbMesh->SetSimulatePhysics(false);
	OrbMesh->SetIsReplicated(false);

	// 默认外观：引擎自带球体（直径 100cm）。挑它当兜底是为了「拖进关卡就能看见、能调」，
	// 不用先去翻一个模型出来。换模型时在 BP Details 覆盖 OrbMeshAsset。
	OrbMeshAsset = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")));
}

void AUltOrb::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 半径/缩放是"改属性 → 派生组件设置"，用 OnConstruction 而不是构造器：
	// 这样在 BP 编辑器里拖 PickupRadius 滑条，视口里的绿球是**实时**跟手的，不用重新编译/重开关卡。
	if (PickupSphere)
	{
		PickupSphere->SetSphereRadius(FMath::Max(PickupRadius, 1.f));
	}
	if (OrbMesh)
	{
		OrbMesh->SetRelativeScale3D(FVector(FMath::Max(OrbMeshScale, 0.01f)));
	}
}

void AUltOrb::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 软引用懒加载，和 Spike 同一个理由：构造期同步加载网格资产会在编辑器启动阶段
	// 咬住资产注册，某些资产类型直接死锁。加载失败也不报错 —— 没模型只是个空碰撞球，
	// 不影响拾取逻辑，方便纯逻辑调试。
	if (UStaticMesh* Mesh = OrbMeshAsset.LoadSynchronous())
	{
		OrbMesh->SetStaticMesh(Mesh);
	}
}

void AUltOrb::BeginPlay()
{
	Super::BeginPlay();

	// 重叠判定只在服务器绑（客户端上的球是没有碰撞的装饰件）。
	// 注意这里**不发奖** —— 重叠只负责把"谁站在球里"写进角色，发奖要等按住 F 蓄满 3 秒。
	if (HasAuthority())
	{
		PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &AUltOrb::OnPickupBeginOverlap);
		PickupSphere->OnComponentEndOverlap.AddDynamic(this, &AUltOrb::OnPickupEndOverlap);
	}

	// 初始化表现。客户端不能指望 OnRep 会调 —— 球出生时服务器和客户端的 bAvailable
	// 都是构造默认值 true，值没变就不会触发 OnRep，球会漏画成"隐身但有碰撞"。
	ApplyAvailabilityVisual();
}

void AUltOrb::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// --- 视觉：自转 + 上下浮动（各端本地跑，不复制任何变换）---
	if (OrbMesh)
	{
		if (const UWorld* World = GetWorld())
		{
			// 用世界时间而不是自己累加 DeltaTime：各端算出来的相位必然一致（后进玩家/观战者
			// 看到的球位置和一直在场的玩家完全相同），也不受丢帧影响。
			const float T = World->GetTimeSeconds();
			OrbMesh->SetRelativeRotation(FRotator(0.f, T * SpinSpeed, 0.f));
			OrbMesh->SetRelativeLocation(FVector(0.f, 0.f, FMath::Sin(T * BobSpeed * 2.f * PI) * BobHeight));
		}
	}

	// --- 蓄力计时：只有服务器跑 ---
	if (!HasAuthority() || !bIsChanneling) return;

	// 蓄力者没了（死亡销毁 / 掉线）：直接打断，别让球卡在"被吃"状态
	if (!IsValid(CurrentChanneller) || CurrentChanneller->IsElimmed())
	{
		CancelChannel();
		return;
	}

	// 移动或离地打断（和拆包同一条判定，阈值也是同一个量级）
	if (CurrentChanneller->GetVelocity().Size2D() > ChannelCancelSpeed ||
		(CurrentChanneller->GetCharacterMovement() && CurrentChanneller->GetCharacterMovement()->IsFalling()))
	{
		CancelChannel();
		return;
	}

	ChannelProgress += (DeltaTime / FMath::Max(ChannelDuration, 0.01f)) * 100.f;

	if (ChannellerController)
	{
		PushChannelUI(ChannelProgress / 100.f);
	}

	if (ChannelProgress >= 100.f)
	{
		ChannelProgress = 100.f;
		CompleteChannel();
	}
}

void AUltOrb::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUltOrb, bAvailable);
	DOREPLIFETIME(AUltOrb, ChannelProgress);
	DOREPLIFETIME(AUltOrb, bIsChanneling);
}

void AUltOrb::SetAvailable(bool bNewAvailable)
{
	// 可用性是服务器权威的：非权威机的调用直接丢掉，避免客户端本地状态和服务器打架。
	// （客户端只能通过复制被动跟进，见 OnRep_bAvailable）
	if (!HasAuthority()) return;

	if (bAvailable == bNewAvailable) return;

	bAvailable = bNewAvailable;

	// 球被消耗掉了，但蓄力状态还挂着（理论上走不到：CompleteChannel 先清了蓄力者再调这里；
	// 但回合重置那一路是直接 SetAvailable(true)，如果恰好有人在吃，状态会不一致）。兜一下。
	if (!bAvailable && bIsChanneling)
	{
		CancelChannel();
	}

	// ⚠️ 权威机改自己的复制属性**不会**触发 OnRep，必须手动应用一次
	ApplyAvailabilityVisual();

	if (bAvailable)
	{
		MulticastPlayRespawnFX();
	}
}

void AUltOrb::OnRep_bAvailable()
{
	ApplyAvailabilityVisual();
}

void AUltOrb::ApplyAvailabilityVisual()
{
	if (OrbMesh)
	{
		// 隐藏时保留碰撞（false 参数 = 不要清掉碰撞），否则球隐形了还能被踩 —— 不过下面
		// 紧接着就按可用性关掉碰撞了，这里传 false 只是不想让两步互相干扰。
		OrbMesh->SetVisibility(bAvailable, false);
	}

	// 碰撞只在服务器有意义（客户端从构造起就是 NoCollision），所以这里也是服务器专属。
	if (!HasAuthority()) return;
	if (!PickupSphere) return;

	if (bAvailable)
	{
		PickupSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		// 重新开碰撞时强制重算一次重叠：
		// BeginOverlap 只会在"从没重叠变成重叠"时触发一次 —— 如果有人正站在球里，
		// 而球又刚好在这时重生，不强制重算的话他就得先走出去再走进来才能重新进范围。
		// 回合重置时所有人都在出生点，理论上碰不到，但这是零成本的兜底。
		PickupSphere->UpdateOverlaps();
	}
	else
	{
		// 关掉碰撞（而不是只在 StartChannel 里靠 bAvailable 早退）：
		// 这样球被吃掉的瞬间，站在球里的人立刻解除重叠（OnPickupEndOverlap 会清掉他的
		// OverlappingOrb），不会出现"球没了但按 F 还在吃空气"。
		PickupSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

// ---------------------------------------------------------------------------
//  重叠：只负责维护角色的 OverlappingOrb，不发奖
// ---------------------------------------------------------------------------

void AUltOrb::OnPickupBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	// 服务器专属委托，这里的 HasAuthority 是冗余保险
	if (!HasAuthority() || !bAvailable) return;

	// 只认角色。球放在地上，理论上只有 Pawn 能触发，但掉落物/投射物也可能带 Pawn 通道的响应。
	if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(OtherActor))
	{
		Character->SetOverlappingOrb(this);
	}
}

void AUltOrb::OnPickupEndOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	if (!HasAuthority()) return;

	// ⚠️ 不清蓄力状态：站着不动是吃球的正常姿势，EndOverlap 只代表他走出去了，
	// 那一刻 Tick 里的速度判定自然会把蓄力打断（人走了必然有速度）。
	// 这里只负责把"站在球里"这个引用摘掉。
	if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(OtherActor))
	{
		Character->ClearOverlappingOrb(this);
	}
}

// ---------------------------------------------------------------------------
//  蓄力
// ---------------------------------------------------------------------------

bool AUltOrb::IsChannelingBy(const ABlasterCharacter* Character) const
{
	return bIsChanneling && Character && CurrentChanneller == Character;
}

bool AUltOrb::StartChannel(ABlasterCharacter* Character)
{
	if (!HasAuthority() || !bAvailable || !Character) return false;

	// 已经在被吃（别人或自己）→ 不重入。多个人同时按 F 只有第一个生效。
	if (bIsChanneling) return false;

	// 空手蒙太奇那段不可打断：吃球也不许插进去。
	// 下面会让角色进 ECS_Defusing 并把枪收起来，而空手那边也在收枪 + 揣着自己的收尾计时器，
	// 两个状态互相顶 = 收尾顺序不确定（可能出现"球吃完了枪没掏回来"）。
	if (Character->IsEmptyHandLocked()) return false;

	ABlasterPlayerState* PS = Character->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return false;

	// 大招已攒满的人不能吃 —— Valorant 同款行为，也顺手避免了"满大的队友抢在需要的人前面
	// 把球吃掉"。返回 false（而不是静默 return），让角色那边继续尝试捡枪。
	if (PS->IsUltReady()) return false;

	bIsChanneling = true;
	ChannelProgress = 0.f;
	CurrentChanneller = Character;
	ChannellerController = Cast<ABlasterPlayerController>(Character->GetController());

	// 蓄力时不能持枪：把手上的枪收回挂点、角色变空手，并进入 ECS_Defusing 阻止开火/换弹/切枪
	// （和拆包共用这个状态 —— 项目里没有专门的"吃球"动画，复用它至少能保证姿势是"空手双手前伸"
	// 而不是落到 AnimBP 没处理的枚举值上。以后做了专属动画再考虑拆一个新 state）
	Character->SetCombatState(ECombatState::ECS_Defusing);
	if (UCombatComponent* OrbCombat = Character->GetCombatComponent())
	{
		OrbCombat->HolsterEquippedWeapon();
	}

	// 服务器上记一份"我正在吃哪个球"，供角色按 1/2 掏枪时找到它来打断（客户端不需要，走 RPC）
	Character->SetChannelingOrb(this);

	// 立刻推一次 0%，让进度条在按下的同一帧就出现（不然要等 1/60 秒后的第一次 Tick）
	PushChannelUI(0.f);

	return true;
}

void AUltOrb::CancelChannel(bool bRestoreWeapon)
{
	if (!HasAuthority()) return;
	if (!bIsChanneling) return;

	ABlasterCharacter* Channeller = CurrentChanneller;

	// 隐藏进度条 —— ⚠️ 必须在清 ChannellerController **之前**调，PushChannelUI 就靠它找 HUD。
	if (ChannellerController)
	{
		PushChannelUI(-1.f);
	}

	// 一段式：打断就是清零重来，没有中途保存点。
	// ⚠️ 别把 Spike::ServerCancelDefuse 那套 SegmentPercent（拆过 50% 保留在 50）抄过来 ——
	// 拆包是两段式，大招球不是。
	bIsChanneling = false;
	ChannelProgress = 0.f;
	CurrentChanneller = nullptr;
	ChannellerController = nullptr;

	if (Channeller)
	{
		Channeller->SetChannelingOrb(nullptr);
		Channeller->SetCombatState(ECombatState::ECS_Unoccupied);

		// 自动掏回武器（主武器优先）。按 1/2 主动掏枪时调用方传 false —— 它紧接着自己会切。
		if (bRestoreWeapon)
		{
			if (UCombatComponent* OrbCombat = Channeller->GetCombatComponent())
			{
				OrbCombat->EquipBestOwnedWeapon();
			}
		}
	}
}

void AUltOrb::CompleteChannel()
{
	if (!HasAuthority() || !bIsChanneling) return;

	ABlasterCharacter* Channeller = CurrentChanneller;

	// 先清蓄力状态再消耗球 —— 否则 SetAvailable(false) 里的"球没了但还在蓄力"兜底分支
	// 会把这次正常完成当成异常打断处理，白跑一遍 CancelChannel。
	bIsChanneling = false;
	ChannelProgress = 0.f;
	CurrentChanneller = nullptr;
	ChannellerController = nullptr;

	if (Channeller)
	{
		Channeller->SetChannelingOrb(nullptr);
		Channeller->SetCombatState(ECombatState::ECS_Unoccupied);

		// 大招点：只有在服务器这条分支上加（不能写进 MulticastPlayPickupFX —— 那是各端都跑的）
		if (ABlasterPlayerState* PS = Channeller->GetPlayerState<ABlasterPlayerState>())
		{
			PS->AddUltPoints(UltPointValue);
		}

		// 吃完自动掏回武器
		if (UCombatComponent* OrbCombat = Channeller->GetCombatComponent())
		{
			OrbCombat->EquipBestOwnedWeapon();
		}

		// 隐藏进度条
		if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(Channeller->GetController()))
		{
			PC->ClientUpdateSpikeProgress(TEXT(""), -1.f);
		}
	}

	// 消耗球（内部会隐藏 + 关碰撞 + 清掉站在球里那个人的 OverlappingOrb）
	SetAvailable(false);

	MulticastPlayPickupFX();
}

void AUltOrb::PushChannelUI(float Progress)
{
	if (!ChannellerController) return;

	// 复用 HUD 上拆包/安包那一条进度条（CharacterOverlay 的 SpikeStatusText + SpikeTimerBar），
	// 只换状态文字。省掉一整套新控件，而且玩家已经认识"底部那条进度条 = 我在做长按动作"。
	// 进度是 0-1 的比例（和 Spike 的 ClientUpdateSpikeProgress 约定一致）；负数 = 隐藏。
	ChannellerController->ClientUpdateSpikeProgress(
		Progress >= 0.f ? TEXT("Taking Orb") : TEXT(""),
		Progress);
}

// ---------------------------------------------------------------------------
//  FX
// ---------------------------------------------------------------------------

void AUltOrb::MulticastPlayPickupFX_Implementation()
{
	if (PickupSound)
	{
		// 3D 空间音：从球的位置发，能听出方位。用默认衰减即可，需要更远的可听距离再说。
		UGameplayStatics::PlaySoundAtLocation(this, PickupSound, GetActorLocation());
	}

	if (PickupEffect)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, PickupEffect, GetActorLocation());
	}
}

void AUltOrb::MulticastPlayRespawnFX_Implementation()
{
	if (RespawnSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, RespawnSound, GetActorLocation());
	}
}
