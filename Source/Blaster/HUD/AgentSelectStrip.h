#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "AgentSelectStrip.generated.h"

class UButton;
class UTextBlock;

// Lobby 底部共用英雄条（Valorant 式）。五张卡：Jett / Sage / Phoenix / Clove / Random。
// 纯 C++ 自建 WidgetTree，由 ULobbyOverlay 挂进其根 Canvas 底部中央（随 overlay 一起进出）。
// 数据源 = 本地玩家 PlayerState.Agent（复制），Overlay 每轮刷新把值推进来画高亮。
UCLASS()
class BLASTER_API UAgentSelectStrip : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual bool Initialize() override;

	// 把选中高亮切到对应卡（None → 高亮 Random 卡）。未变化则跳过。
	void SetCurrentAgent(EBlasterAgent Agent);
	FORCEINLINE EBlasterAgent GetCurrentAgent() const { return CurrentAgent; }

protected:
	UFUNCTION()
	void HandleJettClicked();

	UFUNCTION()
	void HandleSageClicked();

	UFUNCTION()
	void HandlePhoenixClicked();

	UFUNCTION()
	void HandleCloveClicked();

	UFUNCTION()
	void HandleRandomClicked();

private:
	// ⚠️ 这个数组**和 EBlasterAgent 没有任何自动联系** —— 加英雄时枚举改了这里不会跟着变，
	//    漏改的表现是「新英雄在大厅里根本不出现」，不报错。改这里的同时必须同步
	//    NumCards、CardAgents、下面 switch 里的点击绑定、以及 AgentSelectStrip.cpp 的
	//    HandleXxxClicked 实现（四处，缺一不可）。
	static constexpr int32 NumCards = 5;

	void BuildTree();
	void RequestSelect(EBlasterAgent Agent);
	void ApplySelectionVisuals();

	EBlasterAgent CardAgents[NumCards] = {
		EBlasterAgent::Jett,
		EBlasterAgent::Sage,
		EBlasterAgent::Phoenix,
		EBlasterAgent::Clove,
		EBlasterAgent::None   // None = "随机"
	};

	// 由 WidgetTree 持有（children），此处仅作引用；勿 UPROPERTY 包装成 C 数组
	UButton* CardButtons[NumCards] = {};
	UTextBlock* CardLabels[NumCards] = {};

	EBlasterAgent CurrentAgent = EBlasterAgent::None;
	bool bTreeBuilt = false;
};
