#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PlantZone.generated.h"

class UPlantZoneBoxComponent;
class UDecalComponent;
class UProceduralMeshComponent;
class UMaterialInterface;
class AZoneCornerMarker;

// 可下包区域：只有身处此区域内的 TeamA 角色才能下包（服务器按多边形点内判定）。
// 区域形状 = 场景里摆放的 ZoneCornerMarker 角点标记连成的多边形（俯视 2D，Z 忽略），任意凸/凹形状都支持。
// 同一个多边形同时用于：① 服务器下包判定 ② 小地图显示。
// 兜底：未配置角点标记时退化为 ZoneBox 的矩形（老安装区也能用）。
UCLASS()
class BLASTER_API APlantZone : public AActor
{
	GENERATED_BODY()

public:
	APlantZone();

	virtual void BeginPlay() override;

	// 判断世界坐标点是否在任意一个安装区多边形内（服务器下包判定用）
	static bool IsPointInAnyZone(const UObject* WorldContextObject, const FVector& WorldPoint);

	// 判断点是否在本区域内（2D 射线法点内多边形，忽略 Z；未配置角点时用 Box 矩形兜底）
	bool IsPointInZone(const FVector& WorldPoint) const;

	// 区域显示多边形（优先角点标记，未配置时用 ZoneBox 兜底成矩形四角）。小地图组件读取用
	TArray<FVector> GetDisplayCorners() const;

	// 该角点标记是否被本区域引用（编辑器可视化器拖动标记时查找所属区域用）
	bool ContainsMarker(const AZoneCornerMarker* Marker) const;

	// --- 局内地面可视化（任意多边形：半透明填面 + 描边勾边）---
	// 勾边半宽（cm）：沿每条边生成一段 2×该值 宽的亮边带
	UPROPERTY(EditDefaultsOnly, Category = "Plant Zone")
	float ZoneOutlineHalfWidthCm = 20.f;

	// 填面 / 勾边材质。为空时回退到 /Game/Materials/M_ZoneMeshFill、M_ZoneMeshOutline
	UPROPERTY(EditDefaultsOnly, Category = "Plant Zone")
	TObjectPtr<UMaterialInterface> ZoneFillMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "Plant Zone")
	TObjectPtr<UMaterialInterface> ZoneOutlineMaterial;

private:
	// 有效角点：从角点标记读取世界坐标（俯视 2D）
	TArray<FVector> GetEffectiveCorners() const;

	// 区域角点标记：在编辑器里摆放小球标注轮廓角点，运行时隐藏。选中安装区后在此数组里拾取它们即可
	UPROPERTY(EditInstanceOnly, Category = "Plant Zone")
	TArray<AZoneCornerMarker*> CornerMarkers;

	// 区域几何根组件（仅作根变换 + 地面贴花锚点；碰撞/重叠不再用于玩法判定）。
	// 用专用子类 UPlantZoneBoxComponent 便于编辑器可视化器精准挂载。
	UPROPERTY(VisibleAnywhere, Category = "Plant Zone")
	UPlantZoneBoxComponent* ZoneBox;

	// 地面描边贴花（朝下投影，M_ZoneOutline 材质）。
	// 注意：多边形区域与矩形贴花可能不完全吻合，如需精确可用自定义多边形贴花/网格。
	UPROPERTY(VisibleAnywhere, Category = "Plant Zone")
	UDecalComponent* ZoneDecal;

	// 运行时程序化地面网格：BeginPlay 从角点生成「半透明填面 + 勾边」（任意凸/凹多边形）。
	// 角点标记是关卡里摆的 actor、各端都有 → 每个客户端本地生成即可，零网络复制。
	UPROPERTY(VisibleAnywhere, Category = "Plant Zone")
	UProceduralMeshComponent* ZoneMesh;

	// 用当前角点重建地面网格（有效角点 <3 时保持矩形贴花兜底，不建网格）
	void BuildZoneVisual();
};
