#include "BlastBarrier.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

ABlastBarrier::ABlastBarrier()
{
	// 关卡里摆的静态摆件，不 Tick：显隐和碰撞都是"切一次"，没有每帧要做的事
	PrimaryActorTick.bCanEverTick = false;

	// 状态要传到客户端（见头文件那段"为什么状态要复制"）
	bReplicates = true;
	bAlwaysRelevant = true;      // 墙离本地玩家可能很远（在对面出生区），别被相关性剔除掉
	SetReplicateMovement(false); // 它不动，位置来自关卡

	WallMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WallMesh"));
	SetRootComponent(WallMesh);

	WallMesh->SetCollisionProfileName(TEXT("BlockAll"));   // 挡人、也挡子弹
	WallMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // 默认关，由 ApplyState 打开
	WallMesh->SetGenerateOverlapEvents(false);

	/*
	 * 组件本身的复制关掉。
	 *
	 * USceneComponent 的 bVisible / RelativeTransform 是**推送式复制**（bIsPushBased），
	 * 而我们的显隐是两端各自从 bBarrierActive 算出来的 —— 要是组件还参与复制，
	 * 服务端那次 SetVisibility 会自己推送一份过去，跟复制过来的 bool 抢同一件事。
	 * 干脆让它不复制：状态只有一个来源（那个 bool）。
	 */
	WallMesh->SetIsReplicated(false);

	// 引擎自带的 1 米立方体：缩放 5 倍就是 5 米长，摆位时直接拉
	static ConstructorHelpers::FObjectFinder<UStaticMesh> MeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (MeshFinder.Succeeded())
	{
		WallMesh->SetStaticMesh(MeshFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MatFinder(TEXT("/Game/Materials/M_BlastBarrier"));
	if (MatFinder.Succeeded())
	{
		bDefaultMaterialLoaded = true;
		WallMesh->SetMaterial(0, MatFinder.Object);
	}
}

void ABlastBarrier::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!WallMesh) return;

	// 在编辑器里**永远看得见** —— 你要靠它对着地形摆位置、调长度。
	// 运行时 OnConstruction 跑完紧接着就是 BeginPlay，ApplyState() 会立刻按 bBarrierActive
	// 改回来（默认关），中间不跨帧，所以不会在游戏里闪一下。
	WallMesh->SetVisibility(true);

	// 换了材质就换上（BP / 关卡实例上指定的值在构造期是读不到的，得在这儿读）
	if (BarrierMaterial)
	{
		WallMesh->SetMaterial(0, BarrierMaterial);
	}
}

void ABlastBarrier::BeginPlay()
{
	Super::BeginPlay();

	if (!bDefaultMaterialLoaded && !BarrierMaterial)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[出生屏障] %s 没拿到材质（/Game/Materials/M_BlastBarrier 不在？）→ 会显示成引擎默认灰色。"),
			*GetName());
	}

	ApplyState();
}

void ABlastBarrier::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABlastBarrier, bBarrierActive);
}

void ABlastBarrier::SetBarrierActive(bool bNewActive)
{
	// 只有服务器能改状态 —— 客户端里 GameMode 这个类根本不存在，但保险起见挡一道：
	// 客户端要是自己改了本地 bool，复制过来时会和服务器打架。
	if (!HasAuthority()) return;

	if (bBarrierActive == bNewActive) return;

	bBarrierActive = bNewActive;
	ApplyState();
}

void ABlastBarrier::OnRep_BarrierActive()
{
	ApplyState();
}

void ABlastBarrier::ApplyState()
{
	if (!WallMesh) return;

	WallMesh->SetVisibility(bBarrierActive);

	/*
	 * 碰撞用 QueryAndPhysics 而不是 QueryOnly：
	 * 墙是静止的、不参与物理模拟，但这个 mesh 的 mobility 是 Static，
	 * QueryAndPhysics + Static 是这个引擎里"实心障碍物"的标准组合
	 *（Profiles 里 BlockAll 那条就是这个设置）。用 QueryOnly 的话角色撞上去会穿。
	 *
	 * 关的时候用 NoCollision 而不是"把响应全设成 Ignore"：前者连查询都跳过，是真的没有开销，
	 * 而且不会留下一个"看得见轮廓、其实能走过去"的中间态。
	 */
	WallMesh->SetCollisionEnabled(bBarrierActive
		? ECollisionEnabled::QueryAndPhysics
		: ECollisionEnabled::NoCollision);
}
