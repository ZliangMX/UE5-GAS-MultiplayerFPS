// Valorant 局内公告面板 —— 就是 BUY PHASE 那个框。
//
// 素材：E:\Notion\claude-temp\hud\ann\*.png（重绘脚本 ann_draw.py，同目录有 top_src.png / ann_qa.png /
//       ann_undress.png 三个校验图）→ 已导入 /Game/Assets/Textures/HUD/Announce/T_Ann*（4x 导入）。
//
// ★ 和上面两个 Valorant HUD 一样是**矢量重绘**不是抠图。面板是半透明压在场景上的（实测面板内比同行
//   面板外亮 delta≈(+26,+35,+40)），背景一变换读数就跟着变，抠不出干净 alpha；文字更不能抠，要动态换。
//   反解出来是 C≈(194,196,200) @ alpha 0.30（按这个参数把源图那层反解掉以后，面板边界处接缝在 ±7
//   以内，说明解对了）。
//
// ★★ 这个类**继承 UAnnouncement**，这是故意的：
//   UAnnouncement 的五个 UTextBlock*（WarmupTime / AnnouncementText / InfoText / RoundResultText /
//   TeamSwapText）就是 BlasterPlayerController 那十几处写入的落点。继承下来以后
//   `BlasterHUD->Announcement->InfoText->SetText(...)` 这些调用**一个字都不用改**，
//   只要把 ABlasterHUD 建实例时用的类换成这个子类即可（见 BlasterHUD.cpp::AddAnnouncement）。
//
//   代价（见 NormalizeAndLayout）：那五个块在这个类里是**只读的数据源**（永远是 Collapsed，
//   不显示），面板上真正显示的是本类自己建的五个显示块。分两层的理由是"拉开字间距"不是幂等的 ——
//   把加工过的文本原地写回去，下一帧再加工一遍就会越插越宽。
//
// ★ 坐标和 UValorantTopHUD / UValorantBottomHUD 同一个设计分辨率（源图 2559x1439），一律 1x 数字。
#pragma once

#include "CoreMinimal.h"
#include "Blaster/HUD/Announcement.h"
#include "ValorantAnnouncement.generated.h"

class UBorder;
class UCanvasPanel;
class UHorizontalBox;
class UImage;
class USizeBox;
class UTextBlock;
class UVerticalBox;
class UWidgetTree;

// 面板底色三态。实测值见 fps_project_progress（六十三）：
//   中性灰 = BUY PHASE / 准备 / 结算前的常规提示
//   青     = 本回合获胜（WON）
//   红     = 本回合落败（LOST）
// ★ 三态的**不透明度也不一样**（灰 0.30，青/红 0.50）—— 结果面板明显比 buy phase 那层厚。
//   所以底色不能靠给贴图染色做（乘法只能把灰压暗，青/红需要比灰更亮更饱和，染不上去），
//   得是一块独立的纯色 Border，见 BuildLayout 的 Border_PanelFill。
UENUM(BlueprintType)
enum class EValorantAnnPanelTone : uint8
{
	// 按大字内容自己判断（"WON" -> 青，"LOST" -> 红，其它 -> 中性灰）。默认。
	Auto    UMETA(DisplayName = "Auto"),
	Neutral UMETA(DisplayName = "Neutral (grey)"),
	Won     UMETA(DisplayName = "Won (teal)"),
	Lost    UMETA(DisplayName = "Lost (red)")
};

UCLASS()
class BLASTER_API UValorantAnnouncement : public UAnnouncement
{
	GENERATED_BODY()

public:
	// 把版面烤进一个已存在的 WidgetBlueprint 资产（编辑器工具；Python 建好资产后调它）。
	// 路径形如 /Game/Blueprints/HUD/WBP_ValorantAnnouncement.WBP_ValorantAnnouncement。幂等：重烤先摘旧树。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	static bool BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath);

	// 调试用：把某个 WidgetBlueprint 里的控件树（名字 / 类 / Canvas 槽位 / 字体 / 贴图 /
	// render transform）打进日志。和 Bake 用同一套反射 ——
	// ★ 存在的理由：UWidget::WidgetTree 在 Python 那边是 **protected**，脚本
	//   `get_editor_property("widget_tree")` 会直接报错，所以烤完版面没法从 Python 核对，
	//   只能从 C++ 侧读。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	static bool DumpLayout(const FString& WidgetBlueprintObjectPath);

	// 版面设计分辨率（= 重绘时用的源图尺寸，和另外两个 Valorant HUD 一致）
	static constexpr float DesignWidth = 2559.f;
	static constexpr float DesignHeight = 1439.f;

