// Valorant 底部 HUD：Jett 技能条（C/Q/E/X）+ 血/甲/弹那圈装饰。
//
// 素材：E:\Notion\claude-temp\hud\out\4x\*.png（抠图过程 + 坐标表见同目录 README.txt）
//       → 已导入 /Game/Assets/Textures/HUD/Jett/T_HUD_*（4x 导入）。
//
// ★ 为什么版面写在 C++ 里，而不是在 UMG 设计器里摆：
//   引擎没把 UWidgetTree 暴露给 Python（unreal.WidgetTree 类根本不存在，UWidgetBlueprint 也没有
//   widget_tree 属性 —— 实测过），所以脚本生成不了控件树，只能在 C++ 里建。
//   于是版面写成一张表（.cpp 里的 GLayoutPieces），有两条落地路径：
//     1) 纯代码：RebuildWidget 里建树（没有挂 WBP 时兜底，CreateWidget 直接用这个 C++ 类就行）；
//     2) 烤进资产：BakeLayoutIntoWidgetBlueprint 把同一张表烤进 WBP_ValorantBottomHUD ——
//        烤完 WBP 里就有摆好的 Image/TextBlock，可以在 UMG 设计器里拖拽微调（那时以 WBP 为准）。
//
// ★ 坐标：源图 2559x1439（README 坐标表就是这个框里的像素），控件尺寸一律用 1x 数字。
//   贴图是 4x 的，缩到 1/4 显示 —— 缩比整数、边缘比 1x 图干净得多。
//   锚点分三类：血/甲贴左、技能条与飞线贴底中、弹药贴右。像素尺寸不随分辨率缩放（和 Valorant 一样）。
//
// 数据源和 USkillBarWidget 同源：本地 ASC 直读 / 观战读服务器快照；血甲弹读角色的复制属性。
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "ValorantBottomHUD.generated.h"

class UCanvasPanel;
class UImage;
class UTextBlock;
class UTexture2D;
class UWidgetTree;
class ABlasterCharacter;
class ABlasterPlayerController;

// 一个普通技能槽用到的 5 个控件（C / Q / E 各一组）。
// 包成 USTRUCT 是为了能挂 UPROPERTY —— 指针指向的都是 WidgetTree 里的控件（树自己就是 UPROPERTY，
// 控件不会被 GC），标 UPROPERTY 只是跟工程里其它 widget 的 BindWidget 写法保持一致。
USTRUCT()
struct FValorantSkillSlotWidgets
{
	GENERATED_BODY()

	// 底框
	UPROPERTY(Transient) TObjectPtr<UImage> Frame;
	// 技能图标（没充能时压到 42% 透明度 = 素材里 _gray 那版的透明度）
	UPROPERTY(Transient) TObjectPtr<UImage> Icon;
	// 充能条（有充能 = 青蓝）。**按格**显示：素材本身分格，剩几格就只画左边那几格，
	// 用掉的格不画（不是压扁成半条进度条）—— 实现是槽宽 + UVRegion 同比例缩，见 .cpp。
	UPROPERTY(Transient) TObjectPtr<UImage> BarFill;
	// 键位字母（素材是字形图，不是文本）
	UPROPERTY(Transient) TObjectPtr<UImage> Key;
	// 框右上角那根小横杠 = "这个技能现在可用"
	UPROPERTY(Transient) TObjectPtr<UImage> Dash;

	// 充能条的"满格"宽度（像素，= 版面里那条的原始宽度）。刷新时把 slot 宽度缩到
	// BarFullWidth * (剩几格/总格数)，并同步缩 UV —— 用掉的格子直接不进采样。
	// 值不写在这张表里，而是 ResolveWidgets 时从 slot 现读（在建树/WBP 里那份版面定下来之后读，
	// 这样你在 UMG 设计器里把条拖宽了也认）；取到的是所有调用里的最大值，不会被刷窄的宽度污染。
	float BarFullWidth = 0.f;

	// 整个槽显示/隐藏（没这个技能时整组收掉，不留空框）
	void SetVisible(bool bVisible) const;
};

UCLASS()
class BLASTER_API UValorantBottomHUD : public UUserWidget
{
	GENERATED_BODY()

public:
	// 把版面烤进一个已存在的 WidgetBlueprint 资产（编辑器工具；Python 建好资产后调它）。
	// 路径形如 /Game/Blueprints/HUD/WBP_ValorantBottomHUD.WBP_ValorantBottomHUD。
	// 幂等：重烤会先摘掉旧版面（RemoveWidget 递归）。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	static bool BakeLayoutIntoWidgetBlueprint(const FString& WidgetBlueprintObjectPath);

