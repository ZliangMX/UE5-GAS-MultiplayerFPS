#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ZoneCornerMarker.generated.h"

class UZoneCornerMarkerComponent;

// 安装区角点标记：在编辑器里摆放的小球，用来"可视化"地标注 APlantZone 的轮廓角点。
// 运行时隐藏、无碰撞，仅作为 APlantZone 读取世界坐标来定义区域的锚点。
UCLASS()
class BLASTER_API AZoneCornerMarker : public AActor
{
	GENERATED_BODY()

public:
	AZoneCornerMarker();

private:
	// 编辑器里可见、运行时隐藏的小球标记（专用组件类便于编辑器可视化器挂载）
	UPROPERTY(VisibleAnywhere, Category = "Corner Marker")
	UZoneCornerMarkerComponent* MarkerMesh;
};