	// 安包/拆包进度条（"DEFUSING" 那个框 + 底下分两半的条）。见 Announcement.h 里的基类说明。
	// 只记状态，真正的显隐/进度在 NormalizeAndLayout 里做（和公告那几行走同一条路）。
	virtual void SetSpikeStatus(const FString& Status, float Progress) override;

	// ★ 覆写成"只清字、不藏块"（基类是 SetVisibility(Hidden)）。理由见基类注释 ——
	//   一句话：进度条画在本 widget 里面，InProgress 每回合都来，藏了就整局看不到。
	virtual void OnRoundStarted() override;

	// 面板底色。默认 Auto（按大字内容判断）。想让结算时按输赢上色、又不想把
	// AnnouncementText 改成 "WON"/"LOST" 的话，外部显式调这个就行。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	void SetPanelTone(EValorantAnnPanelTone Tone);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	// 建 UMG 树：运行时兜底路径和"烤进 WBP"两条路共用这一份。
	static void BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root);

	// 安包/拆包那一组（标签框 + 分两半的进度条）。单独一个函数只是因为 BuildLayout 已经够长了。
	static void BuildSpikeBar(UWidgetTree* Tree, UCanvasPanel* Root);

	// 按名字把控件抓出来（代码建的树和烤进 WBP 的树用同一套名字）。
	// 继承来的那五个数据源指针也在这里赋值 —— 不赋值的话 PlayerController 那边写进来的字全丢了。
	void ResolveWidgets();

	// 把数据源里的原文加工（全大写 / 拉开字间距）后写到显示块上，并按"有没有内容"决定每一行的显隐。
	void NormalizeAndLayout();

	// 当前该用哪一档底色（Requested 是 Auto 时按大字内容判）。MainValue 是已 ToUpper 的大字。
	static EValorantAnnPanelTone ResolveTone(EValorantAnnPanelTone Requested, const FString& MainValue);

private:
	// --- 底板 ---
	// 半透明**纯色**填充。三态靠它换色，贴图 T_AnnPanel 里只有"框"（顶部高光 + 四角角标）。
	UPROPERTY(Transient) TObjectPtr<UBorder> PanelFill;
	// 框：顶部 3 行渐隐高光 + 四角角标 + 小方块，压在上面。中间是透明的。
	UPROPERTY(Transient) TObjectPtr<UImage> PanelImage;

	// --- 中间大字（显示块；内容是 AnnouncementText 或 WarmupTime 加工来的）---
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MainText;

	// --- 大字下面那行（回合结果 / 换边提示，两个都空就整行收掉）---
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> ExtraBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ExtraRoundText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ExtraSwapText;

	// --- 底部提示行（PRESS [B] <InfoText>，InfoText 空了就整行收掉）---
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> HintBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HintPressText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> InfoDisplayText;

	// --- 安包/拆包进度条（和公告面板抢同一块地方，二选一显示）---
	// Box_Spike 是整组（标签框 + 条）的竖排容器。
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> SpikeBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpikeStatusText;
	// 黄色填充外面那层 SizeBox —— 按 Progress 改 WidthOverride 就是进度。
	// （没用引擎的 UProgressBar：它自带一套皮肤，而这里只要一个纯色块。）
	UPROPERTY(Transient) TObjectPtr<USizeBox> SpikeFillBox;
	// 条那**一行**（Size_SpikeBar，标签框是它的兄弟不是子级）。只在纯倒计时
	// （Progress < 0，没人安/拆）时整行收掉 —— 那时只该剩上面那个文字框。
	UPROPERTY(Transient) TObjectPtr<USizeBox> SpikeBarCell;

	// SetSpikeStatus 推来的状态。bSpikeActive = 有人正在安/拆（Progress >= 0）
	bool bSpikeActive = false;
	float SpikeProgress = 0.f;
	FString SpikeStatus;

	// 版面只建一次（RebuildWidget 会被调多次：改 DPI、加进视口、重新 TakeWidget）
	bool bLayoutBuilt = false;

	// 底色。Requested 是外部要的（默认 Auto），Applied 是上一帧真写进 Border 的那档 ——
	// 没变就不重复 SetBrushColor（每帧写一次会白白让 Slate 重画）
	EValorantAnnPanelTone RequestedTone = EValorantAnnPanelTone::Auto;
	EValorantAnnPanelTone AppliedTone = EValorantAnnPanelTone::Neutral;
};
