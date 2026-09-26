#include "BlasterGameInstance.h"

void UBlasterGameInstance::SetTeamByOrder(int32 Order, ETeam Team)
{
	TeamByOrder.Add(Order, Team);
}

ETeam UBlasterGameInstance::GetTeamByOrder(int32 Order) const
{
	const ETeam* Found = TeamByOrder.Find(Order);
	return Found ? *Found : ETeam::ET_None;
}

void UBlasterGameInstance::SetAgentByOrder(int32 Order, EBlasterAgent Agent)
{
	AgentByOrder.Add(Order, Agent);
}

EBlasterAgent UBlasterGameInstance::GetAgentByOrder(int32 Order) const
{
	const EBlasterAgent* Found = AgentByOrder.Find(Order);
	return Found ? *Found : EBlasterAgent::None;
}

void UBlasterGameInstance::ClearTeamAssignments()
{
	TeamByOrder.Empty();
	AgentByOrder.Empty();
}
