#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "AgentSelectStrip.generated.h"

class UButton;
class UTextBlock;

// Lobby 底部共用英雄条（Valorant 式）。四张卡：Jett / Sage / Phoenix / Random。
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
	void HandleRandomClicked();

private:
	static constexpr int32 NumCards = 4;

	void BuildTree();
	void RequestSelect(EBlasterAgent Agent);
	void ApplySelectionVisuals();

	EBlasterAgent CardAgents[NumCards] = {
		EBlasterAgent::Jett,
		EBlasterAgent::Sage,
		EBlasterAgent::Phoenix,
		EBlasterAgent::None   // None = "随机"
	};

	// 由 WidgetTree 持有（children），此处仅作引用；勿 UPROPERTY 包装成 C 数组
	UButton* CardButtons[NumCards] = {};
	UTextBlock* CardLabels[NumCards] = {};

	EBlasterAgent CurrentAgent = EBlasterAgent::None;
	bool bTreeBuilt = false;
};
