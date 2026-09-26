// Fill out your copyright notice in the Description page of Project Settings.


#include "WushuFPAnimInstance.h"
#include "BlasterCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Blaster/Weapon/Weapon.h"
#include "AnimNotify_ReloadFinished.h"
#include "Blaster/BlasterComponent/CombatComponent.h"

void UWushuFPAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	// 只缓存角色，**不在这里给 WeaponType / bWeaponEquipped / bIsInAir / bIsCrouched / Speed 赋值**：
	// 它们的默认值写在头文件上（枚举默认步枪、Speed 默认 0），Persona 预览靠的就是那个默认值
	// —— 预览里 TryGetPawnOwner 是空的，NativeUpdateAnimation 会直接 return。
	// 在这里赋一次反而会把预览带跑（Persona 会初始化预览实例）。
	BlasterCharacter1 = Cast<ABlasterCharacter>(TryGetPawnOwner());
}

void UWushuFPAnimInstance::NativeUpdateAnimation(float DeltaTime)
{
	Super::NativeUpdateAnimation(DeltaTime);

	// 手模是挂在角色身上的组件，TryGetPawnOwner() 拿到的是同一个角色；
	// 生成顺序上偶尔会晚一帧，所以这里补一次（和 UBlasterAnimInstance 一样的写法）
	if (BlasterCharacter1 == nullptr)
	{
		BlasterCharacter1 = Cast<ABlasterCharacter>(TryGetPawnOwner());
	}
	if (BlasterCharacter1 == nullptr) return;

	bIsInAir = BlasterCharacter1->GetCharacterMovement()->IsFalling();
	bWeaponEquipped = BlasterCharacter1->IsWeaponEquipped() && !BlasterCharacter1->IsSpikeDrawn();

	// 蹲下：直接读 ACharacter 那个复制属性（远端玩家也有值），
	// 和 ABlasterCharacter::GetFPCrouchDrop() 用的是同一个判定 —— 手模的蹲姿和视角下移同步。
	bIsCrouched = BlasterCharacter1->bIsCrouched;

	// 水平速度（cm/s），和 UBlasterAnimInstance::NativeUpdateAnimation 里那两行同一套算法：
	// 去掉 Z 再取长度 —— 第一人称的手部摆动只看平面速度，跳起/下落那一瞬的竖直速度
	// 不该把走路姿势带跑（空中姿势是 bIsInAir 在管）。
	FVector Velocity = BlasterCharacter1->GetVelocity();
	Velocity.Z = 0.f;
	Speed = Velocity.Size();

	/*
	 * 八向方向（给手模的状态机用）：和第三人称那边同一个函数、同一个口径。
	 * 基准是**角色正前方**而不是镜头 —— 本工程 bUseControllerRotationYaw = true，
	 * 角色 Yaw 永远跟着视角，两者等价；写 GetActorForwardVector 是为了和第三人称
	 * 以及 UBlasterCharacterAnimInstance::Direction 完全对齐，将来关了那个开关也不会分叉。
	 *
	 * 静止时函数返回 false、原地不动（保留上一帧方向），理由见头文件。
	 */
	MovementDirection8::FromVelocity(Velocity, BlasterCharacter1->GetActorForwardVector(),
		FMath::Square(MovementDirectionMinSpeed), MovementDirection);

	// 空手那一段（Jett 放完 E / Q）：手模要单独做姿态就靠它。注意不能用 !bWeaponEquipped 代替 ——
	// 掏出尖刺包时那个也是 false，但那时候不是空手。
	bEmptyHand = BlasterCharacter1->IsEmptyHandLocked();

	// 取武器类型必须自己判空：ABlasterCharacter::GetWeaponType() 内部直接解引用
	// EquippedWeapon（BlasterCharacter.h 里那句 `return EquippedWeapon->GetWeaponType();`），
	// 空手时调它会崩。所以走 GetEquippedWeapon() 自己判断。
	//
	// 另外武器类型只在"真的持枪"时才算数 —— 掏出尖刺包时 bWeaponEquipped 已经是 false，
	// 这里跟着回到 EWT_MAX，手模就会切回空手姿势，和第三人称那边同步。
	AWeapon* Weapon = BlasterCharacter1->GetEquippedWeapon();

	/*
	 * ——— 持技能投掷物期间：报"技能类型"而不是 EWT_MAX ———
	 *
	 * 三个技能**站着待命那段姿势在动画蓝图里做**（用户要求），而 ABP 只能靠 WeaponType 分辨姿势 ——
	 * 偏偏持技能期间枪是收起来的（EquippedWeapon == nullptr），照上面那句只会得到 EWT_MAX，
	 * ABP 那边就是"空手"。所以这里插一层：手上有投掷物时把 WeaponType 报成那个技能对应的类型
	 *（见 GetThrowableWeaponType），ABP 照 WeaponType 切到"举着它待命"即可。
	 *
	 * ★ 动作那几段（拿起 / 丢出去 / 按住 / 收起）由 C++ 播一次性蒙太奇，**不经过这里** ——
	 *   槽位上有蒙太奇在出力时蒙太奇说了算，演完回落到这个待命姿势。所以这里不需要再报段位。
	 * ★ 读的是**本机**那份（本机按下技能键就成立）：手模只在本机播，不等服务器。
	 * ★ 顺序：投掷物排在枪前面判 —— 持技能期间 EquippedWeapon 本来就是空的，两者不会同时成立。
	 */
	ThrowableKind = BlasterCharacter1->GetHeldThrowableKind();

	if (ThrowableKind != EBlasterThrowableKind::None)
	{
		WeaponType = GetThrowableWeaponType(ThrowableKind);
		return;
	}

	/*
	 * ——— 掏出尖刺包（按 4）期间：报 EWT_Spike ———
	 *
	 * 同理：掏着包时枪收在挂点上（EquippedWeapon == nullptr），照下面那句只会得到 EWT_MAX，
	 * ABP 那边就是"空手"。用户要做**拿着包的时候**的动画机，所以按复制的 bSpikeDrawn 插一层。
	 * 哨兵用 return 而不是 else：包和投掷物天然互斥（掏包要求 ECS_Unoccupied，持技能期间不是），
	 * 写成 return 是为了和上面那条对称、也免得以后加分支时漏判。
	 */
	if (BlasterCharacter1->IsSpikeDrawn())
	{
		WeaponType = EWeaponType::EWT_Spike;
		return;
	}

	WeaponType = (bWeaponEquipped && Weapon != nullptr)
		? Weapon->GetWeaponType()
		: EWeaponType::EWT_MAX;
}

void UWushuFPAnimInstance::FinishReload()
{
	// 和 UBlasterCharacterAnimInstance::FinishReload() 完全同一条路（共用一个实现）。
	// 差别只在于调用的时机受限于"手模动画只在本机播"—— 详见头文件上的注释。
	UAnimNotify_ReloadFinished::TriggerFinishReload(this);
}

void UWushuFPAnimInstance::FinishEmptyHand()
{
	// 和第三人称那边完全同一条路（都转调 ABlasterCharacter::EmptyHandFinish，自带幂等门禁）。
	// 差别只在于手模动画只在本机播：这个入口只解决"射手本人这台"的收尾，
	// 服务器上别人那台没有手模动画，只能等保险丝。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(this);
	if (Character == nullptr) return;
	Character->EmptyHandFinish();
}

void UWushuFPAnimInstance::EquipFinish()
{
	// 同上。手模这套挂掏枪结束是最合适的：掏枪动画播完 = 枪真的到手上了。
	ABlasterCharacter* Character = UAnimNotify_ReloadFinished::ResolveCharacter(this);
	if (Character == nullptr) return;
	UCombatComponent* Combat = Character->GetCombatComponent();
	if (Combat == nullptr) return;
	Combat->EquipFinish();
}
