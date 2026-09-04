#include "CombatComponent.h"
#include "DrawDebugHelpers.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/Weapon/Weapon.h"
#include "Components/SphereComponent.h"
#include "Engine/SkeletalMeshSocket.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Camera/CameraComponent.h"
#include "TimerManager.h"
#include "Sound/SoundCue.h"
#include "Blaster/Character/BlasterAnimInstance.h"


UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	BaseWalkSpeed = 600.f;
	AimWalkSpeed = 450.f;
	ScopedWalkSpeed = 350.f;
}

void UCombatComponent::BeginPlay()
{
	Super::BeginPlay();


	if (Character)
	{
		UE_LOG(LogTemp, Warning, TEXT("Begin"));	
		Character->GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed;

		if (Character->GetFollowCamera())
		{
			DefaultFOV = Character->GetFollowCamera()->FieldOfView;
			CurrentFOV=DefaultFOV;
		}
		if (Character->HasAuthority())
		{
			InitializeCarriedAmmo();
		}
	}
}

void UCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	if (Character && Character->IsLocallyControlled())
	{
		TraceUnderCrosshairs(HitTarget);
		SetHUDCrosshairs(DeltaTime);
		InterpFOV(DeltaTime);
	}
}

void UCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UCombatComponent, PrimaryWeapon);
	DOREPLIFETIME(UCombatComponent, SecondaryWeapon);
}

void UCombatComponent::FireButtonPressed(bool bPressed)
{
	bFireButtonPressed = bPressed;
	
	
	if (bFireButtonPressed&&Character->GetEquippedWeapon()!=nullptr)
	{
		Fire();
		
	}
}

void UCombatComponent::ThrowGrenade()
{
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied)return;
	Character->SetCombatState(ECombatState::ECS_ThrowingGrenade);
	if (Character)
	{
		Character->PlayThrowGrenadeMontage();
		ShowAttachGrenade(true);
		AttachActorToLeftHand(Character->GetEquippedWeapon());
	}
	if (!Character->HasAuthority())
	{
		ServerThrowGrenade();
	}
}

void UCombatComponent::ShotgunShellReload()
{

	if (Character && Character->HasAuthority())
	{
		UpdateShotgunAmmoValues();
	}
	
}

void UCombatComponent::ShowAttachGrenade(bool bShowGrenade)
{
	if (Character && Character->GetAttachedGrenade())
	{
		Character->GetAttachedGrenade()->SetVisibility(bShowGrenade);
	}
}

void UCombatComponent::JumpToShotGunEnd()
{
	UAnimInstance* AnimInstance = Character->GetMesh()->GetAnimInstance();
	UE_LOG(LogTemp,Warning,TEXT("JumpToEnd"));
	if (AnimInstance && Character->GetReloadMontage())
	{
		
		AnimInstance->Montage_JumpToSection(FName("ShotgunEnd"));
	}
}

void UCombatComponent::ThrowGrenadeFinished()
{
	Character->SetCombatState(ECombatState::ECS_Unoccupied);
	AttachActorToRightHand(Character->GetEquippedWeapon());
}

void UCombatComponent::LaunchGrenade()
{
	
}

void UCombatComponent::ServerFire_Implementation(const FVector_NetQuantize& TraceHitTarget)
{
	MulticastFire(TraceHitTarget);
}

void UCombatComponent::MulticastFire_Implementation(const FVector_NetQuantize& TraceHitTarget)
{
	if (Character->GetEquippedWeapon() == nullptr) return;

	if (Character && Character->GetCombatState() == ECombatState::ECS_Reloading && Character->GetEquippedWeapon()->GetWeaponType() == EWeaponType::EWT_Shotgun)
	{
		Character->PlayFireMontage();
		Character->GetEquippedWeapon()->Fire(TraceHitTarget);
		Character->SetCombatState(ECombatState::ECS_Unoccupied);
		return;
	}
	if (Character && Character->GetCombatState()==ECombatState::ECS_Unoccupied)
	{
		Character->PlayFireMontage();
		Character->GetEquippedWeapon()->Fire(TraceHitTarget);
	}
}

