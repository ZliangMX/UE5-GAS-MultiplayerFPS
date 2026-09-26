// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "JettCharacter.generated.h"

class UAnimMontage;
class UAnimSequenceBase;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;

/*
 * Jett（捷风）—— BP_JettCharacter 的 C++ 父类。
 *
 * 继承层次：
 *
 *     ABlasterCharacter（通用骨架：移动、战斗、CombatState、大招状态机……）
 *          └── AJettCharacter（捷风）  →  BP_JettCharacter（网格/动画/输入/技能表）
 *
 * 这个类目前只装一件事：**刃风暴那套飞刀的视觉**（一个骨骼网格 + 5 把小刀）。
 * 大招的"逻辑"（生成武器 / 收招 / 扣大招点）还在 ABlasterCharacter 上，见那边的
 * ServerStartBladeStorm / ServerEndBladeStorm —— 要搬的话按下面那份清单来。
 * 为什么先只搬视觉：它天然是英雄专属的（只有 Jett 有飞刀），而且不牵扯任何数据/结算，
 * 搬出来零风险；逻辑那部分还被 PC/HUD/能力按 ABlasterCharacter* 调着，要一起动。
 *
 * ⚠️ 这个类**不设任何资源默认值**：网格 / 动画类 / 蒙太奇一律配在 BP 上（改资源不用重新编译）。
 *    组件本身在这里建（KnifeRigMesh + 5 把小刀），资源在 BP_JettCharacter 的详情面板里填。
 *
 * ⚠️ BP_JettCharacter 里原来手搭的那一套（KnifeSkeletalMesh 组件 + BeginPlay 里的
 *    AddStaticMeshComponent/AttachToComponent 串）**必须删掉**，否则场上有两套飞刀。
 */
UCLASS()
class BLASTER_API AJettCharacter : public ABlasterCharacter
{
	GENERATED_BODY()

public:
	AJettCharacter(const FObjectInitializer& ObjectInitializer);

	// 编辑器里改 KnifeStaticMesh / KnifeSocketNames 立刻在 BP 预览里生效（不然要进 PIE 才看得见）
	virtual void OnConstruction(const FTransform& Transform) override;
	// 运行时再来一遍：BP 子类填的值到这时才是最终值（构造期只读得到 CDO 的）
	virtual void PostInitializeComponents() override;

	// ——— 飞刀的可见性 / 动画 ———
	//
	// 状态由**武器的弹匣**推着走，这里不自己攒一份计数：
	//   "手上还剩几把刀" == AJettKnives 的 Ammo（它已经复制、已经本地预测、HUD 读的也是它）。
	//   自己再维护一份就多一个可能对不上的副本 —— 那正是"服务器上还剩 3 把、客户端显示 4 把"的来源。
	// 所以这里只有"按当前 Ammo 重算一遍显隐"，调几次都一样（幂等）。
	//
	// 显隐的总开关是**"手上是不是正拿着飞刀"**（大招生效中 **且** EquippedWeapon 就是 AJettKnives），
	// 不是"大招生不生效"。理由见 .cpp 里 ResolveKnivesRemaining 的注释 —— 开着大按 1/2 切枪
	// 时那 5 把刀必须跟着收起来（用户 2026-09-18 的要求）。

	// 按当前武器弹匣刷新整套飞刀的显隐，并在"刚拿上刀"的上升沿播掏出动画。
	// 所有会改变显隐的事件都汇到这里：开大/收招、换武器、扔刀、击杀刷新。
	// 异常情况（大招生效中但拿的是枪 / 武器还没复制到）见 .cpp
	UFUNCTION(BlueprintCallable, Category = "Knife")
	void SyncKnivesToAmmo();

	// 播刀骨骼上的攻击蒙太奇并跳到第 ThrowIndex 段（1..5）。由 AJettKnives::Fire 调。
	// 只在"这一台机器"上播 —— 每台机器都会各自走到这里（本机是本地预测，其他机器是多播），
	// 所以动画和随之而来的动画通知在所有端上是一致的，不需要额外复制。
	void PlayKnifeAttackMontage(int32 ThrowIndex);

	// 掏出飞刀的那条蒙太奇（刀骨骼上）。由 SyncKnivesToAmmo 在"刚拿上刀"时调，
	// 于是开大那一次和"大招生效期间再按 X 把刀掏回来"那一次都会重播一遍。
	void PlayKnifeEquipMontage();


