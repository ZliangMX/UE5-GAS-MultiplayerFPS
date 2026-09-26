#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "ZoneCornerMarkerComponent.generated.h"

// 角点标记专用 StaticMesh 组件：与普通 UStaticMeshComponent 区分开，
// 编辑器可视化器同时挂在这上面，这样拖动角点标记时也能实时画出所属安装区的多边形线框。
UCLASS()
class BLASTER_API UZoneCornerMarkerComponent : public UStaticMeshComponent
{
	GENERATED_BODY()
};
