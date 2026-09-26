// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_ReloadFinished.generated.h"

class ABlasterCharacter;

/*
 * ——— 换弹结束的动画通知 ———
 *
 * 它做的事就一件：反查到本角色的 UCombatComponent，调 FinishReloading()。
 * 也就是"换弹结束、可以切状态了"这个时机由**动画**说了算，不是由计时器说了算。
 *
 * ⚠ 2026-09-26 起本工程**没有任何武器在用它**：所有换弹蒙太奇上的这条通知都摘掉了
 *   （角色身上那条共用蒙太奇 Mon_RifleReload 上留着的六个是同名**无类**的通知，本来就不做任何事），
 *   换弹时长改由服务器计时器独家决定（AWeapon::ReloadTime / GetReloadDuration），
 *   理由见 UCombatComponent::StartEquipTimer 的注释。类留着，将来想换回"动画驱动"时还用得上。
 *
 * 想用它的话，挂哪条蒙太奇很关键：
 *   • 想让它**真的收尾**（尤其是服务器那份结算 —— 填弹匣、扣备弹只认 HasAuthority）
 *     就得挂**第三人称那条**（武器蓝图的 ThirdPersonReloadMontage，或角色身上的 ReloadMontage）：
 *     它在所有机器上都播，服务器才收得到。手模那条是**只在本机播**的，挂它上面等于
 *     "别人的换弹服务器永远等不到通知，只能等计时器"。
 *   • 但代价是时长变成**第三人称动画**的长度 —— 和玩家看得见的第一人称手模不是一条，
 *     两条动画长度不一样时手会差半拍（这正是 2026-09-26 把它全摘掉的原因）。
 *
 * ⚠ 用它的前提是**保留**服务器计时器（AWeapon::ReloadTime / StartReloadTimer）当保险丝：
 *   这条通知一旦丢了（换骨架、换个动画蓝图、蒙太奇被人换掉、独立服务器上没有动画在 tick），
 *   没有保险丝就是**所有人永久卡在 ECS_Reloading** —— 开不了枪、切不了枪、什么都不报
 *  （这段血泪史见 Weapon.h 里 ReloadTime 上面那段注释）。
 *   FinishReloading() 自己有幂等门禁（状态已经不是 ECS_Reloading 就直接走人），
 *   所以先到的那个收尾、后到的那个空转，不会填两遍弹。
 *
 * ⚠ 霰弹枪不接（见 .cpp 里的实现注释）：它是一发一发压的，接了这个通知会一次填满。
 */
UCLASS(meta = (DisplayName = "Reload Finished"))
class BLASTER_API UAnimNotify_ReloadFinished : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/*
	 * 从「正在播这条动画的东西」反查到本角色。三种入参都吃：
	 *   · USkeletalMeshComponent（通知拿到的就是它）→ 取 GetOwner()：
	 *       角色网格 / 手模 → owner 直接是角色；武器网格 → owner 是 AWeapon，再往上摸一层
	 *   · UAnimInstance（动画蓝图侧自己调的时候直接传 this 最省事）→ 走 GetOwningActor()
	 *   · AActor → 直接用
	 * 查不到就返回 nullptr（Persona 里预览、编辑器里空跑都会走到这条，属正常，不是错误）。
	 */
	static ABlasterCharacter* ResolveCharacter(const UObject* AnimOwnerOrMesh);

	/*
	 * 通知和动画蓝图**两条路共用的正门**：反查角色 → 把不该接的情况拦掉 → 调 FinishReloading()。
	 * 两个 AnimInstance 上的 FinishReload() 就是转调这里，保证两个入口行为永远一致。
	 * 动画蓝图里想从自己的事件图/自定义蓝图通知触发，走这个（或 AnimInstance 上那个同名函数）。
	 */
	static void TriggerFinishReload(const UObject* AnimOwnerOrMesh);
};
