// Valorant 技能的 GAS 基类。首版实现「顺风冲刺」这类自我增益技能的通路：
// 冷却（非堆叠 Duration GE 计层 = 充能）+ 服务器权威冲刺位移。
// 子类/蓝图资产只需配置 CooldownEffectClass / MaxCharges / DashSpeed / DashDuration。

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
// 空手蒙太奇按方向挑分段 —— FBlasterEmptyHandMontages::GetLongestSectionLength 要这个枚举
#include "Blaster/BlasterTypes/MovementDirection.h"
#include "BlasterGameplayAbility.generated.h"

class UAbilitySystemComponent;
class ABlasterCharacter;
class UTexture2D;

// 技能类型：既是 HUD 图标的选择键，也是"这个英雄有没有这个技能"的判据
//（角色侧按 SkillType 扫 DefaultAbilities 找 CDO，C++ 不需要认识具体英雄）。
//
// 图标画法两级：技能资产上填了 SkillIcon 贴图就画贴图，没填才退回按本枚举画的矢量形状。
// 所以老技能（Sage/Phoenix/Clove）不填图标也照常显示，不会因为这次改动变成空白槽。
UENUM(BlueprintType)
enum class EBlasterSkillType : uint8
{
	// 顺风冲刺（→ 箭头）
	Dash		UMETA(DisplayName = "Dash"),
	// 治疗（十字）
	Heal		UMETA(DisplayName = "Heal"),
	// 墙体/屏障（横线）
	Wall		UMETA(DisplayName = "Wall"),
	// 侦察/闪光（圆环+点）
	Recon		UMETA(DisplayName = "Recon"),
	// 烟雾/遮挡（云）
	Smoke		UMETA(DisplayName = "Smoke"),
	// 闪光（星芒）
	Flash		UMETA(DisplayName = "Flash"),
	// 未归类（方块）
	Custom		UMETA(DisplayName = "Custom"),

	// —— Jett 的两个普通技能 ——
	// ★ 新枚举值一律**加在最后**：这上面每一个值都是顺序编号，插在中间会把后面
	//   已经序列化进 BP 资产的老值整体挪位（GA_Sage_Heal 会突然变成别的类型）。
	// Jett Q 腾空（向上箭头的云）
	Updraft		UMETA(DisplayName = "Updraft"),
	// Jett C 逐风云（云 + 拖尾）
	Cloudburst	UMETA(DisplayName = "Cloudburst"),
	// Phoenix Q 火球 / Hot Hands（火焰）
	Fireball	UMETA(DisplayName = "Fireball")
};

// 一次查询返回的充能/冷却状态，供 HUD Widget 每帧读取。
// Widget 侧拿到 ASC 后调 UBlasterGameplayAbility::GetCooldownInfo(ASC)，无需能力实例。
USTRUCT(BlueprintType)
struct FBlasterAbilityCooldownInfo
{
	GENERATED_BODY()
public:
	// 当前可用充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 Charges = 0;

	// 最大充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 MaxCharges = 0;

	// 距下一层充能恢复的剩余秒数（无冷却中 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float TimeUntilNextCharge = 0.f;

	// 单层冷却时长（秒）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float CooldownDuration = 0.f;

	// 数据是否有效（找到 ASC 且配了冷却 GE）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	bool bValid = false;
};

// 服务器生成的"技能条显示快照"，按 SkillSlotIndex 排序后放进 ABlasterCharacter::ReplicatedSkills。
// 观战者（阵亡看队友）读不到队友 ASC 的能力（GAS ActivatableAbilities 只复制给拥有者），
// 所以服务器每帧基于自己的权威 ASC 重算这份快照、只复制到所有客户端画远程技能条。
// 时间字段一律用"绝对服务器时间"（客户端 PC->GetServerTime() 换算剩余秒数）；0 = 无该状态。
USTRUCT(BlueprintType)
struct FBlasterReplicatedSkill
{
	GENERATED_BODY()
public:
	// 槽位是否有效（false = 空位，客户端跳过）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bValid = 0;

	// 技能类型（HUD 按它画矢量图标）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	EBlasterSkillType SkillType = EBlasterSkillType::Custom;

	// 技能条槽位（0=C、1=Q、2=E；INDEX_NONE = 不进这三格，大招就是 -1）。
	// ★ 底栏 HUD 靠它认格，**不是**靠 SkillType —— 类型是"哪个英雄的哪个技能"，每个英雄都不同，
	//   按类型点名只有 Jett 那三个（Cloudburst/Updraft/Dash）认得出来，别的英雄整条技能栏都不刷。
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 SkillSlotIndex = INDEX_NONE;

	// 最大充能层数
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 MaxCharges = 0;

	// 当前满充能层数（服务器权威）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	int32 Charges = 0;

