#include "BlasterGameplayTagLibrary.h"
#include "Blaster/Abilities/BlasterGameplayTags.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"

FGameplayTagContainer UBlasterGameplayTagLibrary::GetJettDashAbilityTags()
{
	FGameplayTagContainer Container;
	Container.AddTag(BlasterGameplayTags::Ability_Jett_Dash);
	return Container;
}

FGameplayTagContainer UBlasterGameplayTagLibrary::GetJettDashCooldownTags()
{
	FGameplayTagContainer Container;
	Container.AddTag(BlasterGameplayTags::Cooldown_Jett_Dash);
	return Container;
}

void UBlasterGameplayTagLibrary::SetJettDashAbilityTags(UBlasterGameplayAbility* AbilityCDO)
{
	if (!AbilityCDO)
	{
		return;
	}
	AbilityCDO->AbilityTags.AddTag(BlasterGameplayTags::Ability_Jett_Dash);
}

void UBlasterGameplayTagLibrary::SetPhoenixCurveballAbilityTags(UBlasterGameplayAbility* AbilityCDO)
{
	if (!AbilityCDO)
	{
		return;
	}
	AbilityCDO->AbilityTags.AddTag(BlasterGameplayTags::Ability_Phoenix_Curveball);
}
