// 编辑器用的"动画蓝图结构"读取工具。
//
// ⚠️ 存在的理由和 BlasterAnimMontageLibrary 一样：**Python 读不到这些东西**。
//    UEdGraph::Nodes / UEdGraphNode::Pins / UBlendProfile::ProfileEntries 在 C++ 里是 public，
//    但 UE 的 Python 胶水层一律拒：
//        AnimationGraph: Property 'Nodes' ... is protected and cannot be read
//        BlendProfile:   Property 'ProfileEntries' ... is protected and cannot be read
//    而"哪个槽接了哪条路、每层被哪个遮罩挡住、遮罩里到底有哪些骨头"全在这三样里，
//    光看资产属性（槽名、遮罩资产路径）是拼不出来的。
//
// 只读，不改任何东西。

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlasterAnimGraphLibrary.generated.h"

class UAnimBlueprint;

UCLASS()
class BLASTEREDITOR_API UBlasterAnimGraphLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/*
	 * 把动画蓝图整张动画图（含状态机的子图、状态里的小图）打印成一坨文本：
	 *   · 每个图的每个节点：类名、节点标题、引脚连到谁
	 *   · Slot 节点：槽名
	 *   · LayeredBoneBlend 节点：混合模式、每层权重、每层的遮罩来源
	 *     （BlendMask 模式打遮罩资产里逐根骨头的权重；BranchFilter 模式打骨骼分支过滤）
	 *
	 * @param InAnimBlueprint 目标动画蓝图
	 * @return 文本。InAnimBlueprint 为空或没有 AnimGraph 时返回说明性的字符串。
	 */
	UFUNCTION(BlueprintCallable, Category = "Blaster|Animation")
	static FString DumpAnimGraph(UAnimBlueprint* InAnimBlueprint);
};
