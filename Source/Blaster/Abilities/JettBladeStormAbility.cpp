#include "JettBladeStormAbility.h"

#include "Blaster/Character/BlasterCharacter.h"
// KnivesClass 是 TSubclassOf<AJettKnives>，用的时候要 AJettKnives 的完整定义
//（TSubclassOf::operator* 会调 StaticClass）。**必须显式 include**：
// 之前是靠 unity build 蹭别的 cpp 的 include，编译分组一变就编不过。
#include "Blaster/Weapon/JettKnives.h"

UJettBladeStormAbility::UJettBladeStormAbility()
{
	bIsUltimate = true;

	// 扣点时机：**大招结束才扣**（"飞刀用完大招充能开始重新算"）。
	// 和 Phoenix「回到标记点才扣」是同一套思路 —— 时机按技能定，具体扣点写在
	// ABlasterCharacter::ServerEndBladeStorm()，三条收招路径（扔完 / 到期 / 阵亡）共用它。
	bSpendUltPointsOnActivate = false;

	// 大招不占普通技能槽：技能条最左边那个大招位是 PS 的大招点画的，这里再画一个就会多出一个空槽
	bShowInSkillBar = false;

	// 大招只有一发。消费口是"扣光大招点"而不是冷却 GE，所以充能检查天然恒过 ——
	// MaxCharges 只是个 >0 的占位，让基类的门控不至于 return false。
	MaxCharges = 1;
}

bool UJettBladeStormAbility::CanActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}

	/*
	 * 已经在**刃风暴**里 → 照样允许按 X。
	 *
	 * 这时候 X 不是"再放一次大招"，而是"把飞刀掏出来/收起来"这个开关
	 *（ExecuteSkillAction 里按同一个角色状态分流，见 ServerToggleBladeStormKnives 的注释）。
	 * 用户 2026-09-18："x期间装备别的枪要把knife隐藏了，然后再次按x可以呼出飞镖并且重新播放equip动画"。
	 *
	 * ⚠ 别把这里放开的地方扩大到"任意大招生效中都放行"：刃风暴和别的英雄大招是互斥的
	 *（下面那句就是干这个的），只有刃风暴自己才允许再按一次。
	 * 也不能靠"能力还在激活中"来挡/放 —— 这个能力是即放即结束的，激活标志留不住。
	 */
	const ABlasterCharacter* Char = ActorInfo ? Cast<ABlasterCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (Char && Char->IsUltimateActive() && !Char->IsBladeStormActive())
	{
		return false;
	}

	return true;
}

void UJettBladeStormAbility::ExecuteSkillAction()
{
	const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo();

	// ⚠️ LocalPredicted 下客户端预测实例也会跑到这里，而生成飞刀/计时/收刀全是服务器权威的，
	// 先守卫权威再动手。
	if (Info && Info->IsNetAuthority())
	{
		if (ABlasterCharacter* Char = Cast<ABlasterCharacter>(Info->AvatarActor.Get()))
		{
			// 已经在刃风暴里 → 这一次 X 是"掏出/收起飞刀"，不是再开一次大招。
			// 分流放在角色侧：只有它知道手上的刀、被顶掉的自带刀和当前大招生效状态。
			if (Char->IsBladeStormActive())
			{
				Char->ServerToggleBladeStormKnives();
			}
			else if (!KnivesClass)
			{
				// 没配蓝图子类的 KnivesClass —— 按 X 会静默无效。这条日志是唯一的线索。
				UE_LOG(LogTemp, Warning,
					TEXT("[刃风暴] %s 没有填 KnivesClass，大招不会生效（请在 GA_Jett_BladeStorm 的蓝图子类里填 BP_JettKnives）"),
					*GetName());
			}
			else
			{
				// 生成飞刀 + 塞进近战槽 + 掏出来 + 进入大招生效状态，全在角色侧
				//（那边才有 World、Combat 和大招计时的家）
				Char->ServerStartBladeStorm(KnivesClass, Duration, KnifeCount);
			}
		}
	}

	// 即放即结束：大招的持续状态由角色（生效标志 + 到期定时器）和飞刀武器本身维护，
	// 能力这边不留实例。客户端预测实例也必须一起结束，否则"能力仍在激活中"会把再次释放永久挡死。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