	// 攻击蒙太奇第 ThrowIndex 段（1..5）的名字。默认就是序号 "1".."5"。
	//
	// 刀骨骼那条（KnifeAttack）和第一人称手模那条（AJettKnives::FPFireMontage）**共用**这
	// 一个约定 —— 两条蒙太奇的分段都照着 1..5 命名就能自动对上。
	// 不这么做就得在 BP 里配两份段名，迟早改了一条忘了另一条。
	static FName MakeKnifeAttackSectionName(int32 ThrowIndex);

	// 手上还剩几把刀（0..5）。**纯表现状态，不复制** —— 每台机器上都由同一条
	// Fire() → 蒙太奇 → 动画通知链自己算出来。仅供调试/BP 读取用，
	// 逻辑判定一律读武器的 Ammo。
	FORCEINLINE int32 GetKnivesRemaining() const { return KnivesRemaining; }

	FORCEINLINE USkeletalMeshComponent* GetKnifeRigMesh() const { return KnifeRigMesh; }

	// 小刀数量（组件数）。和 AJettKnives::MagCapacity（玩法上的刀数）是两件事，
	// 改了一个记得看另一个 —— 5 把刀配 3 发弹匣的话，扔两刀就会有两把永远不消失。
	static constexpr int32 KnifeMeshCount = 5;

protected:
	// 大招生效/结束的视觉都挂在这条链上：整套飞刀的显隐 + 5 把小刀。
	//
	// 三个调用点都汇到这里：复制到达（客户端）、以及 ServerStartUltimate/ServerEndUltimate
	// 里的手动调用（权威机改自己的复制属性不会触发 OnRep，那边手动刷一次）。
	// 只是转发给 SyncKnivesToAmmo（掏出动画的上升沿判断也在那边，理由见 .cpp）。
	virtual void OnRep_ActiveUltimate() override;

	/*
	 * 手上换了武器 → 重算飞刀显隐（见基类 ABlasterCharacter::OnEquippedWeaponChanged 的声明）。
	 *
	 * 这条覆写是"大招生效期间装备别的枪要把刀藏起来"能成立的那一环：
	 * 切枪不改 ActiveUltimate、也不改近战槽，只改 EquippedWeapon —— 不在这条链上补一手，
	 * 那 5 把刀就会一直挂在身上跟着跑。
	 *
	 * 基类的两个调用点（服务器 SetEquippedWeapon、客户端 OnRep_EquipWeapon）都会走到这里，
	 * 包括"收枪"（EquippedWeapon == nullptr）那一次。
	 */
	virtual void OnEquippedWeaponChanged() override;

