#include "BlasterGameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"	// FBlasterEmptyHandMontages::GetLongestPlayLength 要 GetPlayLength()
#include "Blaster/BlasterTypes/MovementDirection.h"	// MovementDirection8::ToSectionName
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterComponent/BlasterMovementComponent.h"

UBlasterGameplayAbility::UBlasterGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
}

float FBlasterEmptyHandMontages::GetLongestPlayLength(const UAnimSequenceBase* FP, const UAnimSequenceBase* UB, const UAnimSequenceBase* LB)
{
	// 三条骨架不一样、时长也可能不一样（上下半身分开导出时尤其容易差一两帧）。
	// 服务器那条兜底计时器得等**最后一条**播完才收尾，所以取最长的那条 —— 取最短会出现
	// "动画还在动，枪已经掏出来了"，枪和空手姿势叠在一起。
	float Longest = 0.f;
	for (const UAnimSequenceBase* M : {FP, UB, LB})
	{
		if (M)
		{
			Longest = FMath::Max(Longest, M->GetPlayLength());
		}
	}
	return Longest;
}

FName FBlasterEmptyHandMontages::ResolveSectionName(const UAnimMontage& Montage, EMovementDirection8 Direction)
{
	// 1) 精确匹配（八向资产走这条）
	const FName ExactName = MovementDirection8::ToSectionName(Direction);
	if (Montage.IsValidSectionName(ExactName))
	{
		return ExactName;
	}

	// 2) 退到四向：资产只做了前后左右（第一人称手模就是），斜向合并到东 / 西
	const EMovementDirection8 Cardinal = MovementDirection8::CollapseToCardinal(Direction);
	if (Cardinal != Direction)
	{
		const FName CardinalName = MovementDirection8::ToSectionName(Cardinal);
		if (Montage.IsValidSectionName(CardinalName))
		{
			return CardinalName;
		}
	}

	// 3) 都没有 → 调用方按"整条播"处理
	return NAME_None;
}

float FBlasterEmptyHandMontages::GetSectionLength(const UAnimSequenceBase* Asset, EMovementDirection8 Direction)
{
	// 八个方向是一个蒙太奇里的 8 个分段（名字 = 枚举名，见 MovementDirection8::ToSectionName）。
	// 这里要的是"**这一趟实际会播的那一段**有多长"，不是整条蒙太奇的长度。
	//
	// ⚠ 挑段必须和播放那边用同一个 ResolveSectionName：第一人称只有四段，斜向会退到平移，
	//   这里要是还按八向的段名去查就会查不到 → 退回"整条长度"→ 保险丝被撑成 4 段之和。
	if (Asset == nullptr) return 0.f;

	// 裸序列（现造动态蒙太奇那条路）：只有一刀切，长度就是它自己的长度
	const UAnimMontage* AsMontage = Cast<UAnimMontage>(Asset);
	if (AsMontage == nullptr) return Asset->GetPlayLength();

	// 没挑出段来（单方向的老资产）→ 退回整条的长度，和"不跳段、整条播"的行为对上
	const FName SectionName = ResolveSectionName(*AsMontage, Direction);
	if (SectionName == NAME_None) return AsMontage->GetPlayLength();

	return AsMontage->GetSectionLength(AsMontage->GetSectionIndex(SectionName));
}

float FBlasterEmptyHandMontages::GetLongestSectionLength(const UAnimSequenceBase* FP, const UAnimSequenceBase* UB,
	const UAnimSequenceBase* LB, EMovementDirection8 Direction)
{
	float Longest = 0.f;
	for (const UAnimSequenceBase* M : {FP, UB, LB})
	{
		Longest = FMath::Max(Longest, GetSectionLength(M, Direction));
	}
	return Longest;
}

float FBlasterEmptyHandMontages::GetFuseDuration(const UAnimSequenceBase* FP, const UAnimSequenceBase* UB,
	const UAnimSequenceBase* LB, EMovementDirection8 Direction)
{
	/*
	 * ★ 以**第一人称**那条为准 —— 三条是给三块不同屏幕看的，而"这次动作为什么要等"
	 *   只影响射手本人那块（第三人称那具身体对他 SetOwnerNoSee）。
	 *   取三条里最长的那条 = 让射手替他**看不见**的那条动画等：实测 N 方向
	 *   FP 0.6167s / TP 0.8333s → 手模在末帧上定格 0.2167s 才等到掏枪。
	 *
	 * 代价：FP 比 TP 短的方向，别人屏幕上这条冲刺动画的尾巴被切掉（N 最多 0.2167s），
	 * 接缝是掏枪蒙太奇的 0.25s blend-in。要两边都不切只能把 FP/TP 的分段长度对齐（资产活）。
	 */
	const float FP_Length = GetSectionLength(FP, Direction);
	if (FP_Length > 0.f) return FP_Length;

	// 没配第一人称（走 ABP 状态机那条路，或者只配了第三人称）→ 退回三条里最长的那个
	return GetLongestSectionLength(FP, UB, LB, Direction);
}