	// 下一层充能恢复的绝对服务器时间（无冷却中 / 永不回复(如 Sage 每回合一次) = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float NextChargeServerEndTime = 0.f;

	// 单层冷却时长（恢复进度分母；非时长型冷却 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float CooldownDuration = 0.f;

	// 冷却数据有效（有 ASC 且配了冷却 GE）——镜像本地 GetCooldownInfo().bValid
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bCooldownValid = 0;

	// 武装（逐风）窗口进行中
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	uint8 bArmed = 0;

	// 武装窗口绝对结束服务器时间（未武装 = 0）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float ArmedServerEndTime = 0.f;

	// 武装窗口总时长（窗口进度分母）
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	float ArmedWindowDuration = 0.f;

	// 图标贴图（从能力 CDO 抄过来）。
	// 观战者读不到队友的 ASC 能力，只拿得到这份快照 —— 不把图标一起复制过来，
	// 观战时技能条上的图标就会全变成矢量兜底形状，和活着时看到的对不上。
	UPROPERTY(BlueprintReadOnly, Category = "Blaster|Ability")
	TObjectPtr<UTexture2D> SkillIcon = nullptr;
};

/*
 * ——— 技能放完之后的「空手蒙太奇」三件套 ———
 *
 * 用途：Jett 的 E / Q 放完之后有一小段时间手上什么都没拿（空手姿态的蒙太奇），
 * 这段时间**不可打断**（见 ECombatState::ECS_EmptyHand 的注释），蒙太奇一结束自动掏回最强的武器。
 *
 * 三个槽对应三个**不同的骨架 / 不同的动画实例**，各播各的、互不覆盖，别填错：
 *   · FirstPerson       → **手模**（FPArmsMesh，骨架 FP_Wushu_S0_Skeleton）。只在射手本机播。
 *                         槽名要和 ABP_WushuFP 里那个 Slot 节点一致（现役叫 "DefaultSlot"）。
 *   · ThirdPersonUpper  → **角色第三人称的上半身**（ABP_BlasterCharacter 的 UpperBody 槽）。
 *                         所有机器都播，别人看得见。
 *   · ThirdPersonLower  → 同一具身体的**下半身**（ABP_BlasterCharacter 的 LowerBody 槽）。
 *                         和上面那条**同时**播（C++ 里第二条带 bStopAllMontages=false，
 *                         否则会把上一条顶掉，变成只有半身在动）。
 *
 * ★ 槽名不在能力上填，在**角色**上（ABlasterCharacter::EmptyHandFPSlotName / UB / LB）——
 *   因为槽是动画蓝图里的节点，而动画蓝图是角色的资产。默认值已经填好现役的三个名字。
 *
 * ★ 填 **AnimSequence 就行，不用自己做蒙太奇**：手模/身体是靠动画图里的 Slot 节点
 *   接住蒙太奇的，而 C++ 会在播放时按槽名现造一条**动态蒙太奇**把序列塞进去 ——
 *   等于替你省掉了"新建蒙太奇资产 → 拖轨道 → 起槽名"那几步，也就没有槽名填错这个坑。
 *   已经做成 AnimMontage 的也照样能填（走正常 Montage_Play，自带槽名和通知）。
 *   两种情况下**挂在这条动画上的动画通知都会响**（引擎 FAnimMontageInstance::HandleEvents
 *   会把 SlotAnimTracks 里那条序列的通知一起派发）—— 想让动画末帧精确收尾就挂
 *   UAnimNotify_EmptyHandFinished，不挂也行（有服务器计时器兜底）。
 *
 * ★★ 八个方向：**一条蒙太奇装 8 个分段**，不是 8 个蒙太奇资产（那样要 8×3 = 24 个）。
 *   一条槽一个蒙太奇，里面切 8 个 section，名字就叫 N / NE / E / SE / S / SW / W / NW
 *   （= EMovementDirection8 的枚举名，见 BlasterTypes/MovementDirection.h）。
 *   播放时先 Play 再 Montage_JumpToSection 跳到"这一趟往哪走"的那一段。
 *
 *   用这个模式时**必须填 AnimMontage**（裸序列现造出来的动态蒙太奇只有一个叫 "Default"
 *   的分段，装不下八个方向 —— 填序列 = 不管往哪走都播同一条）。
 *
 *   建资产时的三条约定：
 *     1. 8 个 section，名字严格是那八个（大小写随意，FName 比较忽略大小写）。
 *        **只做四个方向也行**（第一人称手模就是这样）：用 N / E / S / W，
 *        斜向会自动退到平移那一段（NE/SE → E，NW/SW → W，见
 *        MovementDirection8::CollapseToCardinal）—— 不用为它单独配资产或改代码
 *     2. **每个 section 的 Next Section 设成 None** —— 引擎建分段时默认把上一段指向下一段，
 *        不改的话"跳到 SE 播完会接着播 S、SW……"一路播到末尾。C++ 收尾时会强停一次兜底，
 *        但别依赖它，Next Section = None 才是正解
 *     3. 收尾通知 UAnimNotify_EmptyHandFinished 挂在**每一段的最后一帧**（不是整条末尾）
 *
 *   只做一个方向（或者暂时不想分方向的）也完全可以：分段名对不上时 C++ 不跳段、整条播，
 *   单方向的老资产原样能用；兜底计时器也会跟着退回按**整条**长度算。
 *   ⚠ 但"配了八段、某一段名字打错"不会有任何报错 —— 它会静默退到四向那一段
 *   （斜向播成了平移）。看空手日志里那句"找不到分段"能定位。
 *
 * ⚠ 三条都可以留空，留空 = 这条不播。**三条全空**时这个技能只是不播蒙太奇 ——
 *   默认算"没配"、整个空手流程跳过（状态也不切）；想在没动画时也进空手，
 *   把 UBlasterGameplayAbility::bEnableEmptyHandWithoutMontage 勾上即可。
 */
