#include "BlasterGameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/BlasterComponent/BlasterMovementComponent.h"

UBlasterGameplayAbility::UBlasterGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
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

	FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
	FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(CooldownEffectClass, 1.f, Ctx);
	if (!SpecHandle.IsValid())
	{
		return;
	}
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
	if (MaxCharges <= 0)
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
		return;
	}

	// 传统一次性技能：服务器施加冷却 + 执行技能动作（基类=冲刺；子类可覆写为抛掷等）
	if (ActorInfo && ActorInfo->IsNetAuthority() && CooldownEffectClass)
	{
		ApplyChargeCooldown();
	}
	ExecuteSkillAction();
}

void UBlasterGameplayAbility::ExecuteSkillAction()
{
	StartDash();
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

	// 能力生命周期定时器：冲刺时长后 EndAbility（撞墙由 HandleDashFinished 提前触发）。
	// 位移由 CMM 推进，不依赖能力定时器；EndAbility 会清能力定时器，不影响 CMM 移动。
	GetWorld()->GetTimerManager().SetTimer(DashTimer, this, &UBlasterGameplayAbility::OnDashTimerExpired, DashDuration, false);
}

void UBlasterGameplayAbility::HandleDashFinished()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DashTimer);
	}
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UBlasterGameplayAbility::OnDashTimerExpired()
{
	// 冲刺时长兜底（CMM 通常同时结束；这里收尾能力生命周期）
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UBlasterGameplayAbility::StopDash()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DashTimer);
	}
	ACharacter* Char = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (Char)
	{
		if (UBlasterMovementComponent* MoveComp = Cast<UBlasterMovementComponent>(Char->GetCharacterMovement()))
		{
			MoveComp->StopDash();
		}
	}
}
