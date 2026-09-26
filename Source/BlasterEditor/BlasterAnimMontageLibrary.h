// 编辑器用的动画蒙太奇拼装工具。
//
// ⚠️ 存在的唯一理由：**蒙太奇的槽轨道和分段从 Python 改不了**。
//    UAnimMontage::SlotAnimTracks / CompositeSections 都是 `UPROPERTY()`（没带 Edit/Blueprint
//    标记），UE 的 Python 胶水层把这种属性一律当"protected"拒绝访问：
//        AnimMontage: Failed to find property 'slot_anim_tracks'
//    而 Python 里连 FSlotAnimationTrack / FAnimTrack 这两个结构体都没暴露（unreal.AnimSegment
//    和 unreal.CompositeSection 有，但它们装不进那两个数组）。所以只能下沉到 C++。
//
// 这里只做"往一条已存在的蒙太奇里填内容"，**不负责建资产/存盘** ——
// 建资产走 Python 的 unreal.AnimMontageFactory（已验证可行），存盘走
// EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)（见 ue_python_bp_api_gotchas）。
// 职责这样切，是为了让这个类里面只剩"拼数据"这一件事需要看懂。

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlasterAnimMontageLibrary.generated.h"

class UAnimMontage;
class UAnimSequenceBase;

UCLASS()
class BLASTEREDITOR_API UBlasterAnimMontageLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/*
	 * 把 InMontage 重新填成一条"时间轴型"空手蒙太奇：Intro → Loop(自接自，无限循环) → Outro。
	 *
	 * 三段各自独占一个槽轨道上的连续片段，起点依次累加；分段名、下一段指向由参数给。
	 * 中间那段自接自（NextSectionName = 自己）是"无限循环"的关键 ——
	 * FAnimMontageInstance::Advance 用的是 NextSections[CurrentSectionIndex]，
	 * 指回自己就一直原地循环（见 ABlasterCharacter::EndCloudburstHold 靠 JumpToSection("Outro") 跳出）。
	 *
	 * @param InMontage        目标蒙太奇（已存在，Python 那边用 AnimMontageFactory 建好的）
	 * @param InSettingsTemplate 可空。非空就照抄它的混合设置（BlendIn/BlendOut/BlendOutTriggerTime/
	 *                         BlendMode/BlendProfile/bEnableAutoBlendOut/RateScale）。
	 *                         传本工程已有的空手蒙太奇（FP_Wushu_S0_E_Dash_Montage）可以保证
	 *                         新蒙太奇的混合行为和它们一致，不用去猜那几个值该填多少。
	 * @param InSlotName       槽名（FP 用 DefaultSlot，第三人称上半身用 UpperBody）
	 * @param bLoopMiddle      中间段是否自接自（true = 无限循环）
	 * @return 成功与否。失败会打 Warning 说明原因。
	 */
	UFUNCTION(BlueprintCallable, Category = "Blaster|Animation")
	static bool BuildTimelineMontage(
		UAnimMontage* InMontage,
		UAnimMontage* InSettingsTemplate,
		UAnimSequenceBase* InIntro,
		UAnimSequenceBase* InLoop,
		UAnimSequenceBase* InOutro,
		FName InSlotName,
		FName InIntroSection,
		FName InLoopSection,
		FName InOutroSection,
		bool bLoopMiddle);

	/*
	 * 把一条蒙太奇的槽/片段/分段结构导出成一段**纯 ASCII** 的文字，用于验收。
	 *
	 * 为什么需要它：蒙太奇的槽轨道和分段从 Python 读不出来（和写不进去是同一个原因），
	 * 所以"新进程重新载入、确认真的存对了"这件事，只能借 C++ 的嘴。
	 * 返回字符串而不是直接 UE_LOG：这样 Python 能把它写进 UTF-8 的报告文件，
	 * 不用跟 engine log 的编码较劲（中文在 log 里会变成 ?）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Blaster|Animation")
	static FString DescribeMontage(UAnimMontage* InMontage);

	/*
	 * 把一条已有蒙太奇里的某个分段改成**自接自**（NextSectionName = 自己），也就是让整条蒙太奇无限循环。
	 *
	 * 存在的理由和上面两个一样：这件事在 Python 里做不了（CompositeSections 是 protected）。
	 * 而它是个很容易漏的坑 —— 蒙太奇的"循环"不写在蒙太奇对象上，写在那一段的"下一段"字段里，
	 * 蒙太奇编辑器里也是藏在选中分段后的 Section 详情里。漏了的话表现是：
	 * **蒙太奇照播、播完就停**，槽位让回动画机、姿势弹回去 ——
	 * 从画面上看很像"动作被打断/又重新做了一遍"，但和"没播"完全不是一回事，很难往这上面想。
	 *
	 * ⚠ 只对**单段**蒙太奇用（本工程现有的拆包待命那两条就是）。多段的循环该走 BuildTimelineMontage
	 *   的 bLoopMiddle，那里要连 Intro/Outro 一起排才对。
	 *
	 * @param InSectionName 要循环的分段名。传 None 表示"这条蒙太奇里唯一的那一段"。
	 * @return 成功与否。失败会打 Warning 并把实际存在的段名列出来。
	 */
	UFUNCTION(BlueprintCallable, Category = "Blaster|Animation")
	static bool SetMontageSectionLoop(UAnimMontage* InMontage, FName InSectionName = NAME_None);
};
