// 技能投掷物动画的"这一段收尾演完了"通知。
//
// ★ 摆哪儿：**摆在第三人称动画里**，具体是**两段收尾动作**（丢出去 Throw / 放下 Lift）的末尾。
//   摆在末尾 = "收尾演到底了" —— 代码那边收到通知就掏枪（时机由**动画**说了算，
//   不再由代码按资产长度算的秒数猜）。按约定要等两件事都齐了：动作演完、枪回手里。
//
// 拿起 / 按住那两段**不用摆**：它们演完不需要 C++ 做任何事（之后就是动画蓝图那个
// "举着它待命"的姿势，见 ABlasterCharacter::PlayThrowableSet 上头那段）。
//
// 和 UAnimNotify_EmptyHandFinished 是同一套用法（那条是空手拿枪那一路，这条是持技能投掷物这一路）：
//   · 通知是**正门**，角色里那根按 Length 起的计时器只是**备胎**（漏摆 / 资产长度取不到时兜底）；
//   · 两边都进 ABlasterCharacter::ThrowableFinisherFinished，先到的生效、后到的天然是空操作，
//     所以漏摆、或者摆多了都不会出错；
//   · 裸序列（AnimSequence）当资产时，它自己身上摆的通知照样会响（动态蒙太奇会转发事件）。
//
// ⚠️ 这是**类通知**（UAnimNotify 子类），不会触发动画蓝图里那些 AnimNotify_* 事件 ——
//    要蓝图那边同时收到的话得用 UAnimNotify_BlueprintSpawnable 或者加个 AnimNotifyState。
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"

#include "AnimNotify_ThrowableSkillFinished.generated.h"

/**
 * 技能投掷物（Phoenix E 闪光 / Q 火球 / C 火墙）的收尾那一段演完了 —— 把枪掏回来。
 *
 * 摆第三人称那两段收尾动作（丢出去 / 放下）的末尾。摆手模上的会被忽略
 *（掏枪是身体 + 手模一起的事，以身体那条为准，所以配资产时让手模那条短一点）。
 */
UCLASS(meta = (DisplayName = "Throwable Skill Finished"))
class BLASTER_API UAnimNotify_ThrowableSkillFinished : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;

	virtual FString GetNotifyName_Implementation() const override;
};