void UCombatComponent::Fire()
{
	if (CanFire())
	{
		bCanFire=false;
		ServerFire(HitTarget.ImpactPoint);
		if (Character->GetEquippedWeapon())
		{
			CrosshairShootFactor=.75f;
			// 后坐力：Fire() 只在开枪的本地客户端执行，所以每把枪的后坐只作用于自己。
			// 数值全部来自当前武器。若武器配了弹道序列（RecoilPitchPattern/RecoilYawPattern），
			// 按本次连发的第 N 发取序列值 → 弹道固定可压枪；留空则退回单值+随机水平。
			// 间隔过久没开火（换弹/切枪/松手点射）→ 弹道回到第 0 发。
			AWeapon* Weapon = Character->GetEquippedWeapon();
			if (GetWorld()->GetTimeSeconds() - LastRecoilShotTime > RecoilPatternResetDelay)
			{
				RecoilShotCount = 0;
			}
			float Pitch = Weapon->GetRecoilPitch();
			float Yaw = FMath::FRandRange(-Weapon->GetRecoilYaw(), Weapon->GetRecoilYaw());
			const TArray<float>& PitchPattern = Weapon->GetRecoilPitchPattern();
			const TArray<float>& YawPattern = Weapon->GetRecoilYawPattern();
			if (PitchPattern.Num() > 0)
			{
				Pitch = PitchPattern[FMath::Min(RecoilShotCount, PitchPattern.Num() - 1)];
			}
			if (YawPattern.Num() > 0)
			{
				Yaw = YawPattern[FMath::Min(RecoilShotCount, YawPattern.Num() - 1)];
			}
			Character->AddRecoil(Pitch, Yaw, Weapon->GetMaxRecoilPitch());
			RecoilShotCount++;
			LastRecoilShotTime = GetWorld()->GetTimeSeconds();
		}
		StartFireTimer();
	}

}

void UCombatComponent::AttachActorToRightHand(AActor* ActorToAttach)
{
	if (Character == nullptr||Character->GetMesh() == nullptr||ActorToAttach == nullptr)return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(RightHandSocket);
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach,Character->GetMesh());
	}
}

void UCombatComponent::AttachActorToLeftHand(AActor* ActorToAttach)
{
	if (Character == nullptr||Character->GetMesh() == nullptr||ActorToAttach == nullptr|| Character->GetEquippedWeapon() == nullptr)return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(LeftHandSocket);
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach,Character->GetMesh());
	}
}

void UCombatComponent::StartFireTimer()
{
	if (Character->GetEquippedWeapon()==nullptr || Character==nullptr) return;
	
	Character->GetWorldTimerManager().SetTimer(
		FireTimer,
		this,
		&UCombatComponent::FireTimerFinished,
		Character->GetEquippedWeapon()->FireDelay
	);
	
}

void UCombatComponent::FireTimerFinished()
{
	if (Character->GetEquippedWeapon() == nullptr || Character == nullptr) return;
	bCanFire=true;
	if (bFireButtonPressed && Character->GetEquippedWeapon()->bAutomatic)
	{
		Fire();
	}
	ReloadEmptyWeapon();
}

bool UCombatComponent::CanFire()
{
	if (Character->GetEquippedWeapon()==nullptr) return false;
	if (!Character->GetEquippedWeapon()->IsEmpty() && bCanFire && Character->GetCombatState() == ECombatState::ECS_Reloading && Character->GetEquippedWeapon()->GetWeaponType() == EWeaponType::EWT_Shotgun)return  true;
	return !Character->GetEquippedWeapon()->IsEmpty() && bCanFire && Character->GetCombatState()==ECombatState::ECS_Unoccupied;
}

void UCombatComponent::InitializeCarriedAmmo()
{
	CarriedAmmoMap.Emplace(EWeaponType::EWT_AssaultRifle,StartingARAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_Pistol,StartingPistolAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_Shotgun,StartingShotgunAmmo);
	CarriedAmmoMap.Emplace(EWeaponType::EWT_SniperRifle,StartingSniperAmmo);
}


void UCombatComponent::OnRep_PrimaryWeapon(AWeapon* LastPrimaryWeapon)
{
	if (PrimaryWeapon && Character && PrimaryWeapon != Character->GetEquippedWeapon())
	{
		PrimaryWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
		AttachActorToSocket(PrimaryWeapon, PrimaryHolsterSocket);
	}
}