int32 UBlasterGameplayAbility::GetCharges() const
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	return GetCharges(ASC);
}

int32 UBlasterGameplayAbility::GetCharges(const UAbilitySystemComponent* ASC) const
{
	if (!ASC || !CooldownEffectClass) return MaxCharges;
	const int32 ActiveCooldowns = ASC->GetGameplayEffectCount(CooldownEffectClass, nullptr);
	return FMath::Max(0, MaxCharges - ActiveCooldowns);
}

float UBlasterGameplayAbility::GetTimeUntilNextCharge(const UAbilitySystemComponent* ASC) const
{
	if (!ASC || !CooldownEffectClass) return 0.f;

	// 冷却 GE 按 EffectDefinition 匹配，拿所有活跃实例的剩余时间 → 取最早到期的
	FGameplayEffectQuery Query;
	Query.EffectDefinition = CooldownEffectClass;
	const TArray<float> Remaining = ASC->GetActiveEffectsTimeRemaining(Query);

	float Min = -1.f;
	for (const float R : Remaining)
	{
		if (Min < 0.f || R < Min) Min = R;
	}
	return FMath::Max(0.f, Min);
}

void UBlasterGameplayAbility::ApplyChargeCooldown()
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC || !CooldownEffectClass) return;

	// 单层冷却时长（从冷却 GE 的 Duration 读，兜底 7.5s）
	float ChargeDuration = 7.5f;
	const UGameplayEffect* Def = CooldownEffectClass->GetDefaultObject<UGameplayEffect>();
	// ★ "局内不恢复"的特例：冷却 GE 用 DurationPolicy=Infinite 时**绝不能碰 spec 的 Duration**。
	//   原因在引擎里：FGameplayEffectSpec::SetDuration 带 bDurationLocked 守卫
	//   （GameplayEffect.cpp:1788），只要在应用前 SetDuration(x, true) 锁过一次，后面引擎
	//   自己那次 SetDuration(INFINITE_DURATION, false)（同文件 4101~4107）就静默失效，
	//   于是这层冷却会在 x 秒后自己到期 ——「一格充能、局内不恢复」会悄悄变成
	//   「每 7.5 秒回一格」，而且 HUD 上看起来还算正常，极难查。
	//   不锁的话引擎会把它设成 -1(INFINITE_DURATION)：不注册到期定时器（同文件 4128 只在
	//   Duration > 0 时注册），CheckDuration 里 `Duration > 0.f` 也为假 → 永不失效。
	//   GetActiveEffectsTimeRemaining 对无限效果返回 -1 → GetTimeUntilNextCharge 归 0
	//    → HUD 不显示倒计时、充能点一直灰，正是想要的表现。
	//   一回合结束角色被销毁重建（新 ASC）时它自然消失，所以不用手动清。
	const bool bInfiniteCooldown = (Def && Def->DurationPolicy == EGameplayEffectDurationType::Infinite);

	FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
	FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(CooldownEffectClass, 1.f, Ctx);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	if (bInfiniteCooldown)
	{
		ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
		return;
	}

	if (Def && Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		Def->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, ChargeDuration);
	}
	if (ChargeDuration <= 0.f)
	{
		ChargeDuration = 7.5f;
	}

	// 顺序恢复：新一层时长 = 当前最早到期冷却的剩余 + 单层时长。
	// 第一层（无冷却中）时长 = 7.5s；之后每层都等上一层回满再开始自己的完整计时。
	const float CurrentRemaining = GetTimeUntilNextCharge(ASC);
	const float NewDuration = FMath::Max(ChargeDuration, CurrentRemaining + ChargeDuration);

	SpecHandle.Data->SetDuration(NewDuration, true);
	ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
}

