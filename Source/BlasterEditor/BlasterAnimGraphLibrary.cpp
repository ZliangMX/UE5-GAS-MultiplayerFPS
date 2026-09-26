#include "BlasterAnimGraphLibrary.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateNode.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "ReferenceSkeleton.h"

namespace
{
	// 图 → 节点标题（"UpperBody" / "Layered blend per bone" / 状态名 …），比类名好认
	FString NodeLabel(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("<null>");
		}
		FString Label = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" "));
		return Label.IsEmpty() ? Node->GetClass()->GetName() : Label;
	}

	// 深度优先把整张动画图展开：状态机节点带出状态机图，状态节点带出状态里的图
	void GatherGraphs(UEdGraph* Graph, TArray<UEdGraph*>& Out, TSet<UEdGraph*>& Seen, int32 Depth)
	{
		if (!Graph || Seen.Contains(Graph) || Depth > 8)
		{
			return;
		}
		Seen.Add(Graph);
		Out.Add(Graph);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
			{
				GatherGraphs(SMNode->EditorStateMachineGraph, Out, Seen, Depth + 1);
			}
			else if (UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Node))
			{
				GatherGraphs(StateNode->GetBoundGraph(), Out, Seen, Depth + 1);
			}
		}
	}

	FString MaskProfileDesc(const UBlendProfile* Profile)
	{
		if (!Profile)
		{
			return TEXT("<空>");
		}

		FString Out = FString::Printf(TEXT("%s（%s）"),
			*Profile->GetName(),
			Profile->GetMode() == EBlendProfileMode::BlendMask ? TEXT("BlendMask 模式") : TEXT("WeightFactor 模式"));

		const USkeleton* Owner = Profile->OwningSkeleton;
		const int32 Num = Profile->GetNumBlendEntries();
		Out += FString::Printf(TEXT("｜%d 根骨头："), Num);

		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FBlendProfileBoneEntry& Entry = Profile->GetEntry(Index);
			FString BoneName = Entry.BoneReference.BoneName.ToString();
			if (BoneName.IsEmpty() && Owner && Owner->GetReferenceSkeleton().IsValidIndex(Entry.BoneReference.BoneIndex))
			{
				// 编辑器里存的可能只有索引
				BoneName = Owner->GetReferenceSkeleton().GetBoneName(Entry.BoneReference.BoneIndex).ToString();
			}
			Out += FString::Printf(TEXT("%s=%.2f "), *BoneName, Entry.BlendScale);
		}
		return Out;
	}

	FString NodeDetail(const UEdGraphNode* Node)
	{
		if (const UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(Node))
		{
			return FString::Printf(TEXT("槽名 = %s"), *SlotNode->Node.SlotName.ToString());
		}

		if (const UAnimGraphNode_LayeredBoneBlend* BlendNode = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
		{
			const FAnimNode_LayeredBoneBlend& Blend = BlendNode->Node;
			FString Out = FString::Printf(TEXT("模式=%s 网格空间旋转混合=%s｜层数=%d"),
				Blend.BlendMode == ELayeredBoneBlendMode::BlendMask ? TEXT("BlendMask") : TEXT("BranchFilter"),
				Blend.bMeshSpaceRotationBlend ? TEXT("开") : TEXT("关"),
				Blend.BlendPoses.Num());

			for (int32 Layer = 0; Layer < Blend.BlendPoses.Num(); ++Layer)
			{
				const float Weight = Blend.BlendWeights.IsValidIndex(Layer) ? Blend.BlendWeights[Layer] : -1.f;
				Out += FString::Printf(TEXT("\n          层[%d] 权重=%.2f 遮罩="), Layer, Weight);

				if (Blend.BlendMode == ELayeredBoneBlendMode::BlendMask)
				{
					Out += Blend.BlendMasks.IsValidIndex(Layer) ? MaskProfileDesc(Blend.BlendMasks[Layer]) : TEXT("<没填遮罩>");
				}
				else if (Blend.LayerSetup.IsValidIndex(Layer))
				{
					for (const FBranchFilter& Filter : Blend.LayerSetup[Layer].BranchFilters)
					{
						Out += FString::Printf(TEXT("%s(深度%d) "), *Filter.BoneName.ToString(), Filter.BlendDepth);
					}
				}
				else
				{
					Out += TEXT("<没填>");
				}
			}
			return Out;
		}

		if (const UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
		{
			return FString::Printf(TEXT("子图 = %s"),
				SMNode->EditorStateMachineGraph ? *SMNode->EditorStateMachineGraph->GetName() : TEXT("<空>"));
		}

		if (const UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Node))
		{
			const UEdGraph* Bound = StateNode->GetBoundGraph();
			return FString::Printf(TEXT("状态图 = %s"), Bound ? *Bound->GetName() : TEXT("<空>"));
		}

		return FString();
	}
}

