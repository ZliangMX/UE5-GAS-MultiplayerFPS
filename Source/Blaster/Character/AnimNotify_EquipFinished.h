// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_EquipFinished.generated.h"

/*
 * ——— 掏枪结束的动画通知 ———
 *
 * 把状态从 ECS_Equip 放回 ECS_Unoccupied，也就是「枪举好了、可以开火了」这个时刻
 * 由**动画**说了算，而不是由计时器说了算。
 *
 * ⚠ 2026-09-26 起本工程**没有任何武器在用它**：所有掏枪蒙太奇上的这条通知都摘掉了，
 *   掏枪时长改由服务器计时器独家决定（AWeapon::EquipTime / GetEquipDuration）。
 *   摘掉的原因是把每一条都量过之后发现它和玩家看到的东西对不上 —— 见
 *   UCombatComponent::StartEquipTimer 的注释（一句话：通知挂在第三人称那条动画上、
 *   落在末帧之前，于是时长变成第三人称动画的长度，和第一人称手模差半拍）。
 *   类留着，是因为下面这些坑仍然成立、将来想换回"动画驱动"时还用得上。
 *
 * 想用它的话，挂哪条很关键：
 *   • 挂 **FPEquipMontage**（推荐）：这个状态只影响射手本人开不开得了枪，而手模动画
 *     正好只在本机播 —— 时长就是玩家看得见的那条动画。
 *   • 挂第三人称那条：所有人看到的身体动作和状态对齐，但玩家自己的手会差半拍。
 *   • 两条**都挂** = 谁先到谁收尾（先响的那条决定时长），实测先响的总是第三人称那条。
 *
 * ⚠ 用它的前提是**保留**服务器计时器（AWeapon::EquipTime / StartEquipTimer）当保险丝：
 *   通知一丢（换骨架、换 ABP、蒙太奇被替换、独立服务器不 tick 动画）就没有收尾的人，
 *   结果是永久卡在 ECS_Equip —— 开不了枪、换不了弹，什么都不报。
 *   UCombatComponent::EquipFinish() 自带幂等门禁，先到的收尾、后到的空转。
 *
 * ⚠ 想换成"ABP 事件"那条路（在 ABP 事件图里接 AnimNotify_XXX），别用这条 C++ 通知：
 *   带通知类的通知**不会**触发 ABP 的 AnimNotify_* 事件，而应该改用轨道右键菜单里的
 *   "New Notify..."（只给名字、不给类），事件里再调两个 AnimInstance 上留的 EquipFinish 节点。
 *   带类的通知走的是它自己的 Notify()，也就是这条正在做的事。
 */
UCLASS(meta = (DisplayName = "Equip Finished"))
class BLASTER_API UAnimNotify_EquipFinished : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
