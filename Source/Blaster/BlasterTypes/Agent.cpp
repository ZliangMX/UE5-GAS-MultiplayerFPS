#include "Agent.h"

namespace BlasterAgent
{
	FString ToDisplayName(EBlasterAgent Agent)
	{
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return TEXT("Jett");
		case EBlasterAgent::Sage:		return TEXT("Sage");
		case EBlasterAgent::Phoenix:	return TEXT("Phoenix");
		case EBlasterAgent::Clove:		return TEXT("Clove");
		case EBlasterAgent::None:
		default:						return TEXT("Random");
		}
	}

	bool IsPlayable(EBlasterAgent Agent)
	{
		return Agent >= FirstPlayable && Agent <= LastPlayable;
	}

	EBlasterAgent GetRandomAgent()
	{
		const int32 Index = FMath::RandRange((int32)FirstPlayable, (int32)LastPlayable);
		return (EBlasterAgent)Index;
	}

	int32 GetUltPointsRequired(EBlasterAgent Agent)
	{
		// 各英雄大招点数：Jett 7 / Phoenix 7 / Sage 7 / Clove 8。
		// 2026-09-19 按用户给的档位改过：火男 6→7、贤者 8→7、Clove 7→8（Jett 不动）。
		// 点数来源见 ABlasterPlayerState::AddUltPoints 的注释（击杀/阵亡/安包/拆包/大招球）。
		//
		// ⚠️ 每个新英雄都**显式写一条 case**，哪怕值正好等于 default 的兜底值。
		//    依赖兜底会让「改兜底值」悄悄改掉那个英雄的档位。
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return 7;
		case EBlasterAgent::Sage:		return 7;
		case EBlasterAgent::Phoenix:	return 7;
		case EBlasterAgent::Clove:		return 8;
		case EBlasterAgent::None:
		default:						return 7; // 未选英雄的兜底：按 Jett 的档
		}
	}

	FLinearColor GetAccentColor(EBlasterAgent Agent)
	{
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return FLinearColor(0.15f, 0.72f, 0.85f, 1.f); // 青
		case EBlasterAgent::Sage:		return FLinearColor(0.25f, 0.72f, 0.45f, 1.f); // 绿
		case EBlasterAgent::Phoenix:	return FLinearColor(0.95f, 0.45f, 0.15f, 1.f); // 橙
		case EBlasterAgent::Clove:		return FLinearColor(0.90f, 0.30f, 0.62f, 1.f); // 品红（暮蝶）
		case EBlasterAgent::None:
		default:						return FLinearColor(0.55f, 0.55f, 0.55f, 1.f); // 灰
		}
	}

	FString GetPortraitPath(EBlasterAgent Agent)
	{
		// 顶部比分栏头像格的贴图（UValorantAgentPortrait 用）。
		// ⚠️ 返回的是**对象路径**（包名.对象名），LoadObject 直接能吃。
		// ⚠️ 每个新英雄都显式写一条 case，理由同 GetUltPointsRequired。
		switch (Agent)
		{
		case EBlasterAgent::Jett:		return TEXT("/Game/Assets/Textures/HUD/Portraits/T_AgentPortrait_Jett.T_AgentPortrait_Jett");
		case EBlasterAgent::Sage:		return TEXT("/Game/Assets/Textures/HUD/Portraits/T_AgentPortrait_Sage.T_AgentPortrait_Sage");
		case EBlasterAgent::Phoenix:	return TEXT("/Game/Assets/Textures/HUD/Portraits/T_AgentPortrait_Phoenix.T_AgentPortrait_Phoenix");
		case EBlasterAgent::Clove:		return TEXT("/Game/Assets/Textures/HUD/Portraits/T_AgentPortrait_Clove.T_AgentPortrait_Clove");
		case EBlasterAgent::None:
		default:						return FString(); // 空 = 不画头像，只留描边环
		}
	}
}
