#include "LobbyAgentShowcase.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Animation/AnimSequence.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialDomain.h"   // MD_Surface
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ALobbyAgentShowcase::ALobbyAgentShowcase()
{
	PrimaryActorTick.bCanEverTick = false;

	// 火球的默认大小：占位网格是引擎自带球（半径 50 = 直径 100cm），
	// 结构体默认的 1.0 会给你一个**一米大的球**顶在手上。0.3 → 约 30cm 一团，
	// 和握在手里的东西一个量级；角色那边那颗球（ABlasterCharacter::HeldFireballScale）
	// 用的也是 0.3，保持一致。
	// （⚠️ 这是**世界口径**的缩放，不是组件的相对缩放 —— 骨架带整体缩放时，
	//   ApplyPlacement 会按挂点的世界缩放反算，所以这个数就是"引擎里看到的实际倍数"。）
	FireballPlacement.Scale = 0.3f;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ShowcaseMesh"));
	SetRootComponent(Mesh);
	// 纯展示：不参与碰撞/查询，也不接收贴花。角色是本地摆件，别去碰地图里的东西。
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->bReceivesDecals = false;
	Mesh->SetCastShadow(true);

	// --- 手上那个常驻挂件（Jett 的刀）---
	// 构造函数里这句挂载用的是 **C++ 默认值 R_WeaponPoint** —— 你在 BP 里改
	// KnifePlacement.Socket 的话，这一句是不认的（构造函数跑的时候 BP 的类默认值
	// 还没套到这个对象上，和 ABlasterCharacter::HeldFireballFPSocket 那个坑一模一样）。
	// 真正生效的是 RefreshHandProps() 那次重挂，它在 OnConstruction / SetAgent 里各跑一次。
	HandProp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HandProp"));
	HandProp->SetupAttachment(Mesh, KnifePlacement.Socket);
	HandProp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandProp->SetGenerateOverlapEvents(false);
	HandProp->SetCanEverAffectNavigation(false);
	HandProp->SetVisibility(false);   // 没定英雄之前手上什么都没有

	// --- 手上那颗火球（他和别人看的都是同一个组件：大厅只有一台观察者）---
	HandFireballMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HandFireballMesh"));
	HandFireballMesh->SetupAttachment(Mesh, FireballPlacement.Socket);
	HandFireballMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandFireballMesh->SetGenerateOverlapEvents(false);
	HandFireballMesh->SetCanEverAffectNavigation(false);
	// 发光球不投影：一颗悬在手心的光球投出个硬阴影反而怪，角色那边也是这么关的。
	HandFireballMesh->bCastDynamicShadow = false;
	HandFireballMesh->CastShadow = false;
	HandFireballMesh->SetVisibility(false);

	// 占位网格：引擎自带球（半径 50）。发光材质在 RefreshHandProps 里给。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> HandFireballMeshAsset(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (HandFireballMeshAsset.Succeeded())
	{
		HandFireballMesh->SetStaticMesh(HandFireballMeshAsset.Object);
	}

	// 火球的特效组件。bAutoActivate=false：配的是哪个资产在构造函数里读不到（BP 的值还没上来），
	// 而且"现在手上该不该有球"也得到运行时才知道 —— 两件事都交给 ApplyHandFireballVisual。
	HandFireballFX = CreateDefaultSubobject<UNiagaraComponent>(TEXT("HandFireballFX"));
	HandFireballFX->SetupAttachment(Mesh, FireballPlacement.Socket);
	HandFireballFX->SetAutoActivate(false);
	HandFireballFX->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandFireballFX->bCastDynamicShadow = false;
	HandFireballFX->SetVisibility(false);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("ShowcaseCamera"));
	Camera->SetupAttachment(Mesh);
	// 摆位由 ApplyCameraPlacement 统一负责（那五个数在头文件里，蓝图可改）
	ApplyCameraPlacement();

	// 初始不显示；真正的可见性由 SetAgent 决定（没资源的英雄就一直是隐藏的）
	SetActorHiddenInGame(true);
}

