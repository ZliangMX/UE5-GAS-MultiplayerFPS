#pragma once
#include "CoreTypes.h"

UENUM(BlueprintType)
enum class ECombatState : uint8
{
	ECS_Unoccupied UMETA(DisplayName = "Unoccupied"),
	ECS_Reloading UMETA(DisplayName = "Reloading"),
	ECS_ThrowingGrenade UMETA(DisplayName = "ThrowingGrenade"),
	ECS_Planting UMETA(DisplayName = "Planting"),
	ECS_Defusing UMETA(DisplayName = "Defusing"),

	/*
	 * 掏枪中 —— 掏枪动画正在播的那一小段。
	 *
	 * 这个状态里**还能**干的事：切枪（1/2/3/4、捡枪以外）、掏技能/大招、瞄准、丢手雷之外的动作。
	 * **不能**干的事：开火（CanFire 只认 ECS_Unoccupied）、换弹（Reload 只认 ECS_Unoccupied）、
	 * 检视、捡地上的枪。
	 *
	 * 出口只有一个：UCombatComponent::EquipFinish()（换弹那套的翻版 —— 动画通知驱动，
	 * 服务器计时器兜底，两个入口共用一个幂等门禁）。
	 *
	 * ⚠ 新增状态值一律**加在 ECS_MAX 前面**，不要往中间插：中间插会把后面所有值的序号整体
	 *   挪一位，而这是要复制的枚举（同一局里两端版本不一致就跑偏了）。
	 */
	ECS_Equip UMETA(DisplayName = "Equip"),

	/*
	 * 空手 —— Jett 放完 E / Q 之后那段"手上什么都没拿"的蒙太奇。
	 *
	 * 和 ECS_Equip 最大的区别：**这一段是不可打断的**。ECS_Equip 里还能切枪、能放技能；
	 * 这里什么都不许插进来 —— 开火、换弹、切枪、捡枪、掏尖刺、拾检视、放技能、安包拆包、
	 * 吃大招球、丢东西，全部挡住。理由是这个动作本身只有零点几秒，中途换一把枪 /
	 * 起个技能只会让手和枪的状态对不上（枪已经挂回背上，而新动画又把身体拽走）。
	 *
	 * 「挡住」由这些地方一起保证（改这个状态时挨个确认一遍）：
	 *   · 开火 / 换弹 / 检视 / 瞄准 —— 只认 ECS_Unoccupied（CanFire / Reload / Inspect），
	 *     而且空手时 EquippedWeapon 是 nullptr，瞄准那条自己就挡了
	 *   · 切枪 / 捡枪 / 掏尖刺 / 技能结束掏枪 / 买枪 —— 走 UCombatComponent::CanChangeWeapon()
	 *     （只放行 Unoccupied / Equip）
	 *   · 技能 / 大招 —— UBlasterGameplayAbility::CanActivateAbility
	 *   · 安包 / 拆包 —— ABlasterCharacter::SpikePressed
	 *   · 吃大招球 —— AUltOrb::StartChannel
	 *   · 按 G 丢武器 —— ABlasterCharacter::ServerDrop_Implementation
	 *   · 买枪扣钱 —— ABlasterPlayerController::ServerBuyWeapon（挡在扣钱**之前**，
	 *     否则就是"钱花了、枪没到手"）
	 *
	 * 出口只有一个：ABlasterCharacter::EmptyHandFinish()，自带幂等门禁。收尾**主路是动画** ——
	 * 身体那条（第三人称）蒙太奇上的收尾通知 UAnimNotify_EmptyHandFinished
	 *（手模那条只在本机播，C++ 里直接滤掉）；保险丝（EmptyHandTimer）是备胎，恒起但正常不响
	 *（通知一到就把它清掉），只在通知没响时才到点收尾，并且会给通知让路 —— 挂着通知时
	 *  它的时长往后推 EmptyHandFuseExtraDelay。
	 * ⚠ 这两条都不是"进空手"的入口：进只有 BeginEmptyHand / RestartEmptyHand 那一拍
	 *  （之前"冲刺结束再兜底进一次空手"那版是多余的第二次进，冲刺动画会从头再播一遍）。
	 * 收尾顺序是「先回 ECS_Unoccupied，再调 EquipBestOwnedWeapon() 掏最强的武器」，
	 * 顺序不能反 —— EquipBestOwnedWeapon 自己走 CanChangeWeapon()，状态还停在
	 * ECS_EmptyHand 的话它把自己挡掉，人就空着手走到回合结束了。
	 *
	 * ⚠ 新增状态值一律**加在 ECS_MAX 前面**，不要往中间插：中间插会把后面所有值的序号整体
	 *   挪一位，而这是要复制的枚举（同一局里两端版本不一致就跑偏了）。
	 */
	ECS_EmptyHand UMETA(DisplayName = "EmptyHand"),

	ECS_MAX UMETA(DisplayName = "DefaultMax")
};

UENUM(BlueprintType)
enum class EWeaponSlot : uint8
{
	ESlot_Primary UMETA(DisplayName = "Primary"),
	ESlot_Secondary UMETA(DisplayName = "Secondary"),
	ESlot_Melee UMETA(DisplayName = "Melee"),
	ESlot_Spike UMETA(DisplayName = "Spike"),

	ESlot_MAX UMETA(DisplayName = "DefaultMAX")
};

/*
 * 当前正在生效的大招是哪一个。
 *
 * 为什么需要这个而不是一个 bool：两个大招的"生效实体"长得完全不一样
 *   · Phoenix「再来一次」—— 状态就在角色自己身上（标记点 + 满血满甲拉回 + 阵亡不真死）
 *   · Jett「刃风暴」   —— 状态是手里那把飞刀（掏出/扔完/收刀）
 * 但「还剩几秒」和「结束时该收回什么」这两件事是 HUD 和死亡结算都要问的，
 * 所以统一记在角色上，收尾时按这个枚举分派（见 ABlasterCharacter::ServerEndUltimate）。
 *
 * 注意它同时**兼任"这个大招会不会让我免死"**：
 * 目前只有 Phoenix 的再来一次会在阵亡时拦截死亡（见 ABlasterCharacter::ServerElim），
 * 所以那里判的是 IsRunItBackActive() 而不是笼统的 IsUltimateActive()。
 *
 * 新增大招：加一个枚举值，再在 ServerEndUltimate 的 switch 里加一条收尾分支。
 */
UENUM(BlueprintType)
enum class EActiveUltimate : uint8
{
	EUA_None UMETA(DisplayName = "None"),
	EUA_RunItBack UMETA(DisplayName = "Run It Back (Phoenix)"),
	EUA_BladeStorm UMETA(DisplayName = "Blade Storm (Jett)"),

	EUA_MAX UMETA(DisplayName = "DefaultMAX")
};