void UCombatComponent::OnRep_SecondaryWeapon(AWeapon* LastSecondaryWeapon)
{
	if (SecondaryWeapon && Character && SecondaryWeapon != Character->GetEquippedWeapon())
	{
		SecondaryWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
		AttachActorToSocket(SecondaryWeapon, SecondaryHolsterSocket);
	}
}

void UCombatComponent::DropEquippedWeapon()
{
	AWeapon* Weapon = Character ? Character->GetEquippedWeapon() : nullptr;
	if (!Weapon) return;

	if (Weapon == PrimaryWeapon)   PrimaryWeapon = nullptr;
	if (Weapon == SecondaryWeapon) SecondaryWeapon = nullptr;
	Character->SetEquippedWeapon(nullptr);
	Weapon->Dropped();
}

// 死亡时两把武器都掉到地上（主+副），Combat 槽位清空 → 下一回合不再继承
void UCombatComponent::DropAllWeapons()
{
	if (!Character) return;

	AWeapon* Primary = PrimaryWeapon;
	AWeapon* Secondary = SecondaryWeapon;

	if (Primary) DropWeaponFromSlot(Primary);
	if (Secondary) DropWeaponFromSlot(Secondary);
}

void UCombatComponent::UpdateCarriedAmmo()
{
	if (Character->GetEquippedWeapon() == nullptr)return;
	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		Character->SetCarriedAmmo(CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]);
	}
	
	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
}

void UCombatComponent::PlayEquipWeaponSound()
{
	if (Character)
	{
		if (Character->GetEquippedWeapon())
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				Character->GetEquippedWeapon()->EquipSound,
				Character->GetActorLocation()
			);
		}
	}
}

void UCombatComponent::ReloadEmptyWeapon()
{
	if (Character->GetEquippedWeapon())
	{
		if (Character->GetEquippedWeapon()->IsEmpty())
		{
			Reload();
		}
	}
}

void UCombatComponent::AttachActorToSocket(AActor* ActorToAttach, FName SocketName)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr) return;
	const USkeletalMeshSocket* Socket = Character->GetMesh()->GetSocketByName(SocketName);
	if (Socket)
	{
		Socket->AttachActor(ActorToAttach, Character->GetMesh());
	}
}

FName UCombatComponent::GetHolsterSocketForWeapon(AWeapon* Weapon) const
{
	return (Weapon == SecondaryWeapon) ? SecondaryHolsterSocket : PrimaryHolsterSocket;
}

void UCombatComponent::ResetRecoilPattern()
{
	RecoilShotCount = 0;
	LastRecoilShotTime = 0.f;
}

void UCombatComponent::EquipSlotWeapon(AWeapon* WeaponToEquip)
{
	if (!Character || !WeaponToEquip) return;
	if (WeaponToEquip == Character->GetEquippedWeapon()) return;
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied) return;

	// 切枪后弹道从第 0 发重新开始（每把枪各跳各的序列）
	ResetRecoilPattern();

	if (Character->IsSpikeDrawn()) HolsterSpike();

	AWeapon* Previous = Character->GetEquippedWeapon();
	if (Previous)
	{
		Previous->SetWeaponState(EWeaponState::EWS_Equipped);
		AttachActorToSocket(Previous, GetHolsterSocketForWeapon(Previous));
	}

	Character->SetEquippedWeapon(WeaponToEquip);
	WeaponToEquip->SetWeaponState(EWeaponState::EWS_Equipped);
	WeaponToEquip->SetOwner(Character);
	AttachActorToSocket(WeaponToEquip, RightHandSocket);
	WeaponToEquip->SetHUDAmmo();

	UpdateCarriedAmmo();
	PlayEquipWeaponSound();
	ReloadEmptyWeapon();

	Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = true;
}

void UCombatComponent::SwitchWeapon(EWeaponSlot Slot)
{
	if (!Character || !Character->HasAuthority()) return;
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied) return;

	switch (Slot)
	{
	case EWeaponSlot::ESlot_Primary:
		EquipSlotWeapon(PrimaryWeapon);
		break;
	case EWeaponSlot::ESlot_Secondary:
		EquipSlotWeapon(SecondaryWeapon);
		break;
	case EWeaponSlot::ESlot_Melee:
		break;
	case EWeaponSlot::ESlot_Spike:
		DrawSpike();
		break;
	default:
		break;
	}
}

