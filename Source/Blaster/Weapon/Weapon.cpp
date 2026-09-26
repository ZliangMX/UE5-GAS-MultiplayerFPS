// Fill out your copyright notice in the Description page of Project Settings.


#include "Weapon.h"
#include "Components/SphereComponent.h"
#include "Components/WidgetComponent.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Net/UnrealNetwork.h"
#include "Animation/AnimationAsset.h"
// 播动画用：蒙太奇走槽位（AnimMontage + AnimInstance），序列走单节点（AnimCompositeBase 里
// 有 UAnimSequenceBase 和 FAnimTrack / FAnimSegment 的完整定义，GetAnimReference() 在那儿）
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Casing.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"	// ComputeThisKillIndex 要读 GetRoundKills
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Texture2D.h"

namespace
{
	/*
	 * 准心贴图默认值用的加载器。
	 * ConstructorHelpers 只能在构造期用；包一层省得 5 张贴图写 5 遍样板。
	 * 每个武器类的 CDO 只构造一次，这点查找开销可以忽略。
	 */
	UTexture2D* DefaultCrosshairTexture(const TCHAR* Path)
	{
		ConstructorHelpers::FObjectFinder<UTexture2D> Finder(Path);
		return Finder.Succeeded() ? Finder.Object : nullptr;
	}

	/*
	 * 把一个"静态网格装饰"（弹匣、瞄准镜）挂到武器网格的某根 socket 上，
	 * 并把缩放/微调按**世界口径**换算好写进去。两边的坑一模一样，所以共用这一份：
	 *
	 *   · 挂点的世界缩放里含着枪骨架根骨骼那 100 倍（瓦的米制曲线 → 厘米）。
	 *     位置/旋转刚才已经吸到 socket 上了（相对为 0），剩下两项都得拿这个缩放除一下：
	 *       目标世界缩放 = WorldScale（默认 1 = 静态网格资产原大小，其局部数据就是厘米）
	 *       → 那 100 倍在相对值里被 /100 抵消掉
	 *       微调按"世界厘米"填，同样除回去才能在 ×100 的骨骼空间里挪对距离
	 *   · 组件详情里的 Relative Scale/Location 会被这里覆盖 —— 要调大小/位置请填武器上的属性，
	 *     别去改组件，也别去改静态网格资产的轴心。
	 */
	void AttachDecorationToSocket(
		UStaticMeshComponent* Decoration,
		USkeletalMeshComponent* ParentMesh,
		FName SocketName,
		float WorldScale,
		const FVector& LocationOffset,
		const FRotator& RotationOffset)
	{
		if (Decoration == nullptr || ParentMesh == nullptr) return;

		// 已经挂在目标网格的目标骨骼上就别重挂：OnConstruction 每次改属性都会跑一遍，
		// 无条件重挂会把蓝图反复标脏、也没必要。挂点变了（换枪 / 第一人称副本显隐）才重挂。
		if (Decoration->GetAttachParent() != ParentMesh
			|| Decoration->GetAttachSocketName() != SocketName)
		{
			// SnapToTargetNotIncludingScale = 位置/旋转吸附到 socket、缩放先按"保持世界缩放"算（下面再改成目标值）
			Decoration->AttachToComponent(
				ParentMesh,
				FAttachmentTransformRules::SnapToTargetNotIncludingScale,
				SocketName);
		}

		const FVector ParentScale = ParentMesh->GetSocketTransform(SocketName, RTS_World).GetScale3D();

		auto DivideByParentScale = [](float Value, float Scale)
		{
			return FMath::IsNearlyZero(Scale) ? Value : Value / Scale;
		};

		Decoration->SetRelativeLocation(FVector(
			DivideByParentScale(LocationOffset.X, ParentScale.X),
			DivideByParentScale(LocationOffset.Y, ParentScale.Y),
			DivideByParentScale(LocationOffset.Z, ParentScale.Z)));
		Decoration->SetRelativeRotation(RotationOffset.Quaternion());
		Decoration->SetRelativeScale3D(FVector(
			DivideByParentScale(WorldScale, ParentScale.X),
			DivideByParentScale(WorldScale, ParentScale.Y),
			DivideByParentScale(WorldScale, ParentScale.Z)));
	}
}