USTRUCT(BlueprintType)
struct FBlasterEmptyHandMontages
{
	GENERATED_BODY()
public:
	// 第一人称（手模）。只在本机播，槽名对齐 ABP_WushuFP
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|EmptyHand")
	TObjectPtr<class UAnimSequenceBase> FirstPerson = nullptr;

	// 第三人称上半身。槽名对齐 ABP_BlasterCharacter 的 UpperBody
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|EmptyHand")
	TObjectPtr<class UAnimSequenceBase> ThirdPersonUpper = nullptr;

	// 第三人称下半身。槽名对齐 ABP_BlasterCharacter 的 LowerBody
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|EmptyHand")
	TObjectPtr<class UAnimSequenceBase> ThirdPersonLower = nullptr;

	// 三条全空 = 不启用（技能行为完全不变）
	bool IsEmpty() const
	{
		return FirstPerson == nullptr && ThirdPersonUpper == nullptr && ThirdPersonLower == nullptr;
	}

	// 三条里最长的那条有多长（秒）。★ 八个方向的动画装在**一个**蒙太奇里（8 个分段）时
	// 别用这个：拿到的是八段加起来的总长。算保险丝请用下面的 GetLongestSectionLength。
	float GetLongestPlayLength() const { return GetLongestPlayLength(FirstPerson, ThirdPersonUpper, ThirdPersonLower); }

	// 同上，但收三个裸指针（角色身上那三条复制过来的副本是分开存放的，凑不出一个结构体）
	static float GetLongestPlayLength(const class UAnimSequenceBase* FP, const class UAnimSequenceBase* UB, const class UAnimSequenceBase* LB);

	/*
	 * 单条资产里，**这个方向实际会播的那一段**有多长（秒）。
	 *
	 * 蒙太奇：按 ResolveSectionName 挑段（和播放那边同一个函数）→ GetSectionLength。
	 * 裸序列（现造动态蒙太奇那条路）：只有一刀切，就是它自己的长度。
	 * 蒙太奇里挑不出段（单方向的老资产）→ 退回整条长度，和"不跳段、整条播"的行为对上。
	 * 传 nullptr → 0。
	 */
	static float GetSectionLength(const class UAnimSequenceBase* Asset, EMovementDirection8 Direction);

	/*
	 * 三条里，**所跳的那个分段**最长有多长（秒）。
	 *
	 * 和 GetLongestPlayLength 的区别：蒙太奇里八段是首尾相接的，整条长度 = 八段之和，
	 * 拿它当保险丝会给出 8 倍的时间（"动画早播完了，人还空着手站半天"）。
	 */
	static float GetLongestSectionLength(const class UAnimSequenceBase* FP, const class UAnimSequenceBase* UB,
		const class UAnimSequenceBase* LB, EMovementDirection8 Direction);