void ALobbyAgentShowcase::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 在 BP_LobbyAgentShowcase 里改那几个数后，编辑器视口靠这一步立刻刷新预览；
	// 运行时 SpawnActor 也会走这里，所以摆位只有这一个出口。
	ApplyCameraPlacement();

	// 挂件同理。CurrentAgent 是 None 时这里不会有任何东西显示出来
	//（常驻挂件的网格来自英雄表，表里查不到就没有）—— 那是对的：
	// 编辑器里要看刀挂得正不正，把 ShowcaseMesh 的 Skeletal Mesh 临时指到
	// CS_Wushu_S0_Skelmesh、进 PIE 走一遍大厅最快。
	RefreshHandProps();
}

void ALobbyAgentShowcase::ApplyCameraPlacement()
{
	if (!Camera)
	{
		return;
	}

	Camera->SetRelativeLocation(FVector(CameraForwardOffset, 0.f, CameraHeight));
	Camera->SetRelativeRotation(FRotator(CameraPitch, CameraYaw, 0.f));
	Camera->SetFieldOfView(CameraFOV);
}

void ALobbyAgentShowcase::BeginPlay()
{
	Super::BeginPlay();
	SetActorHiddenInGame(true);
}

void ALobbyAgentShowcase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IntroTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(IntroTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

bool ALobbyAgentShowcase::FindShowcaseEntry(EBlasterAgent Agent, FShowcaseEntry& OutEntry)
{
	switch (Agent)
	{
	case EBlasterAgent::Phoenix:
		// 手上不留常驻挂件：他那个火球是**动画通知**开出来的（UAnimNotify_LobbyHeldFireball），
		// 因为"哪一帧把火球搓出来"只有动画知道。
		OutEntry = {
			TEXT("/Game/ValorantAssets/CharSelect/CS_Phoenix_S0_Skelmesh"),
			TEXT("/Game/ValorantAssets/CharSelect/CS_Phoenix_S0_CharSelect_Intro"),
			TEXT("/Game/ValorantAssets/CharSelect/CS_Phoenix_S0_Idle"),
			nullptr
		};
		return true;

	case EBlasterAgent::Jett:
		// Jett 走的是 "Wushu" 那套网格（Valorant 内部代号）。骨架/动画和 Phoenix 的是
		// 两套独立的资产，各自绑定，别指望能混用。
		//
		// 手上那把刀 = 她大招（X）那把刀的静态网格，和大招里那 5 把飞刀用的是同一个资产
		//（AJettCharacter::KnifeStaticMesh）。挂点用角色骨架的 R_WeaponPoint（右手），
		// 和飞刀骨骼上那 5 个 KnifeNSocket 不是一回事：那 5 把是挂在**刀骨架**上的。
		//
		// 路径写全（包名.对象名）：这个资产的包名和对象名同名，省略也能加载，
		// 但写全了不依赖加载器的兜底规则。
		OutEntry = {
			TEXT("/Game/ValorantAssets/CharSelect/CS_Wushu_S0_Skelmesh"),
			TEXT("/Game/ValorantAssets/CharSelect/CS_Wushu_S0_Intro"),
			TEXT("/Game/ValorantAssets/CharSelect/CS_Wushu_S0_Idle_Loop"),
			TEXT("/Game/ValorantAssets/Ability_X_Knives/AB_Wushu_S0_X_Staticmesh.AB_Wushu_S0_X_Staticmesh")
		};
		return true;

	default:
		// Sage / Clove：工程里还没有选人资源（网格和动画都没导）→ 大厅中间留空。
		// 以后把资源导进同一个目录，在这里加一个 case 即可，别处都不用动。
		return false;
	}
}

void ALobbyAgentShowcase::SetAgent(EBlasterAgent Agent)
{
	if (Agent == CurrentAgent)
	{
		return;
	}
	CurrentAgent = Agent;

	// 换人先把手上的火球复位：上一个英雄搓出来的球不能留在下一个人手上。
	// （通知只在动画里那一刻响一次，不复位的话它就会一直挂着。）
	bHandFireballVisible = false;

	FShowcaseEntry Entry;
	if (!FindShowcaseEntry(Agent, Entry))
	{
		if (IntroTimerHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(IntroTimerHandle);
		}
		if (Mesh)
		{
			Mesh->SetSkeletalMesh(nullptr);
		}
		RefreshHandProps();
		SetActorHiddenInGame(true);
		return;
	}

	USkeletalMesh* LoadedMesh = LoadObject<USkeletalMesh>(nullptr, Entry.MeshPath);
	if (!LoadedMesh)
	{
		// 资源没导进来/路径写错：和"没资源"一样处理，别让半截状态留在屏幕上
		UE_LOG(LogTemp, Warning, TEXT("LobbyAgentShowcase: 展示网格加载失败 %s"), Entry.MeshPath);
		RefreshHandProps();
		SetActorHiddenInGame(true);
		return;
	}

	SetActorHiddenInGame(false);

	// ⚠️ 顺序不能反：换网格会把组件上的材质覆盖数组整个重建，所以补材质必须在
	//    SetSkeletalMesh 之后。
	Mesh->SetSkeletalMesh(LoadedMesh);
	PatchBrokenMaterialSlots();

	// 挂件跟在网格后面（挂点属于骨架，网格换了挂点才存在）
	RefreshHandProps();

	UAnimSequence* Intro = LoadObject<UAnimSequence>(nullptr, Entry.IntroPath);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, Entry.IdlePath);

	if (IntroTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(IntroTimerHandle);
	}

	if (Intro && Intro->GetPlayLength() > 0.f)
	{
		Mesh->PlayAnimation(Intro, /*bLooping=*/false);
		if (Idle)
		{
			// 出场播完就切待机循环。选人界面用定时器硬切足够 —— 不需要为了一个
			// "Intro→Idle" 过渡去做 Montage（那套槽轨道反而更难搞）。
			//
			// ★ 换序列**不会**动火球的显隐：通知响一次就把状态留在那儿了。
			//   想让它到某个时刻消失，在动画上再摆一条 bShow=false 的通知即可。
			GetWorldTimerManager().SetTimer(IntroTimerHandle, this,
				&ALobbyAgentShowcase::OnIntroFinished, Intro->GetPlayLength(), false);
		}
	}
	else if (Idle)
	{
		// 没有出场动画就直接循环待机
		Mesh->PlayAnimation(Idle, /*bLooping=*/true);
	}
}

