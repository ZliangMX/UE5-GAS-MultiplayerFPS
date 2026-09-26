// Fill out your copyright notice in the Description page of Project Settings.


#include "Blaster/Character/Agents/JettCharacter.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Weapon/Weapon.h"
// Cast<AJettKnives> 在 ResolveKnivesRemaining 里 —— 显隐的判据是"手上那把是不是飞刀"
#include "Blaster/Weapon/JettKnives.h"

AJettCharacter::AJettCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	/*
	 * ——— 飞刀的骨骼网格 ———
	 *
	 * 和 BP_JettCharacter 里手搭的那个 SkeletalMeshComponent 结构完全一样，只是搬进了 C++：
	 * 挂在根上（BP 里那个的相对变换就是相对 DefaultSceneRoot 的，而这里根是胶囊体）、
	 * 常驻、只在**手里拿着飞刀**时可见（大招生效中 **且** 手上那把是 AJettKnives；
	 * 大招生效期间切枪 → 藏起来，再按 X 掏回来 → 又可见，见 ResolveKnivesRemaining）。
	 */
	KnifeRigMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("KnifeRigMesh"));
	KnifeRigMesh->SetupAttachment(RootComponent);

	/*
	 * 纯装饰：**一定要关碰撞**。
	 *
	 * 它带着 5 把刀悬在角色身上，开着碰撞的话枪械射线（ECC_Visibility）会先打中自己手上的刀 ——
	 * 表现是"对着人开枪，伤害全被自己的刀吃了、命中特效糊在手上"。
	 * 这和第一人称手模必须不碰撞是同一个道理（手模那边是在 BP 的碰撞预设里关的）。
	 */
	KnifeRigMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	KnifeRigMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	KnifeRigMesh->SetGenerateOverlapEvents(false);
	KnifeRigMesh->CastShadow = false;
	KnifeRigMesh->bCastDynamicShadow = false;

	/*
	 * ——— 5 把小刀 ———
	 *
	 * 名字 KnifeMesh1..KnifeMesh5（BP 的组件树里按这个名字找）。
	 * 用"一个静态网格资源 + 5 个组件"而不是 5 个资源槽：小刀本来就是同一个模型。
	 */
	KnifeMeshes.Reserve(KnifeMeshCount);
	for (int32 Index = 1; Index <= KnifeMeshCount; ++Index)
	{
		const FName ComponentName(*FString::Printf(TEXT("KnifeMesh%d"), Index));
		UStaticMeshComponent* Knife = CreateDefaultSubobject<UStaticMeshComponent>(ComponentName);

		// 先挂在骨骼网格上占位（**不指定 socket**）—— 具体挂哪个 socket 由 RefreshKnifeMeshes
		// 在 OnConstruction / PostInitializeComponents 里做。
		// 理由和 AWeapon 的弹匣/瞄准镜一模一样：构造期只读得到 CDO 的值，
		// 而 socket 名是能在 BP 里改的，那时候还没生效。
		Knife->SetupAttachment(KnifeRigMesh);

		Knife->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Knife->SetCollisionResponseToAllChannels(ECR_Ignore);
		Knife->SetGenerateOverlapEvents(false);
		Knife->CastShadow = false;
		Knife->bCastDynamicShadow = false;
		Knife->SetCanEverAffectNavigation(false);

		KnifeMeshes.Add(Knife);
	}

	/*
	 * socket 名和消失顺序给一套默认值。
	 *
	 * 这两个不是"资源"（资源按这个类的约定一律配在 BP 上），而是**资产自带的结构信息**：
	 * socket 名就写在 AB_Wushu_S0_X_Skelmesh 上、顺序是从骨骼位置量出来的（见头文件），
	 * 所以写死在 C++ 里是对的 —— 新建一个 Jett 蓝图就开箱能用。
	 * 两者都还是 EditAnywhere，BP 里照样能改。
	 */
	KnifeSocketNames =
	{
		FName(TEXT("Knife1Socket")),
		FName(TEXT("Knife2Socket")),
		FName(TEXT("Knife3Socket")),
		FName(TEXT("Knife4Socket")),
		FName(TEXT("Knife5Socket")),
	};

	// "右左右左中" —— 为什么是这个顺序（以及藏反了怎么改）见头文件里 KnifeHideOrder 的注释
	KnifeHideOrder = { 1, 2, 3, 4, 5 };
}