	// 把 WBP 里烤好的控件树打到日志里核对（UWidget::WidgetTree 在 Python 里是 protected，
	// 只能走 C++）。和 UValorantAnnouncement::DumpLayout 一个用途：版面加了新件、重烤完
	// 之后确认它真的进了树 —— 树以 WBP 为准，没进去 ResolveWidgets 就是找不到。
	// 日志用 Display 级：headless 命令行的控制台会把 Log 级过滤掉。
	UFUNCTION(BlueprintCallable, Category = "Blaster|HUD")
	static bool DumpLayout(const FString& WidgetBlueprintObjectPath);

	// 版面设计分辨率（= 抠图源图尺寸）。坐标表里的数字都是这个框里的像素。
	// ★ 这只是"版面表的坐标系"，不是 UMG 画布的坐标系 —— 画布的 slate 尺寸是工程 DPI 曲线定的
	//   （本工程 = 引擎默认曲线，恒等于 1920x1080）。两者差 1.33 倍，入槽前要换算，
	//   见 .cpp 里的 LayoutScale。直接拿设计像素当 slot 偏移写过一次，结果是底栏糊成一坨。
	static constexpr float DesignWidth = 2559.f;
	static constexpr float DesignHeight = 1439.f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	// 建 UMG 树：运行时兜底路径和"烤进 WBP"两条路共用这一份。
	static void BuildLayout(UWidgetTree* Tree, UCanvasPanel* Root);

	// 按名字把"会变的那几个"控件抓出来（代码建的和烤进 WBP 的用同一套名字）。
	void ResolveWidgets();

	// 数值刷新：血/甲/弹走角色复制属性，技能/大招走 ASC（和 USkillBarWidget 同源）。
	void UpdateDisplay();

	// 把整块 HUD 显示/收起（Lobby、阵亡、没有显示目标时收掉）
	void SetHUDVisible(bool bVisible);

private:
	// 找出某个技能条槽位对应的那组控件（SkillSlotIndex 0/1/2 → C/Q/E 三组；
	// 大招不在里面，它只有图标 + 7 格）。★ 认的是槽位号不是技能类型 —— 见 .cpp 里的说明。
	FValorantSkillSlotWidgets* FindSlotWidgets(int32 SkillSlotIndex);

	// --- 左侧：护甲六边 + 血线 ---
	UPROPERTY(Transient) TObjectPtr<UImage> HexArmor;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ArmorText;
	// 血线分两层：底（整条 42% 透明）+ 面（按当前血量横向缩短）。
	// 素材是一条等宽的线，横向压扁 == 裁短（视觉上没区别），所以不用额外做遮罩。
	UPROPERTY(Transient) TObjectPtr<UImage> HealthTrack;
	UPROPERTY(Transient) TObjectPtr<UImage> HealthFill;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HealthText;

	// --- 右侧：弹药线 + 菱形 + 数字 ---
	UPROPERTY(Transient) TObjectPtr<UImage> AmmoLine;
	UPROPERTY(Transient) TObjectPtr<UImage> AmmoIcon;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AmmoText;

	// --- 底中：大招（X）---
	UPROPERTY(Transient) TObjectPtr<UImage> FrameX;
	UPROPERTY(Transient) TObjectPtr<UImage> IconX;
	// 7 格充能点：按比例点亮（点亮图 / 未点亮图两张，刷新时换 brush）
	UPROPERTY(Transient) TArray<TObjectPtr<UImage>> UltPips;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> UltTimerText;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> PipLitTexture;
	UPROPERTY(Transient) TObjectPtr<UTexture2D> PipEmptyTexture;

	// --- 底中：C / Q / E 三个普通技能槽（按 SkillSlotIndex 0/1/2 认格，与英雄无关）---
	UPROPERTY(Transient) FValorantSkillSlotWidgets SkillSlotC;  // SkillSlotIndex 0
	UPROPERTY(Transient) FValorantSkillSlotWidgets SkillSlotQ;  // SkillSlotIndex 1
	UPROPERTY(Transient) FValorantSkillSlotWidgets SkillSlotE;  // SkillSlotIndex 2

	// 版面只建一次（RebuildWidget 会被调多次：改 DPI、加进视口、重新 TakeWidget）
	bool bLayoutBuilt = false;
};