FBlasterAbilityCooldownInfo UBlasterGameplayAbility::GetCooldownInfo(const UAbilitySystemComponent* ASC) const
{
	FBlasterAbilityCooldownInfo Info;
	Info.MaxCharges = MaxCharges;

	if (!ASC || !CooldownEffectClass)
	{
		// 找不到 ASC / 未配置冷却 → 视为满充能但无效（HUD 隐藏）
		Info.Charges = MaxCharges;
		Info.bValid = false;
		return Info;
	}

	Info.Charges = GetCharges(ASC);
	Info.TimeUntilNextCharge = GetTimeUntilNextCharge(ASC);

	// 单层冷却时长（从冷却 GE 的 Duration 读）
	const UGameplayEffect* Def = CooldownEffectClass->GetDefaultObject<UGameplayEffect>();
	if (Def && Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
	{
		Def->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Info.CooldownDuration);
	}

	Info.bValid = true;
	return Info;
}

bool UBlasterGameplayAbility::IsUltPointsReady(const FGameplayAbilityActorInfo* ActorInfo) const
{
	// 非大招不受大招点门控
	if (!bIsUltimate) return true;

	const APawn* Pawn = ActorInfo ? Cast<APawn>(ActorInfo->AvatarActor.Get()) : nullptr;
	const ABlasterPlayerState* PS = Pawn ? Pawn->GetPlayerState<ABlasterPlayerState>() : nullptr;

	// 拿不到 PS（测试机器人 / 大厅里还没进对局的裸 Pawn）→ 放行。
	// 这里刻意"失败开放"：挡错的后果是技能完全放不出来、还查不出原因（TestBot 就没 PS）；
	// 而"该不该让这个角色放大招"在角色侧还有一层 bDisableGameplay 门槛兜着。
	if (!PS) return true;

	return PS->IsUltReady();
}

void UBlasterGameplayAbility::SpendUltPoints()
{
	if (!bIsUltimate) return;

	// 只在服务器扣：LocalPredicted 下客户端也会跑 ActivateAbility，两边都扣 = 双倍消耗。
	const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo();
	if (!Info || !Info->IsNetAuthority()) return;

	const APawn* Pawn = Cast<APawn>(Info->AvatarActor.Get());
	ABlasterPlayerState* PS = Pawn ? Pawn->GetPlayerState<ABlasterPlayerState>() : nullptr;
	if (PS)
	{
		PS->SpendAllUltPoints();
	}
}

bool UBlasterGameplayAbility::CanActivateAbility(
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
	 * 空手中：默认谁都不许放技能（E / Q / C / X 一起，都从这条过）。
	 *
	 * 放在最前面、连武装第二段一起挡掉：那段空手是**不可打断**的（见 ECombatState::ECS_EmptyHand），
	 * 中途插一个冲刺/大招，枪已经挂回背上了，动画却把人拽走 —— 收尾时"掏回最强的武器"
	 * 会和技能自己的收枪/掏枪互相打架，谁先谁后全看时序。
	 *
	 * ★ 例外：技能资产上勾了 bAllowedDuringEmptyHand 的（目前只有 Jett E 逐风）—— 它要能在
	 *   Q 腾空那段空手里冲出去。放行之后不是"动画把人拽走"了：E 会接手那段空手
	 *  （停掉 Q 的蒙太奇、播冲刺那条、保险丝按冲刺重算，见 ABlasterCharacter::RestartEmptyHand），
	 *   收尾还是同一个 EmptyHandFinish，不会两边抢。
	 *
	 * 读的是角色身上那个**复制过来的** ECS_EmptyHand，所以客户端在预测阶段就挡住了
	 * （不会出现"本地先冲出去、服务器不认、再被拉回来"）。克隆没拿到角色的极端情况放行 ——
	 * 挡错的代价是技能彻底放不出来且查不出原因，而空手那几百毫秒本身有服务器兜底。
	 */
	if (const ABlasterCharacter* Character = ActorInfo ? Cast<ABlasterCharacter>(ActorInfo->AvatarActor.Get()) : nullptr)
	{
		if (Character->IsEmptyHandLocked() && !bAllowedDuringEmptyHand)
		{
			return false;
		}
	}
	if (MaxCharges <= 0)
	{
		return false;
	}
	// 大招门控：点数没攒满就不给放。放在武装早退之前 —— 武装是"已经付过费"的状态，
	// 但走武装的大招同样得先过点数这关（真有这种设计的话）。
	if (!IsUltPointsReady(ActorInfo))
	{
		return false;
	}
	// 已武装 → 允许释放（第二段不受充能限制）
	if (bUseArmedActivation && bArmed)
	{
		return true;
	}
	// 充能检查：活跃冷却实例数 >= MaxCharges 说明层数用完，不能施放
	if (ActorInfo && CooldownEffectClass)
	{
		UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
		if (ASC)
		{
			const int32 ActiveCooldowns = ASC->GetGameplayEffectCount(CooldownEffectClass, nullptr);
			if (ActiveCooldowns >= MaxCharges)
			{
				return false;
			}
		}
	}
	return true;
}

void UBlasterGameplayAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	// 每次激活先把上一趟的冲刺方向清掉：这个能力是 InstancedPerActor（实例跨次激活复用），
	// DashDirection 不清就会让"这次根本没冲刺"的技能（Q 腾空、大招……）也拿到上次的残值，
	// 空手动画会跳到一个和实际移动方向无关的分段去。StartDash 里会重新赋值。
	DashDirection = FVector::ZeroVector;

	if (bUseArmedActivation && !bArmed)
	{
		// —— 第一段：武装（消耗 1 层充能 + 起风，窗口内再按释放） ——
		// 服务器施加冷却：非堆叠 Duration GE，每个独立实例 = 每层独立计时
		if (ActorInfo && ActorInfo->IsNetAuthority() && CooldownEffectClass)
		{
			ApplyChargeCooldown();
		}
		bArmed = true;
		ArmedDeadline = GetWorld()->GetTimeSeconds() + ArmedWindowDuration;
		OnArmedChanged(true);

		// 第一段立即结束（实例保持，可再次激活进入第二段）。
		// ⚠️ 引擎 EndAbility 会 ClearAllTimersForObject(this)（AbilitySystem.ClearAbilityTimers=1 默认开），
		// 武装窗口定时器必须放在 EndAbility 之后设，否则 12s 到期永不触发（武装态残留）。
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		GetWorld()->GetTimerManager().SetTimer(ArmedTimer, this, &UBlasterGameplayAbility::OnArmedWindowExpired, ArmedWindowDuration, false);
		return;
	}

	if (bUseArmedActivation && bArmed)
	{
		// —— 第二段：释放（冲刺） ——
		bArmed = false;
		ArmedDeadline = 0.0;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(ArmedTimer);
		}
		OnArmedChanged(false);
		StartDash();

		/*
		 * ★ 按下第二下的**这一帧**就进空手蒙太奇（收枪 + 播冲刺动画），不等冲刺位移走完。
		 *
		 * 为什么必须放在这儿：冲完之后再播等于"人已经冲到位、站定了，动画才刚开始" ——
		 * 按 E 那一下到动画起手之间隔着整个 DashDuration（0.5s），看起来就是动画没跟上手。
		 * 一次性技能那条路本来就走在同一拍上（见下面 ExecuteSkillAction 之后那句），
		 * 这里原来少了这一句，属于二段式分支提前 return 漏掉的。
		 *
		 * 方向也更准：此刻 CMM 刚把 Velocity 清成 0（StartDash 里），ResolveEmptyHandDirection
		 * 会直接落到 StartDash 算好的 DashDirection 上，不会去猜速度。
		 *
		 * ★ 冲刺结束那边**不再碰空手**（HandleDashFinished 里那句兜底已删）：空手什么时候结束
		 *   只认挂在蒙太奇上的收尾通知（外加角色上那条保险丝，只在通知没响时才到点），
		 *   见 ABlasterCharacter::EmptyHandFinish / StartEmptyHandTimer。
		 *   原来那句兜底是为了盖"蒙太奇比冲刺短、空手提前收尾"的边角，但通知就挂在冲刺结束
		 *   那一刻附近（差不到一帧），"提前收尾"其实是常态 → 一次冲刺进两次空手、冲刺动画
		 *   从头播两遍。多一条收尾路就多一个时序分叉，删掉比给它补门禁省事。
		 *
		 * ★ 这里传 true（重入时顶掉当前那段）：这条路现在是**唯一**能撞上"已经在空手里"的
		 *   调用点 —— 勾了 bAllowedDuringEmptyHand 的技能可以在这段空手里被激活，
		 *   于是"Q 的空手还在播、E 的二段冲出去"这时 StartDash 和 BeginEmptyHand 是同一拍。
		 *   不顶掉的话人冲出去了动画还停在 Q，收尾也还按 Q 那条通知的时序走。
		 *   另一处调用点（下面一次性技能那条）保持默认 false，语义是"已经在里面了就别动"。
		 */
		StartEmptyHandIfConfigured(/*bRestartIfAlreadyEmptyHanded=*/true);
		return;
	}

	// 传统一次性技能：服务器施加冷却 + 执行技能动作（基类=冲刺；子类可覆写为抛掷等）
	if (ActorInfo && ActorInfo->IsNetAuthority() && CooldownEffectClass)
	{
		ApplyChargeCooldown();
	}
	// 大招点：激活即扣（可按技能关掉，见 bSpendUltPointsOnActivate）。
	// 和冷却一样只在服务器扣 —— LocalPredicted 下客户端也跑这条，两边都扣就双倍。
	if (bIsUltimate && bSpendUltPointsOnActivate && ActorInfo && ActorInfo->IsNetAuthority())
	{
		SpendUltPoints();
	}
	ExecuteSkillAction();

	// 动作已经踢出去了（Q 是把人往上弹，那一瞬间就完成）→ 有配就接着进空手蒙太奇。
	// 冲刺类（子类没覆写 ExecuteSkillAction）也走在同一拍上：StartDash 已经把人踢出去了，
	// 冲刺动画就从这一帧起手（见上面二段式分支里那段注释）。冲刺结束那边不碰空手。
	StartEmptyHandIfConfigured();
}