// Sets default values
AWeapon::AWeapon()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);

	WeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
	SetRootComponent(WeaponMesh);

	WeaponMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Ignore);
	WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
	WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	WeaponMesh->SetCustomDepthStencilValue(CUSTOM_DEPTH_BLUE);
	WeaponMesh->MarkRenderStateDirty();
	EnableCustomDepth(true);
	
	AreaSphere = CreateDefaultSubobject<USphereComponent>(TEXT("AreaSphere"));
	AreaSphere->SetupAttachment(RootComponent);
	AreaSphere->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	AreaSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PickupWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("PickupWidget"));
	PickupWidget->SetupAttachment(RootComponent);
	/*
	 *拾取提示牌（World 空间的 WidgetComponent）。
	 *
	 *引擎给 World 空间的 widget 自动造一块「按绘制尺寸的平板碰撞体」（UMG/WidgetComponent.cpp
	 *UpdateBodySetup），默认预设 "UI" → Visibility = Block。而 SetVisibility(false) 只关渲染、
	 ***不关碰撞**：一把掉在地上的枪，牌子是不显示的，那块看不见的板子照样挡射线。
	 *枪本身已经进了「忽略整棵挂载树」的白名单，但它作为独立掉落物躺在地上时就没人替它忽略了。
	 */
	PickupWidget->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PickupWidget->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);

	/*
	 * 弹匣（静态网格）。
	 *
	 * 这里只创建 + 挂到武器网格上（先挂在根上占位），**具体挂哪根骨骼**由
	 * UpdateMagazineAttachment() 在两个时机去挂：
	 *   · OnConstruction —— 编辑器里改 MagazineMesh / MagazineSocket 立刻能在 BP 预览里看见
	 *   · PostInitializeComponents —— 运行时（蓝图子类覆盖过的值到这时才是最终值）
	 * 不在构造函数里挂的原因：构造期只读得到 CDO 的值，蓝图子类填的骨骼名那时候还没生效。
	 *
	 * 不碰撞、不投影：弹匣纯装饰，命中判定和它无关。
	 */
	MagazineMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MagazineMesh"));
	MagazineMeshComp->SetupAttachment(WeaponMesh);
	MagazineMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MagazineMeshComp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	MagazineMeshComp->SetGenerateOverlapEvents(false);
	MagazineMeshComp->bCastDynamicShadow = false;
	MagazineMeshComp->CastShadow = false;
	MagazineMeshComp->SetVisibility(false);

	/*
	 * 备用弹匣（常驻别在枪身上那块，静态网格）。和上面那块完全同套路：
	 * 这里只建组件 + 挂着占位，具体挂哪根骨骼由 UpdateMagazineExtraAttachment() 去挂。
	 * 默认不显示 —— 没填 MagazineExtraMesh 就是空的（只有枪模上真有备弹位的枪才需要填）。
	 */
	MagazineExtraMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MagazineExtra"));
	MagazineExtraMeshComp->SetupAttachment(WeaponMesh);
	MagazineExtraMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MagazineExtraMeshComp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	MagazineExtraMeshComp->SetGenerateOverlapEvents(false);
	MagazineExtraMeshComp->bCastDynamicShadow = false;
	MagazineExtraMeshComp->CastShadow = false;
	MagazineExtraMeshComp->SetVisibility(false);

	/*
	 * 瞄准镜（静态网格）。
	 *
	 * 和弹匣完全同一套路：这里只建组件 + 挂着占位（先挂根上），**具体挂哪根骨骼**由
	 * UpdateScopeAttachment() 在 OnConstruction / PostInitializeComponents 两个时机去挂 ——
	 * 构造期只读得到 CDO 的值，蓝图子类填的 ScopeSocket 那时候还没生效。
	 * 默认挂 ReflexSocket。
	 *
	 * 不碰撞、不投影：瞄准镜是纯装饰，命中判定和它无关（弹道走的是真枪枪口 socket）。
	 */
	ScopeMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ScopeMesh"));
	ScopeMeshComp->SetupAttachment(WeaponMesh);
	ScopeMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ScopeMeshComp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	ScopeMeshComp->SetGenerateOverlapEvents(false);
	ScopeMeshComp->bCastDynamicShadow = false;
	ScopeMeshComp->CastShadow = false;
	ScopeMeshComp->SetVisibility(false);

	/*
	 * 准心贴图默认值。
	 *
	 * 准心本来是**每把武器 BP 各自填**的（霰弹枪用的就是另一套），基类原来留 nullptr。
	 * 问题在于漏填是**静默**的：HUD 那边 `if (HUDPackage.CrosshairCenter)` 逐层判空，
	 * 5 张全是 nullptr 就一张都不画 —— 表现是"掏出这把武器后准心整个消失"，
	 * 而且没有任何日志。从 C++ 类新建的武器 BP（Jett 飞刀）就是这么中招的。
	 *
	 * 给一套默认，漏填至少长成"和步枪一样"；BP 里照样能覆盖成别的。
	 */
	CrosshairCenter = DefaultCrosshairTexture(TEXT("/Game/Assets/Textures/Crosshairs/Primary/Crosshair_Center.Crosshair_Center"));
	CrosshairLeft   = DefaultCrosshairTexture(TEXT("/Game/Assets/Textures/Crosshairs/Primary/Crosshair_Left.Crosshair_Left"));
	CrosshairRight  = DefaultCrosshairTexture(TEXT("/Game/Assets/Textures/Crosshairs/Primary/Crosshair_Right.Crosshair_Right"));
	CrosshairTop    = DefaultCrosshairTexture(TEXT("/Game/Assets/Textures/Crosshairs/Primary/Crosshair_Top.Crosshair_Top"));
	CrosshairBottom = DefaultCrosshairTexture(TEXT("/Game/Assets/Textures/Crosshairs/Primary/Crosshair_Bottom.Crosshair_Bottom"));
}

bool AWeapon::GetMuzzleLocation(FVector& OutLocation) const
{
	// ⚠ 这里用 DoesSocketExist + GetSocketTransform，**不能**用 GetSocketByName（原来就是这么写的）：
	//   · DoesSocketExist / GetSocketTransform 两样都认 —— 真 socket 返回 socket 变换，
	//     名字其实是**骨骼**时退到骨骼变换（引擎源码 USkinnedMeshComponent::GetSocketTransform）；
	//   · GetSocketByName 只查 socket 表、查不到骨骼，骨骼名一律返回 nullptr。
	//   实测：BP_Shotgun 的 Slim_Mesh 上只有一个 Muzzle **骨骼**、没有 MuzzleFlash socket ——
	//   用 GetSocketByName 的话霰弹枪的枪口火光和弹道轨迹都会"找不到枪口"（轨迹会退回眼睛起点）。
	if (WeaponMesh == nullptr || !WeaponMesh->DoesSocketExist(MuzzleFlashSocket)) return false;

	OutLocation = WeaponMesh->GetSocketTransform(MuzzleFlashSocket).GetLocation();
	return true;
}

bool AWeapon::GetViewMuzzleTransform(FTransform& OutTransform) const
{
	// 和 PlayFireEffects 取的是同一个 socket、同一个网格 —— 那边的写法原来内联在这里，
	// 抽出来是因为弹道轨迹也要它（轨迹必须从"看得见的枪口"出发，见 AHitScanWeapon::SpawnTracerFX）。
	// socket/骨骼的取舍同 GetMuzzleLocation。
	USkeletalMeshComponent* ViewMesh = GetViewMesh();
	if (ViewMesh == nullptr || !ViewMesh->DoesSocketExist(MuzzleFlashSocket)) return false;

	OutTransform = ViewMesh->GetSocketTransform(MuzzleFlashSocket);
	return true;
}

