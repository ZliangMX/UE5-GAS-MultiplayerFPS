// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimNotify_ReloadFinished.h"
#include "BlasterCharacter.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Weapon/Weapon.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

ABlasterCharacter* UAnimNotify_ReloadFinished::ResolveCharacter(const UObject* AnimOwnerOrMesh)
{
	if (AnimOwnerOrMesh == nullptr) return nullptr;

	// const_cast 只在这里做一次：下面的 Cast<> 是引擎那套，喂 const 指针会挑到
	// 返回 const 类型的重载，后面 GetOwner()/GetOwningActor() 就都不好往下写了。
	UObject* Source = const_cast<UObject*>(AnimOwnerOrMesh);

	AActor* Owner = nullptr;
	if (const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Source))
	{
		Owner = Mesh->GetOwner();
	}
	else if (const UAnimInstance* AnimInstance = Cast<UAnimInstance>(Source))
	{
		Owner = AnimInstance->GetOwningActor();
	}
	else if (AActor* AsActor = Cast<AActor>(Source))
	{
		Owner = AsActor;
	}
	if (Owner == nullptr) return nullptr;

	if (ABlasterCharacter* Character = Cast<ABlasterCharacter>(Owner)) return Character;

	// 武器网格 / 挂在武器上的那些：owner 是 AWeapon，再往上摸一层就是角色。
	return Cast<ABlasterCharacter>(Owner->GetOwner());
}

void UAnimNotify_ReloadFinished::TriggerFinishReload(const UObject* AnimOwnerOrMesh)
{
	ABlasterCharacter* Character = ResolveCharacter(AnimOwnerOrMesh);
	if (Character == nullptr) return;

	UCombatComponent* Combat = Character->GetCombatComponent();
	if (Combat == nullptr) return;

	/*
	 * 霰弹枪不接这个通知。
	 *
	 * 它是一发一发压的：节奏归 CombatComponent 那条循环计时器管（StartShotgunShellTimer，
	 * 每 ShotgunShellTime 压一发），收尾也归它（压满/没备弹 → JumpToShotGunEnd → FinishReloading）。
	 * 而蒙太奇上挂了这个通知的话，压几发就响几次，每响一次都调 FinishReloading() ——
	 * 它里面走的是 UpdateAmmoValues()「按当前备弹把弹匣一次性填满」，
	 * 于是第一发就把整管塞满、备弹一次扣光，后面几条压弹动画全白播。
	 *
	 * 判定用武器类型（和 StartReloadTimer 里走哪条计时器的判定是同一条），
	 * 不看"压到第几发"—— 那种状态不该由动画侧反推。
	 */
	const AWeapon* Equipped = Character->GetEquippedWeapon();
	if (Equipped != nullptr && Equipped->GetWeaponType() == EWeaponType::EWT_Shotgun) return;

	/*
	 * FinishReloading() 自己能兜：
	 *   · HasAuthority() 才改状态/填弹 —— 客户端这条通知只是"动画播完了"，不碰权威值；
	 *   · 幂等门禁 —— 状态已经不是 ECS_Reloading（计时器先收的尾）就直接走人，不会填两遍。
	 */
	Combat->FinishReloading();
}

void UAnimNotify_ReloadFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	TriggerFinishReload(MeshComp);
}

FString UAnimNotify_ReloadFinished::GetNotifyName_Implementation() const
{
	return TEXT("Reload Finished");
}
