#include "PlantZone.h"
#include "PlantZoneBoxComponent.h"
#include "ZoneCornerMarker.h"
#include "Components/DecalComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Algo/Reverse.h"

namespace
{
	// 2D 多边形有向面积（>0 逆时针 / <0 顺时针，XY 平面俯视）
	float SignedArea2D(const TArray<FVector2D>& Pts)
	{
		float Area = 0.f;
		const int32 N = Pts.Num();
		for (int32 i = 0; i < N; ++i)
		{
			const FVector2D& P = Pts[i];
			const FVector2D& Q = Pts[(i + 1) % N];
			Area += P.X * Q.Y - Q.X * P.Y;
		}
		return 0.5f * Area;
	}

	// 点 P 是否落在三角形 ABC 内（含边上；耳切判断用，宽松一点没关系）
	bool PointInTriangle2D(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		auto Sign = [](const FVector2D& P1, const FVector2D& P2, const FVector2D& P3) -> float
		{
			return (P1.X - P3.X) * (P2.Y - P3.Y) - (P2.X - P3.X) * (P1.Y - P3.Y);
		};
		const float D1 = Sign(P, A, B);
		const float D2 = Sign(P, B, C);
		const float D3 = Sign(P, C, A);
		const bool bNeg = (D1 < 0.f) || (D2 < 0.f) || (D3 < 0.f);
		const bool bPos = (D1 > 0.f) || (D2 > 0.f) || (D3 > 0.f);
		return !(bNeg && bPos);
	}

	// 耳切三角剖分：输入必须是有序、不自交的简单多边形（凸/凹都行）。
	// 输出三角形顶点索引（引用原始 Pts 下标），失败返回 false（退化/自交）。
	bool EarClipTriangulate(const TArray<FVector2D>& Pts, TArray<int32>& OutTris)
	{
		const int32 N = Pts.Num();
		if (N < 3) return false;

		TArray<int32> Idx;
		Idx.Reserve(N);
		for (int32 i = 0; i < N; ++i) Idx.Add(i);

		// 耳切假设多边形是逆时针；顺时针就先反转工作序
		if (SignedArea2D(Pts) < 0.f)
		{
			Algo::Reverse(Idx);
		}

		OutTris.Reset();
		while (Idx.Num() > 3)
		{
			const int32 M = Idx.Num();
			bool bClipped = false;
			for (int32 i = 0; i < M; ++i)
			{
				const int32 iPrev = (i + M - 1) % M;
				const int32 iNext = (i + 1) % M;
				const int32 A = Idx[iPrev], B = Idx[i], C = Idx[iNext];
				const FVector2D& PA = Pts[A];
				const FVector2D& PB = Pts[B];
				const FVector2D& PC = Pts[C];

				// 凹点（叉积 <=0）不是耳
				const float Cross = (PB.X - PA.X) * (PC.Y - PA.Y) - (PB.Y - PA.Y) * (PC.X - PA.X);
				if (Cross <= 0.f) continue;

				// 耳内不能有其它顶点
				bool bHasInside = false;
				for (int32 j = 0; j < M; ++j)
				{
					if (j == iPrev || j == i || j == iNext) continue;
					if (PointInTriangle2D(Pts[Idx[j]], PA, PB, PC)) { bHasInside = true; break; }
				}
				if (bHasInside) continue;

				OutTris.Add(A); OutTris.Add(B); OutTris.Add(C);
				Idx.RemoveAt(i);
				bClipped = true;
				break;
			}
			if (!bClipped) return false; // 退化/自交，放弃剖分
		}
		if (Idx.Num() == 3)
		{
			OutTris.Add(Idx[0]); OutTris.Add(Idx[1]); OutTris.Add(Idx[2]);
			return true;
		}
		return false;
	}
}

APlantZone::APlantZone()
{
	PrimaryActorTick.bCanEverTick = false;

	ZoneBox = CreateDefaultSubobject<UPlantZoneBoxComponent>(TEXT("ZoneBox"));
	SetRootComponent(ZoneBox);
	// 玩法判定已改为多边形点内检测，Box 仅作根变换 + 贴花锚点，不参与碰撞/重叠
	ZoneBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ZoneBox->SetGenerateOverlapEvents(false);

	// 地面描边贴花：贴花沿局部 -X 方向投影，所以转 90° 让 -X 指向下方（世界 -Z）
	ZoneDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("ZoneDecal"));
	ZoneDecal->SetupAttachment(RootComponent);
	ZoneDecal->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
	ZoneDecal->DecalSize = FVector(300.f, 300.f, 300.f);

	// 程序化地面网格：角点模式的多边形可视化（填面 + 勾边），BeginPlay 时生成。
	// 顶点按「世界坐标」生成，因此必须 SetAbsolute 忽略父级(ZoneBox)变换，
	// 否则世界坐标会被根组件的位置/旋转再叠加一次，网格整体偏走。
	ZoneMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ZoneMesh"));
	ZoneMesh->SetupAttachment(RootComponent);
	ZoneMesh->SetAbsolute(true, true, true);
	ZoneMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ZoneMesh->SetCastShadow(false);
}