void AWeapon::EnableCustomDepth(bool bEnable)
{
	if (WeaponMesh)
	{
		WeaponMesh->SetRenderCustomDepth(bEnable);
	}
}

void AWeapon::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		AreaSphere->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		AreaSphere->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Overlap);
		AreaSphere->OnComponentBeginOverlap.AddDynamic(this, &AWeapon::OnSphereOverlap);
		AreaSphere->OnComponentEndOverlap.AddDynamic(this, &AWeapon::OnSphereEndOverlap);
	}
	if (PickupWidget)
	{
		PickupWidget->SetVisibility(false);
	}
}

void AWeapon::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AWeapon::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AWeapon, WeaponState);
	DOREPLIFETIME(AWeapon,Ammo);
}

void AWeapon::SetHUDAmmo()
{
	BlasterOwnerCharacter = BlasterOwnerCharacter==nullptr? Cast<ABlasterCharacter>(GetOwner()) : BlasterOwnerCharacter;
	if (BlasterOwnerCharacter)
	{
		BlasterOwnerController = BlasterOwnerController==nullptr? Cast<ABlasterPlayerController>(BlasterOwnerCharacter->Controller) : BlasterOwnerController;
		if (BlasterOwnerController)
		{
			BlasterOwnerController->SetHUDWeaponAmmo(Ammo);
		}
	}
}

void AWeapon::OnRep_Owner()
{
	Super::OnRep_Owner();
	if (!Owner)
	{
		BlasterOwnerCharacter = nullptr;
		BlasterOwnerController = nullptr;
	}
	else
	{
		SetHUDAmmo();
	}
	
}

void AWeapon::OnSphereOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(OtherActor);
	if (BlasterCharacter)
	{
		BlasterCharacter->SetOverlappingWeapon(this);

	}
}

void AWeapon::OnSphereEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(OtherActor);
	if (BlasterCharacter)
	{
		BlasterCharacter->SetOverlappingWeapon(nullptr);
	}
}

void AWeapon::OnRep_Ammo()
{
	BlasterOwnerCharacter = BlasterOwnerCharacter==nullptr? Cast<ABlasterCharacter>(GetOwner()) : BlasterOwnerCharacter;
	if (BlasterOwnerCharacter && BlasterOwnerCharacter->GetCombatComponent() && IsFull())
	{
		BlasterOwnerCharacter->GetCombatComponent()->JumpToShotGunEnd();
	}
	SetHUDAmmo();
}

void AWeapon::SpendRound()
{
	Ammo = FMath::Clamp(Ammo - 1, 0,MagCapacity);
	SetHUDAmmo();
}

void AWeapon::SetWeaponVisible(bool bVisible)
{
	// 藏的是**整个 actor**，不是只藏 WeaponMesh 那一个组件。
	//
	// 引擎里 USceneComponent::ShouldRender() 的条件是
	//   IsVisible() && (!Owner || !Owner->IsHidden())      （SceneComponent.cpp:3194）
	// 也就是"组件自己可见" **且** "它所属的 actor 没被隐藏"。所以 SetActorHiddenInGame
	// 一次就把这个 actor 的**所有**组件都盖住，不用管它们挂在谁下面。
	//
	// 为什么不能用 WeaponMesh->SetVisibility(false, true)（传播给子组件）：
	// 弹匣/瞄准镜/备用弹匣（MagazineMeshComp / ScopeMeshComp / MagazineExtraMeshComp）
	// 虽然也是这个 actor 的组件，
	// 但 UpdateAttachedMeshes 会按"本人看到的是真枪还是第一人称副本"把它们**临时挂到副本
	// 那个网格下面**（父级变成了角色的组件）。父级一换，传播式隐藏就够不着它们了
	// —— 表现是"收枪后枪身没了、弹匣还挂在背上"。走 actor 隐藏则跟父级无关。
	//
	// 顺带覆盖：拾取提示框（PickupWidget）也在这次隐藏里，不用单独处理。
	// 碰撞不受影响（SetActorHiddenInGame 只管渲染），AreaSphere 的开关照旧由状态机管。
	//
	// 刻意**不碰** SetOwnerNoSee：那是"第一人称要不要用副本"的开关，由角色的
	// UpdateFPWeaponMesh 按自己那份状态决定。这里动它会造成「OnRep_WeaponState 和
	// OnRep_EquipWeapon 到达顺序不同 → 第一人称里真枪和副本同时显示（两把枪）」这种
	// 跟着网络抖动出现的偶发问题。收枪时也不需要它：隐藏对本人和其它人一样有效。
	SetActorHiddenInGame(!bVisible);
}

void AWeapon::SetWeaponState(EWeaponState State)
{
	WeaponState = State;
	switch (WeaponState)
	{
	case EWeaponState::EWS_Equipped:
	case EWeaponState::EWS_Holstered:
		// 两种状态只在**渲染**上分家，物理/碰撞/拾取提示完全一样，所以合并写。
		ShowPickupWidget(false);
		AreaSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WeaponMesh->SetSimulatePhysics(false);
		WeaponMesh->SetEnableGravity(false);
		WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		EnableCustomDepth(false);

		SetWeaponVisible(WeaponState == EWeaponState::EWS_Equipped);
		break;
	case EWeaponState::EWS_Dropped:
		if (HasAuthority())
		{
			AreaSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		}
		WeaponMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
		WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Ignore);
		WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
		WeaponMesh->SetSimulatePhysics(true);
		WeaponMesh->SetEnableGravity(true);
		WeaponMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

		WeaponMesh->SetCustomDepthStencilValue(CUSTOM_DEPTH_BLUE);
		WeaponMesh->MarkRenderStateDirty();
		EnableCustomDepth(true);
		// ★ 掉在地上必须还原可见：上面 EWS_Holstered 把它藏了，
		//   不还原的话"收起来的枪被丢掉"会在地上躺着一把看不见的枪。
		SetWeaponVisible(true);
		break;
	}
}