	/*
	 * ★ 服务器兜底计时器实际用的时长：**以第一人称那条为准**（配了就用它的分段长度，
	 *   没配才退回三条里最长的那个）。
	 *
	 * 为什么不取"三条里最长"：三条是给**三块不同的屏幕**看的 ——
	 *   · 第一人称（手模）→ 射手**本人**屏幕上看到的，也就是"这次动作为什么要等"
	 *     这件事唯一会影响到的画面
	 *   · 第三人称上下半身 → 别人眼里的你，射手本人看不见（那具身体对他是 SetOwnerNoSee）
	 * 取最长 = 让**射手**替他看不见的那条动画等：N 方向实测 FP 0.6167s / TP 0.8333s，
	 * 于是手模在末帧上定格 0.2167s 才等到掏枪（S 0.1061 / W 0.1333 / E 0.0333）。
	 * 按 FP 收尾则是"手模这一段播完的那一帧就开始掏枪"。
	 *
	 * 代价（明确写下来）：FP 比 TP 短的那几个方向，别人的屏幕上这条冲刺动画的
	 * **尾巴会被切掉**（N 最多 0.2167s），接缝是掏枪蒙太奇的 blend-in（0.25s 交叉淡化）。
	 * 想彻底两边都不切，只能把 FP / TP 各方向的**分段长度对齐**（改动画资产，不是改代码）。
	 *
	 * 空手那三条一个都没配（走 ABP 状态机那条路）时返回 0，调用方用配置的兜底值。
	 */
	static float GetFuseDuration(const class UAnimSequenceBase* FP, const class UAnimSequenceBase* UB,
		const class UAnimSequenceBase* LB, EMovementDirection8 Direction);

	/*
	 * 从一条蒙太奇里挑出"这个方向该播哪一段" —— 播放和算时长**共用这一个函数**，
	 * 保证两边挑出来的永远是同一段（两边各写一遍是最容易悄悄对不上的地方）。
	 *
	 * 两步：
	 *   1. 精确匹配分段名（N / NE / E / …）
	 *   2. 没有 → 退到**四向**那一段（NE/SE → E，NW/SW → W，见 MovementDirection8::CollapseToCardinal）
	 *      第一人称的手模就只做了四个方向，斜着走时按横移那套播
	 * 都没有（或者资产里压根没分段）→ 返回 NAME_None，调用方按"不跳段、整条播"处理。
	 *
	 * 反过来提醒：这份资产**配了八段但某一段名字打错**时，会静默退到四向那一段 ——
	 * 表现是"斜向播成了平移"，不报错。这时看空手日志里那句"找不到分段"就能定位。
	 */
	static FName ResolveSectionName(const class UAnimMontage& Montage, EMovementDirection8 Direction);
};

/*
 * 持技能投掷物（Phoenix E 闪光 / Q 火球 / C 火墙）时，**某一段动作**（拿起 / 丢出去 / 按住 / 收起）
 * 要用的一整套动画。
 *
 * 和上面 FBlasterEmptyHandMontages 是同一个形状、同一个理由 —— 第一人称手模和第三人称身体
 * 是**两套骨架**（也不是同一个动画实例，见 ABlasterCharacter::PlayThrowableTrack），
 * 所以第一人称必须单独给一条；第三人称再按上下半身分两条槽。
 *
 * 跟空手那套的区别只有一条：这里**不要方向分段**（投掷物不分八个方向走）。
 *
 * ⚠️ 三条都可以留空，留空 = 那一段的那条轨道不播任何东西（不报错，行为跟没这个功能一样）。
 *    常见配法：只填第一人称那条（本人看得到手，别人看不看得到无所谓时）。
 *
 * ⚠️ 收尾时机（什么时候算演完、什么时候掏枪）靠用户在**第三人称**那条末尾摆的
 *    UAnimNotify_ThrowableSkillFinished；第一人称那条比身体短一点最省事
 *    （掏枪是按身体那条的通知兑现的，手模还没演完就会看到动作被切）。
 *
 * 详细规则见 ABlasterCharacter::PlayThrowableTrack 上头的注释。
 */
USTRUCT(BlueprintType)
struct FBlasterThrowableMontages
{
	GENERATED_BODY()
public:
	// 第一人称（手模 FPArmsMesh）。只在本机播，槽名对齐 ABP_WushuFP
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|Throwable")
	TObjectPtr<class UAnimSequenceBase> FirstPerson = nullptr;

	// 第三人称上半身。槽名对齐 ABP_BlasterCharacter 的 UpperBody
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|Throwable")
	TObjectPtr<class UAnimSequenceBase> ThirdPersonUpper = nullptr;

	// 第三人称下半身。槽名对齐 ABP_BlasterCharacter 的 LowerBody
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blaster|Throwable")
	TObjectPtr<class UAnimSequenceBase> ThirdPersonLower = nullptr;

	// 三条全空 = 这个阶段的这条轨道不播
	bool IsEmpty() const
	{
		return FirstPerson == nullptr && ThirdPersonUpper == nullptr && ThirdPersonLower == nullptr;
	}

	// 三条里最长的那条有多长（秒）—— 收尾段的备胎计时器按它算（见 ABlasterCharacter::BeginThrowableFinisher）。
	// 直接借空手那套的静态版本，长短算法（蒙太奇取整条 / 裸序列取自身）两边保持一份。
	float GetLongestPlayLength() const
	{
		return FBlasterEmptyHandMontages::GetLongestPlayLength(FirstPerson, ThirdPersonUpper, ThirdPersonLower);
	}

