// Copyright PingLiangYe. All Rights Reserved.

using UnrealBuildTool;

public class FbxPipeline : ModuleRules
{
	public FbxPipeline(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
			// 后续版本加窗口/导入执行时会补：Slate、SlateCore、UnrealEd、WorkspaceMenuStructure、EditorStyle。
		});
	}
}
