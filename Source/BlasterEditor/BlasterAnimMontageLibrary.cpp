#include "BlasterAnimMontageLibrary.h"

#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"

namespace
{
	const TCHAR* BoolStr(bool b) { return b ? TEXT("true") : TEXT("false"); }
}

bool UBlasterAnimMontageLibrary::BuildTimelineMontage(
	UAnimMontage* InMontage,
	UAnimMontage* InSettingsTemplate,
	UAnimSequenceBase* InIntro,
	UAnimSequenceBase* InLoop,
	UAnimSequenceBase* InOutro,
	FName InSlotName,
	FName InIntroSection,
	FName InLoopSection,
	FName InOutroSection,
	bool bLoopMiddle)
{
#if WITH_EDITOR
	if (InMontage == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BuildTimelineMontage] 目标蒙太奇为空"));
		return false;
	}
	if (InSlotName.IsNone())
	{
		// 槽名空 = 谁都不会去播它（引擎那侧 CreateSlotAnimationAsDynamicMontage 也直接报错返回）
		UE_LOG(LogTemp, Warning, TEXT("[BuildTimelineMontage] %s 的槽名为空"), *InMontage->GetName());
		return false;
	}

	const UAnimSequenceBase* Parts[3] = { InIntro, InLoop, InOutro };
	const FName SectionNames[3] = { InIntroSection, InLoopSection, InOutroSection };
	const TCHAR* PartLabels[3] = { TEXT("Intro"), TEXT("Loop"), TEXT("Outro") };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (Parts[Index] == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BuildTimelineMontage] %s 的 %s 段序列为空"),
				*InMontage->GetName(), PartLabels[Index]);
			return false;
		}
	}

	InMontage->Modify();

	/*
	 * —— 槽 + 片段 ——
	 *
	 * 先把槽整个清掉再 AddSlot：UAnimMontage 的构造函数里已经 AddSlot("DefaultSlot") 了，
	 * 不清就变成"两条槽"（多出来那条是空的，在蒙太奇编辑器里看着很莫名其妙）。
	 */
	InMontage->SlotAnimTracks.Empty();
	FSlotAnimationTrack& Track = InMontage->AddSlot(InSlotName);
	FAnimTrack& AnimTrack = Track.AnimTrack;
	AnimTrack.AnimSegments.Empty();

	float Cursor = 0.f;
	float SectionStartTimes[3] = { 0.f, 0.f, 0.f };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		SectionStartTimes[Index] = Cursor;

		FAnimSegment& Segment = AnimTrack.AnimSegments.AddDefaulted_GetRef();
		// bInitialize=true：把 AnimStartTime=0 / AnimEndTime=整条长度 / PlayRate=1 / LoopCount=1 一次设好，
		// 顺便 UpdateCachedPlayLength()（编辑器里用来标记"片段长度和源资产不一致"）
		Segment.SetAnimReference(const_cast<UAnimSequenceBase*>(Parts[Index]), /*bInitialize=*/true);
		// ★ StartPos 必须在 SetAnimReference **之后**写：bInitialize 那条分支会把 StartPos 清成 0
		Segment.StartPos = Cursor;

		// GetLength() = LoopCount * (AnimEndTime - AnimStartTime) / |PlayRate|
		Cursor += Segment.GetLength();
	}

	/*
	 * —— 分段 ——
	 *
	 * 用引擎自己的 AddAnimCompositeSection 而不是手搓 FCompositeSection：
	 * 它内部做 Link(this, StartTime) —— 从槽 0 的轨道里按时间找出所属片段
	 *（FAnimTrack::GetSegmentIndexAtTime 是**倒着**找的，所以正好落在边界上的时间会归给后一段，
	 * 也就是 Intro 的结尾那一帧算 Loop 的），把 SegmentIndex / LinkedSequence /
	 * SegmentBeginTime / SegmentLength / LinkValue 一次算对。
	 */
	InMontage->CompositeSections.Empty();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const int32 NewIndex = InMontage->AddAnimCompositeSection(SectionNames[Index], SectionStartTimes[Index]);
		if (NewIndex == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BuildTimelineMontage] %s 加分段 %s 失败（重名？）"),
				*InMontage->GetName(), *SectionNames[Index].ToString());
			return false;
		}
	}

	/*
	 * 显式重写"下一段"。
	 *
	 * AddAnimCompositeSection 只会做一件事：**上一段的 NextSectionName 为空**时把它补成新加的这段。
	 * 也就是加完之后默认是 Intro → Loop → Outro 一条直线。
	 * 中间那段要**自接自**（NextSectionName = 自己）才能无限循环：
	 * FAnimMontageInstance::Advance 用的是 NextSections[CurrentSectionIndex]，指回自己就原地循环。
	 * 最后一段必须是 None，不然播完 Outro 会跳回 Intro 再来一遍。
	 */
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FCompositeSection& Section = InMontage->CompositeSections[Index];
		if (Index == 0)
		{
			Section.NextSectionName = SectionNames[1];
		}
		else if (Index == 1)
		{
			Section.NextSectionName = bLoopMiddle ? SectionNames[1] : SectionNames[2];
		}
		else
		{
			Section.NextSectionName = NAME_None;
		}
	}

	// 让所有 linkable element（分段 + 通知）按新轨道重算一遍自己的段信息。
	// 引擎在蒙太奇编辑器里改完轨道也会走这一趟。
	InMontage->UpdateLinkableElements();

	/*
	 * —— 时长 ——
	 *
	 * UAnimMontage 没有覆写 GetPlayLength()，它返回的是 UAnimSequenceBase::SequenceLength 这个
	 * **存盘字段**，不会自己跟着片段长度走。所以必须显式设一次，否则：
	 *   · 存了个 0 或旧值进资产；
	 *   · 下次 PostLoad 时引擎发现"和 CalculateSequenceLength() 对不上"，自己修 + 打一条
	 *     "please resave the asset" 的 Display 日志 —— 看着像我们存坏了。
	 * SequenceLength 本身是 protected + UE_DEPRECATED 的（直接写编译不过），
	 * 走公开的 SetCompositeLength：编辑器分支里它通过 DataModel 的 Controller 改，
	 * 和蒙太奇编辑器里拖长轨道走的是同一条路。
	 * ⚠ SetCompositeLength 内部是**直接解引用 Controller 成员**的，不一定已经初始化，
	 *   所以先 GetController() 把那个 transient 对象建出来（它内部会 ValidateModel + SetModel）。
	 */
	const float CalculatedLength = InMontage->CalculateSequenceLength();
	if (InMontage->IsDataModelValid())
	{
		InMontage->GetController();
		InMontage->SetCompositeLength(CalculatedLength);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[BuildTimelineMontage] %s 没有可用的 DataModel，时长没能写进资产（载入时引擎会自己按片段长度修好）"),
			*InMontage->GetName());
	}

	// —— 混合设置照抄模板 ——
	if (InSettingsTemplate != nullptr)
	{
		// 顺手把模板的槽名打出来：新蒙太奇的槽名是调用方传的，对不对得拿真资产对照
		//（FP 的 dash 蒙太奇槽名是不是 DefaultSlot、TP 的是不是 UpperBody，不该靠猜）
		FString TemplateSlots;
		for (const FSlotAnimationTrack& TemplateSlot : InSettingsTemplate->SlotAnimTracks)
		{
			TemplateSlots += FString::Printf(TEXT("[%s 片段=%d]"), *TemplateSlot.SlotName.ToString(),
				TemplateSlot.AnimTrack.AnimSegments.Num());
		}
		UE_LOG(LogTemp, Display, TEXT("[BuildTimelineMontage] 模板 %s 的槽：%s autoBlendOut=%s BlendIn=%.3f BlendOut=%.3f BlendOutTrigger=%.3f"),
			*InSettingsTemplate->GetName(), *TemplateSlots, BoolStr(InSettingsTemplate->bEnableAutoBlendOut),
			InSettingsTemplate->BlendIn.GetBlendTime(), InSettingsTemplate->BlendOut.GetBlendTime(),
			InSettingsTemplate->BlendOutTriggerTime);

		InMontage->BlendIn = InSettingsTemplate->BlendIn;
		InMontage->BlendOut = InSettingsTemplate->BlendOut;
		InMontage->BlendModeIn = InSettingsTemplate->BlendModeIn;
		InMontage->BlendModeOut = InSettingsTemplate->BlendModeOut;
		InMontage->BlendOutTriggerTime = InSettingsTemplate->BlendOutTriggerTime;
		InMontage->BlendProfileIn = InSettingsTemplate->BlendProfileIn;
		InMontage->BlendProfileOut = InSettingsTemplate->BlendProfileOut;
		InMontage->bEnableAutoBlendOut = InSettingsTemplate->bEnableAutoBlendOut;
		InMontage->RateScale = InSettingsTemplate->RateScale;
	}
	else
	{
		// 没有模板时的兜底：不自动混合出去 = 播完停在 Outro 最后一帧（本工程其它空手蒙太奇都是这个设定，
		// 免得空手 idle 和收尾姿势之间被引擎插一段混合）
		InMontage->bEnableAutoBlendOut = false;
	}

	InMontage->MarkPackageDirty();

	// —— 读回自检 ——
	UE_LOG(LogTemp, Display, TEXT("[BuildTimelineMontage] %s 目标槽=%s 用模板=%s"),
		*InMontage->GetName(), *InSlotName.ToString(),
		InSettingsTemplate ? *InSettingsTemplate->GetName() : TEXT("None"));
	UE_LOG(LogTemp, Display, TEXT("%s"), *DescribeMontage(InMontage));

	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[BuildTimelineMontage] 只有编辑器构建才有（WITH_EDITOR）"));
	return false;