	// 单条资产的长度（秒），不填返回 0。给"这一条轨道自己算自己的收尾时间"用。
	static float GetPlayLength(const class UAnimSequenceBase* Asset)
	{
		return Asset ? Asset->GetPlayLength() : 0.f;
	}
};

UCLASS()
class BLASTER_API UBlasterGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UBlasterGameplayAbility();

	// —— 冷却 / 充能 ——
	// 冷却用「非堆叠 Duration GameplayEffect」：每次施法 Apply 一个独立实例、各自到期移除。
	// 活跃实例数 = 已消耗的充能数；CanActivate 检查 活跃实例数 < MaxCharges。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cooldown")
	TSubclassOf<class UGameplayEffect> CooldownEffectClass;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cooldown")
	int32 MaxCharges = 2;

	// 当前剩余充能（0..MaxCharges），供 HUD/激活检查使用
	int32 GetCharges() const;

	// 指定 ASC 视角的充能（CDO 也能用，不依赖实例）
	int32 GetCharges(const UAbilitySystemComponent* ASC) const;

	// 查询充能/冷却信息（供 HUD Widget 每帧读取；ASC 必传，CDO 即可调用）
	UFUNCTION(BlueprintCallable, Category = "Ability|Cooldown")
	FBlasterAbilityCooldownInfo GetCooldownInfo(const UAbilitySystemComponent* ASC) const;

	// —— 技能条（HUD）显示配置 ——
	// 技能类型（既是 HUD 图标兜底形状的选择键，也是角色侧"有没有这个技能"的判据）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	EBlasterSkillType SkillType = EBlasterSkillType::Custom;

	// 技能图标贴图（HUD 技能条按图标框原始像素尺寸画，所以导入时别缩放）。
	// 留空 = 退回按 SkillType 画的矢量形状 —— 老技能不用动，新技能想好看就填一张。
	// 约定的规格：白 / 近白图形 + 透明底，画的时候整体染白；没充能时按 42% 透明度画灰，
	// 所以**只需要一张图**，不用再出一份灰色的（素材里的 _gray 就是这个意思）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	TObjectPtr<UTexture2D> SkillIcon = nullptr;

	// 是否显示在技能条（被动/纯冷却技能可关掉）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	bool bShowInSkillBar = true;

	// 技能条槽位顺序（越小越靠左；INDEX_NONE 排最后）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|HUD")
	int32 SkillSlotIndex = INDEX_NONE;

	// —— 大招（Valorant 的 X 技能）——
	// 开启后：CanActivateAbility 额外要求 PlayerState 的大招点攒满（见 IsUltPointsReady）。
	// 同时也是角色侧"按 X 该放哪个技能"的查找键 —— ABlasterCharacter::GetUltimateAbilityCDO
	// 扫 DefaultAbilities 找第一个 bIsUltimate 的。一个角色只配一个大招。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Ultimate")
	bool bIsUltimate = false;

	// 激活时自动扣光大招点。
	// ⚠️ 绝大多数大招（Jett 刃风暴 / Sage 复活）按下即扣，保持 true 即可。
	// Phoenix「再来一次」必须设 false —— 它是"阵亡/到期后回到标记点"，点数要在**回到原点那一刻**
	// 才扣（用户明确要求）。那种能力自己不靠这个开关，在回程结算里调 SpendUltPoints()。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Ultimate")
	bool bSpendUltPointsOnActivate = true;

	// 是不是大招
	bool IsUltimate() const { return bIsUltimate; }

	// 大招点是否已攒满。**非大招恒 true** —— 普通技能不该受大招点影响。
	bool IsUltPointsReady() const { return IsUltPointsReady(GetCurrentActorInfo()); }
	bool IsUltPointsReady(const FGameplayAbilityActorInfo* ActorInfo) const;

	// 扣光大招点（服务器权威，非权威机调用直接忽略）。
	// 大招在**正确的时机**自己调 —— 不一定在激活时，见 bSpendUltPointsOnActivate 的注释。
	UFUNCTION(BlueprintCallable, Category = "Ability|Ultimate")
	void SpendUltPoints();

	// —— 冲刺（顺风） ——
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Dash")
	float DashSpeed = 3500.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Dash")
	float DashDuration = 0.5f;

	/*
	 * ——— 放完技能之后的空手蒙太奇（Jett E / Q 那一段）———
	 *
	 * 填了就会在**技能动作做完那一刻**进入 ECS_EmptyHand：手上的枪收回挂点、身体走空手姿态，
	 * 这段时间任何操作都插不进来，播完自动掏回最强的武器（主武器优先）。
	 * 三条全空 = 这个技能不启用这套（默认），所以没配的技能行为一点不变；
	 * 走状态机路线、一个槽都不想填的话，勾下面的 bEnableEmptyHandWithoutMontage。
	 *
	 * 触发点（基类里替你调好了，子类一般不用管）：
	 *   · 一次性技能（Q 腾空这种）→ ActivateAbility 里 ExecuteSkillAction() 之后**立刻**
	 *   · 冲刺类技能（E 逐风）→ **按下释放的那一帧**（StartDash 之后立刻，和上面同一拍）：
	 *     二段式的第二段按下 / 不走二段式的第一次按下都一样。这样收枪和冲刺动画
	 *     跟着按键起手，而不是等冲刺位移走完（0.5s）人站定了才开始挥。
	 *     **进**空手只有这一拍；**出**空手只认蒙太奇上的收尾通知 —— 冲刺结束那边不碰它
	 *     （见 HandleDashFinished 的注释，那里删掉的那句兜底就是"播两遍"的来源）。
	 *   · 武装（第一段）不触发 —— 那时候还没真的放出去
	 * 子类若把动作做成了更长的一串（自己的计时器 / 自己的收尾回调），在收尾那里调一下
	 * StartEmptyHandIfConfigured() 即可。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability|EmptyHand")
	FBlasterEmptyHandMontages EmptyHandMontages;

	/*
	 * 一个蒙太奇槽都不填，也照样进 ECS_EmptyHand —— 动画完全交给 ABP 的状态机自己播。
	 *
	 * 勾这个的场景：不想走"蒙太奇盖在槽上"这条路，而是在动画蓝图里读 bEmptyHand
	 * 切一个专门的状态。这时状态上的那三个槽留空，角色不会自己播任何东西
	 * （PlayEmptyHandMontages 对空槽是空转），但"不可打断 + 自动掏最强武器"这套照样生效。
	 *
	 * ⚠ 走这条路有两个额外责任，都落在 ABP 这边：
	 *   1. 状态必须自己退出 —— 没有蒙太奇就没有末帧通知，收尾只能靠状态机的退出条件调
	 *      UBlasterCharacterAnimInstance::FinishEmptyHand()（或它对应的第一人称版本）。
	 *   2. 保险丝这时靠 ABlasterCharacter::EmptyHandFallbackDuration 兜底（一个槽都没填，
	 *      算不出蒙太奇长度）—— 把它设成**不小于**状态机长度，否则保险丝会在动画播到一半时
	 *      把人拉回 ECS_Unoccupied（表现就是动画被硬切一刀）。
	 *      （这是保险丝最要紧的场景：这条路上"收尾通知"根本不存在，状态机的退出条件就是唯一
	 *        的主路，服务器只能靠保险丝兜。填了蒙太奇的那条路也有保险丝，但会给通知让路 ——
	 *        见 ABlasterCharacter::StartEmptyHandTimer。）
	 *
	 * 默认 false：不勾就等于旧行为（三条全空 = 这个技能不进入空手流程）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability|EmptyHand")
	bool bEnableEmptyHandWithoutMontage = false;

	/*
	 * 空手中（ECS_EmptyHand）也允许放这个技能 —— 默认 false，也就是维持"谁都不许插进来"。
	 *
	 * 目前**只有 Jett E（逐风）打开**，为的是"起风 → Q 腾空 → 空中按 E 冲出去"这套连招：
	 * Q 的空手那零点几秒里按第二下 E 本来会被 CanActivateAbility 挡掉，整套动作就断了。
	 *
	 * 打开之后这个技能的**两段**都放行（武装那一下也放行）：
	 *   · 只放行第二段的话，"Q 途中直接按 E"打不出来 —— 那时还没武装，第一下就被挡掉了，
	 *     玩家得先起好风再按 Q 才行。两段一起放行才符合直觉。
	 *   · 一段是纯状态（扣充能 + 起风，没有动画也没有收枪），放行它不会让手和枪对不上。
	 *   · 二段（真冲出去）会**顶掉当前那段空手蒙太奇**：停旧的、播冲刺那条（收尾从此跟着
	 *     新那条的收尾通知走），见 ABlasterCharacter::RestartEmptyHand。
	 *
	 * ⚠ 别拿 bUseArmedActivation 当判据：那是"二段式激活"的开关，和"空手中放不放行"
	 *   是两件事，混用会让以后任何一个新的二段式技能也偷偷穿进空手。
	 *
	 * ⚠ 打开前想清楚收尾：空手收尾会掏回最强的武器，这个技能自己的动画再把人拽走的话，
	 *   两边的收尾会抢时序 —— 目前只有"空手 → 再进空手（顶掉旧的）"这一种组合是安全的。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability|EmptyHand")
	bool bAllowedDuringEmptyHand = false;

	// 这个技能配了空手蒙太奇没有（三条全空 = 没配）
	bool HasEmptyHandMontages() const { return !EmptyHandMontages.IsEmpty(); }

	/*
	 * ——— 持投掷物的**动作**动画：拿起 / 丢出去 / 按住 / 收起 ———
	 *
	 * 只对**持投掷物类**的技能有意义（Phoenix E 闪光 / Q 火球 / C 火墙 —— 按技能键收枪 →
	 * 手上拿着东西 → 丢出去/再按一下收起来），每段的资产填在下面四组里，每组三条槽
	 * （第一人称 / 第三人称上半身 / 第三人称下半身，形状和上面的空手那套一样）。
	 *
	 * ★ 全是**一次性**动作，彼此不接续（谁在哪一刻播见 ABlasterCharacter::PlayThrowable*）：
	 *   ① 按下技能键（收枪那一刻）→ Equip
	 *   ② 丢出去那一下 → Throw（闪光 / 火球走这条）
	 *   ③ 按住左键 → Fire（火墙控球那段）；④ 松手 / 球飞完 → Lift（收起）
	 *   这四段演完都回到"举着待命"那个姿势 —— 那一段**不在 C++ 里**，是动画蓝图按
	 *   WeaponType = EWT_Phoenix* 切过去的（见 UBlasterGameplayAbility 上面那三个 WeaponType 值的注释）。
	 *   所以别在这里做循环、也别把"站着待命"塞进某一组槽里。
	 *
	 * ★ 每段的**收尾时刻**由用户在**第三人称蒙太奇末尾**摆的动画通知
	 *   UAnimNotify_ThrowableSkillFinished 说了算（什么时候算演完、什么时候掏枪）；
	 *   C++ 只有一根"通知漏摆"的备胎计时器。
	 *   打断（再按一下 Q/E/C、按 1/2/3、死亡…）随时可以，整套直接停掉回正常姿势。
	 *
	 * ★ 填 AnimSequence 就行，不必自己做蒙太奇 —— 和空手那套一样，C++ 播放时按槽名现造一条
	 *   动态蒙太奇把序列塞进去（槽名在**角色**上，见 ABlasterCharacter::EmptyHandFPSlotName 一族的注释）。
	 *   已经做成 AnimMontage 的也能填（走 Montage_Play，自带槽名/通知）。
	 *
	 * ★ 四组**各自独立留空**：没做的那一段什么都不播（不会"半截卡住"）。
	 *   四组全空 = 这个技能不播任何投掷物动画（现有行为，一点不变）。
	 *
	 * ⚠️ 槽位顺序（哪个技能在技能条第几个）和这里无关：那是 SkillSlotIndex，
	 *    两者是"显示在哪"和"播什么动画"两件事。
	 */
	// ① 拿起：按下技能键那一刻（收枪的同时）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Throwable")
	FBlasterThrowableMontages ThrowableEquipMontages;

	// ② 丢出去：那一下的一次性动画（闪光 / 火球走这条）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Throwable")
	FBlasterThrowableMontages ThrowableThrowMontages;

	// ③ 按住（**目前只有火墙 C 用**）：按住左键控球那一段，一次性
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Throwable")
	FBlasterThrowableMontages ThrowableFireMontages;

	// ④ 收起（同上，火墙专用）：松手 / 球飞完那一下，一次性
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Throwable")
	FBlasterThrowableMontages ThrowableLiftMontages;

	// 这个技能配了投掷物动画没有（四组全空 = 没配，播的时候全是空转）
	bool HasThrowableMontages() const
	{
		return !ThrowableEquipMontages.IsEmpty() || !ThrowableThrowMontages.IsEmpty()
			|| !ThrowableFireMontages.IsEmpty() || !ThrowableLiftMontages.IsEmpty();
	}

	// 这个技能要不要进空手流程：填了任意一个槽，或者显式勾了"不用蒙太奇"
	bool IsEmptyHandEnabled() const { return bEnableEmptyHandWithoutMontage || HasEmptyHandMontages(); }

	// —— 二段式武装（Valorant Jett E 逐风） ——
	// 开启后：第一次按键 = 武装（消耗 1 层充能 + 起风），ArmedWindowDuration 秒内再按 = 释放冲刺；
	// 窗口内未释放则充能作废。关闭则保持旧行为（按一下直接冲）。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|TwoStage")
	bool bUseArmedActivation = false;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|TwoStage")
	float ArmedWindowDuration = 12.f;

	// 当前是否处于武装（逐风）状态。实例成员不复制——客户端靠预测保持一致。
	// 超过武装窗口自动视为未武装（防御：引擎 EndAbility 会清能力定时器，定时器可能失效）。
	bool IsArmed() const { return bArmed && (!GetWorld() || GetWorld()->GetTimeSeconds() < ArmedDeadline); }
	// 原始武装标记（不被窗口期限屏蔽），角色 Tick 兜底清理过期武装态用
	bool IsArmedRaw() const { return bArmed; }

	// 武装窗口到期强制解除（防御：GAS EndAbility 会 ClearAllTimersForObject 清掉能力定时器）
	void ForceExpireArmedWindow();

	// 武装窗口剩余秒数（未武装返回 0），HUD 倒计时用
	float GetArmedTimeRemaining() const;

	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override;
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility) override;