void UBlasterGameplayAbility::ExecuteSkillAction()
{
	StartDash();
}

void UBlasterGameplayAbility::StartEmptyHandIfConfigured(bool bRestartIfAlreadyEmptyHanded)
{
	// 没填槽、也没勾"不用蒙太奇" = 这个技能不启用这套，一点行为都不改
	if (!IsEmptyHandEnabled()) return;

	if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo()))
	{
		/*
		 * 顺便把"这一趟往哪个方向去的"报给角色：八个方向的空手动画装在同一个蒙太奇的 8 个分段里，
		 * 角色要靠它决定跳哪一段。
		 *
		 * 只有冲刺类能给出精确方向（DashDirection 是那一趟现算的）；其余技能传零向量，
		 * 角色那边按"速度方向"推 —— 服务器读不到远端玩家的移动输入（LastControlInputVector
		 * 只在本地更新，见 ABlasterCharacter::Move 里那段），但速度是复制的，两边一致。
		 *
		 * ⚠ DashDirection 是**成员变量**且这个能力是 InstancedPerActor（实例跨次激活复用），
		 *   所以 ActivateAbility 里进来时会清一次，保证这里读到的不是上次冲刺留下的残值。
		 */
		if (bRestartIfAlreadyEmptyHanded)
		{
			Character->RestartEmptyHand(EmptyHandMontages, DashDirection);
		}
		else
		{
			Character->BeginEmptyHand(EmptyHandMontages, DashDirection);
		}

		// 刚起来的那一拍交给子类（默认空转）。放这儿而不是放 ExecuteSkillAction 里：
		// 基类这句 StartEmptyHandIfConfigured 排在 ExecuteSkillAction() **之后**，
		// 保险丝是在 BeginEmptyHand 里按动画长度算的 —— 子类想在它之后再改才改得动。
		// 已经在空手里（上面那次是幂等早退）时这一下也是安全的：改保险丝那个函数自己会再判一次。
		OnEmptyHandStarted();
	}
}

void UBlasterGameplayAbility::OnEmptyHandStarted()
{
	// 默认什么都不做 —— 保险丝就用角色按动画长度算出来的那个值。
}


void UBlasterGameplayAbility::CancelAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateCancelAbility)
{
	if (bArmed)
	{
		bArmed = false;
		ArmedDeadline = 0.0;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(ArmedTimer);
		}
		OnArmedChanged(false);
	}
	StopDash();
	Super::CancelAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancelAbility);
}

float UBlasterGameplayAbility::GetArmedTimeRemaining() const
{
	if (!bArmed || !GetWorld()) return 0.f;
	return FMath::Max(0.f, (float)(ArmedDeadline - GetWorld()->GetTimeSeconds()));
}

void UBlasterGameplayAbility::OnArmedChanged(bool bNowArmed)
{
	if (ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo()))
	{
		Char->SetSkillArmed(bNowArmed);
	}
}

void UBlasterGameplayAbility::OnArmedWindowExpired()
{
	ForceExpireArmedWindow();
}

void UBlasterGameplayAbility::ForceExpireArmedWindow()
{
	if (!bArmed) return;
	// 窗口结束未释放 → 充能作废（已在武装时消耗）
	bArmed = false;
	ArmedDeadline = 0.0;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ArmedTimer);
	}
	OnArmedChanged(false);
}