void UCombatComponent::DrawSpike()
{
	if (!Character || !Character->HasAuthority()) return;
	if (!Character->CarriedSpike || Character->IsSpikeDrawn()) return;
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied) return;

	AWeapon* Gun = Character->GetEquippedWeapon();
	if (Gun)
	{
		Gun->SetWeaponState(EWeaponState::EWS_Equipped);
		AttachActorToSocket(Gun, GetHolsterSocketForWeapon(Gun));
		Character->SetEquippedWeapon(nullptr);
	}

	Character->SetSpikeDrawn(true);
	Character->CarriedSpike->Draw(Character);
}

void UCombatComponent::HolsterSpike()
{
	if (!Character || !Character->HasAuthority()) return;
	if (!Character->CarriedSpike || !Character->IsSpikeDrawn()) return;

	Character->SetSpikeDrawn(false);
	Character->CarriedSpike->Holster(Character);
}

void UCombatComponent::StartSpikePlant()
{
	if (Character && Character->CarriedSpike && Character->IsSpikeDrawn())
	{
		Character->CarriedSpike->StartPlant(Character);
	}
}

void UCombatComponent::CancelSpikePlant()
{
	if (Character && Character->CarriedSpike)
	{
		Character->CarriedSpike->CancelAction();
	}
}

void UCombatComponent::DropWeaponFromSlot(AWeapon* Weapon)
{
	if (!Weapon) return;
	if (Weapon == PrimaryWeapon)   PrimaryWeapon = nullptr;
	if (Weapon == SecondaryWeapon) SecondaryWeapon = nullptr;
	if (Weapon == Character->GetEquippedWeapon()) Character->SetEquippedWeapon(nullptr);
	Weapon->Dropped();
}

void UCombatComponent::EquipWeapon(AWeapon* WeaponToEquip)
{
	if (Character == nullptr || WeaponToEquip == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("WeaponToEquip == nullptr."));
		return;
	}
	if (Character->GetCombatState() != ECombatState::ECS_Unoccupied) return;

	EWeaponSlot Target = WeaponToEquip->IsSecondaryWeapon() ? EWeaponSlot::ESlot_Secondary : EWeaponSlot::ESlot_Primary;
	AWeapon*& SlotRef = (Target == EWeaponSlot::ESlot_Secondary) ? SecondaryWeapon : PrimaryWeapon;
	if (SlotRef) DropWeaponFromSlot(SlotRef);

	SlotRef = WeaponToEquip;
	EquipSlotWeapon(WeaponToEquip);
}

void UCombatComponent::Reload()
{
	if (Character->GetCarriedAmmo()>0 && Character->GetCombatState()==ECombatState::ECS_Unoccupied && Character->GetEquippedWeapon() && !Character->GetEquippedWeapon()->IsFull())
	{
		ServerReload();
	}
}

void UCombatComponent::FinishReloading()
{
	if (Character==nullptr)return;
	if (Character->HasAuthority())
	{
		Character->SetCombatState(ECombatState::ECS_Unoccupied);
		UpdateAmmoValues();
	}
	if (bFireButtonPressed)
	{
		Fire();
	}
	
}

void UCombatComponent::ServerReload_Implementation()
{
	if (Character ==  nullptr || Character->GetEquippedWeapon()==nullptr) return;

	Character->SetCombatState(ECombatState::ECS_Reloading);
	HandleReload();
}

void UCombatComponent::UpdateAmmoValues()
{
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;
	int32 ReloadAmount = AmountToReload();
	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]-= ReloadAmount;
		Character->SetCarriedAmmo(CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]);
	}
	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
	Character->GetEquippedWeapon()->AddAmmo(ReloadAmount);	
}

