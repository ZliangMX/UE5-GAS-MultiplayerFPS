#include "PhoenixRunItBackAbility.h"

#include "Blaster/Character/BlasterCharacter.h"

UPhoenixRunItBackAbility::UPhoenixRunItBackAbility()
{
	bIsUltimate = true;

	// ⚠️ 不在这里扣点 —— 回程结算那一刻才扣（见类头注释）
	bSpendUltPointsOnActivate = false;

	// 大招不占普通技能槽：技能条最左边那个大招位是 PS 的大招点画的（走的是另一条路），
	// 这里再画一个就会多出一个空槽。
	bShowInSkillBar = false;

	// 大招只有一发。这里不用配 CooldownEffectClass（消费口是"扣光大招点"，不是冷却 GE），
	// 所以充能检查天然恒过 —— MaxCharges 只是个 >0 的占位，让基类的门控不至于 return false。
	MaxCharges = 1;
}

bool UPhoenixRunItBackAbility::CanActivateAbility(
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

	// 已经有一个大招在生效（再来一次 / Jett 刃风暴）→ 不能再放。
	// 必须靠角色状态挡，不能靠"能力还在激活中" —— 这个能力是即放即结束的（见 ExecuteSkillAction），
	// 激活标志留不住。而且客户端有复制延迟，判客户端本地状态也不可靠，最终以服务器这次判定为准。
	const ABlasterCharacter* Char = ActorInfo ? Cast<ABlasterCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (Char && Char->IsUltimateActive())
	{
		return false;
	}

	return true;
}

void UPhoenixRunItBackAbility::ExecuteSkillAction()
{
	const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo();

	// ⚠️ LocalPredicted 下客户端预测实例也会跑到这里，而放标记 / 计时 / 回程结算全是服务器权威的，
	// 先守卫权威再动手。
	if (Info && Info->IsNetAuthority())
	{
		if (ABlasterCharacter* Char = Cast<ABlasterCharacter>(Info->AvatarActor.Get()))
		{
			// 标记点、计时器、"阵亡时拉回"的拦截全在角色侧（拦截点在 ABlasterCharacter::ServerElim）。
			// 走通用的大招生效入口，把"是哪一个"告诉角色 —— 阵亡时只有 EUA_RunItBack 会拦下死亡。
			Char->ServerStartUltimate(EActiveUltimate::EUA_RunItBack, Duration);
		}
	}

	// 即放即结束：大招的持续状态由角色维护（ActiveUltimate + UltimateEndTime + 到期定时器），能力这边不留实例。
	// 客户端预测实例也必须一起结束，否则"能力仍在激活中"会把再次释放永久挡死。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}