void APlantZone::BeginPlay()
{
	Super::BeginPlay();

	if (GetEffectiveCorners().Num() >= 3)
	{
		BuildZoneVisual();
	}
	// 未配置角点（矩形兜底）时保持老 ZoneDecal 矩形描边不变
}

void APlantZone::BuildZoneVisual()
{
	UWorld* World = GetWorld();
	const TArray<FVector> Raw = GetEffectiveCorners();
	const int32 N = Raw.Num();
	if (N < 3) return;

	// 角点落到地面高度：每个角点垂直向下打一条射线找地板（角点小球可能略高于地面；
	// 拿不到命中就用角点自身 Z）。填面/勾边都贴着地板生成，避免悬空/穿地。
	TArray<float> FloorZ;
	FloorZ.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		float Z = Raw[i].Z;
		if (World)
		{
			FVector Start = Raw[i]; Start.Z = Raw[i].Z + 150.f;
			FVector End = Raw[i];   End.Z = Raw[i].Z - 20000.f;
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility))
			{
				Z = Hit.ImpactPoint.Z;
			}
		}
		FloorZ[i] = Z;
	}

	// --- 填面：耳切三角剖分（XY 平面），凸/凹都覆盖整块可下包区 ---
	TArray<FVector2D> Pts2D;
	Pts2D.Reserve(N);
	for (const FVector& C : Raw) Pts2D.Add(FVector2D(C.X, C.Y));

	TArray<FVector> FillVerts;
	TArray<int32> FillTris;
	const bool bFillOK = EarClipTriangulate(Pts2D, FillTris) && FillTris.Num() >= 3;
	if (bFillOK)
	{
		FillVerts.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector V = Raw[i];
			V.Z = FloorZ[i] + 1.5f; // 略抬离地板防 z-fight
			FillVerts.Add(V);
		}
	}

	// --- 勾边：每条边生成一段细长带（2 个三角形），凸/凹都天然正确 ---
	const float HalfW = FMath::Max(ZoneOutlineHalfWidthCm, 1.f);
	TArray<FVector> BandVerts;
	TArray<int32> BandTris;
	for (int32 i = 0; i < N; ++i)
	{
		const int32 j = (i + 1) % N;
		FVector2D Dir(Raw[j].X - Raw[i].X, Raw[j].Y - Raw[i].Y);
		const float Len = Dir.Size();
		if (Len < KINDA_SMALL_NUMBER) continue;
		Dir /= Len;

		const FVector2D Perp(-Dir.Y, Dir.X); // 边的垂直方向
		const float ZA = FloorZ[i] + 5.f;
		const float ZB = FloorZ[j] + 5.f;
		const int32 Base = BandVerts.Num();
		BandVerts.Add(FVector(Raw[i].X + Perp.X * HalfW, Raw[i].Y + Perp.Y * HalfW, ZA));
		BandVerts.Add(FVector(Raw[i].X - Perp.X * HalfW, Raw[i].Y - Perp.Y * HalfW, ZA));
		BandVerts.Add(FVector(Raw[j].X - Perp.X * HalfW, Raw[j].Y - Perp.Y * HalfW, ZB));
		BandVerts.Add(FVector(Raw[j].X + Perp.X * HalfW, Raw[j].Y + Perp.Y * HalfW, ZB));

		BandTris.Add(Base + 0); BandTris.Add(Base + 1); BandTris.Add(Base + 2);
		BandTris.Add(Base + 0); BandTris.Add(Base + 2); BandTris.Add(Base + 3);
	}
	const bool bBandOK = BandTris.Num() >= 3;

	if (!bFillOK && !bBandOK) return; // 全失败：不建任何 section

	// 依次建 section：先填面后勾边
	int32 SectionIdx = 0;
	TArray<FVector> UpNormal; // 全部朝上，材质 two-sided，正反不影响
	if (bFillOK)
	{
		UpNormal.Init(FVector(0.f, 0.f, 1.f), FillVerts.Num());
		ZoneMesh->CreateMeshSection(SectionIdx++, FillVerts, FillTris, UpNormal, {}, {}, {}, /*bCreateCollision=*/false);
	}
	if (bBandOK)
	{
		UpNormal.Init(FVector(0.f, 0.f, 1.f), BandVerts.Num());
		ZoneMesh->CreateMeshSection(SectionIdx++, BandVerts, BandTris, UpNormal, {}, {}, {}, /*bCreateCollision=*/false);
	}

	// 材质：优先细节面板配置，缺省回退到固定路径资产
	auto LoadMat = [](const TObjectPtr<UMaterialInterface>& ConfigMat, const TCHAR* FallbackPath) -> UMaterialInterface*
	{
		if (ConfigMat) return ConfigMat;
		return LoadObject<UMaterialInterface>(nullptr, FallbackPath);
	};
	UMaterialInterface* FillMat = LoadMat(ZoneFillMaterial, TEXT("/Game/Materials/M_ZoneMeshFill.M_ZoneMeshFill"));
	UMaterialInterface* OutlineMat = LoadMat(ZoneOutlineMaterial, TEXT("/Game/Materials/M_ZoneMeshOutline.M_ZoneMeshOutline"));

	if (bFillOK && FillMat) ZoneMesh->SetMaterial(0, FillMat);
	if (bBandOK)
	{
		if (OutlineMat) ZoneMesh->SetMaterial(bFillOK ? 1 : 0, OutlineMat);
	}

	// 诊断日志：PIE 下 Output Log 过滤 PlantZone 可看到生成情况
	UE_LOG(LogTemp, Warning, TEXT("[PlantZone] %s 网格: corners=%d fill=%s band=%s sections=%d fillMat=%s outlineMat=%s meshWorld=%s rootWorld=%s"),
		*GetName(), N,
		bFillOK ? TEXT("Y") : TEXT("N"), bBandOK ? TEXT("Y") : TEXT("N"),
		ZoneMesh->GetNumSections(),
		FillMat ? *FillMat->GetName() : TEXT("NULL"),
		OutlineMat ? *OutlineMat->GetName() : TEXT("NULL"),
		*ZoneMesh->GetComponentLocation().ToString(),
		*ZoneBox->GetComponentLocation().ToString());

	// 角点模式用网格可视化，矩形贴花收起来（避免两个框叠着显示）
	if (ZoneDecal)
	{
		ZoneDecal->SetVisibility(false);
		ZoneDecal->SetHiddenInGame(true);
	}
}