FString UBlasterAnimGraphLibrary::DumpAnimGraph(UAnimBlueprint* InAnimBlueprint)
{
	if (!InAnimBlueprint)
	{
		return TEXT("DumpAnimGraph：传入的动画蓝图是空的。");
	}

	// 主图就是那张叫 AnimGraph 的 AnimationGraph
	UEdGraph* Root = nullptr;
	{
		TArray<UEdGraph*> Candidates;
		Candidates.Append(InAnimBlueprint->FunctionGraphs);
		Candidates.Append(InAnimBlueprint->UbergraphPages);
		for (UEdGraph* Graph : Candidates)
		{
			if (Graph && Graph->IsA<UAnimationGraph>())
			{
				Root = Graph;
				break;
			}
		}
	}
	if (!Root)
	{
		return FString::Printf(TEXT("DumpAnimGraph：%s 里没找到 AnimationGraph。"), *InAnimBlueprint->GetName());
	}

	TArray<UEdGraph*> Graphs;
	TSet<UEdGraph*> Seen;
	GatherGraphs(Root, Graphs, Seen, 0);

	FString Out = FString::Printf(TEXT("动画蓝图 %s：共 %d 张图\n"), *InAnimBlueprint->GetName(), Graphs.Num());

	for (UEdGraph* Graph : Graphs)
	{
		Out += FString::Printf(TEXT("\n===== 图 %s（%s）%d 个节点 =====\n"),
			*Graph->GetName(), *Graph->GetClass()->GetName(), Graph->Nodes.Num());

		// 先建索引，连线里好打印"连到第几个节点"
		TMap<const UEdGraphNode*, int32> Index;
		for (int32 NodeIndex = 0; NodeIndex < Graph->Nodes.Num(); ++NodeIndex)
		{
			Index.Add(Graph->Nodes[NodeIndex], NodeIndex);
		}

		for (int32 NodeIndex = 0; NodeIndex < Graph->Nodes.Num(); ++NodeIndex)
		{
			UEdGraphNode* Node = Graph->Nodes[NodeIndex];
			if (!Node)
			{
				continue;
			}

			Out += FString::Printf(TEXT("[%02d] %-34s %s\n"), NodeIndex, *Node->GetClass()->GetName(), *NodeLabel(Node));

			const FString Detail = NodeDetail(Node);
			if (!Detail.IsEmpty())
			{
				Out += FString::Printf(TEXT("       %s\n"), *Detail);
			}

			for (const UEdGraphPin* Pin : Node->GetAllPins())
			{
				if (!Pin || Pin->LinkedTo.Num() == 0)
				{
					continue;
				}
				FString Targets;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* LinkedNode = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
					if (!LinkedNode)
					{
						continue;
					}
					Targets += FString::Printf(TEXT("[%d]%s(%s) "),
						Index.Contains(LinkedNode) ? Index[LinkedNode] : -1,
						*Linked->PinName.ToString(),
						*NodeLabel(LinkedNode));
				}
				Out += FString::Printf(TEXT("        %s %s -> %s\n"),
					Pin->Direction == EGPD_Input ? TEXT("入") : TEXT("出"),
					*Pin->PinName.ToString(), *Targets);
			}
		}
	}

	return Out;
}