bool AWeapon::IsEmpty()
{
	return Ammo <= 0;
}

bool AWeapon::IsFull()
{
	return Ammo == MagCapacity;
}

void AWeapon::OnRep_WeaponState()
{
	switch (WeaponState)
	{
	case EWeaponState::EWS_Equipped:
	case EWeaponState::EWS_Holstered:
		ShowPickupWidget(false);
		WeaponMesh->SetSimulatePhysics(false);
		WeaponMesh->SetEnableGravity(false);
		WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		EnableCustomDepth(false);

		// 收起来的枪不渲染 —— 服务器那份在 SetWeaponState 里已经藏了，
		// 这里是**其他客户端**（以及本人，武器状态是复制属性）那几份。
		// 靠 WeaponState 这个 ReplicatedUsing 走，所以不用为"切枪"再发一条 RPC：
		// 武器状态本身已经复制到每一台机器了。
		SetWeaponVisible(WeaponState == EWeaponState::EWS_Equipped);
		break;
	case EWeaponState::EWS_Dropped:
		WeaponMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
		WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Ignore);
		WeaponMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
		WeaponMesh->SetSimulatePhysics(true);
		WeaponMesh->SetEnableGravity(true);
		WeaponMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

		WeaponMesh->SetCustomDepthStencilValue(CUSTOM_DEPTH_BLUE);
		WeaponMesh->MarkRenderStateDirty();
		EnableCustomDepth(true);
		SetWeaponVisible(true);
		break;
	}
}

void AWeapon::ShowPickupWidget(bool bShowWidget)
{
	if (PickupWidget)
	{
		PickupWidget->SetVisibility(bShowWidget);
	}
}

bool AWeapon::IsHeadshot(const FHitResult& Hit) const
{
	return HeadBoneName != NAME_None && Hit.BoneName == HeadBoneName;
}

float AWeapon::GetDamageForHit(float BaseDamage, const FHitResult& Hit) const
{
	return IsHeadshot(Hit) ? BaseDamage * HeadshotMultiplier : BaseDamage;
}

void AWeapon::PlayFireEffects()
{
	// 枪口火光（MuzzleFlash socket）+ 枪声。投影武器（步枪）此前只有投射物，
	// 没火光没声音；移到基类后所有武器开火都有统一反馈。
	//
	// socket 取**本人实际看到的那个网格**（GetViewMesh）：开了第一人称副本时真枪对本人是隐藏的，
	// 还按真枪算的话火光会飘在"空中的第三人称手"那个位置，看着像枪在空中开火。
	FTransform MuzzleTransform;
	if (MuzzleFlash && GetViewMuzzleTransform(MuzzleTransform))
	{
		/*
		 * 只取位置和朝向，**不把 socket 的缩放带出去**（所以用 Location/Rotation 那一版重载，
		 * 而不是直接把 FTransform 丢进去）。
		 *
		 * 枪口 socket 的缩放是绑定/挂点那套的产物，不是"这把特效该多大"：手模那条链上
		 * 骨头带 100 倍、socket 自己带 0.01（见 UpdateFPWeaponMesh 的注释），武器网格上
		 * 也可能是别的数。原样传给粒子的话，特效会被悄悄放大或缩小 100 倍 ——
		 * 大到手电筒一样亮瞎眼，或者小到一个像素看不见，而代码里一点线索都没有。
		 */
		UGameplayStatics::SpawnEmitterAtLocation(
			GetWorld(),
			MuzzleFlash,
			MuzzleTransform.GetLocation(),
			MuzzleTransform.Rotator()
		);
	}
	if (FireSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, FireSound, GetActorLocation());
	}
}

void AWeapon::Fire(const FVector& HitTarget)
{
	// 枪自己那条开火动画（FireAnimation，各武器蓝图自己挂）。自动识别序列/蒙太奇，
	// 并让本机那份第一人称副本同步播同一条。
	PlayFireAnimation();

	// 上面那条是**武器网格**（所有人机器上都看得到枪在动）；
	// 这一条是**射手本人的手模**（FPFireMontage，各武器蓝图自己挂）。
	//
	// 放这儿不会重复播：CombatComponent 的 MulticastFire 在射手自己的机器上是直接 return 的
	// （本地预测的 Fire() 已经播过一遍），所以每台机器最多走到一次。
	//
	// 例外：AJettKnives 的"全部投掷"分支故意绕开了 Super::Fire（它要连续甩好几把，
	// 走基类会每把都重播一次枪口动画），那条路径下这条蒙太奇也不会播。
	PlayFPFireMontage();

	PlayFireEffects();
	if (CasingClass)
	{
		// 抛壳口同理取本人看到的那个网格（第一人称副本），不然弹壳会从看不见的真枪那儿飞出来
		USkeletalMeshComponent* EjectMesh = GetViewMesh();
		const USkeletalMeshSocket* EjectSocket = EjectMesh ? EjectMesh->GetSocketByName(AmmoEjectSocket) : nullptr;
		if (EjectSocket)
		{
			FTransform SocketTransform = EjectSocket->GetSocketTransform(EjectMesh);

			UWorld* World = GetWorld();
			if (World)
			{
				World->SpawnActor<ACasing>(
					CasingClass,
					SocketTransform.GetLocation(),
					SocketTransform.GetRotation().Rotator()
				);
			}

		}
		
	}
	SpendRound();
}

void AWeapon::PlayFPArmsMontage(UAnimSequenceBase* Asset)
{
	if (Asset == nullptr) return;

	// 角色从 GetOwner() 拿：UCombatComponent::EquipSlotWeapon 里 SetOwner(Character) 保证它在
	//（AActor::Owner 本身也走复制），武器丢在地上时 Dropped() 会清掉 → 这里自然什么都不做。
	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	// 该不该播（是不是本地控制的那台机器、手模在不在）由角色那边判断，武器不管这些。
	// 第二个参数只在 Asset 是**裸序列**时用得上（要拿它的槽名去造动态蒙太奇）：
	// 从 FPFireMontage 上取 —— 那一条是已经在手模动画图上验证过能播的，它的槽名就是对的。
	OwnerCharacter->PlayFPArmsMontage(Asset, FPFireMontage);
}