void UCombatComponent::UpdateShotgunAmmoValues()
{
	UE_LOG(LogTemp,Warning,TEXT("ShotGunReload"))
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;
	if (Character->GetEquippedWeapon()->IsFull() || Character->GetCarriedAmmo() == 0)
	{
		//jump to shotgun section
		JumpToShotGunEnd();
		return;
	}

	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]-= 1;
		Character->SetCarriedAmmo(CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()]);
	}
	Controller = Controller == nullptr? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;
	if (Controller)
	{
		Controller->SetHUDCarriedAmmo(Character->GetCarriedAmmo());
	}
	Character->GetEquippedWeapon()->AddAmmo(1);
	bCanFire = true;
	
	if (Character->GetEquippedWeapon()->IsFull() || Character->GetCarriedAmmo() == 0)
	{
		//jump to shotgun section
		JumpToShotGunEnd();
	}
}


void UCombatComponent::HandleReload()
{
	Character->PlayReloadMontage();
}

int32 UCombatComponent::AmountToReload()
{
	if (Character->GetEquippedWeapon()==nullptr) return 0;
	int RoomInMag =Character->GetEquippedWeapon() ->GetMagCapacity()-Character->GetEquippedWeapon()->GetAmmo();
	if (CarriedAmmoMap.Contains(Character->GetEquippedWeapon()->GetWeaponType()))
	{
		int AmmoCarried = CarriedAmmoMap[Character->GetEquippedWeapon()->GetWeaponType()];
		int least = FMath::Min(RoomInMag, AmmoCarried);
		return FMath::Clamp(RoomInMag,0,least);
	}
	return 0;
}

void UCombatComponent::ServerThrowGrenade_Implementation()
{
	Character->SetCombatState(ECombatState::ECS_ThrowingGrenade);
	if (Character)
	{
		Character->PlayThrowGrenadeMontage();
		ShowAttachGrenade(true);
		AttachActorToLeftHand(Character->GetEquippedWeapon());
			
	}
}


void UCombatComponent::SetAiming(bool bIsAiming)
{
	if (Character==nullptr || Character->GetEquippedWeapon()==nullptr) return;
	if (Character->GetLocalRole() == ROLE_Authority)
	{
		Character->SetAiming(bIsAiming);
		Character->GetCharacterMovement()->MaxWalkSpeed = (bIsAiming && Character->GetEquippedWeapon() && Character->GetEquippedWeapon()->CanScope()) ? ScopedWalkSpeed : (bIsAiming ? AimWalkSpeed : BaseWalkSpeed);

	}
	else
	{
		ServerSetAiming(bIsAiming);
	}
	if (Character->IsLocallyControlled() && Character->GetEquippedWeapon()->CanScope())
	{
		Character->ShowSniperScopeWidget(bIsAiming);
	}
}

void UCombatComponent::TraceUnderCrosshairs(FHitResult& TraceHitResult)
{
	FVector2D ViewportSize;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);

	}

	FVector2D CrosshairLocation(ViewportSize.X / 2.f, ViewportSize.Y / 2.f);
	FVector CrosshairWorldPosition;
	FVector CrosshairWorldDirection;
	bool bScreenToWorld = UGameplayStatics::DeprojectScreenToWorld(
		UGameplayStatics::GetPlayerController(this, 0),
		CrosshairLocation,
		CrosshairWorldPosition,
		CrosshairWorldDirection
	);
	
	if (bScreenToWorld)
	{
		FVector Start = CrosshairWorldPosition;

		if (Character)
		{
			float DistanceToCharacter = (Character->GetActorLocation()-Start).Size();
			Start+=CrosshairWorldDirection*(DistanceToCharacter+100.f);
		}
		FVector End = Start + CrosshairWorldDirection * TRACE_LENGTH;
		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(Character);
		GetWorld()->LineTraceSingleByChannel(
			TraceHitResult,
			Start,
			End,
			ECollisionChannel::ECC_Visibility,
			QueryParams
		);
		if (TraceHitResult.GetActor() && TraceHitResult.GetActor()->Implements<UInteractWithCrosshairsInterface>())
		{
			HUDPackage.CrosshairsColor=FLinearColor::Red;
		}
		else
		{
			HUDPackage.CrosshairsColor=FLinearColor::White;	
		}
	}
	if (TraceHitResult.bBlockingHit==false)
	{
		TraceHitResult.ImpactPoint=CrosshairWorldPosition+CrosshairWorldDirection*TRACE_LENGTH;
	}
}

