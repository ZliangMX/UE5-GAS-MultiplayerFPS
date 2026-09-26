#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// 编辑器专属模块：注册视口预览可视化器（FBlasterVisualizer）——
// APlantZone 的角点连线轮廓 + ASpike 的方块碰撞体尺寸预览。
// 仅编辑器目标编译/加载，不会进入游戏包。
class FBlasterEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// 真正向 GUnrealEd 注册可视化器。必须在 GUnrealEd 已创建后调用（StartupModule 或 OnPostEngineInit）
	static void RegisterBlasterVisualizers();
};