	/*
	 * ——— 飞刀那套视觉 ———
	 *
	 * 一个骨骼网格 KnifeRigMesh（AB_Wushu_S0_X_Skelmesh + 动画蓝图 AB_X）
	 * + 5 个小刀静态网格，挂在它的 Knife1Socket..Knife5Socket 上。
	 *
	 * 为什么放在**角色**上而不是武器（AJettKnives）上：
	 *   · 用户要求"这个 skeletal 会一直存在，只有大招 x 期间可见" —— 它得比武器活得久。
	 *     武器是大招期间临时生成、收招就销毁的；挂在它身上就等于"收招即消失"，
	 *     而且每开一次大招都要重建一遍。
	 *   · 5 把刀在**所有机器**上都要跟着同样的动画、在同样的时机消失。放在角色上，
	 *     每台机器各自跑同一条 Fire() → 蒙太奇 → 通知链，天然一致，不需要多复制一份状态。
	 *
	 * BP 里要填的（组件已经在这里建好了，只差资源）：
	 *   · KnifeRigMesh 的 Skeletal Mesh = AB_Wushu_S0_X_Skelmesh、Anim Class = AB_X_C
	 *   · KnifeRigMesh 的相对变换 = 原来 BP 里那套
	 *     （实测 2026-09-18：位置 (62.5, 0, 78.2)、旋转 P90/Y0/R270、缩放 1，挂在胶囊体下）
	 *   · KnifeStaticMesh = AB_Wushu_S0_X_Staticmesh（5 把小刀共用；留空则用组件上各自配的）
	 *   · KnifeAttack = AB_Wushu_S0_X_Attack（5 段）、KnifeEquip = AB_Wushu_S0_X_Equip_Montage
	 *
	 * ⚠️ 上面这两条是**刀骨架**的蒙太奇，只会打在 KnifeRigMesh 上 —— 别拿去填
	 *    BP_JettKnives 的 FPFireMontage / FPEquipMontage（那四个是**手模骨架**的）。
	 *    填错不会报错，而是把手模的 Skeleton/Root 两根同名骨改写成刀骨架的值（差 120°），
	 *    第一人称相机挂在手模的 Camera 骨上 → **整个视角被拧转**。
	 *
	 * ⚠️ AB_X 这条动画蓝图里必须有 Slot 节点（默认那个 DefaultSlot 就行）——
	 *    蒙太奇是走槽位播的，没有 Slot 节点就什么都播不出来（引擎那边只有一条警告，很难查）。
	 *    这条动画蓝图目前只有 Root→StateMachine→SequencePlayer，**没有 Slot**。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Knife")
	TObjectPtr<USkeletalMeshComponent> KnifeRigMesh;

	// 5 把小刀（KnifeMesh1..KnifeMesh5）。在 BP 的组件树里能单独调变换，但**别去改它们的
	// Relative Scale** —— 缩放由 KnifeWorldScale 统一算（见那里），手动改会被下次刷新覆盖。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Knife")
	TArray<TObjectPtr<UStaticMeshComponent>> KnifeMeshes;

	/*
	 * 5 把小刀共用的静态网格资源，填一次。
	 *
	 * 用"一个资源 + 5 个组件"而不是 5 个资源槽：小刀本来就是同一个模型，
	 * 在 BP 里填 5 遍是纯粹的重复劳动，还容易漏。填完由 OnConstruction 发给每个组件。
	 *
	 * ⚠️ **留空 = 不去动组件上已有的网格**（保持你在 BP 组件树里逐个配好的那份），
	 *    不是"清空"。所以两种配法都能用：这里填一个、或者 5 个组件各填各的。
	 *    —— 这条是踩过坑才写死的：以前是无条件 SetStaticMesh，留空时会把 5 个组件上
	 *    配好的网格全部抹成 null，而编辑器预览照常（OnConstruction 跑在预览实例上，
	 *    不改模板），只有进 PIE 才全空，表现是"刀一把都看不着"且不报任何错。
	 */
	UPROPERTY(EditAnywhere, Category = "Knife")
	TObjectPtr<UStaticMesh> KnifeStaticMesh;

	// 每把小刀挂到哪个 socket。默认 Knife1Socket..Knife5Socket —— 和
	// AB_Wushu_S0_X_Skelmesh 上的 socket 同名，开箱即用，一般不用动。
	//
	// ⚠️ socket 名是 "Knife1Socket"，不是 "Knife1"。后者是**骨骼**名 —— 挂骨骼名也能挂上
	//    （引擎找不到同名 socket 就退回按骨骼挂），但 5 个组件的挂点名一旦手滑写成同一个，
	//    表现是"5 把刀全叠在一根骨骼上"，而且没有任何报错。
	UPROPERTY(EditAnywhere, Category = "Knife")
	TArray<FName> KnifeSocketNames;

	/*
	 * 小刀的**世界口径**大小（1 = 静态网格资产原大小；它的顶点数据本身就是厘米，
	 * 量过 `AB_Wushu_S0_X_Staticmesh` 的包围盒 = 约 19cm × 4cm）。
	 *
	 * 小刀挂在骨骼上会**连父级的缩放一起继承**，所以相对缩放要按挂点的世界缩放反算，
	 * 换算和 AWeapon::AttachDecorationToSocket 是同一套：
	 *     组件的相对缩放 = KnifeWorldScale / 挂点的世界缩放
	 *
	 * 量过当前这套资产（KnifeRigMesh 相对缩放 1、骨架无整体缩放），这个除法算出来就是 1 ——
	 * 今天它不改变任何东西。留着是给"换骨架"兜底：骨架一旦带 0.01 之类的整体缩放，
	 * 不留这个除法的话 19cm 的刀会跟着变成 0.19cm，肉眼看不见且 BP 上看不出异常。
	 */
	UPROPERTY(EditAnywhere, Category = "Knife")
	float KnifeWorldScale = 1.f;