#endif // WITH_EDITOR
}

bool UBlasterAnimMontageLibrary::SetMontageSectionLoop(UAnimMontage* InMontage, FName InSectionName)
{
#if WITH_EDITOR
	if (InMontage == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SetMontageSectionLoop] 目标蒙太奇为空"));
		return false;
	}

	// 传 None = "唯一的那一段"。多段蒙太奇上这样传是含糊的（到底循环哪段？），直接拒掉让调用方写清楚。
	int32 SectionIndex = InSectionName.IsNone() ? INDEX_NONE : InMontage->GetSectionIndex(InSectionName);
	if (SectionIndex == INDEX_NONE)
	{
		if (!InSectionName.IsNone() || InMontage->CompositeSections.Num() != 1)
		{
			FString Existing;
			for (const FCompositeSection& Section : InMontage->CompositeSections)
			{
				Existing += FString::Printf(TEXT("[%s]"), *Section.SectionName.ToString());
			}
			UE_LOG(LogTemp, Warning,
				TEXT("[SetMontageSectionLoop] %s 里没有分段 %s（实际有 %d 段：%s）。")
				TEXT("多段蒙太奇必须显式点名，而且要循环的那段得自己写对（Intro/Outro 不能自接自）。"),
				*InMontage->GetName(), *InSectionName.ToString(), InMontage->CompositeSections.Num(), *Existing);
			return false;
		}
		SectionIndex = 0;
	}

	InMontage->Modify();

	/*
	 * 自接自 = 无限循环。
	 *
	 * FAnimMontageInstance::Advance 走的是 NextSections[CurrentSectionIndex]，
	 * 指回自己就一直在这一段里原地转 —— 和 BuildTimelineMontage 里 bLoopMiddle 那条是同一个机制。
	 * 注意**不能**靠 bEnableAutoBlendOut / LoopingCount 去凑：前者只管播完混合出去，
	 * 后者是片段级的（一个片段里同一段序列重复几次），都不会让整条蒙太奇循环。
	 */
	FCompositeSection& Section = InMontage->CompositeSections[SectionIndex];
	Section.NextSectionName = Section.SectionName;

	InMontage->UpdateLinkableElements();
	InMontage->MarkPackageDirty();

	UE_LOG(LogTemp, Display, TEXT("[SetMontageSectionLoop] %s 的段 %s 已改成自接自（无限循环）："),
		*InMontage->GetName(), *Section.SectionName.ToString());
	UE_LOG(LogTemp, Display, TEXT("%s"), *DescribeMontage(InMontage));

	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[SetMontageSectionLoop] 只有编辑器构建才有（WITH_EDITOR）"));
	return false;