void UBlasterGameplayAbility::StartDash()
{
	ACharacter* Char = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Char)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	// 方向：优先客户端按 E 时 RPC 上来的冲刺方向（服务器读不到远端玩家的
	// GetLastMovementInputVector——LastControlInputVector 只在本地角色更新，远端恒 0），
	// 回退最后移动输入方向（本地/预测可用），再回退面朝方向。
	FVector Dir = FVector::ZeroVector;
	if (ABlasterCharacter* BC = Cast<ABlasterCharacter>(Char))
	{
		BC->ConsumePendingDashDirection(Dir);
	}
	if (Dir.IsNearlyZero())
	{
		Dir = Char->GetLastMovementInputVector();
		if (Dir.IsNearlyZero()) Dir = Char->GetActorForwardVector();
	}
	Dir.Z = 0.f;
	if (!Dir.Normalize()) Dir = Char->GetActorForwardVector();
	DashDirection = Dir;

	// 位移交给 CMM 自定义移动模式（UBlasterMovementComponent::PhysCustom）：
	// 客户端/服务器实例都调 StartDash → 本地玩家立即开冲（CMM 本地预测 + ServerMove 回放 +
	// SmoothCorrection 修正，不再等服务器每帧 SetActorLocation 复制回来 → 客户端不卡）。
	// 客户端和服务器方向一致（客户端算的 = RPC 发的），回放位置一致，无回弹。
	UBlasterMovementComponent* MoveComp = Cast<UBlasterMovementComponent>(Char->GetCharacterMovement());
	if (MoveComp)
	{
		MoveComp->OnBlasterDashFinished.RemoveAll(this);
		MoveComp->OnBlasterDashFinished.AddUObject(this, &UBlasterGameplayAbility::HandleDashFinished);
		MoveComp->StartDash(DashDirection, DashSpeed, DashDuration);
	}

	/*
	 * 这里**故意没有**"冲刺时长"定时器（原来有一条，DashDuration 到点自己 EndAbility）。
	 *
	 * 能力的生命周期收尾只认一条路：CMM 的 OnBlasterDashFinished（位移真的走完 / 撞墙）。
	 * 位移本来就由 CMM 逐帧推进，它什么时候停自己最清楚 —— 再挂一个等长定时器，
	 * 只会在同一帧里多出一个"谁先到"的分叉，而这个分叉已经咬过一次人：
	 * 冲刺收尾那条路和空手收尾的通知谁先到，决定了一次冲刺进几次空手
	 *（见 HandleDashFinished 的注释）。
	 *
	 * 代价：没有自定义移动组件（StartDash 里那个 Cast 失败）时没人广播 → 能力实例不会自己收尾。
	 * 本工程 ABlasterCharacter 恒定带 UBlasterMovementComponent，这条不成立；
	 * 真要复用到别的角色上，在那儿补广播，别在这儿补定时器。
	 */
}

void UBlasterGameplayAbility::HandleDashFinished()
{
	/*
	 * 冲刺位移结束（撞墙也算）→ 只收能力自己的生命周期。
	 *
	 * ★ 这里**不再碰空手** —— 原来还有一句 StartEmptyHandIfConfigured() 当兜底，已经删掉。
	 *   空手什么时候结束只有一个依据：挂在蒙太奇上的 UAnimNotify_EmptyHandFinished
	 *（ABlasterCharacter::EmptyHandFinish；通知没响时还有角色上那条保险丝兜底，
	 *   但那是"收尾"的备胎，不是"再进一次空手"的理由）。那条兜底的语义本来是"人已经在 ECS_EmptyHand 里
	 *   就幂等早退，只有蒙太奇比冲刺短、空手提前收尾了才真再进一次"，听着安全，实际那个
	 *   "边角"就是常态：通知挂在冲刺结束那一刻附近（差不到一帧），两条收尾路谁先到全看
	 *   同一帧里组件 tick 的次序 —— 通知先到的那次，这里看到的已经不是 ECS_EmptyHand 了，
	 *   幂等门禁失效 → 收枪 + 重进空手 + 冲刺动画从头再播一遍。
	 *   一次冲刺播两遍第一人称 dash 动画就是这么来的。
	 *
	 *   所以原则是：**收尾只留一条路**。动画知道"这段演完了"，那就让动画说；
	 *   能力这边只管自己的生命周期，不再对同一个状态做第二遍判断。
	 */
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UBlasterGameplayAbility::StopDash()
{
	ACharacter* Char = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (Char)
	{
		if (UBlasterMovementComponent* MoveComp = Cast<UBlasterMovementComponent>(Char->GetCharacterMovement()))
		{
			MoveComp->StopDash();
		}
	}
}