// 下面四个是给各游戏逻辑用的入口：蒙太奇留空 = 那条动作不播手模动画（别处照常）。
void AWeapon::PlayFPFireMontage()    { PlayFPArmsMontage(FPFireMontage); }

void AWeapon::JumpFPFireMontageToSection(FName Section)
{
	// 没配蒙太奇 / 段名是 None → 没什么可跳的（段名 None 会让 Montage_JumpToSection 找不到段，
	// 表现是"什么都不发生"，和现在直接返回一个效果，不如早点走）
	if (FPFireMontage == nullptr || Section.IsNone()) return;

	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	// 手模只在本机渲染 —— 和 PlayFPArmsMontage 里那条判断是同一个理由
	//（远端机器上 FPArmsMesh 连组件 tick 都是关的，动画根本不跑）
	if (!OwnerCharacter->IsLocallyControlled()) return;

	USkeletalMeshComponent* FPArms = OwnerCharacter->GetFPArmsMesh();
	if (FPArms == nullptr) return;

	UAnimInstance* AnimInstance = FPArms->GetAnimInstance();
	if (AnimInstance == nullptr) return;

	// 一定要带上第二个参数：不带的话 Montage_JumpToSection 跳的是**这个网格上当前活跃的那条**，
	// 而我们只该动自己这条。
	//
	// IsPlaying 这个判断也是必须的：调用点是 Fire() 里的 PlayFPFireMontage() 之后，
	// 那条蒙太奇要是长度只有一两帧、或者被别的动作顶掉了，这时候跳段会失败并刷一条警告。
	if (AnimInstance->Montage_IsPlaying(FPFireMontage))
	{
		// 段名对不上是**静默失败**（Montage_JumpToSection 找不到段就什么都不做，然后从头播/继续播
		// 当前段），表现成"五段动作永远只出第一段"，很难查。所以这里先自己查一遍再跳：
		// 段名对了照常跳，不对就留一条 warning 说明是哪个资产缺哪个段。
		if (FPFireMontage->GetSectionIndex(Section) == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[手模] %s 上找不到段 \"%s\"（这条蒙太奇一共 %d 段）→ 这次跳段不生效，")
				TEXT("手模动作会一直停在第 1 段。检查这一段的段名是不是数字（\"1\"..\"5\"）。"),
				*FPFireMontage->GetName(), *Section.ToString(), FPFireMontage->CompositeSections.Num());
			return;
		}

		AnimInstance->Montage_JumpToSection(Section, FPFireMontage);
	}
}

void AWeapon::JumpThirdPersonFireMontageToSection(FName Section)
{
	if (ThirdPersonFireMontage == nullptr || Section.IsNone()) return;

	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	USkeletalMeshComponent* BodyMesh = OwnerCharacter->GetMesh();
	if (BodyMesh == nullptr) return;

	UAnimInstance* AnimInstance = BodyMesh->GetAnimInstance();
	if (AnimInstance == nullptr) return;

	// 只动**自己这条**：JumpToSection 不带第二个参数时跳的是"这个网格上当前活跃的那条"，
	// 我们没资格动别人的动作。这条判断还有个附带作用 —— 调用点在 Fire() 之后，
	// 理论上那条蒙太奇可能已经播完了，那时候跳段没有意义（还会刷一条含糊的警告）。
	if (!AnimInstance->Montage_IsPlaying(ThirdPersonFireMontage)) return;

	// 段名对不上是**静默失败**（找不到段就什么都不做，然后继续播当前段），表现成
	// "五段动作永远只出第一段"或者"第 3、4、5 次扔刀看起来和第 1 次一样"，很难查。
	// 所以这里先自己查一遍：段名在就跳，不在就留一条 warning 说清是哪个资产缺哪个段。
	if (ThirdPersonFireMontage->GetSectionIndex(Section) == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[第三人称] %s 上找不到段 \"%s\"（这条蒙太奇一共 %d 段）→ 这次跳段不生效，")
			TEXT("身体会继续播当前那一段。检查一下段名是不是数字（\"1\"..\"5\"）。"),
			*ThirdPersonFireMontage->GetName(), *Section.ToString(), ThirdPersonFireMontage->CompositeSections.Num());
		return;
	}

	AnimInstance->Montage_JumpToSection(Section, ThirdPersonFireMontage);
}

void AWeapon::PlayFPEquipMontage()   { PlayFPArmsMontage(FPEquipMontage); }
void AWeapon::PlayFPReloadMontage()
{
	PlayFPArmsMontage(FPReloadMontage);
}
void AWeapon::PlayFPInspectMontage() { PlayFPArmsMontage(FPInspectMontage); }

/*
 * 右键动作那两条。**留空 = 退回老行为**，所以两个字段都可以先空着、一条一条补。
 *
 * 两条的"老行为"不一样，各自写在下面了；共同点是：右键那条路（AJettKnives::ThrowAllKnives）
 * 不经过 AWeapon::Fire，所以开火那两条蒙太奇都不会自动播，得这里单独来一次。
 */
void AWeapon::PlayFPRightClickMontage()
{
	if (FPRightClickMontage == nullptr)
	{
		// 老行为：把开火那条手模蒙太奇从头播一遍（五段里就是第 1 段）。
		// 留空时手模就靠它顶着，不会变成"按了右键手不动"。
		PlayFPFireMontage();
		return;
	}

	PlayFPArmsMontage(FPRightClickMontage);
}

