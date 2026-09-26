// 角色"手上拿着一个技能投掷物"的种类。
//
// 这几种状态共用同一套骨架（ABlasterCharacter 里 bThrowableHolding 那一族）：
//   按技能键收枪 → 手上拿着东西 → 左键/右键按各自约定丢出去 → 掏回原来那把枪。
// 1/2/3 键、换弹、捡枪、放大招……在持投掷物期间全部让路（各自都有 bThrowableHolding 门槛）。
//
// 为什么用一个枚举而不是两个 bool：状态天然互斥（只有两只手，也只可能拿着一样东西），
// 而且右下角技能条要**知道是哪一个**才能高亮对应的槽 —— 光知道"在持投掷物"不够。
//
// ⚠️ 这个枚举会复制（UPROPERTY(Replicated)）并序列化进蓝图，新值一律加在最后。
#pragma once

#include "CoreMinimal.h"

#include "Blaster/Weapon/WeaponTypes.h"

#include "ThrowableKind.generated.h"

UENUM(BlueprintType)
enum class EBlasterThrowableKind : uint8
{
	// 空手（没有在持投掷物）
	None		UMETA(DisplayName = "None"),
	// Phoenix E 曲线闪光：左键 = 左拐抛掷 / 右键 = 右拐抛掷
	Flash		UMETA(DisplayName = "Flash"),
	// Phoenix Q 火球：**左键 = 丢出去**（2026-09-21 用户改的，之前是右键；
	// 原版 Hot Hands 也是左键 FIRE，所以这一改反而更贴原版）。右键不丢东西。
	Fireball	UMETA(DisplayName = "Fireball"),
	/*
	 * Phoenix C 火墙：和上面两个**手感不一样**，多了"按住"这一段 ——
	 *   按 C 武装（收枪、手上什么都没有，球是隐形无碰撞的）→ **按住左键发射**
	 *   → 按住期间球跟准心飞、同时在地上立起烟墙 → 松手（或球飞完）就收尾掏枪。
	 * 发射之后这个"按住控球"的状态由 ABlasterCharacter::bBlazeFiring 记着，
	 * 上面两个是"一按就出去、立刻收尾"，所以它们那条路上看不到这个标志。
	 */
	Wall		UMETA(DisplayName = "Wall")
};

/*
 * 手上这个投掷物对应哪个 EWeaponType —— 只给动画蓝图当"状态键"用。
 *
 * 持技能期间枪是收着的，动画机拿不到任何 Weapon 来问类型（ABlasterCharacter::GetWeaponType()
 * 空手时还会崩），所以两个动画实例改成先问这里：持着东西就用这里的值报 WeaponType，
 * 空手才回 EWT_MAX。和 EWT_BladeStorm 同一个套路（"ABP 只能靠 WeaponType 分辨姿势，
 * 而那个信息它拿不到"），理由见 WeaponTypes.h 里那三个新值的注释。
 *
 * 定义在头文件里（inline）是因为两个动画实例（第一人称 / 第三人称）都要用，
 * 而它们各自在别的模块翻译单元里。
 */
FORCEINLINE EWeaponType GetThrowableWeaponType(EBlasterThrowableKind Kind)
{
	switch (Kind)
	{
	case EBlasterThrowableKind::Flash:		return EWeaponType::EWT_PhoenixCurveball;
	case EBlasterThrowableKind::Fireball:	return EWeaponType::EWT_PhoenixFireball;
	case EBlasterThrowableKind::Wall:		return EWeaponType::EWT_PhoenixBlaze;
	default:								return EWeaponType::EWT_MAX;
	}
}

/*
 * 持技能期间的动画分两半，各自有归属（2026-09-20 大改后）：
 *
 *   · **站着待命那一段在动画蓝图里**：持着东西期间两个动画实例把 WeaponType 报成
 *     GetThrowableWeaponType(Kind)（见 WushuFPAnimInstance / BlasterCharacterAnimInstance），
 *     ABP 按 WeaponType 切到"举着它待命"的状态，一直维持到 WeaponType 变回去。
 *     所以 C++ 不播、也不需要知道"现在停在待命姿势上"这件事。
 *   · **动作那几段（拿起 / 丢出去 / 按住 / 收起）由 C++ 播蒙太奇**，资产挂在技能上
 *     （UBlasterGameplayAbility 的 ThrowableEquipMontages / ThrowMontages / FireMontages /
 *     LiftMontages），全是一次性的，播完就回 ABP 那个待命姿势。
 *     每段的收尾（什么时候算演完、什么时候掏枪）由用户摆在**第三人称蒙太奇末尾**的
 *     动画通知 UAnimNotify_ThrowableSkillFinished 说了算。
 *
 * 所以这里**没有**"当前演到哪一段"的枚举 —— 那套阶段机（两条轨道记段位 + 计时器接续）
 * 已经删掉了：段与段之间不再需要 C++ 接续，ABP 的 WeaponType 状态就是待命姿势的本体。
 */
