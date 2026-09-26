// Copyright PingLiangYe. All Rights Reserved.

#include "Modules/ModuleManager.h"

/** FbxPipeline 编辑器插件模块。
 *
 *  当前版本 (0.1) 只含两块：
 *    1) 命名校验核心 FbxNaming（纯 C++、零编辑器依赖，可跑自动化测试）
 *    2) 针对命名规则的自动化测试（Automation 面板里跑）
 *
 *  菜单入口 / Slate 窗口 / FBX 批量导入执行器在后续版本加入，
 *  届时在这个模块的 StartupModule 里注册 Editor Delegates 和窗口。
 */
class FFbxPipelineModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FFbxPipelineModule, FbxPipeline)