	/*
	 * 5 把小刀的**消失顺序**：数组元素是刀槽编号（1..5），按顺序一把一把不见。
	 *
	 * 默认 {1,2,3,4,5} 不是随手填的 —— 量过骨骼位置（AB_Wushu_S0_X_Idle 第 0 帧，相对骨架原点）：
	 *     Knife1 (z=-70.4)  Knife3 (z=-63.4)   ← 同一侧
	 *     Knife2 (z=+74.4)  Knife4 (z=+67.3)   ← 另一侧
	 *     Knife5 (z= -1.9)                     ← 正中间那把
	 * 1/3 一侧、2/4 另一侧、5 居中 —— 于是 {1,2,3,4,5} 天然就是"一侧、另一侧、一侧、另一侧、
	 * 最后中间"，正好是用户要的"右左右左中"（原话："右左右左中，写死"）。
	 *
	 * ⚠️ 有一件事从坐标上看不出来：1/3 那一侧到底是玩家的右边还是左边。要是实际藏反了
	 *    （先藏了左边），改成 {2,1,4,3,5} 就是镜像过去的顺序，别的都不用动。
	 */
	UPROPERTY(EditAnywhere, Category = "Knife")
	TArray<int32> KnifeHideOrder;

	// 攻击蒙太奇（刀骨骼上）。5 段，段名 "1".."5"。**单扔**专用（左键），右键不播它。
	//
	// 挂在这条上面的动画通知（UAnimNotify_KnifeConsumed）是"这一把刀该消失了"的唯一来源，
	// 所以单扔那条路必须一直播得出来；右键那条不播它，改为在 AJettKnives::ThrowAllKnives
	// 里手动推 SyncKnivesToAmmo()。
	//
	// ⚠ 曾经还有一条右键专用的 KnifeThrowAll 字段，2026-09-18 按用户要求删掉了
	//   （"右键刀是没有动画的，只有 fp 和 tp"）—— 右键的动画只在手模和身体那两条链上。
	UPROPERTY(EditAnywhere, Category = "Knife")
	TObjectPtr<UAnimMontage> KnifeAttack;

	// 掏出飞刀的蒙太奇（刀骨骼上）。大招开始那一刻播。
	UPROPERTY(EditAnywhere, Category = "Knife")
	TObjectPtr<UAnimMontage> KnifeEquip;

private:
	// 把 KnifeStaticMesh 发给 5 个组件 + 按 KnifeSocketNames 重新挂到 socket 上。
	// OnConstruction / PostInitializeComponents 各调一次（理由见 .cpp）。
	void RefreshKnifeMeshes();

	// 按 KnivesRemaining 重算 5 把小刀和整个骨骼网格的显隐。幂等。
	void RefreshKnifeVisibility();

	// 当前该显示几把刀：手上正拿着飞刀 → 读武器弹匣；否则 → 0（整套隐藏）。
	//
	// ⚠ 不是 const：判据要读 GetEquippedWeapon()，而 ABlasterCharacter 上那个访问器不是 const 的
	//（所以这里也别写成 const 再 const_cast，白白多一层难查的绕路）。
	int32 ResolveKnivesRemaining();

	// 手上还剩几把刀（表现用，不复制）。见 GetKnivesRemaining 的注释。
	int32 KnivesRemaining = 0;

	/*
	 * 上一帧飞刀是不是"已经掏出来了"（KnivesRemaining > 0）—— 用来抓掏出动画的上升沿。
	 *
	 * 之前叫 bKnifeRigWasActive、判据是"大招生不生效"，2026-09-18 改成判"手上有没有刀"：
	 * 现在"拿上刀"有两个入口（开大、以及大招生效期间再按 X 掏回来），按老的判据，
	 * 第二次掏回来时大招一直是生效的 → 判不出上升沿 → 掏出动画不播（而用户明确要求重播）。
	 *
	 * 不存成"上一帧的 ActiveUltimate"再自己做边沿判断，是因为 SyncKnivesToAmmo 会被
	 * OnRep 和多个手动点反复调（服务器侧），任何"只在一处做边沿判断"的写法都会漏掉别的入口。
	 */
	bool bKnivesDrawn = false;
};