void AWeapon::PlayThirdPersonRightClickMontage()
{
	// 没配就是**什么都不做**：留空 = 这一次右键身体不出手，不是"少了本来有的一条"。
	//
	// ⚠ 飞刀那条路上，这一句之前身体**已经**动过了 —— UCombatComponent::Fire 先按
	//   ShouldPlayBodyFireMontage()（AJettKnives 现在返回"配了 ThirdPersonFireMontage 没有"）
	//   播了一遍通用开火蒙太奇。这里配了的话是同一帧把它顶掉（PlayAnimAssetOnInstance 里
	//   bStopAllMontages=true）；不配的话，那一条就留着播完 —— 也就是"右键出了一次单扔的动作"。
	//   想把全扔完全交给通用那条、自己不动手，就这样留空即可。
	if (ThirdPersonRightClickMontage == nullptr) return;

	ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return;

	// 槽名（Asset 是裸序列时要用）从 ThirdPersonFireMontage 上取 —— 那是一条身体蒙太奇，
	// 它的槽名必然是身体动画图里那个 Slot 节点的名字。取不到就用 "UpperBody" 兜底。
	OwnerCharacter->PlayThirdPersonMontage(ThirdPersonRightClickMontage, ThirdPersonFireMontage);
}

/**
 * 一条动画资产的**实际播完时长**（秒）。
 *
 * ⚠ 为什么不能直接用 `GetPlayLength()`：它返回的是资产里的原始 `SequenceLength`，
 *   **不含 `RateScale`**（`AnimSequenceBase.cpp` 里就是原样 return）。而播放时速率是
 *   `Sequence_PlayRate * RateScale`（裸序列，`AnimSequenceBase.cpp` 的 TickAssetPlayer）
 *   或 `InPlayRate * Montage->RateScale`（蒙太奇，`AnimInstance.cpp` 的 Montage_PlayInternal）
 *   —— 所以「资产上写着 8.79 秒」的蒙太奇配上倍率 2.0，实际 4.40 秒就播完了。
 *
 * 换弹/掏枪的状态时长必须按**实际**时长算：这两段由**服务器那条计时器独家收尾**
 *（见 GetReloadDuration / GetEquipDuration 的调用方；蒙太奇上原本挂着的结束通知已在
 *  2026-09-26 全部摘掉，理由见 CombatComponent::StartEquipTimer 的注释）。
 * 用原始长度会让状态和动画对不上 —— 短了是「动画还没播完就能开枪」，长了是
 *「动画早就播完了、还站着不能开枪」。
 */
static float GetEffectivePlayLength(const UAnimSequenceBase* Asset)
{
	if (Asset == nullptr) return 0.f;

	// RateScale 在编辑器里有 ClampMin=0.0001，但蓝图 CDO 能绕过 —— 兜一下，别除出 inf
	const float RateScale = FMath::Max(Asset->RateScale, 0.0001f);
	return Asset->GetPlayLength() / RateScale;
}

float AWeapon::GetReloadDuration() const
{
	// 显式配了就用配的（想做「比动画快一点」的手感就填这个）
	if (ReloadTime > 0.f) return ReloadTime;

	// 没配就按第一人称手模换弹蒙太奇的**实际**时长 —— 那是玩家换弹时**真正看到**的那条动画，
	// 用它的时长最贴合手感（第三人称那条现在绑在别的骨架上、播不出来，也不该拿来定时长）。
	// 注意这里读的是蒙太奇资产自己的长度，跟它能不能在当前骨架上播没有关系。
	if (FPReloadMontage) return FMath::Max(GetEffectivePlayLength(FPReloadMontage), 0.1f);

	// 两条都没有：给个能用的默认值。**绝不能返回 0** —— 那会让计时器立刻到点、
	// 换弹瞬间完成，看起来像"没换弹"。
	return 2.f;
}

float AWeapon::GetEquipDuration() const
{
	// 显式配了就用配的
	if (EquipTime > 0.f) return EquipTime;

	// 没配就按第一人称掏枪蒙太奇的**实际**时长 —— 和换弹那边同一个取舍（玩家真正看到的那条）。
	// 这条计时器是**唯一**判据（动画通知那条路 2026-09-26 已摘掉），所以填多少就是多少，
	// 不再有"谁先到谁收尾"的竞争。
	if (FPEquipMontage) return FMath::Max(GetEffectivePlayLength(FPEquipMontage), 0.1f);

	// 都没有：兜底。同样**绝不能返回 0**（计到点立刻收尾 = 掏枪零耗时，切枪变成瞬掏）。
	return 0.6f;
}

float AWeapon::GetAttackPresentationRemainingTime() const
{
	const ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
	if (OwnerCharacter == nullptr) return 0.f;

	float Remaining = 0.f;

	/*
	 * 一条通道的量法：先问本机这个动画实例"还剩多久"；问不出来时，看要不要退回资产全长。
	 *
	 * bFallbackToFullLength 只给**手模**那两条用，而且只在"本机不是这个角色的控制端"时才给
	 *（服务器看别人的 pawn 就是这种情况）：手模只有本人那台机器渲染、才跑动画，
	 * 服务器上这条测量恒为 0 —— 但那个玩家自己的手模动作是要播完的，他也会被自己的掏枪
	 * 打断。他起播的时间和身体那条差不多，所以拿资产全长当上界（宁多等一点，
	 * 也别砍掉人家的动作）。
	 * 身体那条不退回全长：它在所有机器上都真的在播，量不出来就是真的没在播。
	 */
	auto Accumulate = [&Remaining, OwnerCharacter](const USkeletalMeshComponent* MeshComp,
	                                               const UAnimSequenceBase* Asset,
	                                               bool bFallbackToFullLength)
	{
		if (MeshComp == nullptr || Asset == nullptr) return;

		const float Measured = OwnerCharacter->GetAnimRemainingTime(MeshComp, Asset);
		if (Measured > 0.f)
		{
			Remaining = FMath::Max(Remaining, Measured);
		}
		else if (bFallbackToFullLength)
		{
			Remaining = FMath::Max(Remaining, Asset->GetPlayLength());
		}
	};

	const bool bFallbackFP = !OwnerCharacter->IsLocallyControlled();

	// 手模（FP）：左键那条和右键那条都算一遍。没在播的那条会被上面那句"量不出来"判掉
	//（右键不经过 Fire()，左键不播 FPRightClickMontage），所以不会凭空多等一条。
	Accumulate(OwnerCharacter->GetFPArmsMesh(), FPFireMontage, bFallbackFP);
	Accumulate(OwnerCharacter->GetFPArmsMesh(), FPRightClickMontage, bFallbackFP);

	// 第三人称身体（TP）：同理
	Accumulate(OwnerCharacter->GetMesh(), ThirdPersonFireMontage, false);
	Accumulate(OwnerCharacter->GetMesh(), ThirdPersonRightClickMontage, false);

	return Remaining;
}