void ALobbyAgentShowcase::OnIntroFinished()
{
	FShowcaseEntry Entry;
	if (!FindShowcaseEntry(CurrentAgent, Entry) || !Mesh)
	{
		return;
	}

	if (UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, Entry.IdlePath))
	{
		Mesh->PlayAnimation(Idle, /*bLooping=*/true);
	}
}

void ALobbyAgentShowcase::SetHandFireballVisible(bool bShow)
{
	if (bHandFireballVisible == bShow)
	{
		return;
	}
	bHandFireballVisible = bShow;
	ApplyHandFireballVisual();
}

void ALobbyAgentShowcase::RefreshHandProps()
{
	if (!Mesh)
	{
		return;
	}

	FShowcaseEntry Entry;
	const bool bHasEntry = FindShowcaseEntry(CurrentAgent, Entry);

	// ---------------------------------------------------------- 常驻挂件（Jett 的刀）
	if (HandProp)
	{
		UStaticMesh* PropMesh = (bHasEntry && Entry.HandPropPath)
			? LoadObject<UStaticMesh>(nullptr, Entry.HandPropPath)
			: nullptr;

		if (!PropMesh && bHasEntry && Entry.HandPropPath)
		{
			UE_LOG(LogTemp, Warning, TEXT("LobbyAgentShowcase: 手上挂件加载失败 %s"), Entry.HandPropPath);
		}

		if (HandProp->GetStaticMesh() != PropMesh)
		{
			HandProp->SetStaticMesh(PropMesh);
		}

		// 没有网格就整个藏掉：留着一个空组件挂在骨骼上没有任何意义，
		// 而且它的相对变换还会被动画每帧带着跑。
		HandProp->SetVisibility(PropMesh != nullptr);

		if (PropMesh)
		{
			ApplyPlacement(HandProp, KnifePlacement);
		}
	}

	// ---------------------------------------------------------- 火球（网格 + 材质 + 挂点）
	if (HandFireballMesh)
	{
		// 占位材质：BP 里指派了就用 BP 的；没指派就用项目里那个发光球材质
		//（和 ABlasterCharacter 手上那颗球是同一个资产）。刻意**不**写成
		// ConstructorHelpers 的必需资产 —— 那是项目内容，哪天改名/移走，
		// 这里只是退回引擎默认材质（灰球，但依然看得见），不会把类的构造直接搞崩。
		UMaterialInterface* Mat = HandFireballMaterial;
		if (!Mat)
		{
			Mat = LoadObject<UMaterialInterface>(nullptr,
				TEXT("/Game/Assets/MilitaryWeapSilver/FX/Materials/M_GlowSphere_01.M_GlowSphere_01"));
		}
		if (Mat)
		{
			HandFireballMesh->SetMaterial(0, Mat);
		}

		ApplyPlacement(HandFireballMesh, FireballPlacement);
	}

	if (HandFireballFX)
	{
		if (HandFireballFX->GetAsset() != HandFireballEffect)
		{
			HandFireballFX->SetAsset(HandFireballEffect);
		}

		// 特效的缩放走 HandFireballEffectScale（**世界口径**），和占位球那个
		// FireballPlacement.Scale 是两套数：那 0.3 是按半径 50 的引擎球换算的，
		// 套到 Niagara 上等于把特效整体缩小 3.3 倍。挂点和位置照旧共用一份。
		FLobbyHandPropPlacement FXPlacement = FireballPlacement;
		FXPlacement.Scale = HandFireballEffectScale;
		ApplyPlacement(HandFireballFX, FXPlacement);
	}

	ApplyHandFireballVisual();
}