void AJettCharacter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 编辑器里改 KnifeStaticMesh / socket 名 / 缩放立刻在 BP 预览里生效
	//（不然要进 PIE 才看得见），和 AWeapon::UpdateAttachedMeshes 同一个套路。
	RefreshKnifeMeshes();
	RefreshKnifeVisibility();
}

void AJettCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 构造期只读得到 CDO 的值，BP 子类覆盖过的（网格 / socket 名 / 那个 0.01 的缩放）
	// 到这一步才是最终值，所以必须再来一遍。
	RefreshKnifeMeshes();
	RefreshKnifeVisibility();
}

void AJettCharacter::RefreshKnifeMeshes()
{
	for (int32 Slot = 0; Slot < KnifeMeshes.Num(); ++Slot)
	{
		UStaticMeshComponent* Knife = KnifeMeshes[Slot];
		if (Knife == nullptr || KnifeRigMesh == nullptr) continue;

		/*
		 * ⚠️ **只在填了 KnifeStaticMesh 时才覆盖组件上已有的网格**，不能无条件 SetStaticMesh。
		 *
		 * 无条件写的话，KnifeStaticMesh 留空（None）会把 5 个组件上**分别在 BP 组件树里配好的**
		 * 网格全部抹成 null —— 而 BP 面板上组件那一行明明还显示着 AB_Wushu_S0_X_Staticmesh，
		 * 编辑器预览也照常（OnConstruction 跑在预览实例上，不改模板），
		 * 只有进 PIE 才全空。表现就是"刀一把都看不着"，且没有任何报错。
		 *
		 * 语义上这也和本工程其它地方一致：资源留空 = 保持原样，不是"清空"。
		 * 填了 KnifeStaticMesh 就是"5 把共用这一个"（省得填 5 遍），两种配法都能用。
		 */
		if (KnifeStaticMesh != nullptr)
		{
			Knife->SetStaticMesh(KnifeStaticMesh);
		}

		// 挂点：数组里没配就按序号退回到 KnifeNSocket（和资产上的命名一致）
		const FName SocketName = KnifeSocketNames.IsValidIndex(Slot) && !KnifeSocketNames[Slot].IsNone()
			? KnifeSocketNames[Slot]
			: FName(*FString::Printf(TEXT("Knife%dSocket"), Slot + 1));

		// 已经挂在目标网格的目标 socket 上就别重挂：OnConstruction 每次改属性都会跑一遍，
		// 无条件重挂会把蓝图反复标脏、也没必要（和 AWeapon::AttachDecorationToSocket 同一个理由）。
		if (Knife->GetAttachParent() != KnifeRigMesh || Knife->GetAttachSocketName() != SocketName)
		{
			// SnapToTargetNotIncludingScale = 位置/旋转吸附到 socket、缩放按"保持世界缩放"算
			//（下面再改成目标值）。
			Knife->AttachToComponent(
				KnifeRigMesh,
				FAttachmentTransformRules::SnapToTargetNotIncludingScale,
				SocketName);
		}

		/*
		 * 缩放：把父级（刀骨骼）的缩放抵消掉。
		 *
		 * 量过当前这套资产：KnifeRigMesh 的相对缩放是 1、skeleton 也没有整体缩放，
		 * 所以这条算出来就是 1 —— 今天它不改变任何东西。
		 * 留着是因为小刀挂在骨骼上会**连父级的缩放一起继承**：哪天换了骨架（或者有人手滑把
		 * 挂架的缩放调成 0.01），19cm 的静态网格就会跟着缩成 0.19cm —— 肉眼看不见，
		 * 而且 BP 面板上看不出任何异常。有这个除法在，"KnifeWorldScale = 1" 就永远等于
		 * "静态网格资产原大小"，与骨架怎么缩无关。
		 *
		 * 换算和 AWeapon::AttachDecorationToSocket 是同一套：相对缩放 = 世界口径 / 挂点的世界缩放。
		 */
		const FVector ParentScale = KnifeRigMesh->GetSocketTransform(SocketName, RTS_World).GetScale3D();
		auto DivideByParentScale = [](float Value, float Scale)
		{
			return FMath::IsNearlyZero(Scale) ? Value : Value / Scale;
		};

		Knife->SetRelativeScale3D(FVector(
			DivideByParentScale(KnifeWorldScale, ParentScale.X),
			DivideByParentScale(KnifeWorldScale, ParentScale.Y),
			DivideByParentScale(KnifeWorldScale, ParentScale.Z)));
	}
}

