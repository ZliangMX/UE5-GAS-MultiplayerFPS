// 顶部比分栏里的**单个英雄头像格**（36x36 设计像素）。
//
// 三层同心方（Overlay 里三个 UImage，各自 padding 差 1px）：
//   外层  = 英雄强调色（BlasterAgent::GetAccentColor）→ 1px 描边环
//   中层  = 深色底 → 挡住英雄 PNG 的透明区，别让游戏画面透上来
//   内层  = 头像贴图（32x32）
//
// ★ 贴图路径查表在 BlasterAgent::GetPortraitPath（Agent.cpp），和英雄名/强调色/大招点
//   放一起 —— 加新英雄还是只改那一个文件。
//
// ★ 纯 C++ 建树，没有对应 WBP：这个格子就三层图，没必要开资产。
//   想改样式（圆角、血条、大招点环）可以建个 WBP 派生自它，
//   然后把它填给 UValorantTopHUD::PortraitWidgetClass。
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "ValorantAgentPortrait.generated.h"

class UImage;

UCLASS()
class BLASTER_API UValorantAgentPortrait : public UUserWidget
{
	GENERATED_BODY()

public:
	// 格子边长（设计像素）。顶部 HUD 那两条头像框按这个尺寸排 5 个还绰绰有余。
	static constexpr float TileSize = 36.f;

	// 换英雄：重刷贴图和描边色。传 None 只留边框（还没 roll 到英雄时的占位）。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	void SetAgent(EBlasterAgent NewAgent);

	UFUNCTION(BlueprintPure, Category = "Blaster|HUD")
	EBlasterAgent GetAgent() const { return Agent; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	// 把当前 Agent 的颜色和贴图刷到三个 Image 上（树建好之后调，改 Agent 后再调）
	void ApplyAgent();

	// 描边环（英雄强调色）
	UPROPERTY(Transient) TObjectPtr<UImage> FrameImage;

	// 深色底
	UPROPERTY(Transient) TObjectPtr<UImage> BackImage;

	// 头像本体
	UPROPERTY(Transient) TObjectPtr<UImage> PortraitImage;

	EBlasterAgent Agent = EBlasterAgent::None;

	// 版面只建一次（RebuildWidget 会被调多次，理由同 UValorantTopHUD）
	bool bBuilt = false;
};
