// Jett 的 C「逐风云 / Cloudburst」：指尖冒出一朵云，顺着**角色视角**（含俯仰）直线飞出去。
// 云自己负责"1.5 秒或撞到东西 → 绽放成烟球"，见 AJettCloudburst。
//
// ——— 完整流程（用户 2026-09-19 定的版式）———
//
//   ① 按下 C：进空手 + 开始播那段"抬手施法"的蒙太奇（Intro → Loop 循环），云同时从指尖出现
//   ② 按住期间：**云跟准心**（往哪看云往哪拐，见 AJettCloudburst::SteerTowardsCrosshair）；
//              这段时间**不能掏枪** —— ECS_EmptyHand 本来就挡（UCombatComponent::CanChangeWeapon），
//              这个技能不需要为"挡换枪"额外做什么
//   ③ 结束按住：**松手**，或者**云自己绽放了**（到时限 1.5s / 撞到墙）——
//              两者谁先到算谁（见 AJettCloudburst::Bloom 和 ABlasterCharacter::SkillCReleased）
//   ④ 收尾：播 Outro → Outro 播完掏枪（ABlasterCharacter::EndCloudburstHold）
//
// ★ 和 E 冲刺/Q 腾空那两条最大的区别：那种空手是"**动画播完** → 收尾"，
//   这条是"**玩法事件**（松手 / 云绽放）→ 收尾"。所以：
//     · 空手要撑多久**不由动画长度决定**：覆写 OnEmptyHandStarted() 把保险丝按
//       "云飞多久"重设 —— 不然保险丝会按蒙太奇总长算，云还在飞、人已经被拉回
//       ECS_Unoccupied 了（表现是"按着按着枪自己出来了"）。
//     · 蒙太奇是**时间轴型**（Intro/Loop/Outro 三个流程段，Loop 自接自无限循环），
//       不是八向分段那种。分段名故意不叫 N/E/S/W —— 角色那边挑不到方向段就整条播，
//       正是想要的（见 ABlasterCharacter::PlayEmptyHandTrack 里那段日志分级）。
//
// ⚠️ LocalPredicted 下客户端预测实例也会跑 ExecuteSkillAction，SpawnActor 只能服务器做。

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "JettCloudburstAbility.generated.h"

class AJettCloudburst;

UCLASS()
class BLASTER_API UJettCloudburstAbility : public UBlasterGameplayAbility
{
	GENERATED_BODY()

public:
	UJettCloudburstAbility();

protected:
	virtual void ExecuteSkillAction() override;

	/*
	 * 空手刚起来那一拍（基类在 ExecuteSkillAction **之后**调）：把保险丝按"云飞多久"重设。
	 *
	 * 为什么非得在这儿：角色算保险丝用的是**蒙太奇的长度**（StartEmptyHandTimer →
	 * FBlasterEmptyHandMontages::GetFuseDuration），而这条时间轴型蒙太奇的分段名不是方向名，
	 * 挑不出段就退回"整条长度"——Intro + Loop + Outro 加起来大概两秒出头，
	 * 而按住最多要撑 1.5 秒（FlyDuration）**之后**才播 Outro，正好比它长一点。
	 * 差这一点在正常路径上看不出来（提前收尾由松手/绽放那条路走），
	 * 但只要有人把 Loop 段改短、或者把 FlyDuration 调大，就会变成"云还在飞枪已经端起来了"。
	 * 所以这里显式按玩法时长设一次，不去赌那两个资产的长度关系。
	 */
	virtual void OnEmptyHandStarted() override;

	/*
	 * 这一趟放出去的云（InstancedPerActor：实例跨次激活复用，所以每次激活都要重写）。
	 * 只用来问它"这一趟最多按多久"（保险丝）。云自己会退场（绽放后 0.5 秒），
	 * 所以是弱引用 —— 不该由能力去延它的命。
	 */
	TWeakObjectPtr<AJettCloudburst> ActiveCloud;

	// 云类（BP_JettCloudburst）。**必填** —— 留空按 C 什么都不会发生，会打日志骂人。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cloudburst")
	TSubclassOf<AJettCloudburst> CloudClass;

	// 指尖挂点。填了就优先用角色骨骼网格上这个 socket 的位置当出生点（最准，
	// 手一动云就跟着手）。
	// 留空 = 按下面的偏移量从**眼睛**位置估算：默认值是"只手放在镜头右下方"的量。
	// ⚠️ 只是近似 —— 想让云真的贴着指尖出来，去骨架里找个手部 socket 填进来。
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cloudburst")
	FName SpawnSocketName = NAME_None;

	// 没填 socket 时的出生点偏移（相对眼睛位置，坐标系 = 角色视角：前/右/上）
	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cloudburst")
	float SpawnForwardOffset = 60.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cloudburst")
	float SpawnRightOffset = 22.f;

	UPROPERTY(EditDefaultsOnly, Category = "Ability|Cloudburst")
	float SpawnUpOffset = -12.f;
};
