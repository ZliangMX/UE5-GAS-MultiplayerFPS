// Valorant 顶部比分栏：左右队伍条 + 中央倒计时 + 左右回合数。
//
// 素材：E:\Notion\claude-temp\hud\top\*.png（重绘脚本 top_draw.py，同目录有 top_src.png 和 top_qa.png 校验图）
//       → 已导入 /Game/Assets/Textures/HUD/Top/T_HUDTOP_*（4x 导入）。
//
// ★ 这套素材是**矢量重绘**的，不是抠图。原因：源图里两条队伍条是半透明的，
//   实测青条 (85,115,99)、红条 (135,70,59)，都是"底色 x 背景"的混合值 ——
//   背景（木质靶场）亮一点读数就亮一点，没有可分离的纯色，抠不出干净的 alpha。
//   所以按实测轮廓参数化重画，颜色取实测值、alpha 给 0.8 左右。
//
// ★ 版面照样写在 C++ 里（理由同 ValorantBottomHUD.h：引擎没把 UWidgetTree 暴露给 Python）：
//   GLayoutPieces 一张表，两条落地路径 —— RebuildWidget 现建（无 WBP 兜底）、
//   BakeLayoutIntoWidgetBlueprint 烤进 WBP_ValorantTopHUD（之后在设计器里拖）。
//
// ★ 坐标：和 UValorantBottomHUD 同一个设计分辨率（源图 2559x1439），控件尺寸一律用 1x 数字。
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "ValorantTopHUD.generated.h"

class UCanvasPanel;
class UHorizontalBox;
class UImage;
class UTextBlock;
class UUserWidget;
class UValorantAgentPortrait;
class UWidgetTree;

UCLASS()
class BLASTER_API UValorantTopHUD : public UUserWidget
{
	GENERATED_BODY()

public:
	// 把版面烤进一个已存在的 WidgetBlueprint 资产（编辑器工具；Python 建好资产后调它）。
	// 路径形如 /Game/Blueprints/HUD/WBP_ValorantTopHUD.WBP_ValorantTopHUD。幂等：重烤先摘旧树。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	static bool BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath);

	// 版面设计分辨率（= 重绘时用的源图尺寸，和 UValorantBottomHUD 一致）
	static constexpr float DesignWidth = 2559.f;
	static constexpr float DesignHeight = 1439.f;

	// --- 队伍头像框（左右各一条长条水平框，里面是 HorizontalBox）---
	// 局内往里面塞角色头像 widget：头像从左往右自动排，尺寸由头像自己定（槽是 Automatic，不拉伸）。
	// bAlly: true = 左边我方，false = 右边敌方。SlotPadding = 头像之间的槽间距（像素）。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	void AddTeamPortrait(bool bAlly, UUserWidget* Portrait, float SlotPadding = 4.f);

	// 清空某条队伍条的头像框（换回合/换阵容重建时用）
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	void ClearTeamPortraits(bool bAlly);

	// 直接拿容器本体（想自己排、或者要按顺序找某个头像时用）
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	UHorizontalBox* GetTeamPortraitBox(bool bAlly) const;

	// 按当前 GameState 的 PlayerArray 重建两排头像（本地队伍=左我方，另一队=右敌方）。
	// ★ 每帧都会被 UpdateDisplay 调一次，但内部拿「队伍+英雄」的列表做比对，
	//   没变就直接返回 —— 不会每帧拆了重建。
	// 想留空自己控制的话，把 PortraitWidgetClass 置空并重载这个函数即可，别在别处再 Clear。
	void RefreshTeamPortraits();

	// 头像格子用哪个 widget。默认 UValorantAgentPortrait（纯 C++，没有 WBP）；
	// 想改造型（圆角/血条/大招点环）就做个 WBP 派生自它，填到这里。
	UPROPERTY(EditAnywhere, Category = "Blaster|HUD")
	TSubclassOf<UValorantAgentPortrait> PortraitWidgetClass;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	// 建 UMG 树：运行时兜底路径和"烤进 WBP"两条路共用这一份。
	static void BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root);

	// 按名字把会变的控件抓出来（代码建的树和烤进 WBP 的树用同一套名字）。
	void ResolveWidgets();

	// 数值刷新：回合数读 GameState（按本地玩家队伍分左右），倒计时读 PlayerController。
	void UpdateDisplay();

	// 整块显示/收起（Lobby 里收掉）
	void SetHUDVisible(bool bVisible);

	// 清掉某一侧的旧格子，按 Agents 的顺序重新塞一遍
	void RebuildPortraitRow(bool bAlly, const TArray<EBlasterAgent>& Agents);

private:
	// --- 左：我方回合数（数字是文本，底板是素材）---
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ScoreAllyText;

	// --- 右：敌方回合数 ---
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ScoreEnemyText;

	// --- 中：倒计时 ---
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TimerText;

	// --- 头像框里那层容器（局内往里面加头像 widget）---
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> AllyPortraitBox;
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> EnemyPortraitBox;

	// 上一帧两排头像的「队伍+英雄」快照。和新的比，一样就不重建（见 RefreshTeamPortraits）。
	// 存的是 EBlasterAgent 不是 widget 指针：人退了重进、英雄换了都能靠它发现。
	TArray<EBlasterAgent> LastAllyAgents;
	TArray<EBlasterAgent> LastEnemyAgents;
	bool bPortraitsBuilt = false;

	// 版面只建一次（RebuildWidget 会被调多次：改 DPI、加进视口、重新 TakeWidget）
	bool bLayoutBuilt = false;

	// 当前是否处于"该显示"的状态，避免每帧重复 SetVisibility
	bool bShown = false;
};