void AJettCharacter::RefreshKnifeVisibility()
{
	/*
	 * 总开关是 **KnivesRemaining > 0**（= 手上正拿着飞刀），不是"大招生不生效"。
	 *
	 * 拿"大招生效中"当总开关的话，大招生效期间切枪时挂架本身还是可见的
	 *（只有 5 把小刀被逐个关掉）—— 用户要的是"装备别的枪就把 knife 隐藏"，
	 * 那就是整个挂架都收起来。KnivesRemaining 已经是由 ResolveKnivesRemaining 算好的
	 * 那个复合判据（大招生效 **且** 手上正是飞刀），这里直接复用，不再自己判一遍 ——
	 * 判两遍就是两套可能对不上的条件。
	 */
	const bool bKnivesOut = (KnivesRemaining > 0);

	if (KnifeRigMesh)
	{
		/*
		 * bPropagateToChildren = false 是必须的。
		 *
		 * UE 里组件的可见性**不沿附加关系继承**：父级隐藏时子级照样渲染，除非显式要求往下传
		 *（这是"隐藏了骨骼网格、挂在上面的静态网格还在"那个经典坑）。
		 * 这里小刀是下面**逐个**单独控制的，让父级顺手把它们一起关掉反而会把那份判断抹平
		 *（关掉之后还得一个个打开回来，等于两套状态互相打架）。
		 */
		KnifeRigMesh->SetVisibility(bKnivesOut, /*bPropagateToChildren=*/false);
	}

	/*
	 * 哪几把该藏起来 = KnifeHideOrder 的前 (总刀数 - 还剩几把) 项。
	 *
	 * 每次都**重算**，而不是"每扔一刀藏一个"。重算天然幂等：
	 *   · 击杀刷新（Ammo 0 → 5，5 把又都回来）
	 *   · 客户端被服务器纠正刀数（本地预测多扔了一把）
	 *   · 蒙太奇连播到后面几段、通知多响了几次（见 UAnimNotify_KnifeConsumed 的注释）
	 * 这些情况下结果都对；增量式的写法每一种都要单独补一次"减多了/减少了"的修正。
	 */
	const int32 HiddenCount = FMath::Clamp(KnifeMeshes.Num() - KnivesRemaining, 0, KnifeMeshes.Num());

	for (int32 Slot = 0; Slot < KnifeMeshes.Num(); ++Slot)
	{
		UStaticMeshComponent* Knife = KnifeMeshes[Slot];
		if (Knife == nullptr) continue;

		// KnifeHideOrder 里填的是**刀槽编号**（1..5，和组件名 KnifeMeshN 里的 N 一致）
		const int32 SlotNumber = Slot + 1;

		bool bConsumed = false;
		for (int32 HideIndex = 0; HideIndex < HiddenCount; ++HideIndex)
		{
			if (KnifeHideOrder.IsValidIndex(HideIndex) && KnifeHideOrder[HideIndex] == SlotNumber)
			{
				bConsumed = true;
				break;
			}
		}

		Knife->SetVisibility(bKnivesOut && !bConsumed, /*bPropagateToChildren=*/false);
	}
}