protected:
	void StartDash();
	void StopDash();

	// 一次性技能激活时的"技能动作"（服务器施加冷却后调用）。
	// 基类默认 = StartDash（冲刺类技能）；子类覆写为抛掷闪光等（如 UPhoenixCurveballAbility）。
	// ⚠️ LocalPredicted 客户端预测实例也会跑它：子类若只在服务器上做事（如 SpawnActor），需自行守卫权威。
	virtual void ExecuteSkillAction();

	/*
	 * 技能动作做完了 → 有配就进入空手蒙太奇。
	 * 服务器和预测客户端都会调（LocalPredicted 两边都跑）；真正的状态切换 / 收枪 / 计时器
	 * 都在 ABlasterCharacter::BeginEmptyHand 里按权威分派，能力这边只管"什么时候"。
	 *
	 * @param bRestartIfAlreadyEmptyHanded 已经在空手里时**顶掉**当前那段（而不是幂等早退），
	 *        给二段式释放那条路用（人从 Q 的空手里冲出去，见 bAllowedDuringEmptyHand）。
	 *        另一处调用点（一次性技能那条）保持默认 false —— 那是"已经在里面了就别动"的语义。
	 *        冲刺结束那边**已经不调这个函数**了（收尾只认蒙太奇通知，见 HandleDashFinished）。
	 */
	void StartEmptyHandIfConfigured(bool bRestartIfAlreadyEmptyHanded = false);

	/*
	 * 空手**刚起来**的那一拍，紧接着 StartEmptyHandIfConfigured 里的那次 Begin/RestartEmptyHand 之后
	 * （服务器上才会真的有事发生；非权威机器上角色那两个函数本来就是空转）。
	 *
	 * 默认空转。要覆写它的场景只有一种：**这段空手该持续多久由玩法决定、不是由动画长度决定**。
	 * 角色的保险丝（ABlasterCharacter::StartEmptyHandTimer）是按动画长度算的 —— 那种技能会算出
	 * 一个太短的保险丝，表现是"动画还在播、人已经被拉回 ECS_Unoccupied"。
	 * 覆写里调一下 ABlasterCharacter::SetEmptyHandFuseDuration 把它按玩法时长重设即可，
	 * 例子见 UJettCloudburstAbility（空手要撑到云飞完，不是撑到蒙太奇播完）。
	 *
	 * ⚠ 时序上必须排在这儿：基类那句 StartEmptyHandIfConfigured 是在 ExecuteSkillAction()
	 *   **之后**调的，所以子类在 ExecuteSkillAction 里设保险丝会被它随后按动画长度重新算掉。
	 */
	virtual void OnEmptyHandStarted();

	// 冲刺自然结束 / 撞墙（CMM OnBlasterDashFinished 回调）→ EndAbility。
	// ⚠ 它**不管空手**（收尾归蒙太奇通知，见实现里的注释）—— 这里只收能力自己的生命周期。
	void HandleDashFinished();

	// 距最早到期冷却实例的剩余秒数（= 下一层恢复时间；无冷却中返回 0）
	float GetTimeUntilNextCharge(const UAbilitySystemComponent* ASC) const;

	// 施加一层充能冷却：顺序恢复——新一层的时长 = 当前最早到期冷却剩余 + 单层时长，
	// 保证第二层在第一层回满后才开始走自己的完整计时（充能点进度从 0 开始）
	void ApplyChargeCooldown();

	// 武装状态变化钩子：基类实现 = 通知角色显隐风特效（SetSkillArmed）；子类可扩展音效等
	virtual void OnArmedChanged(bool bNowArmed);

	// 武装窗口到期（未释放，充能作废）
	void OnArmedWindowExpired();

	// 本次冲刺的世界方向，StartDash 里赋值。★ 初值给**零向量**（不是 ForwardVector）：
	// 它同时被 StartEmptyHandIfConfigured 当成"这一趟有没有确定方向"的判据传给角色，
	// 零 = 没有，让角色自己按速度推。给个 ForwardVector 就等于每次都谎报"方向是正前方"。
	FVector DashDirection = FVector::ZeroVector;

private:
	bool bArmed = false;
	FTimerHandle ArmedTimer;
	double ArmedDeadline = 0.0;
};
