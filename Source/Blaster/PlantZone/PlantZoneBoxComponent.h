#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "PlantZoneBoxComponent.generated.h"

// 安装区专用 Box 根组件。
// 编辑器里的 FPlantZoneVisualizer 挂在它上面（实际通过基类 UBoxComponent 匹配）。
// 注意：UBoxComponent 本身没有 Blueprintable（只有 BlueprintSpawnableComponent），
// 所以子类默认不能建蓝图；这里显式加 Blueprintable 才能右键创建蓝图子类。
UCLASS(Blueprintable)
class BLASTER_API UPlantZoneBoxComponent : public UBoxComponent
{
	GENERATED_BODY()
};