int32 AJettCharacter::ResolveKnivesRemaining()
{
	/*
	 * "手上正拿着飞刀" 才是唯一判据 —— 两个条件缺一不可：
	 *   · 刃风暴生效中（没收招）
	 *   · 手上那把**就是飞刀**
	 *
	 * 第二个条件是 2026-09-18 加的（用户："x期间装备别的枪要把knife隐藏了"）。
	 * 在这之前只看"大招生不生效"，于是开着大按 1/2 切枪时，那 5 把刀还挂在身上跟着跑 ——
	 * 手上明明是枪，腰上却飘着一排飞刀。
	 *
	 * 判据用"手上是什么"而不是"近战槽里是什么"：大招生效期间近战槽里**永远**是飞刀
	 *（角色自带的刀被顶进 StashedMeleeWeapon 了），拿它当判据等于没有判据。
	 */
	if (!IsBladeStormActive()) return 0;
	if (Cast<AJettKnives>(GetEquippedWeapon()) == nullptr) return 0;

	// 走 GetCombatComponent() 而不是直接读 Combat —— 那个成员在 ABlasterCharacter 里是 private
	const UCombatComponent* CombatComp = GetCombatComponent();
	const AWeapon* Melee = CombatComp ? CombatComp->GetMeleeWeapon() : nullptr;

	/*
	 * 拿着刀、但武器还没复制到 → 按满的算。
	 *
	 * 客户端上 ActiveUltimate / EquippedWeapon / MeleeWeapon 是**三条独立的复制**，谁先到不确定。
	 * 上面那句 EquippedWeapon 已经到手了、MeleeWeapon 却还没到，就是这里：
	 * 返回 0 的话 5 把刀会被藏起来，而后面 MeleeWeapon 到了没人再刷一次 ——
	 * 表现是"开大了但手上一把刀都看不见"，直到第一次扔刀的通知才恢复。
	 * 按满的算就没有这个窗口（"刚掏出来"本来就该是 5 把）。
	 */
	if (Melee == nullptr) return KnifeMeshes.Num();

	return FMath::Clamp(Melee->GetAmmo(), 0, KnifeMeshes.Num());
}

void AJettCharacter::SyncKnivesToAmmo()
{
	KnivesRemaining = ResolveKnivesRemaining();
	RefreshKnifeVisibility();

	/*
	 * 掏出动画只在**"刚拿上飞刀"那一帧**播（bKnivesDrawn 的上升沿）。
	 *
	 * 为什么把这个判断挪到这里、而不是留在 OnRep_ActiveUltimate 里：
	 * 现在"拿上飞刀"有两个入口 —— 开大的那一下，以及大招生效期间再按一次 X 把刀掏回来
	 *（用户要求"再次按x可以呼出飞镖并且重新播放equip动画"）。留在 OnRep 里只能覆盖前一个。
	 * 这里是所有显隐刷新的**唯一汇合点**（开大/收招/换枪/扔刀/击杀刷新都会走到），
	 * 上升沿放这儿就天然覆盖全部入口，而且各端一致（每台机器都跑同一条复制链）。
	 *
	 * 上升沿判断必须看"显隐是不是刚被打开"，不能用"KnivesRemaining > 0"：
	 * 手上一直拿着刀、扔掉两把再补回来（击杀刷新）时显隐一直开着，不该重播掏出动画。
	 * 之前那个 bKnifeRigWasActive 就是这个用途，只是判据从"大招生效"改成了"拿着刀"。
	 */
	const bool bNowDrawn = (KnivesRemaining > 0);
	if (bNowDrawn && !bKnivesDrawn)
	{
		PlayKnifeEquipMontage();
	}
	bKnivesDrawn = bNowDrawn;
}

void AJettCharacter::OnRep_ActiveUltimate()
{
	Super::OnRep_ActiveUltimate();

	// 显隐 + 掏出动画全在 SyncKnivesToAmmo 里（它自己判上升沿）。
	//
	// OnRep 在生效期间会被反复调 —— 复制到达一次，加上权威机的
	// ServerStartUltimate / ServerEndUltimate / ServerSettleActiveUltimate 里各一次手动补刷
	//（权威机改自己的复制属性不会触发 OnRep，所以那边是手动调的，见各自的注释）。
	// 所以这里必须**幂等**：SyncKnivesToAmmo 读的是当前状态现算，调几次结果都一样，
	// 掏出动画也只在真正的上升沿播一次。
	SyncKnivesToAmmo();
}

