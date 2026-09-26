#pragma once
#include "CoreTypes.h"

#define TRACE_LENGTH 80000.f

#define CUSTOM_DEPTH_PURPLE 250
#define CUSTOM_DEPTH_BLUE 251
#define CUSTOM_DEPTH_TAN 252

// 武器类型已 Valorant 化：仅保留 4 种枪（Vandal/Classic/Operator/Judge）+ 1 种近战。
// 显式赋值保持底层值不变（Pistol=2、Shotgun=4、SniperRifle=5），
// 这样已配置的武器蓝图 WeaponType 不会因删除中间枚举项而错位。
UENUM(BlueprintType)
enum class EWeaponType : uint8
{
	EWT_AssaultRifle = 0 UMETA(DisplayName="Assault Rifle"),
	EWT_Pistol = 2 UMETA(DisplayName="Pistol"),
	EWT_Shotgun = 4 UMETA(DisplayName="Shotgun"),
	EWT_SniperRifle = 5 UMETA(DisplayName="Sniper Rifle"),
	// 近战槽的普通刀（角色自带的那把）。
	// ⚠️ 它**不参与** CarriedAmmoMap（备用弹药表）—— 没有备用弹药，
	//    所以 UpdateCarriedAmmo/AmountToReload 里那些 Contains 判断天然把它排除在外。
	EWT_Melee = 6 UMETA(DisplayName="Melee"),

	/*
	 * Jett 大招「刃风暴」的飞刀。
	 *
	 * 为什么另开一个类型、不蹭 EWT_Melee：
	 *   · 第一人称动画状态机按 WeaponType 换状态（用户要求）。飞刀要一套自己的 idle/移动，
	 *     混在 EWT_Melee 里就得在 ABP 内部再按 "是不是飞刀" 分一次叉 —— 而那个信息
	 *     ABP 拿不到，只能靠 WeaponType。
	 *
	 * 槽位相关的行为**不用**在这里各写一遍：收回 MeleeHolsterSocket / 不能丢 / 不能被捡 /
	 * 移动速度这几条都挂在 AWeapon::IsMeleeWeapon() 上，它现在认 EWT_Melee 和本值两个。
	 * 也就是说"3 号槽"= IsMeleeWeapon()，"哪种 3 号槽武器"= GetWeaponType()。
	 *
	 * ⚠️ 枚举值是**复制**的（WeaponType 走属性复制），新值一律往后追加，绝不能插在中间。
	 */
	EWT_BladeStorm = 7 UMETA(DisplayName="Blade Storm"),

	/*
	 * Phoenix 三个技能投掷物（C 火墙 / Q 火球 / E 曲线闪光）。
	 *
	 * 它们**不是武器**，也不会被设到任何 AWeapon 上 —— 只是给动画蓝图用的"状态键"：
	 * 持技能期间枪是收起来的（EquippedWeapon == nullptr），动画机本来只会看到 EWT_MAX
	 * （空手），没法按 WeaponType 换姿势。用户要求这三个技能的动画在**动画蓝图**里做，
	 * 所以按 EWT_BladeStorm 那套先例给它们各开一个类型：两个动画实例
	 *（UWushuFPAnimInstance / UBlasterCharacterAnimInstance）在持投掷物期间把 WeaponType
	 * 报成这里的值，ABP 里按它切状态即可。
	 *
	 * 这个值给出的就是"**举着它待命**"那一个姿势（ABP 按它切状态）。动作那几段
	 *（拿起 / 丢出去 / 按住 / 收起）是 C++ 播的一次性蒙太奇，不走 WeaponType ——
	 * 槽位上有蒙太奇在出力时蒙太奇说了算，演完自然回落到这里的待命姿势。
	 *
	 * ⚠️ 因为是复制的枚举，值一律往后追加（同 EWT_BladeStorm 那条注释）。
	 */
	EWT_PhoenixCurveball = 8 UMETA(DisplayName="Phoenix Curveball"),	// E 曲线闪光
	EWT_PhoenixFireball = 9 UMETA(DisplayName="Phoenix Fireball"),		// Q 火球
	EWT_PhoenixBlaze = 10 UMETA(DisplayName="Phoenix Blaze"),			// C 火墙

	/*
	 * 爆能器（spike）—— 按 4 把它掏到手上那段时间的"状态键"。
	 *
	 * 和上面三个技能投掷物同一套路：它**不是武器**（ASpike 不是 AWeapon，也不设到任何 AWeapon 上），
	 * 只是给动画蓝图用的状态：掏着 spike 时手上的枪是收起来的（EquippedWeapon == nullptr），
	 * 两个动画实例本来只会报 EWT_MAX（空手），ABP 那边分不出"空手"和"拿着包"。
	 * 用户要做"拿着包的时候"的动画机 —— 就是按这个值在 ABP 里切状态。
	 *
	 * 判定条件是 ABlasterCharacter::IsSpikeDrawn()（复制的）：掏出包到收起包之间成立。
	 * 注意**下包/拆包那两段不算**：那时包已经脱手（下包）或手上是拆包器（拆包），
	 * 那两段是 C++ 播的一次性动画，不走 WeaponType。整套槽在 USpikeAnimSet 那个共享资产里
 *（装备它的角色是 ABlasterCharacter::SpikeAnims），不在这条 WeaponType 链上。
	 *
	 * ⚠️ 因为是复制的枚举，值一律往后追加（同 EWT_BladeStorm 那条注释）。
	 */
	EWT_Spike = 11 UMETA(DisplayName="Spike"),

	EWT_MAX UMETA(DisplayName = "DefaultMAX")
};