void ALobbyAgentShowcase::ApplyHandFireballVisual()
{
	const bool bShow = bHandFireballVisible && !IsHidden();

	// 二选一：配了 Niagara 就用它、把占位球收起来；没配就还是占位球（默认）。
	// 和 ABlasterCharacter::UpdateHeldThrowableVisual 里那个 ApplyHeldVisual 同一套规矩。
	//
	// ★ 藏特效时必须 Deactivate()，不能只 SetVisibility(false) —— 后者只是不画，
	//   Niagara 还在后台继续算粒子（画面看不见、性能照烧）。每次开关都会走到这儿，
	//   所以这里必须幂等。
	const bool bUseFX = bShow && HandFireballFX && HandFireballEffect;

	if (HandFireballFX)
	{
		if (bUseFX)
		{
			HandFireballFX->Activate(/*bReset=*/true);
		}
		else
		{
			HandFireballFX->Deactivate();
		}

		HandFireballFX->SetVisibility(bUseFX, /*bPropagateToChildren=*/true);
	}

	if (HandFireballMesh)
	{
		HandFireballMesh->SetVisibility(bShow && !bUseFX, /*bPropagateToChildren=*/true);
	}
}

void ALobbyAgentShowcase::ApplyPlacement(USceneComponent* Comp, const FLobbyHandPropPlacement& Placement)
{
	if (!Comp || !Mesh)
	{
		return;
	}

	// 空名字当作"没填"处理，退回到默认那个挂点，别让组件掉到网格原点去。
	const FName Socket = Placement.Socket.IsNone() ? FName(TEXT("R_WeaponPoint")) : Placement.Socket;

	// 挂点不存在时引擎不报错，只会在内部退到"挂在网格原点"（表现是一团东西挂在脚边），
	// 而且警告在别的日志分类里、很难查。这里自己报一次，把网格名和挂点名都打出来。
	if (!Mesh->DoesSocketExist(Socket))
	{
		if (!bLoggedMissingSocket)
		{
			bLoggedMissingSocket = true;
			UE_LOG(LogTemp, Warning,
				TEXT("[Lobby] 展示网格 '%s' 上找不到挂点 '%s'（socket 和骨骼都没有），挂件会掉到网格原点（检查 KnifePlacement/FireballPlacement 的 Socket）"),
				*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Socket.ToString());
		}
	}
	else
	{
		bLoggedMissingSocket = false;
	}

	// SnapToTargetNotIncludingScale：位置/旋转吸附到挂点、缩放按"保持世界缩放"算（下面再改成目标值）。
	// 已经挂在目标网格的目标挂点上就别重挂 —— OnConstruction 每次改属性都会跑一遍，
	// 无条件重挂会把蓝图反复标脏（和 AJettCharacter::RefreshKnifeMeshes 同一个理由）。
	if (Comp->GetAttachParent() != Mesh || Comp->GetAttachSocketName() != Socket)
	{
		Comp->AttachToComponent(Mesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
	}

	// 世界口径 → 相对缩放：把挂点的缩放抵消掉，Placement.Scale 才等于"引擎里看到的实际大小"。
	// 换算和 AJettCharacter::RefreshKnifeMeshes 是同一套。
	const FVector ParentScale = Mesh->GetSocketTransform(Socket, RTS_World).GetScale3D();
	auto DivideByParentScale = [](float Value, float Scale)
	{
		return FMath::IsNearlyZero(Scale) ? Value : Value / Scale;
	};

	Comp->SetRelativeScale3D(FVector(
		DivideByParentScale(Placement.Scale, ParentScale.X),
		DivideByParentScale(Placement.Scale, ParentScale.Y),
		DivideByParentScale(Placement.Scale, ParentScale.Z)));

	// ⚠️ 位移/旋转必须放在 AttachToComponent **之后**：挂载那一下会把相对变换吸附成
	//    单位值，先设后挂等于白设。这个坑在 AWeapon::AttachDecorationToSocket 那边也踩过。
	Comp->SetRelativeLocation(Placement.Location);
	Comp->SetRelativeRotation(Placement.Rotation);
}

void ALobbyAgentShowcase::PatchBrokenMaterialSlots()
{
	if (!Mesh)
	{
		return;
	}

	// Jett(Wushu) 的选人网格有 3 个槽挂的是 **parent 为空的 MIC**（Tattoo/Hair），
	// 而工程里没有对应的源材质可补 —— 直接用会渲染成一团默认表现。这里在运行时
	// 顶成引擎默认材质（灰模），至少身形是完整的。
	// 注意这是**组件上的运行时覆盖**，不写资产，所以不会影响别处用到这些 MIC 的地方。
	// 以后把真材质导进来，把它们的 parent 接上，这个函数自然就不再命中那些槽了。
	UMaterialInterface* Fallback = UMaterial::GetDefaultMaterial(MD_Surface);
	if (!Fallback)
	{
		return;
	}

	const int32 NumSlots = Mesh->GetNumMaterials();
	for (int32 i = 0; i < NumSlots; ++i)
	{
		const UMaterialInterface* Current = Mesh->GetMaterial(i);

		bool bBroken = (Current == nullptr);
		if (!bBroken)
		{
			if (const UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Current))
			{
				bBroken = (MIC->Parent == nullptr);
			}
		}

		if (bBroken)
		{
			Mesh->SetMaterial(i, Fallback);
		}
	}
}