void AWeapon::PlayAnimAssetOnMesh(USkeletalMeshComponent* Mesh, UAnimationAsset* Anim)
{
	if (Mesh == nullptr || Anim == nullptr) return;

	// 蒙太奇：交给该网格自己的动画实例，走槽位播放（能被后一条打断、能混合、能查状态）
	if (UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
	{
		if (UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
		{
			AnimInstance->Montage_Play(Montage);
			return;
		}

		// 这个网格没挂动画蓝图 → 蒙太奇没有 Slot 可以接住它，Montage_Play 也没地方调。
		// 退而求其次：直接把蒙太奇里那第一段序列单节点播（观感等同填了序列），并说清楚为什么。
		// 注意这里用的是 GetAnimReference()：AnimSegment::AnimReference 在 5.1 就给弃用了。
		UAnimSequenceBase* Fallback = nullptr;
		if (Montage->SlotAnimTracks.Num() > 0)
		{
			const FAnimTrack& Track = Montage->SlotAnimTracks[0].AnimTrack;
			if (Track.AnimSegments.Num() > 0)
			{
				Fallback = Track.AnimSegments[0].GetAnimReference();
			}
		}

		// 这条日志在静态函数里，没有 this —— 武器名从网格的 Owner 拿（武器的 Owner 就是角色）
		const FString OwnerName = Mesh->GetOwner() ? Mesh->GetOwner()->GetName() : TEXT("?");
		UE_LOG(LogTemp, Warning,
			TEXT("[武器动画] %s 上的 %s：%s 是蒙太奇，但那个网格没挂动画蓝图（蒙太奇要有 Slot 节点才接得住）"
				 "→ 退回单节点播它第一段序列。想用蒙太奇的全部功能就给这个网格挂个带 Slot 的动画蓝图。"),
			*OwnerName, *Mesh->GetName(), *Montage->GetName());

		if (Fallback)
		{
			Mesh->PlayAnimation(Fallback, false);
		}
		return;
	}

	// 普通序列：单节点播放，和原来的 FireAnimation 完全一样（不需要任何动画蓝图）
	Mesh->PlayAnimation(Anim, false);
}

int32 AWeapon::ComputeThisKillIndex(const AController* KillerController)
{
	if (!KillerController) return 1;

	// ★ 这里**不要 +1**。UGameplayStatics::ApplyDamage 是同步的：它内部会走
	//   ABlasterCharacter::ReceiveDamage →（血量归零）GameMode::PlayerEliminated → AddRoundKill()，
	//   所以武器在这一发的下一行调过来时，PlayerState 里的计数**已经**是本杀的数了
	//   （见 Character/BlasterCharacter.cpp:2793-2802）。
	//   早先这里按"读到的是上一杀"写了 +1，结果音效整体往后偏一档：1 杀响第 2 段、
	//   2 杀响第 3 段……（用户实跑发现）。图标侧走的是 GameMode 里不 +1 的那份，所以图标一直是对的。
	const ABlasterPlayerState* KillerPS =
		Cast<ABlasterPlayerState>(KillerController->PlayerState);
	// 判定了击杀，计数至少是 1；真读到 0（异常时序）也按第 1 档处理，别让它掉到"没有档位"回落成普通击杀音。
	return KillerPS ? FMath::Max(1, KillerPS->GetRoundKills()) : 1;
}

void AWeapon::PlayGunAnimation(UAnimationAsset* Anim)
{
	if (Anim == nullptr) return;

	// 本机那份第一人称副本同步播同一条（远端不渲染副本，角色那边自己会挡）。
	// 放在前面：副本存不存在都不影响真枪怎么播，两边各走各的。
	if (ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner()))
	{
		OwnerCharacter->PlayFPWeaponAnimation(Anim);
	}

	// 真枪自己。它挂在第三人称手上，所有机器都看得见 —— 这里**不做**本地判断
	PlayAnimAssetOnMesh(WeaponMesh, Anim);
}

void AWeapon::PlayFireAnimation()    { PlayGunAnimation(FireAnimation); }
void AWeapon::PlayEquipAnimation()   { PlayGunAnimation(EquipAnimation); }
void AWeapon::PlayReloadAnimation()  { PlayGunAnimation(ReloadAnimation); }
void AWeapon::PlayInspectAnimation() { PlayGunAnimation(InspectAnimation); }

USkeletalMeshComponent* AWeapon::GetViewMesh() const
{
	// 本机第一人称正在显示副本 → 本人看到的就是副本，火光/弹壳/弹匣挂它身上。
	// 但副本显示的是**手上这把**枪，所以只有它被装备时才认副本：
	// 背后/地上那些枪问这个函数一律拿真枪，不然它们的火光/弹匣会跑到副本上去。
	if (ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner()))
	{
		if (OwnerCharacter->GetEquippedWeapon() == this)
		{
			if (USkeletalMeshComponent* FPMesh = OwnerCharacter->GetVisibleFPWeaponMesh())
			{
				return FPMesh;
			}
		}
	}

	// 其余情况（没开副本 / 不是本人 / 远端机器 / 不是手上这把）都是真枪 —— 和改之前的行为一致
	return WeaponMesh;
}