#endif // WITH_EDITOR
}

FString UBlasterAnimMontageLibrary::DescribeMontage(UAnimMontage* InMontage)
{
	if (InMontage == nullptr)
	{
		return TEXT("montage=null");
	}

	// 全 ASCII 输出：这份字符串会被 Python 原样写进 UTF-8 报告文件，
	// 混中文的话在场中文/英文编码下会变成一堆 ?
	FString Report = FString::Printf(
		TEXT("montage=%s playlen=%.4f calclen=%.4f autoBlendOut=%s blendIn=%.3f blendOut=%.3f blendOutTrigger=%.3f sections=%d slots=%d"),
		*InMontage->GetName(), InMontage->GetPlayLength(), InMontage->CalculateSequenceLength(),
		BoolStr(InMontage->bEnableAutoBlendOut),
		InMontage->BlendIn.GetBlendTime(), InMontage->BlendOut.GetBlendTime(), InMontage->BlendOutTriggerTime,
		InMontage->CompositeSections.Num(), InMontage->SlotAnimTracks.Num());

	for (int32 SlotIndex = 0; SlotIndex < InMontage->SlotAnimTracks.Num(); ++SlotIndex)
	{
		const FSlotAnimationTrack& Slot = InMontage->SlotAnimTracks[SlotIndex];
		Report += FString::Printf(TEXT("\n  slot[%d] name=%s segments=%d"),
			SlotIndex, *Slot.SlotName.ToString(), Slot.AnimTrack.AnimSegments.Num());

		for (int32 SegIndex = 0; SegIndex < Slot.AnimTrack.AnimSegments.Num(); ++SegIndex)
		{
			const FAnimSegment& Segment = Slot.AnimTrack.AnimSegments[SegIndex];
			Report += FString::Printf(
				TEXT("\n    seg[%d] seq=%s startPos=%.4f anim=%.4f~%.4f rate=%.2f loop=%d"),
				SegIndex, *GetNameSafe(Segment.GetAnimReference().Get()), Segment.StartPos,
				Segment.AnimStartTime, Segment.AnimEndTime, Segment.AnimPlayRate, Segment.LoopingCount);
		}
	}

	for (int32 SectionIndex = 0; SectionIndex < InMontage->CompositeSections.Num(); ++SectionIndex)
	{
		const FCompositeSection& Section = InMontage->CompositeSections[SectionIndex];
		Report += FString::Printf(
			TEXT("\n  sec[%d] name=%s start=%.4f len=%.4f next=%s linkedSeq=%s slotIndex=%d segIndex=%d"),
			SectionIndex, *Section.SectionName.ToString(), Section.GetTime(),
			InMontage->GetSectionLength(SectionIndex),
			*Section.NextSectionName.ToString(), *GetNameSafe(Section.GetLinkedSequence()),
			Section.GetSlotIndex(), Section.GetSegmentIndex());
	}

	return Report;
}
