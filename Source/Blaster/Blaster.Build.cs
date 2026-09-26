// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Blaster : ModuleRules
{
	public Blaster(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		// MultiplayerSessions：启动菜单（Host/Join）在那个插件里，它把玩家填的 ID 暂存在
		// UMultiplayerSessionsSubsystem::PlayerId 上，ABlasterPlayerController 换图后取走用。
		// 依赖方向是单向的（游戏 → 插件），插件不知道游戏的存在。
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Niagara", "UMG", "Slate", "SlateCore", "GameplayAbilities", "GameplayTags", "GameplayTasks", "MultiplayerSessions" });

		PrivateDependencyModuleNames.AddRange(new string[] { "ProceduralMeshComponent" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