void UCombatComponent::SetHUDCrosshairs(float DeltaTime)
{
	if (Character==nullptr)return;

	Controller = Controller==nullptr ? Cast<ABlasterPlayerController>(Character->GetController()): nullptr;
	if (Controller)
	{
		HUD = HUD==nullptr ? Cast<ABlasterHUD>(Controller->GetHUD()):HUD;
		if (HUD)
		{
		
			if (Character->GetEquippedWeapon())
			{
				HUDPackage.CrosshairCenter=Character->GetEquippedWeapon()->CrosshairCenter;
				HUDPackage.CrosshairLeft=Character->GetEquippedWeapon()->CrosshairLeft;
				HUDPackage.CrosshairRight=Character->GetEquippedWeapon()->CrosshairRight;
				HUDPackage.CrosshairTop=Character->GetEquippedWeapon()->CrosshairTop;
				HUDPackage.CrosshairBottom=Character->GetEquippedWeapon()->CrosshairBottom;
			}
			else
			{
				HUDPackage.CrosshairCenter=nullptr;
				HUDPackage.CrosshairLeft=nullptr;
				HUDPackage.CrosshairRight=nullptr;
				HUDPackage.CrosshairTop=nullptr;
				HUDPackage.CrosshairBottom=nullptr;
			}
			// 狙击开镜：镜圈即准星，隐藏普通 HUD 准星（避免镜内出现两套准星）
			if (Character->IsAiming() && Character->GetEquippedWeapon()->CanScope())
			{
				HUDPackage.CrosshairCenter=nullptr;
				HUDPackage.CrosshairLeft=nullptr;
				HUDPackage.CrosshairRight=nullptr;
				HUDPackage.CrosshairTop=nullptr;
				HUDPackage.CrosshairBottom=nullptr;
			}
			//Calculate crosshair spread

			//[0,600] -> [0,1]
			FVector2D WalkSpeedRange(0.f,Character->GetCharacterMovement()->MaxWalkSpeed);
			FVector2D VelocityMultiplierRange(0.f,1.f);
			FVector Velocity=Character->GetVelocity();
			Velocity.Z=0.f;

			CrosshairVelocityFactor = FMath::GetMappedRangeValueClamped(WalkSpeedRange,VelocityMultiplierRange,Velocity.Size());

			if (Character->GetCharacterMovement()->IsFalling())
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor,2.25f,DeltaTime,2.25);
			}
			else
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor,0.f,DeltaTime,30.f);
			}
			if (Character->IsAiming())
			{
				CrosshairAimFactor=FMath::FInterpTo(CrosshairAimFactor,0.58f,DeltaTime,30.f);
			}
			else
			{
				CrosshairAimFactor=FMath::FInterpTo(CrosshairAimFactor,0.f,DeltaTime,30.f);
			}

			CrosshairShootFactor=FMath::FInterpTo(CrosshairShootFactor,0.f,DeltaTime,40.f);
			
			HUDPackage.CrosshairSpread=
				0.5f+
				CrosshairVelocityFactor +
				CrosshairInAirFactor -
				CrosshairAimFactor +
				CrosshairShootFactor;
			
			HUD->SetHUDPackage(HUDPackage);	
			
		}
	}
}



void UCombatComponent::ServerSetAiming_Implementation(bool bIsAiming)
{
	Character->SetAiming(bIsAiming);
	Character->GetCharacterMovement()->MaxWalkSpeed = (bIsAiming && Character->GetEquippedWeapon() && Character->GetEquippedWeapon()->CanScope()) ? ScopedWalkSpeed : (bIsAiming ? AimWalkSpeed : BaseWalkSpeed);
}

void UCombatComponent::InterpFOV(float DeltaTime)
{
	if (Character->GetEquippedWeapon()==nullptr) return;
	if (Character->IsAiming())
	{
		CurrentFOV=FMath::FInterpTo(CurrentFOV,Character->GetEquippedWeapon()->GetZoomFOV(),DeltaTime,Character->GetEquippedWeapon()->GetZoomInterpSpeed());
	}
	else
	{
		CurrentFOV=FMath::FInterpTo(CurrentFOV,DefaultFOV,DeltaTime,ZoomInterpSpeed);
	}
	if (Character && Character->GetFollowCamera())
	{
		Character->GetFollowCamera()->SetFieldOfView(CurrentFOV);
	}
}


