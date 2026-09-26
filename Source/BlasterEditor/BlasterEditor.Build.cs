// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class BlasterEditor : ModuleRules
{
	public BlasterEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"ComponentVisualizers",
			// 读动画蓝图结构（读图里的 Slot / LayeredBoneBlend 节点与连线）要用 AnimGraph 的节点类
			"AnimGraph",
			"AnimGraphRuntime",
			"Blaster"
		});
	}
}