void AJettCharacter::OnEquippedWeaponChanged()
{
	Super::OnEquippedWeaponChanged();

	/*
	 * 手上换了东西 → 重算飞刀显隐。
	 *
	 * 这就是"x期间装备别的枪要把knife隐藏了 / 再按x呼出"能成立的那一环：
	 * 显隐现在跟着**手上那把武器**走（见 ResolveKnivesRemaining），
	 * 而这件事只在"手上换了"的那一刻会变 —— 服务器走 SetEquippedWeapon、
	 * 客户端走 OnRep_EquipWeapon，两条路都汇到基类这个口（见 ABlasterCharacter 的声明）。
	 *
	 * 换的是枪（不是刀）时它一样要跑：那一刻正是"该把 5 把刀藏起来"的时刻。
	 * 幂等，重复调没有副作用 —— 唯一要小心的是别再这里干"每帧"的活（本函数只在意变化的那一帧）。
	 */
	SyncKnivesToAmmo();
}

void AJettCharacter::PlayKnifeEquipMontage()
{
	if (KnifeEquip == nullptr || KnifeRigMesh == nullptr) return;

	// 同上：这两条只会填刀骨架的蒙太奇，填错了会把刀的根链拧掉
	if (!IsMontageCompatibleWithMesh(KnifeEquip, KnifeRigMesh)) return;

	if (UAnimInstance* AnimInstance = KnifeRigMesh->GetAnimInstance())
	{
		AnimInstance->Montage_Play(KnifeEquip);
	}
}

void AJettCharacter::PlayKnifeAttackMontage(int32 ThrowIndex)
{
	if (KnifeAttack == nullptr || KnifeRigMesh == nullptr) return;

	// KnifeAttack / KnifeEquip 这两条必须是**刀骨架**（AB_Wushu_S0_X_Skeleton）的蒙太奇 ——
	// 它们就填在角色上、打在 KnifeRigMesh 上。骨架对不上就静默改写同名骨，见基类里那段注释。
	if (!IsMontageCompatibleWithMesh(KnifeAttack, KnifeRigMesh)) return;

	UAnimInstance* AnimInstance = KnifeRigMesh->GetAnimInstance();
	if (AnimInstance == nullptr)
	{
		// 刀骨骼网格没挂动画蓝图（AB_X）→ 蒙太奇没有 Slot 可以接住它，什么都播不出来。
		// 这条日志值得留着：表现是"刀在手上但从来不挥"，而 BP 上什么都看不出来。
		UE_LOG(LogTemp, Warning,
			TEXT("[飞刀] %s 的 KnifeRigMesh 没挂动画蓝图 → 攻击蒙太奇播不出来（要挂 AB_X）"), *GetName());
		return;
	}

	/*
	 * 每次扔刀都从头重播。
	 *
	 * Montage_Play 的默认参数就是"先停掉别的蒙太奇、这条从第 0 帧开始"，
	 * 所以下面那句 JumpToSection 跳的一定是**刚起来的这一条**，不会跳到别的动画实例上。
	 *
	 * 为什么必须重播：两次扔刀最短间隔 0.25 秒，而整条蒙太奇 5 秒（5 段各约 1 秒）。
	 * 不重播的话第二刀会插进第一段的中间 —— 两段动作串在一起。
	 */
	if (AnimInstance->Montage_Play(KnifeAttack) <= 0.f)
	{
		// 播不起来（这条蒙太奇长度为 0 / 骨架对不上 / 被同槽的更高级蒙太奇挡住）→ 跳段也没意义
		return;
	}

	const FName Section = MakeKnifeAttackSectionName(ThrowIndex);

	// 段名对不上就退回"从头播完整条"。这条判断值得留着 ——
	// Montage_JumpToSection 找不到段时自己只刷一行很含糊的警告，而原因通常是"段名被人改了"。
	if (KnifeAttack->IsValidSectionName(Section))
	{
		AnimInstance->Montage_JumpToSection(Section, KnifeAttack);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[飞刀] %s 的攻击蒙太奇里没有第 %d 段（段名 \"%s\"）→ 这一刀整条从头播"),
			*GetName(), ThrowIndex, *Section.ToString());
	}
}

FName AJettCharacter::MakeKnifeAttackSectionName(int32 ThrowIndex)
{
	// 段名就是序号本身（"1".."5"）。
	// 用户那条 AB_Wushu_S0_X_Attack 的分段正是这么命名的（1/2/3/4/5），所以不用配任何东西。
	// 第一人称手模那条（AJettKnives::FPFireMontage）也走这个约定 —— 见头文件的注释。
	return FName(*FString::Printf(TEXT("%d"), ThrowIndex));
}
