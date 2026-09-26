#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "SpikeAnimSet.generated.h"

class UAnimSequenceBase;

/**
 * 爆能器（spike）的整套动画槽 —— 掏出 / 下包 / 拆包，第一人称 + 第三人称各一份。
 *
 * ── 为什么单独做成一个资产，而不是继续挂在角色蓝图上 ──────────────────────────
 *
 * 这些槽以前是 ABlasterCharacter 上的 10 个 UPROPERTY，谁用谁在**自己的角色蓝图**里填。
 * 2026-09-22 踩到了这个设计的坑：工程里五个角色蓝图（Blaster / Sage / Clove / Jett / Phoenix）
 * **互为兄弟** —— 各自继承自己的原生类（ABlasterCharacter / ASageCharacter / ...），
 * 谁都**不是**谁的子类。于是在 BP_BlasterCharacter 的 Class Defaults 里填好的槽，
 * PIE 里换成 BP_SageCharacter 跑时全是 C++ 默认值（空），
 * 表现是"按 4 掏出包，一点动画都没有" —— 而且**一句日志都没有**：
 * PlayThirdPersonMontage / PlayFPArmsMontage 头一行 `Asset == nullptr` 就静默 return 了。
 *
 * 而瓦的那套爆能器动画本来就跟英雄无关（每个英雄下包都是同一个动作），
 * 复制五份纯粹是浪费和维护陷阱。所以改成一份共享资产，五个角色蓝图都指向它。
 *
 * ⚠ 一个 USpikeAnimSet 实例 = 一套**骨架组合**（TP_Wushu_S0 + FP_Wushu_S0）。
 *   每份资产里 TP/FP 两边的骨架都必须和实际用它的角色对得上，
 *   PlayThirdPersonMontage / PlayFPArmsMontage 里有骨架检查，填错了会打指名道姓的警告。
 *
 * ⚠ 填 AnimSequence（裸序列）或 AnimMontage 都行：
 *   裸序列会被 PlayAnimAssetOnInstance 按槽名现造一条动态蒙太奇播出来。
 */
UCLASS(BlueprintType)
class BLASTER_API USpikeAnimSet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// ——— 掏出包（按 4 切到 spike）———
	// 触发点：SetSpikeDrawn(true)（服务器那条路）+ OnRep_SpikeDrawn（客户端那条路）。
	// 留空 = 没这段，直接摆 ABP 里 EWT_Spike 那层的姿势，不报错。
	UPROPERTY(EditAnywhere, Category = "掏出")
	TObjectPtr<UAnimSequenceBase> EquipTP;

	UPROPERTY(EditAnywhere, Category = "掏出")
	TObjectPtr<UAnimSequenceBase> EquipFP;

	// ——— 下包 ———
	// 触发点：ASpike 的 ServerStartPlant / ServerCancelPlant / 安包完成 三处。
	UPROPERTY(EditAnywhere, Category = "下包")
	TObjectPtr<UAnimSequenceBase> PlantTP;

	UPROPERTY(EditAnywhere, Category = "下包")
	TObjectPtr<UAnimSequenceBase> PlantFP;

	/*
	 * ★ 下半身（LB）—— 下包 / 拆包**必须**配，否则第三人称蹲不下去。
	 *
	 * 现役动画蓝图把上半身槽（UpperBody）只喂给两个遮罩：Body = 脊柱/头颈 7 根、
	 * Arm = 两条胳膊 53 根（BlendMask 语义：没列进去的骨保持 BasePose）。
	 * **腿和 Root/pelvis 都不在里面** —— 它们永远来自 LowerBody 槽。
	 * 而瓦的 TP 资产里没有蹲姿序列（只有一条 Crouch_Jump_LB），
	 * 所以"蹲"这件事只能靠这几条 Bomb LB 里的姿势，别的路都不通：
	 *   · 只播 UB → 上身弯下去、腿还是站着的；
	 *   · Crouch()/UnCrouch() 单独调 → 第三人称零表现（引擎把身体网格的相对 Z 补了回来，
	 *     一降一升正好抵消，见 BlasterCharacter.h 里的注释）。
	 * 全部理由见 ABlasterCharacter::PlayThirdPersonUpperLower。
	 *
	 * 留空 = 这一段的腿不动（上身照旧），不报错。
	 * 填的必须是**身体骨架**（TP_Wushu_S0_*）的下半身序列，填错骨架会打一行警告然后不播。
	 */
	UPROPERTY(EditAnywhere, Category = "下包")
	TObjectPtr<UAnimSequenceBase> PlantLB;

	// ——— 拆包：掏出 → 待命（循环）→ 收起 ———
	// 掏出那一段一开始**就算正在拆包**（进度同时起走，和动画无关）；
	// 掏出播完自动接待命，待命是循环的；打断或拆完都播收起那一段。
	// 段与段之间靠计时器接（掏出播完 → 接待命），时长按各网格上实际播起来的那条算。
	//
	// ⚠ 3P 的"收起"在瓦的资产里**没有对应资产**（只有 Start / Loop + 一条 Montage 版的 Stop），
	//    留空的话打断时身体会硬切回待机姿势，不影响功能。
	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseDrawTP;

	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseIdleTP;

	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseStopTP;

	// 拆包两条的**下半身**（蹲姿）—— 和 PlantLB 同理：不配的话第三人称是"上身蹲、腿站着"。
	// DefuseDrawLB 播完自动接待命那一段（和上半身同一个计时器），DefuseIdleLB 是循环的。
	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseDrawLB;

	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseIdleLB;

	// ⚠ 3P 的"收起"没有 LB 资产（瓦那边连 UB 都没有）—— 收尾时腿是**直接停掉**
	//   （按 blendOut 时间淡回站立），这一段不需要额外的槽。
	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseDrawFP;

	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseIdleFP;

	UPROPERTY(EditAnywhere, Category = "拆包")
	TObjectPtr<UAnimSequenceBase> DefuseStopFP;
};
