// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Abilities/CloveSmoke.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ACloveSmoke::ACloveSmoke()
{
	PrimaryActorTick.bCanEverTick = true;

	SmokeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SmokeMesh"));
	SetRootComponent(SmokeMesh);

	// 纯视觉：碰撞、overlap、导航全关。
	// 这就是「子弹能穿过去」的全部实现 —— 没有任何隐藏通道，只是它压根不参与判定。
	SmokeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SmokeMesh->SetGenerateOverlapEvents(false);
	SmokeMesh->SetCanEverAffectNavigation(false);

	// 250cm 的球投出来的阴影是一大块死黑，比烟本身还显眼。Valorant 的烟也基本不投影。
	SmokeMesh->SetCastShadow(false);

	// 服务器生成 → 复制到所有客户端。位置固定不动，所以不需要移动复制。
	// 不做 bAlwaysRelevant：距离远的烟玩家根本看不到，没必要占带宽。
	bReplicates = true;
	SetReplicateMovement(false);

	// 默认网格：引擎自带球体。BP_CloveSmoke 里可以换成任何以原点为中心的模型。
	// 用 ConstructorHelpers 是为了「C++ 类裸建出来就能看」—— 不然忘了在 BP 里填网格，
	// 表现是「封烟成功了但屏幕上什么都没有」，很难往"没配网格"上想。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		SmokeMesh->SetStaticMesh(SphereMesh.Object);
	}
}

bool ACloveSmoke::BlocksVisionSegment(const FVector& From, const FVector& To) const
{
	/*
	 * 线段到球心的最近距离 ≤ 半径 = 视线穿过了这团烟。
	 *
	 * 用"点到线段的距离"而不是"球心到直线的距离"：后者会把线段**延长线**上的烟也算进来 ——
	 * 表现是"站在烟后面老远的人也被判成看不见"（闪光本来该闪到他）。
	 * FMath::PointDistToSegmentSquared 已经把投影参数夹在线段两端之间，正是要的语义。
	 */
	const float RadiusSq = FMath::Square(FMath::Max(SmokeRadius, 0.f));
	return FMath::PointDistToSegmentSquared(GetActorLocation(), From, To) <= RadiusSq;
}

void ACloveSmoke::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 半径 → 缩放的换算从**网格资产自己的包围球半径**来，不写死引擎球的 50。
	// 这样 BP 里换成球壳模型时，只要模型以原点为中心，SmokeRadius 照样是对的数值。
	float AssetRadius = 50.f; // 引擎 BasicShapes/Sphere 的半径
	if (SmokeMesh && SmokeMesh->GetStaticMesh())
	{
		const float Radius = SmokeMesh->GetStaticMesh()->GetBounds().SphereRadius;
		if (Radius > KINDA_SMALL_NUMBER)
		{
			AssetRadius = Radius;
		}
	}

	BaseScale = FVector(SmokeRadius / AssetRadius);
	SmokeMesh->SetRelativeScale3D(BaseScale * StartScaleRatio);

	// 材质在 OnConstruction 里换而不是在构造函数里：构造函数阶段 BP 子类指派的
	// SmokeMaterial 还没被序列化上去（CDO 的 UPROPERTY 值是资产加载之后才生效的），
	// 在构造函数里读会永远拿到 nullptr。OnConstruction 在 SpawnActor 时必跑，
	// 又早于 BeginPlay 里的 CreateDynamicMaterialInstance，正好赶在 MID 之前。
	if (SmokeMaterial && SmokeMesh)
	{
		SmokeMesh->SetMaterial(0, SmokeMaterial);
	}
}

void ACloveSmoke::BeginPlay()
{
	Super::BeginPlay();

	SpawnTime = GetWorld()->GetTimeSeconds();

	// 动态材质实例要在**两端**都建：服务器不需要好看，但复制到客户端后客户端得能淡入。
	if (SmokeMesh)
	{
		FadeMID = SmokeMesh->CreateDynamicMaterialInstance(0);
	}

	// 只在服务器定寿命。客户端也 SetLifeSpan 的话，两边计时长短一旦有差异
	//（网络延迟/帧率），就会出现「烟在客户端提前消失」或者「服务器没了客户端还留着」。
	// 客户端的销毁老老实实等服务器复制。
	if (HasAuthority())
	{
		SetLifeSpan(SmokeDuration);
	}
}

void ACloveSmoke::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 不需要铺开动画：直接落到最终形态并关掉 Tick，省得每帧白跑。
	if (GrowInTime <= KINDA_SMALL_NUMBER)
	{
		if (FadeMID)
		{
			FadeMID->SetScalarParameterValue(FadeParamName, 1.f);
		}
		if (SmokeMesh)
		{
			SmokeMesh->SetRelativeScale3D(BaseScale);
		}
		SetActorTickEnabled(false);
		return;
	}

	const float Age = GetWorld()->GetTimeSeconds() - SpawnTime;
	const float Alpha = FMath::Clamp(Age / GrowInTime, 0.f, 1.f);

	// 缓出（1-(1-t)^2）：起步快、收尾慢。线性的话像气球被吹大，缓出更像烟铺开。
	const float Eased = 1.f - FMath::Square(1.f - Alpha);

	if (FadeMID)
	{
		FadeMID->SetScalarParameterValue(FadeParamName, Eased);
	}
	if (SmokeMesh)
	{
		SmokeMesh->SetRelativeScale3D(BaseScale * FMath::Lerp(StartScaleRatio, 1.f, Eased));
	}

	// 铺完了就不需要再每帧改材质/缩放，直接关 Tick
	if (Alpha >= 1.f)
	{
		SetActorTickEnabled(false);
	}
}
