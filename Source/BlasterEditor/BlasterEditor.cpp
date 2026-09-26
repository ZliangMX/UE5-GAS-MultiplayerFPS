#include "BlasterEditor.h"

#include "Modules/ModuleManager.h"

#include "ComponentVisualizer.h"
#include "ComponentVisualizers.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "EngineUtils.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

#include "Blaster/PlantZone/PlantZone.h"
#include "Blaster/PlantZone/ZoneCornerMarker.h"
#include "Blaster/Spike/Spike.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"

#define LOCTEXT_NAMESPACE "FBlasterEditorModule"

// 编辑器视口预览可视化器（挂在 UBoxComponent / UStaticMeshComponent 基类上，见下方注册处）。
// 目前管两件事，都按 Owner 类型守卫，不影响场景里其它 Box/Mesh：
//  - 安装区形状：把角点标记连成多边形线框，实时显示区域形状。
//      选中安装区 → 画它自己的多边形；
//      选中/拖动角点标记 → 画所有引用该标记的安装区（拖动时光看线框就知道区域长什么样）。
//  - Spike 方块碰撞体：把物理/拾取用的那个盒子画成橙色有向线框，配合
//      `CollisionBoxExtentOverride` 手调尺寸（改属性线框实时跟手，不用进 PIE）。
class FBlasterVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
};

void FBlasterVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	// 一次性诊断日志：确认可视化器有没有被引擎真正调用（选中/悬停组件时）
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogTemp, Warning, TEXT("[BlasterEditor] FBlasterVisualizer::DrawVisualization first call, component=%s owner=%s"),
			Component ? *Component->GetName() : TEXT("null"),
			Component && Component->GetOwner() ? *Component->GetOwner()->GetName() : TEXT("null"));
	}

	if (!PDI || !Component) return;

	const AActor* Owner = Component->GetOwner();

	// —— Spike 方块碰撞体 ——
	// 用 ComputeCollisionBoxExtent() 而不是组件当前的 extent：编辑器里 PostInitializeComponents
	// 未必跑过，直接读组件会画到构造器那个占位尺寸（25/25/40）上。
	if (const ASpike* Spike = Cast<ASpike>(Owner))
	{
		if (const UBoxComponent* Box = Spike->CollisionBox)
		{
			const FTransform BoxTransform = Box->GetComponentTransform();
			const FVector Extent = Spike->ComputeCollisionBoxExtent();

			// 橙色：和 PIE 里 bDrawDebugCollisionBox 画的颜色一致，对照着调不会串。
			// 注意 6 号参数是**一个 FVector**（引擎同时有个三分量的重载，别传成 Extent.X/Y/Z）
			DrawOrientedWireBox(PDI, BoxTransform.GetLocation(),
				BoxTransform.GetUnitAxis(EAxis::X), BoxTransform.GetUnitAxis(EAxis::Y), BoxTransform.GetUnitAxis(EAxis::Z),
				Extent, FLinearColor(1.f, 0.55f, 0.1f), SDPG_Foreground, 2.f);
		}
	}

	// 收集要画线框的安装区列表
	TArray<const APlantZone*> ZonesToDraw;

	if (const APlantZone* Zone = Cast<APlantZone>(Owner))
	{
		ZonesToDraw.Add(Zone);
	}
	else if (const AZoneCornerMarker* Marker = Cast<AZoneCornerMarker>(Owner))
	{
		for (TActorIterator<APlantZone> It(Component->GetWorld()); It; ++It)
		{
			if (It->ContainsMarker(Marker))
			{
				ZonesToDraw.Add(*It);
			}
		}
	}

	// 亮青色线框：角点标记连成闭合多边形（俯视 2D，忽略 Z）
	const FLinearColor LineColor(0.2f, 0.85f, 1.f);
	constexpr uint8 DepthPriority = SDPG_Foreground;
	constexpr float Thickness = 2.f;

	for (const APlantZone* Zone : ZonesToDraw)
	{
		const TArray<FVector> Corners = Zone->GetDisplayCorners();
		const int32 N = Corners.Num();
		if (N < 3) continue;

		for (int32 i = 0; i < N; ++i)
		{
			PDI->DrawLine(Corners[i], Corners[(i + 1) % N], LineColor, DepthPriority, Thickness);
		}
	}
}

void FBlasterEditorModule::StartupModule()
{
	// 注册必须等到 GUnrealEd 已创建之后：FComponentVisualizersModule::RegisterComponentVisualizer
	// 内部 `if (GUnrealEd != NULL)`，GUnrealEd 为 null 时会静默丢弃（默认阶段模块早于它创建，
	// 之前就因此注册失败、DrawVisualization 永不触发）。延迟到 OnPostEngineInit 再注册，
	// 同时避免在 GUnrealEd 为 null 时强拉加载 ComponentVisualizers（那会把它自己的可视化器也提前
	// 加载并被同样丢弃）。
	if (GUnrealEd)
	{
		RegisterBlasterVisualizers();
	}
	else
	{
		FCoreDelegates::OnPostEngineInit.AddLambda([]()
		{
			RegisterBlasterVisualizers();
		});
	}
}

void FBlasterEditorModule::RegisterBlasterVisualizers()
{
	// 挂到基类 UBoxComponent / UStaticMeshComponent：
	// 引擎的 FindComponentVisualizer 会沿继承链向上匹配基类，这样现有的 BP_PlantZone
	// （老蓝图 SCS 里 ZoneBox 可能是普通 UBoxComponent）和 BP_ZoneMarker 不用重建蓝图就能命中。
	// DrawVisualization 内部按 Owner 类型守卫（APlantZone / AZoneCornerMarker / ASpike），
	// 场景里其它 Box/Mesh 不受影响。
	FComponentVisualizersModule& ComponentVisualizersModule =
		FModuleManager::LoadModuleChecked<FComponentVisualizersModule>("ComponentVisualizers");

	ComponentVisualizersModule.RegisterComponentVisualizer(
		UBoxComponent::StaticClass()->GetFName(),
		MakeShareable(new FBlasterVisualizer));
	ComponentVisualizersModule.RegisterComponentVisualizer(
		UStaticMeshComponent::StaticClass()->GetFName(),
		MakeShareable(new FBlasterVisualizer));

	// 诊断：确认注册真的进了 GUnrealEd->ComponentVisualizerMap
	UE_LOG(LogTemp, Warning, TEXT("[BlasterEditor] RegisterBlasterVisualizers: GUnrealEd=%s, findAfterBox=%s, findAfterMesh=%s"),
		GUnrealEd ? TEXT("valid") : TEXT("NULL"),
		GUnrealEd && GUnrealEd->FindComponentVisualizer(UBoxComponent::StaticClass()).IsValid() ? TEXT("FOUND") : TEXT("NOT_FOUND"),
		GUnrealEd && GUnrealEd->FindComponentVisualizer(UStaticMeshComponent::StaticClass()).IsValid() ? TEXT("FOUND") : TEXT("NOT_FOUND"));
}

void FBlasterEditorModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlasterEditorModule, BlasterEditor)