USkeletalMesh* AWeapon::GetFPViewModelMesh() const
{
	if (FPViewModelMesh) return FPViewModelMesh;
	return WeaponMesh ? WeaponMesh->GetSkeletalMeshAsset() : nullptr;
}

void AWeapon::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 编辑器里改 MagazineMesh / MagazineSocket / ScopeSocket 立刻生效，BP 预览就能看见挂哪了
	UpdateAttachedMeshes();
}

void AWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 运行时再挂一次：蓝图子类覆盖过的那些属性到这时候才是最终值
	UpdateAttachedMeshes();
}

void AWeapon::UpdateAttachedMeshes(USkeletalMeshComponent* ViewMeshOverride)
{
	UpdateMagazineAttachment(ViewMeshOverride);
	UpdateMagazineExtraAttachment(ViewMeshOverride);
	UpdateScopeAttachment(ViewMeshOverride);
}

void AWeapon::UpdateMagazineAttachment(USkeletalMeshComponent* ViewMeshOverride)
{
	if (MagazineMeshComp == nullptr) return;

	// 网格两种填法都认：填了 MagazineMesh 属性就以属性为准；属性留空时**不要**去动
	// 组件上已经选好的静态网格 —— 有人是直接在组件树里给 MagazineMesh 组件选网格的，
	// 以前这里无条件 SetStaticMesh(属性) 会把人家的网格清成 null，弹匣就凭空消失了。
	if (MagazineMesh != nullptr)
	{
		MagazineMeshComp->SetStaticMesh(MagazineMesh);
	}

	const bool bHasMesh = (MagazineMeshComp->GetStaticMesh() != nullptr);
	MagazineMeshComp->SetVisibility(bHasMesh);
	if (!bHasMesh) return;

	// 挂到**本人实际看到的那个网格**上：角色说副本正显示这把枪就挂副本，否则挂真枪。
	// 为什么非挂不可：开着副本时真枪对本人是隐藏的（SetOwnerNoSee），弹匣还留在真枪上
	// 就等于第一人称根本看不到弹匣。远端机器副本永远隐藏 → 行为不变。
	USkeletalMeshComponent* ParentMesh = ViewMeshOverride ? ViewMeshOverride : GetViewMesh();
	AttachDecorationToSocket(
		MagazineMeshComp,
		ParentMesh,
		MagazineSocket,
		MagazineWorldScale,
		MagazineLocationOffset,
		MagazineRotationOffset);
}

void AWeapon::UpdateMagazineExtraAttachment(USkeletalMeshComponent* ViewMeshOverride)
{
	if (MagazineExtraMeshComp == nullptr) return;

	// 和弹匣完全同一套两种填法：填了 MagazineExtraMesh 属性就以属性为准，属性留空时
	// **不要**去动组件上已经选好的静态网格（有人在组件树里直接给组件选网格的情况）。
	if (MagazineExtraMesh != nullptr)
	{
		MagazineExtraMeshComp->SetStaticMesh(MagazineExtraMesh);
	}

	// 没填备用弹匣网格的枪（绝大多数枪）到这里就结束：组件保持隐藏，不占任何开销。
	const bool bHasMesh = (MagazineExtraMeshComp->GetStaticMesh() != nullptr);
	MagazineExtraMeshComp->SetVisibility(bHasMesh);
	if (!bHasMesh) return;

	// 同弹匣：挂到**本人实际看到的那个网格**（开着第一人称副本时就是副本）。备弹位
	// Magazine_ExtraSocket 只在枪身上，人物骨架/手臂网格上没有这个 socket。
	AttachDecorationToSocket(
		MagazineExtraMeshComp,
		ViewMeshOverride ? ViewMeshOverride : GetViewMesh(),
		MagazineExtraSocket,
		MagazineExtraWorldScale,
		MagazineExtraLocationOffset,
		MagazineExtraRotationOffset);
}

void AWeapon::UpdateScopeAttachment(USkeletalMeshComponent* ViewMeshOverride)
{
	if (ScopeMeshComp == nullptr) return;

	// 同弹匣：属性优先，属性留空时不动组件上已经选好的网格
	if (ScopeMesh != nullptr)
	{
		ScopeMeshComp->SetStaticMesh(ScopeMesh);
	}

	const bool bHasMesh = (ScopeMeshComp->GetStaticMesh() != nullptr);
	ScopeMeshComp->SetVisibility(bHasMesh);
	if (!bHasMesh) return;

	// 瞄准镜挂在武器网格的 ReflexSocket 上，第一人称副本显示时这个组件不跟着隐身
	//（SetOwnerNoSee 只管自己那一个图元，不往下传），所以它必须和弹匣一样换到副本上，
	// 否则开副本时瞄准镜会孤零零飘在看不见的枪身位置（就是第三人称那个枪位）。
	USkeletalMeshComponent* ParentMesh = ViewMeshOverride ? ViewMeshOverride : GetViewMesh();
	AttachDecorationToSocket(
		ScopeMeshComp,
		ParentMesh,
		ScopeSocket,
		ScopeWorldScale,
		ScopeLocationOffset,
		ScopeRotationOffset);
}

void AWeapon::Dropped()
{
	SetWeaponState(EWeaponState::EWS_Dropped);
	FDetachmentTransformRules DetachRules(EDetachmentRule::KeepWorld,true);
	SetOwner(nullptr);
	WeaponMesh->DetachFromComponent(DetachRules);
	BlasterOwnerCharacter=nullptr;
	BlasterOwnerController=nullptr;
}

void AWeapon::AddAmmo(int32 AmmoToAdd)
{
	Ammo = FMath::Clamp(Ammo + AmmoToAdd,0,MagCapacity);
	SetHUDAmmo();
}

void AWeapon::SetAmmo(int32 NewAmmo)
{
	Ammo = FMath::Clamp(NewAmmo, 0, MagCapacity);
	SetHUDAmmo();
}