TArray<FVector> APlantZone::GetEffectiveCorners() const
{
	TArray<FVector> Corners;
	Corners.Reserve(CornerMarkers.Num());
	for (const AZoneCornerMarker* Marker : CornerMarkers)
	{
		if (Marker) Corners.Add(Marker->GetActorLocation());
	}
	return Corners;
}

bool APlantZone::ContainsMarker(const AZoneCornerMarker* Marker) const
{
	return Marker && CornerMarkers.Contains(Marker);
}

bool APlantZone::IsPointInZone(const FVector& WorldPoint) const
{
	const TArray<FVector> Corners = GetEffectiveCorners();
	const int32 N = Corners.Num();
	if (N >= 3)
	{
		// 射线法点内多边形（XY 平面，忽略 Z）
		bool bInside = false;
		int32 j = N - 1;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector& A = Corners[i];
			const FVector& B = Corners[j];
			if ((A.Y > WorldPoint.Y) != (B.Y > WorldPoint.Y))
			{
				const float XCross = (B.X - A.X) * (WorldPoint.Y - A.Y) / (B.Y - A.Y) + A.X;
				if (WorldPoint.X < XCross) bInside = !bInside;
			}
			j = i;
		}
		return bInside;
	}

	// 兜底：未配置角点标记时用 ZoneBox 矩形（支持旋转），保证老安装区不摆标记也能下包
	if (!ZoneBox) return false;
	const FVector Center = ZoneBox->GetComponentLocation();
	const FVector Extent = ZoneBox->GetScaledBoxExtent();
	const FVector Local = ZoneBox->GetComponentRotation().UnrotateVector(WorldPoint - Center);
	return FMath::Abs(Local.X) <= Extent.X && FMath::Abs(Local.Y) <= Extent.Y;
}

bool APlantZone::IsPointInAnyZone(const UObject* WorldContextObject, const FVector& WorldPoint)
{
	const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	if (!World) return false;

	for (TActorIterator<APlantZone> It(World); It; ++It)
	{
		if (It->IsPointInZone(WorldPoint)) return true;
	}
	return false;
}

TArray<FVector> APlantZone::GetDisplayCorners() const
{
	const TArray<FVector> Effective = GetEffectiveCorners();
	if (Effective.Num() >= 3) return Effective;

	// 兜底：未配置角点标记时用 ZoneBox 矩形四角（世界坐标，Z 取 0）
	TArray<FVector> Corners;
	if (!ZoneBox) return Corners;

	const FVector Center = ZoneBox->GetComponentLocation();
	const FVector Extent = ZoneBox->GetScaledBoxExtent();
	const FQuat Rot = ZoneBox->GetComponentRotation().Quaternion();
	const FVector LocalCorners[4] = {
		FVector(Extent.X, Extent.Y, 0.f),
		FVector(-Extent.X, Extent.Y, 0.f),
		FVector(-Extent.X, -Extent.Y, 0.f),
		FVector(Extent.X, -Extent.Y, 0.f)
	};
	for (const FVector& L : LocalCorners)
	{
		Corners.Add(Center + Rot.RotateVector(L));
	}
	return Corners;
}
