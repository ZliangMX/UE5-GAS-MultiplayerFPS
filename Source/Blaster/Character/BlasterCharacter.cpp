#include "BlasterCharacter.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/WidgetComponent.h"
#include "Net/UnrealNetwork.h"
#include "Blaster/Weapon/Weapon.h "
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/BlasterComponent/BlasterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "BlasterAnimInstance.h"
#include "Blaster/Blaster.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/PlantZone/PlantZone.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayAbilitySpec.h"
#include "GameplayTagContainer.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "Blaster/Abilities/BlasterGameplayTags.h"
#include "Engine/EngineTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

ABlasterCharacter::ABlasterCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UBlasterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
    PrimaryActorTick.bCanEverTick = true;
    // CharacterMovement 是 ACharacter private 成员，用 public 访问器读（指向由 SetDefaultSubobjectClass 替换成的子类实例）
    BlasterMoveComp = Cast<UBlasterMovementComponent>(GetCharacterMovement());

    SpawnCollisionHandlingMethod=ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(GetMesh());
    CameraBoom->TargetArmLength = 600.f;
    CameraBoom->bUsePawnControlRotation = true;

    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
    FollowCamera->bUsePawnControlRotation = false;

    bUseControllerRotationYaw = false;
    GetCharacterMovement()->bOrientRotationToMovement = true;

    OverheadWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("OverheadWidget"));
    OverheadWidget->SetupAttachment(RootComponent);

    Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("CombatComponent"));
    Combat->SetIsReplicated(true);

    // GAS 技能组件：冷却/充能/网络全走它。挂角色 → 每回合角色重建时充能自然重置（符合 Valorant）
    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AbilitySystemComponent->SetIsReplicated(true);
    AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

    GetCharacterMovement()->NavAgentProps.bCanCrouch = true;
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
    GetMesh()->SetCollisionObjectType(ECC_SkeletalMesh);
    GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
    GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);

    GetCharacterMovement()->RotationRate = FRotator(0.f, 0.f, 850.f);

    TurningInPlace = ETurningInPlace::ETIP_NotTurning;
    NetUpdateFrequency = 66.f;
    MinNetUpdateFrequency = 33.f;

    DissolveTimeline=CreateDefaultSubobject<UTimelineComponent>(TEXT("DissolveTimelineComponent"));

    AttachedGrenade = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AttachedGrenade"));
    AttachedGrenade->SetupAttachment(GetMesh(), GrenadeSocket);
    AttachedGrenade->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // 武装（逐风）风环绕特效：附加到 mesh 跟随身体，默认不激活，武装时才开
    SkillWindEffect = CreateDefaultSubobject<UNiagaraComponent>(TEXT("SkillWindEffect"));
    SkillWindEffect->SetupAttachment(GetMesh());
    SkillWindEffect->SetAutoActivate(false);
    SkillWindEffect->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void ABlasterCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME_CONDITION(ABlasterCharacter, OverlappingWeapon,COND_OwnerOnly);
    DOREPLIFETIME(ABlasterCharacter, bSpikeDrawn);
    DOREPLIFETIME(ABlasterCharacter, EquippedWeapon);
    DOREPLIFETIME(ABlasterCharacter, bAiming);
    DOREPLIFETIME(ABlasterCharacter, ReplicatedAO_Yaw);
    DOREPLIFETIME(ABlasterCharacter, Health);
    DOREPLIFETIME(ABlasterCharacter, CombatState);
    DOREPLIFETIME(ABlasterCharacter, bElimmed);
    DOREPLIFETIME(ABlasterCharacter, bDisableGameplay);
    DOREPLIFETIME(ABlasterCharacter, bIsInvulnerable);
    DOREPLIFETIME_CONDITION(ABlasterCharacter, CarriedAmmo, COND_OwnerOnly);
    DOREPLIFETIME(ABlasterCharacter, bCarryingSpike);
    DOREPLIFETIME(ABlasterCharacter, bSkillArmed);
    // 观战技能条/白闪 远程显示快照（服务器权威）
    DOREPLIFETIME(ABlasterCharacter, ReplicatedSkills);
    DOREPLIFETIME(ABlasterCharacter, ReplicatedBlindServerEndTime);
    DOREPLIFETIME(ABlasterCharacter, ReplicatedBlindOrigin);
    DOREPLIFETIME(ABlasterCharacter, bReplicatedSageSelecting);
    DOREPLIFETIME(ABlasterCharacter, bReplicatedCurveballHolding);
}


void ABlasterCharacter::Destroyed()
{
    Super::Destroyed();

    if (ElimBotComponent)
    {
        ElimBotComponent->DestroyComponent();
    }
    if (Combat && Combat->PrimaryWeapon)
    {
        Combat->PrimaryWeapon->Dropped();
    }
    if (Combat && Combat->SecondaryWeapon)
    {
        Combat->SecondaryWeapon->Dropped();
    }
    bSpikeDrawn = false;

    if (CarriedSpike)
    {
        CarriedSpike->Drop();
        CarriedSpike = nullptr;
        bCarryingSpike = false;
    }
}

void ABlasterCharacter::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);

    if (APlayerController* PC = Cast<APlayerController>(NewController))
    {
        UE_LOG(LogTemp, Warning, TEXT("PossessedBy %s"), *PC->GetName());
        if (PC->IsLocalController())
        {
            UE_LOG(LogTemp, Warning, TEXT("LocallyControlledPlayerController is %s"), *PC->GetName());
            if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
            {
                UE_LOG(LogTemp, Warning, TEXT("PossessedBy: IsLocalController=%d, LocalPlayer=%s"),
       PC->IsLocalController(), LocalPlayer ? *LocalPlayer->GetName() : TEXT("nullptr"));

                if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
                {
                    Subsystem->AddMappingContext(DefaultMappingContext, 0);
                    UE_LOG(LogTemp, Warning, TEXT("Added Input Mapping Context on local controller"));
                }
            }
        }
    }
}

void ABlasterCharacter::BeginPlay()
{
    Super::BeginPlay();

    // GAS：初始化 ASC 与自身的能力 ActorInfo
    if (AbilitySystemComponent)
    {
        AbilitySystemComponent->InitAbilityActorInfo(this, this);
    }
    // 服务器为默认技能列表 Grant（客户端由能力系统自动复制 spec）
    if (HasAuthority())
    {
        if (AbilitySystemComponent)
        {
            for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
            {
                if (AbilityClass)
                {
                    AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
                }
            }
        }
        OnTakeAnyDamage.AddDynamic(this,&ABlasterCharacter::ReceiveDamage);
    }
    if (AttachedGrenade)
    {
        AttachedGrenade->SetVisibility(false);
    }
    if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
        {
            if (DefaultMappingContext)
            {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("DefaultMappingContext is not set for %s!"), *GetName());
            }
        }
    }
}

void ABlasterCharacter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    AimOffset(DeltaTime);

    HideCameraIfCharacterClose();
    UpdateRecoil(DeltaTime);
    PollInit();

    // Sage 选中态：准心扫描队友，HUD 据此显示其血条
    UpdateSageHealTarget();

    // 武装窗口兜底解除：GAS EndAbility 会清能力定时器（AbilitySystem.ClearAbilityTimers），
    // 定时器可能失效 → 用窗口期限兜底，防止过期后武装态（风效/技能条青条）残留。
    if (bSkillArmed && AbilitySystemComponent)
    {
        for (const FGameplayAbilitySpec& Spec : AbilitySystemComponent->GetActivatableAbilities())
        {
            UBlasterGameplayAbility* Ability = Spec.GetPrimaryInstance()
                ? Cast<UBlasterGameplayAbility>(Spec.GetPrimaryInstance())
                : Cast<UBlasterGameplayAbility>(Spec.Ability.Get());
            if (Ability && Ability->IsArmedRaw() && Ability->GetArmedTimeRemaining() <= 0.f)
            {
                Ability->ForceExpireArmedWindow();
            }
        }
    }

    // 技能条远程显示快照：服务器每帧用权威 ASC 重算（观战者/队友据此画别人的技能条）。
    // 值与上一帧一致时引擎不会重发；只更新时才有网络流量。
    if (HasAuthority())
    {
        RefreshReplicatedSkills();
    }
}

void ABlasterCharacter::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    if (Combat)
    {
        Combat->Character = this;
    }
}

void ABlasterCharacter::PlayFireMontage()
{
    if (Combat == nullptr || EquippedWeapon == nullptr)return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance && FireWeaponMontage)
    {
        AnimInstance->Montage_Play(FireWeaponMontage);
        FName SectionName;
        SectionName = bAiming ? FName("RifleAim") : FName("RifleHip");
        AnimInstance->Montage_JumpToSection(SectionName);

    }
}

void ABlasterCharacter::PlayHitReactMontage()
{
    if (Combat == nullptr || EquippedWeapon == nullptr)return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance && HitReactMontage)
    {
        AnimInstance->Montage_Play(HitReactMontage);
        FName SectionName("FromFront");
        AnimInstance->Montage_JumpToSection(SectionName);

    }


}

void ABlasterCharacter::PlayElimMontage()
{

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance && ElimMontage)
    {
        AnimInstance->Montage_Play(ElimMontage);
    }
}

void ABlasterCharacter::PlayReloadMontage()
{
    if (Combat == nullptr || EquippedWeapon == nullptr)return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance && ReloadMontage)
    {
        AnimInstance->Montage_Play(ReloadMontage);
        FName SectionName;

        switch (EquippedWeapon->GetWeaponType())
        {
        case EWeaponType::EWT_AssaultRifle:
            SectionName=FName("Rifle");
            break;
        case EWeaponType::EWT_Pistol:
            SectionName=FName("Pistol");
            break;
        case EWeaponType::EWT_Shotgun:
            SectionName=FName("Shotgun");
            break;
        case EWeaponType::EWT_SniperRifle:
            SectionName=FName("SniperRifle");
            break;
        }

        AnimInstance->Montage_JumpToSection(SectionName);
    }
}

void ABlasterCharacter::PlayThrowGrenadeMontage()
{
    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance && ThrowGrenadeMontage)
    {
            AnimInstance->Montage_Play(ThrowGrenadeMontage);
    }
}

void ABlasterCharacter::MulticastPlantAnimation_Implementation(bool bStart)
{
    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (!AnimInstance) return;

    if (bStart)
    {
        if (PlantMontage)
        {
            AnimInstance->Montage_Play(PlantMontage);
        }
    }
    else
    {
        if (PlantMontage)
        {
            AnimInstance->Montage_Stop(0.2f, PlantMontage);
        }
    }
}

void ABlasterCharacter::Elim()
{
    ServerElim();
    // Round-based mode: no auto-respawn. ElimTimerFinished shows spectator view.
    GetWorldTimerManager().SetTimer(
        ElimTimer,
        this,
        &ABlasterCharacter::ElimTimerFinished,
        ElimDelay
    );
}

void ABlasterCharacter::ServerElim_Implementation()
{
    // 死亡掉枪：主副武器都掉到地上（槽位清空），只有活过上一回合的玩家枪械才会继承
    if (Combat)
    {
        Combat->DropAllWeapons();
    }
    bSpikeDrawn = false;

    // Drop spike if carrying
    if (CarriedSpike)
    {
        CarriedSpike->Drop();
        CarriedSpike = nullptr;
        bCarryingSpike = false;
    }

    bElimmed = true;
    PlayElimMontage();
    if (BlasterPlayerController)
    {
        BlasterPlayerController->SetHUDWeaponAmmo(0);
    }
    //Start dissolve effect
    if (DissolveMaterialInstance)
    {
        DynamicDissolveMaterialInstance=UMaterialInstanceDynamic::Create(DissolveMaterialInstance,this);

        GetMesh()->SetMaterial(0,DynamicDissolveMaterialInstance);
        DynamicDissolveMaterialInstance->SetScalarParameterValue(TEXT("Dissolve"),0.55f);
        DynamicDissolveMaterialInstance->SetScalarParameterValue(TEXT("Glow"),200.f);

    }
    StartDissolve();

    //Disable character movement
    GetCharacterMovement()->DisableMovement();
    GetCharacterMovement()->StopMovementImmediately();
    bDisableGameplay = true;

    if (Combat)
    {
        Combat->FireButtonPressed(false);
    }

    //Disable Collision
    GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    //Spawn elim bot
    if (ElimBotEffect)
    {
        FVector ElimVotSpawnPoint(GetActorLocation().X,GetActorLocation().Y,GetActorLocation().Z+200.f);
        ElimBotComponent=UGameplayStatics::SpawnEmitterAtLocation(GetWorld(),ElimBotEffect,ElimVotSpawnPoint,GetActorRotation());
    }
    bool bHideSniperScope = IsLocallyControlled() && Combat && bAiming && EquippedWeapon && EquippedWeapon->GetWeaponType()==EWeaponType::EWT_SniperRifle;
    if (bHideSniperScope)
    {
        ShowSniperScopeWidget(false);
    }

    // 权威机（Host/单机）上本机阵亡：bElimmed 在服务器直接置位，OnRep_Elim 不会回放，
    // 所以也要在这里通知 PC 延迟进观战。网络客户端的死者由 OnRep_Elim 触发，此处不会命中。
    if (IsLocallyControlled())
    {
        if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
        {
            BPC->HandleLocalPlayerEliminated();
        }
    }
}

void ABlasterCharacter::ElimTimerFinished()
{
    // Round-based mode: don't respawn. Keep in spectator.
    // Let the GameMode control round restart.
}

void ABlasterCharacter::Interact(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
    if (!PS) return;

    // Plant spike
    if (OverlappingSpike && bCarryingSpike && PS->Team == ETeam::ET_TeamA)
    {
        OverlappingSpike->StartPlant(this);
        return;
    }

    // Defuse spike — check overlap
    if (OverlappingSpike && OverlappingSpike->GetSpikeState() == ESpikeState::ESS_Planted && PS->Team == ETeam::ET_TeamB)
    {
        OverlappingSpike->StartDefuse(this);
        return;
    }
}

void ABlasterCharacter::InteractCancel(const FInputActionValue& Value)
{
    if (OverlappingSpike)
    {
        OverlappingSpike->CancelAction();
    }
}

void ABlasterCharacter::Buy(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(Controller);
    if (PC)
    {
        PC->ToggleBuyMenu();
    }
}

void ABlasterCharacter::UpdateDissolveMaterial(float DissolveValue)
{
   if (DynamicDissolveMaterialInstance)
   {
       DynamicDissolveMaterialInstance->SetScalarParameterValue(TEXT("Dissolve"),DissolveValue);
   }
}

void ABlasterCharacter::StartDissolve()
{
    DissolveTrack.BindDynamic(this, &ABlasterCharacter::UpdateDissolveMaterial);
    if (DissolveCurve && DissolveTimeline)
    {
        DissolveTimeline->AddInterpFloat(DissolveCurve,DissolveTrack);
        DissolveTimeline->Play();
    }
}

void ABlasterCharacter::OnRep_CombatState()
{
    switch (CombatState)
    {
    case ECombatState::ECS_Reloading:
        Combat->HandleReload();
        break;
    case ECombatState::ECS_ThrowingGrenade:
        if (!IsLocallyControlled())
        {
            const USkeletalMeshSocket* HandSocket = GetMesh()->GetSocketByName(Combat->LeftHandSocket);
            if (HandSocket)
            {
                HandSocket->AttachActor(GetEquippedWeapon(),GetMesh());
            }
            PlayThrowGrenadeMontage();
            Combat->ShowAttachGrenade(true);

        }
        break;
    }
}

void ABlasterCharacter::Move(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;

    FVector2D MovementVector = Value.Get<FVector2D>();

    // 诊断（节流）：回合后小地图冻结排查 —— 输入有没有到新角色、位移执行没有
    static int32 MoveCounter = 0;
    if (++MoveCounter % 45 == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Move] pawn=%s mv=(%.1f,%.1f) vel=%.0f bDisable=%d"),
            *GetName(), MovementVector.X, MovementVector.Y,
            GetCharacterMovement() ? GetCharacterMovement()->Velocity.Size() : -1.f,
            (int32)bDisableGameplay);
    }

    if (Controller != nullptr)
    {
        const FRotator Rotation = Controller->GetControlRotation();
        const FRotator YawRotation(0, Rotation.Yaw, 0);
        const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
        const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
        AddMovementInput(ForwardDirection , MovementVector.X);
        AddMovementInput(RightDirection , MovementVector.Y);

    }
}

void ABlasterCharacter::Look(const FInputActionValue& Value)
{
    FVector2D LookAxisVector = Value.Get<FVector2D>();
    if (Controller != nullptr)
    {
        AddControllerYawInput(LookAxisVector.X);
        AddControllerPitchInput(LookAxisVector.Y);

        // 诊断（节流）：回合后小地图冻结排查 —— 鼠标视角输入有没有到新角色
        static int32 LookCounter = 0;
        if (++LookCounter % 45 == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Look] pawn=%s mv=(%.1f,%.1f) ctrlYaw=%.0f"),
                *GetName(), LookAxisVector.X, LookAxisVector.Y, GetControlRotation().Yaw);
        }
    }
}

void ABlasterCharacter::Jump()
{
    if (bDisableGameplay)return;
    if (bIsCrouched)
    {
        UnCrouch();
    }
    else
    {
        Super::Jump();
    }
}

void ABlasterCharacter::ReceiveDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
    class AController* InstigatedController, AActor* DamageCauser)
{
    if (bIsInvulnerable) return;

    Health = FMath::Clamp(Health - Damage, 0.0f, MaxHealth);
    UpdateHUDHealth();
    PlayHitReactMontage();

    // 伤害数字：在伤害来源方向显示漂浮数字（ReceiveDamage 只在服务器执行，
    // 本机（host）直接画，远端受害者通过 Client RPC 转发）
    if (Damage > 0.f)
    {
        ABlasterPlayerController* VictimPC = Cast<ABlasterPlayerController>(Controller);
        if (VictimPC)
        {
            FVector DamageOrigin = GetActorLocation();
            if (InstigatedController && InstigatedController->GetPawn())
            {
                DamageOrigin = InstigatedController->GetPawn()->GetActorLocation();
            }
            if (VictimPC->IsLocalController())
            {
                VictimPC->ShowDamageNumber(Damage, DamageOrigin);
                VictimPC->ShowDamageDirection(DamageOrigin);
            }
            else
            {
                VictimPC->ClientShowDamageNumber(Damage, DamageOrigin);
                VictimPC->ClientShowDamageDirection(DamageOrigin);
            }
        }
    }

    if (Health <= 0.0f)
    {
        ABlasterGameMode* BlasterGameMode=GetWorld()->GetAuthGameMode<ABlasterGameMode>();
        if (BlasterGameMode)
        {
            BlasterPlayerController=BlasterPlayerController==nullptr? Cast<ABlasterPlayerController>(Controller):BlasterPlayerController;
            ABlasterPlayerController* AttackerController= Cast<ABlasterPlayerController>(InstigatedController);
            BlasterGameMode->PlayerEliminated(this,BlasterPlayerController,AttackerController);
        }
    }
}


void ABlasterCharacter::BlasterJump(const FInputActionValue& Value)
{
    UE_LOG(LogTemp, Warning, TEXT("Jump"));
    if (bPressedJump == true)return;
    Jump();
}

void ABlasterCharacter::Equip(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bCurveballHolding)return;
    if (Combat && OverlappingWeapon)
    {
        UE_LOG(LogTemp, Warning, TEXT("Equip2"));
        if (GetLocalRole()== ROLE_Authority && OverlappingWeapon!=nullptr)
        {
            Combat->EquipWeapon(OverlappingWeapon);
        }
        else
        {
            ServerEquipButtonPressed();
        }
    }
}

void ABlasterCharacter::Reload(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bCurveballHolding)return;
    if(Combat)
    {
        Combat->Reload();
    }
}

void ABlasterCharacter::BlasterCrouch(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;
    if (bIsCrouched)
    {
        UnCrouch();
    }
    else
    {
        Crouch();

    }
}

void ABlasterCharacter::AimStart(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;

    // Phoenix 持闪光态：右键 = 右拐抛掷（不进瞄准）
    if (bCurveballHolding)
    {
        ThrowCurveball(false);
        return;
    }

    if (Combat)
    {
        Combat->SetAiming(true);
    }

}

void ABlasterCharacter::AimEnd(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;
    if (bCurveballHolding) return; // 持球时 AimStart 直接抛球没进瞄准，忽略收尾
    if (Combat)
    {
        Combat->SetAiming(false);
    }
}

void ABlasterCharacter::FireStart(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;

    // Phoenix 持闪光态：左键 = 左拐抛掷（收枪状态下本就不会开枪）
    if (bCurveballHolding)
    {
        ThrowCurveball(true);
        return;
    }

    // Sage 选中态：左键 = 治疗准心指向的队友（不射击；收枪状态下本就不会开枪）
    if (bSageHealSelecting)
    {
        if (SageHealTarget && IsValidSageHealTarget(SageHealTarget))
        {
            PerformSageHeal(SageHealTarget);
        }
        return;
    }

    if (Combat)
    {
        Combat->FireButtonPressed(true);
    }
}

void ABlasterCharacter::FireEnd(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;
    if (Combat)
    {
        Combat->FireButtonPressed(false);
    }
}

void ABlasterCharacter::ThrowGrenade(const FInputActionValue& Value)
{

    if (bDisableGameplay || bSageHealSelecting || bCurveballHolding)return;
    if (Combat)
    {
        Combat->ThrowGrenade();
    }
}

void ABlasterCharacter::AimOffset(float DeltaTime)
{
    if (bDisableGameplay)
    {
        bUseControllerRotationYaw = false;
        TurningInPlace = ETurningInPlace::ETIP_NotTurning;
        return;
    }
    if (EquippedWeapon == nullptr) return;

    FVector Velocity = GetVelocity();
    Velocity.Z = 0.f;
    float Speed = Velocity.Size();
    bool bIsInAir = GetCharacterMovement()->IsFalling();


    if (Speed <1.0f && !bIsInAir)
    {
        FRotator CurrentAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
        FRotator DeltaAimRotation = UKismetMathLibrary::NormalizedDeltaRotator(CurrentAimRotation,StartingAimRotation);
        AO_Yaw = DeltaAimRotation.Yaw;
        if (TurningInPlace == ETurningInPlace::ETIP_NotTurning)
        {
            InterpAO_Yaw.Yaw = AO_Rotation.Yaw;
        }
        if (IsLocallyControlled())
        {

            ReplicatedAO_Yaw = AO_Yaw;
            ServerAO_Yaw(AO_Yaw);
        }
        else
        {
            // 在服务器或其他客户端上使用同步的值

            AO_Yaw = ReplicatedAO_Yaw;
        }
        bUseControllerRotationYaw = false;
        TurnInPlace(DeltaTime);
    }


    if (Speed > 1.f || bIsInAir)//running,or jumping
    {
        StartingAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);
        AO_Yaw = 0.f;
        bUseControllerRotationYaw = true;


        TurningInPlace = ETurningInPlace::ETIP_NotTurning;
        AO_Rotation.Yaw = 0;
    }

    AO_Pitch = GetBaseAimRotation().Pitch;
    if (AO_Pitch > 90.f && !IsLocallyControlled())
    {
        //map pitch from [270,360) to [-90,0)
        FVector2D InRange(270.f, 360.f);
        FVector2D OutRange(-90.f, 0.f);
        AO_Pitch = FMath::GetMappedRangeValueClamped(InRange, OutRange, AO_Pitch);
    }
}

void ABlasterCharacter::SetAiming(bool bIsAiming)
{
    bAiming = bIsAiming;
}


void ABlasterCharacter::OnRep_OverlappingWeapon(AWeapon* LastWeapon)
{
    if (OverlappingWeapon)
    {
        OverlappingWeapon->ShowPickupWidget(true);
    }
    if (LastWeapon)
    {
        LastWeapon->ShowPickupWidget(false);
    }
}

void ABlasterCharacter::ServerEquipButtonPressed_Implementation()
{

    if (Combat && OverlappingWeapon!=nullptr)
    {
        Combat->EquipWeapon(OverlappingWeapon);
    }
}

void ABlasterCharacter::Drop(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;

    if (GetLocalRole() == ROLE_Authority)
    {
        ServerDrop_Implementation();
    }
    else
    {
        ServerDrop();
    }
}

void ABlasterCharacter::Dash(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    // Lobby（选人）地图：禁用技能键（技能条也整块隐藏，见 BlasterHUD::DrawHUD）。
    // E 键是通用技能键，这里挡掉 = Sage 选中 / Phoenix 持球 / Jett 冲刺全部不可用。
    if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsInLobby())
        {
            return;
        }
    }

    // E 键是通用技能键，按角色配的技能走：Sage 治疗选中 → Phoenix 抛曲线球（闪光）→ Jett 冲刺
    if (HasSageHealAbility())
    {
        SageHealPressed(Value);
        return;
    }
    if (HasCurveballAbility())
    {
        CurveballPressed();
        return;
    }

    // 客户端把冲刺方向先发给服务器：服务器读不到远端玩家的 GetLastMovementInputVector
    //（LastControlInputVector 只在本地角色更新），不传的话服务器会回退到面朝方向 → 方向不对。
    // 与 TryActivateAbilitiesByTag 的可靠 RPC 同通道有序，服务器必先收到方向再激活能力。
    FVector DashDir = GetLastMovementInputVector();
    if (DashDir.IsNearlyZero()) DashDir = GetActorForwardVector();
    DashDir.Z = 0.f;
    if (!DashDir.Normalize()) DashDir = GetActorForwardVector();
    ServerSetPendingDashDirection(DashDir, true);

    // 通过 GAS tag 激活技能（不绑 GAS 输入体系，避免和现有 EnhancedInput 冲突）
    if (AbilitySystemComponent)
    {
        FGameplayTagContainer TagContainer;
        TagContainer.AddTag(BlasterGameplayTags::Ability_Jett_Dash);
        AbilitySystemComponent->TryActivateAbilitiesByTag(TagContainer);
    }
}

void ABlasterCharacter::ServerSetPendingDashDirection_Implementation(const FVector& Dir, bool bHasDirection)
{
    bHasPendingDashDirection = bHasDirection;
    PendingDashDirection = bHasDirection ? Dir.GetSafeNormal() : FVector::ZeroVector;
}

bool ABlasterCharacter::ConsumePendingDashDirection(FVector& OutDir)
{
    if (!bHasPendingDashDirection) return false;
    OutDir = PendingDashDirection;
    bHasPendingDashDirection = false;
    PendingDashDirection = FVector::ZeroVector;
    return true;
}

// --- Phoenix 曲线球（持闪光状态，类似 Sage 选中治疗：E 收枪持球，左键=左拐/右键=右拐抛掷，1/2 打断）---

bool ABlasterCharacter::HasCurveballAbility() const
{
    return GetCurveballAbilityCDO() != nullptr;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetCurveballAbilityCDO() const
{
    for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
    {
        if (!AbilityClass) continue;
        const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
        if (CDO && CDO->SkillType == EBlasterSkillType::Flash)
        {
            return CDO;
        }
    }
    return nullptr;
}

bool ABlasterCharacter::IsCurveballAvailable() const
{
    const UBlasterGameplayAbility* Curveball = GetCurveballAbilityCDO();
    if (!Curveball || !Curveball->CooldownEffectClass || !AbilitySystemComponent) return false;
    const int32 ActiveCooldowns = AbilitySystemComponent->GetGameplayEffectCount(Curveball->CooldownEffectClass, nullptr);
    return ActiveCooldowns < Curveball->MaxCharges;
}

void ABlasterCharacter::CurveballPressed()
{
    if (bDisableGameplay) return;

    if (!bCurveballHolding)
    {
        // 第一段：进入持闪光状态（收枪；有充能才能进，进入本身不扣次数，1/2 可免费打断）
        if (!IsCurveballAvailable()) return;
        EnterCurveballHold();
    }
    else
    {
        // 持球态再按 E = 打断（掏回枪退出）
        ExitCurveballHoldLocal();
    }
}

void ABlasterCharacter::EnterCurveballHold()
{
    if (bCurveballHolding) return;

    // 先退出瞄准再收枪（SetAiming 内部要求有手持武器）
    if (bAiming && Combat) Combat->SetAiming(false);

    bCurveballHolding = true;

    // 本地预测收枪（服务器 RPC 兜底；收枪状态经 EquippedWeapon 复制给所有客户端）
    CurveballHolsteredWeapon = GetEquippedWeapon();
    if (Combat && CurveballHolsteredWeapon)
    {
        CurveballHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
        Combat->AttachActorToSocket(CurveballHolsteredWeapon, Combat->GetHolsterSocketForWeapon(CurveballHolsteredWeapon));
        SetEquippedWeapon(nullptr);
    }

    ServerSetCurveballHolding(true);
}

void ABlasterCharacter::ExitCurveballHoldLocal()
{
    if (!bCurveballHolding) return;
    bCurveballHolding = false;

    // 本地预测掏枪（服务器 RPC 兜底）
    if (Combat && CurveballHolsteredWeapon)
    {
        Combat->EquipSlotWeapon(CurveballHolsteredWeapon);
    }
    CurveballHolsteredWeapon = nullptr;

    ServerSetCurveballHolding(false);
}

void ABlasterCharacter::ThrowCurveball(bool bCurveLeft)
{
    if (!bCurveballHolding) return;

    // 抛掷方向 RPC（可靠 RPC 同通道有序，先于 CallServerTryActivateAbility 到达 → 服务器 spawn 时能读到）
    ServerSetPendingCurveballSide(bCurveLeft);

    if (AbilitySystemComponent)
    {
        FGameplayTagContainer TagContainer;
        TagContainer.AddTag(BlasterGameplayTags::Ability_Phoenix_Curveball);
        AbilitySystemComponent->TryActivateAbilitiesByTag(TagContainer);
    }

    // 本地立即退出持球态（服务器掏枪经复制跟上；若充能已被打空，GA 不激活、只是回归可射击）
    ExitCurveballHoldLocal();
}

void ABlasterCharacter::ServerSetCurveballHolding_Implementation(bool bHolding)
{
    if (bHolding)
    {
        if (bCurveballHolding) return;
        bCurveballHolding = true;
        bReplicatedCurveballHolding = true; // 镜像给观战者技能条高亮 Flash
        // 服务器权威收枪：记录 + 挂回闲置 socket
        CurveballHolsteredWeapon = GetEquippedWeapon();
        if (Combat && CurveballHolsteredWeapon)
        {
            CurveballHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
            Combat->AttachActorToSocket(CurveballHolsteredWeapon, Combat->GetHolsterSocketForWeapon(CurveballHolsteredWeapon));
            SetEquippedWeapon(nullptr);
        }
    }
    else
    {
        if (!bCurveballHolding) return;
        ServerExitCurveballHold();
    }
}

void ABlasterCharacter::ServerExitCurveballHold()
{
    bCurveballHolding = false;
    bReplicatedCurveballHolding = false;
    if (Combat && CurveballHolsteredWeapon)
    {
        Combat->EquipSlotWeapon(CurveballHolsteredWeapon);
    }
    CurveballHolsteredWeapon = nullptr;
}

void ABlasterCharacter::ServerSetPendingCurveballSide_Implementation(bool bCurveLeft)
{
    bHasPendingCurveballSide = true;
    bPendingCurveballLeft = bCurveLeft;
}

bool ABlasterCharacter::ConsumePendingCurveballSide(bool& OutCurveLeft)
{
    if (!bHasPendingCurveballSide) return false;
    OutCurveLeft = bPendingCurveballLeft;
    bHasPendingCurveballSide = false;
    bPendingCurveballLeft = false;
    return true;
}

// --- Sage 治疗（选中治疗）---

bool ABlasterCharacter::HasSageHealAbility() const
{
    return GetSageHealAbilityCDO() != nullptr;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetSageHealAbilityCDO() const
{
    for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
    {
        if (!AbilityClass) continue;
        const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
        if (CDO && CDO->SkillType == EBlasterSkillType::Heal)
        {
            return CDO;
        }
    }
    return nullptr;
}

bool ABlasterCharacter::IsSageHealAvailable() const
{
    const UBlasterGameplayAbility* Heal = GetSageHealAbilityCDO();
    if (!Heal || !Heal->CooldownEffectClass || !AbilitySystemComponent) return false;
    const int32 ActiveCooldowns = AbilitySystemComponent->GetGameplayEffectCount(Heal->CooldownEffectClass, nullptr);
    return ActiveCooldowns < Heal->MaxCharges;
}

bool ABlasterCharacter::IsValidSageHealTarget(const ABlasterCharacter* Other) const
{
    if (!Other || Other == this) return false;
    if (Other->IsElimmed()) return false;
    if (GetDistanceTo(Other) > SageHealRange) return false;
    // 测试机器人（无 PlayerState/队伍）也可作为治疗目标，方便单人测试
    if (Other->IsTestBot()) return true;
    const ABlasterPlayerState* MyPS = GetPlayerState<ABlasterPlayerState>();
    const ABlasterPlayerState* OtherPS = Other->GetPlayerState<ABlasterPlayerState>();
    if (!MyPS || !OtherPS) return false;
    return MyPS->Team == OtherPS->Team;
}

void ABlasterCharacter::SageHealPressed(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    if (!bSageHealSelecting)
    {
        // 第一段：进入选中状态（只有技能还有次数才能进；进入本身不扣次数）
        if (!IsSageHealAvailable()) return;
        EnterSageHealSelect();
    }
    else
    {
        // 选中态再按 E = 打断施法（掏回枪退出）；想再治疗重新按 E 进选中。
        // 实际治疗由选中态下的左键（FireStart）触发。
        ExitSageHealSelectLocal();
    }
}

void ABlasterCharacter::EnterSageHealSelect()
{
    if (bSageHealSelecting) return;

    // 先退出瞄准再收枪（SetAiming 内部要求有手持武器）
    if (bAiming && Combat) Combat->SetAiming(false);

    bSageHealSelecting = true;

    // 本地预测收枪（服务器 RPC 兜底，收枪状态经 EquippedWeapon 复制给所有客户端）
    SageHolsteredWeapon = GetEquippedWeapon();
    if (Combat && SageHolsteredWeapon)
    {
        SageHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
        Combat->AttachActorToSocket(SageHolsteredWeapon, Combat->GetHolsterSocketForWeapon(SageHolsteredWeapon));
        SetEquippedWeapon(nullptr);
    }

    ServerSetSageHealSelecting(true);
}

void ABlasterCharacter::ExitSageHealSelectLocal()
{
    if (!bSageHealSelecting) return;
    bSageHealSelecting = false;
    SageHealTarget = nullptr;

    // 本地预测掏枪（服务器 RPC 兜底）
    if (Combat && SageHolsteredWeapon)
    {
        Combat->EquipSlotWeapon(SageHolsteredWeapon);
    }
    SageHolsteredWeapon = nullptr;

    ServerSetSageHealSelecting(false);
}

void ABlasterCharacter::PerformSageHeal(ABlasterCharacter* Target)
{
    if (!Target || !bSageHealSelecting) return;

    // 服务器执行：校验 + 扣次数 + 回血 + 掏枪（Health/EquippedWeapon 复制自动同步）
    ServerSageHeal(Target);

    // 本地立即退出选中（服务器掏枪会经复制跟上）
    ExitSageHealSelectLocal();
}

void ABlasterCharacter::ServerSetSageHealSelecting_Implementation(bool bSelecting)
{
    if (bSelecting)
    {
        if (bSageHealSelecting) return;
        bSageHealSelecting = true;
        bReplicatedSageSelecting = true; // 镜像给观战者技能条高亮 Heal
        // 服务器权威收枪：记录 + 挂回闲置 socket
        SageHolsteredWeapon = GetEquippedWeapon();
        if (Combat && SageHolsteredWeapon)
        {
            SageHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
            Combat->AttachActorToSocket(SageHolsteredWeapon, Combat->GetHolsterSocketForWeapon(SageHolsteredWeapon));
            SetEquippedWeapon(nullptr);
        }
    }
    else
    {
        if (!bSageHealSelecting) return;
        ServerExitSageHealSelect();
    }
}

void ABlasterCharacter::ServerSageHeal_Implementation(ABlasterCharacter* Target)
{
    // 服务器权威校验（与本地 IsValidSageHealTarget 同规则，防止改客户端作弊）
    if (!Target) return;
    if (!bSageHealSelecting) return;
    if (!IsSageHealAvailable()) return;
    if (!IsValidSageHealTarget(Target)) return;

    // 扣掉本回合次数（无限时长 GE → 技能条变灰点；角色每回合重建自动重置）
    ApplySageHealUsed();

    // 治疗目标（Health 复制 + 目标客户端 OnRep 更新其 HUD）
    Target->HealByAbility(SageHealAmount);

    // 服务器退出选中 + 掏枪
    ServerExitSageHealSelect();
}

void ABlasterCharacter::ServerExitSageHealSelect()
{
    bSageHealSelecting = false;
    bReplicatedSageSelecting = false;
    if (Combat && SageHolsteredWeapon)
    {
        Combat->EquipSlotWeapon(SageHolsteredWeapon);
    }
    SageHolsteredWeapon = nullptr;
}

void ABlasterCharacter::ApplySageHealUsed()
{
    const UBlasterGameplayAbility* Heal = GetSageHealAbilityCDO();
    if (!Heal || !Heal->CooldownEffectClass || !AbilitySystemComponent) return;

    // UE5.4 的 ApplyGameplayEffectToSelf 传效果 CDO（不是 TSubclassOf）
    UGameplayEffect* EffectCDO = Heal->CooldownEffectClass->GetDefaultObject<UGameplayEffect>();
    if (!EffectCDO) return;

    FGameplayEffectContextHandle Ctx = AbilitySystemComponent->MakeEffectContext();
    Ctx.AddSourceObject(this);
    AbilitySystemComponent->ApplyGameplayEffectToSelf(EffectCDO, 1.f, Ctx);
}

void ABlasterCharacter::HealByAbility(float Amount)
{
    if (!HasAuthority() || bElimmed) return;
    const float OldHealth = Health;
    Health = FMath::Min(MaxHealth, Health + Amount);
    if (Health == OldHealth) return;
    // 服务器端（host）自己更新 HUD；远端客户端由 Health 复制 + OnRep_Health 更新
    UpdateHUDHealth();
}

void ABlasterCharacter::UpdateSageHealTarget()
{
    // 只有本地玩家在选中态才扫描；服务器不扫描（治疗目标由客户端 ServerSageHeal RPC 上报）
    if (!IsLocallyControlled() || !bSageHealSelecting)
    {
        SageHealTarget = nullptr;
        return;
    }

    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC)
    {
        SageHealTarget = nullptr;
        return;
    }

    // 本机视口中心 deproject（不复用 Combat::TraceUnderCrosshairs —— 它写死 GetPlayerController(this,0)，多开时对非 P0 不准）
    int32 ViewportSizeX = 0, ViewportSizeY = 0;
    PC->GetViewportSize(ViewportSizeX, ViewportSizeY);
    if (ViewportSizeX <= 0 || ViewportSizeY <= 0)
    {
        SageHealTarget = nullptr;
        return;
    }

    const FVector2D CrosshairLocation(ViewportSizeX * 0.5f, ViewportSizeY * 0.5f);
    FVector WorldPos, WorldDir;
    const bool bDeprojected = UGameplayStatics::DeprojectScreenToWorld(PC, CrosshairLocation, WorldPos, WorldDir);
    if (!bDeprojected)
    {
        SageHealTarget = nullptr;
        return;
    }

    // 起点从自身胶囊往前推一段，避免 trace 打到自己的胶囊/手（与武器 trace 同手法）
    FVector Start = WorldPos;
    const float DistanceToSelf = (GetActorLocation() - Start).Size();
    Start += WorldDir * (DistanceToSelf + 100.f);
    const FVector End = Start + WorldDir * SageHealRange;

    FHitResult Hit;
    FCollisionQueryParams Params;
    Params.AddIgnoredActor(this);
    if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
    {
        ABlasterCharacter* HitChar = Cast<ABlasterCharacter>(Hit.GetActor());
        if (HitChar && IsValidSageHealTarget(HitChar))
        {
            SageHealTarget = HitChar;
            return;
        }
    }
    SageHealTarget = nullptr;
}

void ABlasterCharacter::SetSkillArmed(bool bArmed)
{
    if (bSkillArmed == bArmed) return;
    bSkillArmed = bArmed;
    UpdateSkillArmedVisual();
}

void ABlasterCharacter::OnRep_SkillArmed()
{
    UpdateSkillArmedVisual();
}

void ABlasterCharacter::UpdateSkillArmedVisual()
{
    if (!SkillWindEffect) return;

    if (bSkillArmed)
    {
        if (SkillWindEffectAsset && SkillWindEffect->GetAsset() != SkillWindEffectAsset)
        {
            SkillWindEffect->SetAsset(SkillWindEffectAsset);
        }
        // bReset=true：每次武装重头播，效果一致
        SkillWindEffect->Activate(true);
    }
    else
    {
        SkillWindEffect->Deactivate();
    }
}

bool ABlasterCharacter::IsAnySkillArmed() const
{
    // 复制状态最可靠：武装/解除在服务器和预测客户端都会调 SetSkillArmed → bSkillArmed。
    // （预测失效时也会通过复制补上，约 1 帧延迟，本机无感。）
    if (bSkillArmed) return true;

    // 兜底：遍历能力实例。注意 Spec.Ability 是 CDO（bArmed 恒 false），必须用
    // GetPrimaryInstance() 拿真实实例——实例在首次激活时创建，未激活前返回 null（此时本就未武装）。
    if (!AbilitySystemComponent) return false;
    for (const FGameplayAbilitySpec& Spec : AbilitySystemComponent->GetActivatableAbilities())
    {
        UBlasterGameplayAbility* Ability = Spec.GetPrimaryInstance()
            ? Cast<UBlasterGameplayAbility>(Spec.GetPrimaryInstance())
            : Cast<UBlasterGameplayAbility>(Spec.Ability.Get());
        if (Ability && Ability->IsArmed())
        {
            return true;
        }
    }
    return false;
}

void ABlasterCharacter::ServerDrop_Implementation()
{
    ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();

    // 只丢当前手上的东西：spike 切出(bSpikeDrawn)时才丢 spike，否则丢手持武器
    if (bSpikeDrawn && CarriedSpike)
    {
        if (PS && PS->Team == ETeam::ET_TeamA)
        {
            CarriedSpike->Drop();
        }
        return;
    }

    // Equipped weapon
    if (Combat && EquippedWeapon)
    {
        Combat->DropEquippedWeapon();
    }
}

void ABlasterCharacter::ServerEquipSlot_Implementation(EWeaponSlot Slot)
{
    if (Combat) Combat->SwitchWeapon(Slot);
}

void ABlasterCharacter::ServerStartSpikePlant_Implementation()
{
    if (Combat) Combat->StartSpikePlant();
}

void ABlasterCharacter::ServerCancelSpikePlant_Implementation()
{
    if (Combat) Combat->CancelSpikePlant();
}

void ABlasterCharacter::ServerStartDefuse_Implementation()
{
    if (OverlappingSpike)
    {
        OverlappingSpike->StartDefuse(this);
    }
}

void ABlasterCharacter::ServerCancelDefuse_Implementation()
{
    if (OverlappingSpike)
    {
        OverlappingSpike->CancelAction();
    }
}

void ABlasterCharacter::SelectPrimary(const FInputActionValue& Value)
{
    if (bDisableGameplay || !Combat) return;

    // 持闪光/治疗选中态：1/2 = 打断并掏枪（恢复正常可射击），不切武器
    if (bCurveballHolding)
    {
        ExitCurveballHoldLocal();
        return;
    }
    if (bSageHealSelecting)
    {
        ExitSageHealSelectLocal();
        return;
    }

    if (HasAuthority()) Combat->SwitchWeapon(EWeaponSlot::ESlot_Primary);
    else ServerEquipSlot(EWeaponSlot::ESlot_Primary);
}

void ABlasterCharacter::SelectSecondary(const FInputActionValue& Value)
{
    if (bDisableGameplay || !Combat) return;

    // 持闪光/治疗选中态：1/2 = 打断并掏枪（恢复正常可射击），不切武器
    if (bCurveballHolding)
    {
        ExitCurveballHoldLocal();
        return;
    }
    if (bSageHealSelecting)
    {
        ExitSageHealSelectLocal();
        return;
    }

    if (HasAuthority()) Combat->SwitchWeapon(EWeaponSlot::ESlot_Secondary);
    else ServerEquipSlot(EWeaponSlot::ESlot_Secondary);
}

void ABlasterCharacter::SelectMelee(const FInputActionValue& Value)
{
    // 预留：暂无近战武器
}

void ABlasterCharacter::SpikePressed(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bCurveballHolding || !Combat) return;
    bSpikeHoldThresholdReached = false;
    // 匪家携带 spike → 切出到手上；警方不携带，只准备拆包计时
    if (bCarryingSpike)
    {
        if (HasAuthority()) Combat->DrawSpike();
        else ServerEquipSlot(EWeaponSlot::ESlot_Spike);
    }
    GetWorldTimerManager().SetTimer(SpikeHoldTimer, this, &ABlasterCharacter::SpikeHoldTimerFinished, SpikeHoldThreshold);
}

void ABlasterCharacter::SpikeHoldTimerFinished()
{
    bSpikeHoldThresholdReached = true;
    if (bCarryingSpike)
    {
        // 匪家安包
        if (HasAuthority()) Combat->StartSpikePlant();
        else ServerStartSpikePlant();
    }
    else
    {
        // 警方拆包（服务器校验是否重叠已安放的 spike）
        if (HasAuthority())
        {
            if (OverlappingSpike) OverlappingSpike->StartDefuse(this);
        }
        else ServerStartDefuse();
    }
}

void ABlasterCharacter::SpikeReleased(const FInputActionValue& Value)
{
    GetWorldTimerManager().ClearTimer(SpikeHoldTimer);
    if (bSageHealSelecting || bCurveballHolding) return;
    if (bSpikeHoldThresholdReached)
    {
        if (bCarryingSpike)
        {
            if (HasAuthority()) Combat->CancelSpikePlant();
            else ServerCancelSpikePlant();
        }
        else
        {
            if (HasAuthority())
            {
                if (OverlappingSpike) OverlappingSpike->CancelAction();
            }
            else ServerCancelDefuse();
        }
    }
    bSpikeHoldThresholdReached = false;
}

void ABlasterCharacter::DropCarriedSpike()
{
    if (CarriedSpike)
    {
        CarriedSpike->Drop();
        CarriedSpike = nullptr;
        bCarryingSpike = false;
        bSpikeDrawn = false;
    }
}

bool ABlasterCharacter::IsInPlantZone() const
{
	// 服务器判定：查询所有 APlantZone 多边形（任意形状），角色位置点内即算在区域内
	return APlantZone::IsPointInAnyZone(this, GetActorLocation());
}

void ABlasterCharacter::SetEquippedWeapon(AWeapon* WeaponToEquip)
{
    EquippedWeapon = WeaponToEquip;
}

void ABlasterCharacter::SetSpikeDrawn(bool bDrawn)
{
    bSpikeDrawn = bDrawn;
}

void ABlasterCharacter::OnRep_CarriedAmmo()
{
    BlasterPlayerController = BlasterPlayerController == nullptr? Cast<ABlasterPlayerController>(GetController()) : BlasterPlayerController;
    if (BlasterPlayerController)
    {
        BlasterPlayerController->SetHUDCarriedAmmo(GetCarriedAmmo());
    }

    bool bJumpToShotgunEnd = CombatState == ECombatState::ECS_Reloading &&
        EquippedWeapon!= nullptr&&
            EquippedWeapon->GetWeaponType() == EWeaponType::EWT_Shotgun&&
                CarriedAmmo == 0;
    if (bJumpToShotgunEnd)
    {
        Combat->JumpToShotGunEnd();
    }

}

void ABlasterCharacter::OnRep_EquipWeapon(AWeapon* LastWeapon)
{
    if (!Combat) return;

    if (EquippedWeapon)
    {
        EquippedWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
        Combat->AttachActorToSocket(EquippedWeapon, Combat->RightHandSocket);
        GetCharacterMovement()->bOrientRotationToMovement = false;
        bUseControllerRotationYaw = true;
    }
    if (EquippedWeapon && LastWeapon && LastWeapon != EquippedWeapon)
    {
        // 切换武器：旧武器挂回闲置 socket（丢弃/切出 spike 由武器自身状态或 OnRep_SpikeDrawn 处理）
        LastWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
        Combat->AttachActorToSocket(LastWeapon, Combat->GetHolsterSocketForWeapon(LastWeapon));
    }
}

void ABlasterCharacter::OnRep_SpikeDrawn()
{
    if (!Combat) return;

    if (bSpikeDrawn)
    {
        // 切出 spike：把当前手持枪挂回闲置（spike 挂手由 attachment 复制完成）
        if (EquippedWeapon)
        {
            EquippedWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
            Combat->AttachActorToSocket(EquippedWeapon, Combat->GetHolsterSocketForWeapon(EquippedWeapon));
        }
    }
}

void ABlasterCharacter::ServerAO_Yaw_Implementation(float Yaw)
{

    ReplicatedAO_Yaw = Yaw;
}

void ABlasterCharacter::OnRep_ReplicatedAO_Yaw()
{

}

void ABlasterCharacter::TurnInPlace(float DeltaTime)
{
    if (AO_Yaw > 90.f)
    {
        TurningInPlace = ETurningInPlace::ETIP_Right;

    }
    else if (AO_Yaw < -90.f)
    {
        TurningInPlace = ETurningInPlace::ETIP_Left;
    }
    if (TurningInPlace != ETurningInPlace::ETIP_NotTurning)
    {
        AO_Rotation = FMath::RInterpConstantTo(AO_Rotation, InterpAO_Yaw+FRotator(0.f,AO_Yaw,0.f), DeltaTime, 200.f);

        if (FMath::Abs(FMath::FindDeltaAngleDegrees(AO_Rotation.Yaw, (InterpAO_Yaw + FRotator(0.f, AO_Yaw, 0.f)).Yaw))< 5.f)
        {
            UE_LOG(LogTemp, Warning, TEXT("Turn"));
            AO_Yaw = 0;
            TurningInPlace = ETurningInPlace::ETIP_NotTurning;
            StartingAimRotation = FRotator(0.f, GetBaseAimRotation().Yaw, 0.f);

        }
    }
}

void ABlasterCharacter::ServerUseControllerYaw_Implementation(bool bUse)
{
    bUseControllerRotationYaw = bUse;
}

void ABlasterCharacter::AddRecoil(float Pitch, float Yaw, float MaxPitch)
{
    if (!IsLocallyControlled() || Pitch <= 0.f) return;
    // 累加并限幅：连发会小幅累积（上限来自当前武器，如步枪 5.5°），松手后由回稳拉回
    RecoilPitchOffset = FMath::Min(RecoilPitchOffset + Pitch, MaxPitch);
    RecoilYawOffset = FMath::Clamp(RecoilYawOffset + Yaw, -MaxPitch, MaxPitch);
}

void ABlasterCharacter::UpdateRecoil(float DeltaTime)
{
    if (!IsLocallyControlled() || !FollowCamera) return;

    // 回稳速度按当前手持武器取（手枪利落回正、狙击慢沉），没枪时用默认值
    float RecoverySpeed = 8.f;
    if (AWeapon* Equipped = GetEquippedWeapon())
    {
        RecoverySpeed = Equipped->GetRecoilRecoverySpeed();
    }

    // 后坐力 = 相机本地俯仰偏移 + 水平晃动偏移（不碰控制旋转，和鼠标视角零冲突）。
    // 每帧把偏移应用到相机，同时向 0 回稳。准星追踪屏中点 → 开火瞬间准星上抬，
    // 弹道跟着准星走（真实的"枪口上抬影响命中"），随后镜头自动回正。
    FollowCamera->SetRelativeRotation(FRotator(RecoilPitchOffset, RecoilYawOffset, 0.f));
    RecoilPitchOffset = FMath::FInterpTo(RecoilPitchOffset, 0.f, DeltaTime, RecoverySpeed);
    RecoilYawOffset = FMath::FInterpTo(RecoilYawOffset, 0.f, DeltaTime, RecoverySpeed);
}

void ABlasterCharacter::HideCameraIfCharacterClose()
{
    if (!IsLocallyControlled())return;
    if ((FollowCamera ->GetComponentLocation()-GetActorLocation()).Size() < CameraThreshold)
    {
        GetMesh()->SetVisibility(false);
        if (Combat && EquippedWeapon && EquippedWeapon->GetWeaponMesh())
        {
            EquippedWeapon->GetWeaponMesh()->bOwnerNoSee=true;
        }
    }
    else
    {
        GetMesh()->SetVisibility(true);
        if (Combat && EquippedWeapon && EquippedWeapon->GetWeaponMesh())
        {
            EquippedWeapon->GetWeaponMesh()->bOwnerNoSee=false;
        }
    }
}

void ABlasterCharacter::OnRep_Health()
{
    UpdateHUDHealth();
    PlayHitReactMontage();
}

void ABlasterCharacter::OnRep_Elim()
{
    if (bElimmed)
    {
        PlayElimMontage();
        if (BlasterPlayerController)
        {
            BlasterPlayerController->SetHUDWeaponAmmo(0);
        }
        //Start dissolve effect
        if (DissolveMaterialInstance)
        {
            DynamicDissolveMaterialInstance=UMaterialInstanceDynamic::Create(DissolveMaterialInstance,this);

            GetMesh()->SetMaterial(0,DynamicDissolveMaterialInstance);
            DynamicDissolveMaterialInstance->SetScalarParameterValue(TEXT("Dissolve"),0.55f);
            DynamicDissolveMaterialInstance->SetScalarParameterValue(TEXT("Glow"),200.f);

        }
        StartDissolve();

        //Disable character movement
        GetCharacterMovement()->DisableMovement();
        GetCharacterMovement()->StopMovementImmediately();
        if (BlasterPlayerController)
        {
            DisableInput(BlasterPlayerController);
        }

        //Disable Collision
        GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

        //Spawn elim bot
        if (ElimBotEffect)
        {
            FVector ElimVotSpawnPoint(GetActorLocation().X,GetActorLocation().Y,GetActorLocation().Z+200.f);
            ElimBotComponent=UGameplayStatics::SpawnEmitterAtLocation(GetWorld(),ElimBotEffect,ElimVotSpawnPoint,GetActorRotation());
        }
        bool bHideSniperScope = IsLocallyControlled() && Combat && bAiming && EquippedWeapon && EquippedWeapon->GetWeaponType()==EWeaponType::EWT_SniperRifle;
        if (bHideSniperScope)
        {
            ShowSniperScopeWidget(false);
        }

        // 本机被击杀：通知 PC 延迟一小段死亡镜头后进观战（观察存活队友）
        if (IsLocallyControlled())
        {
            if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
            {
                BPC->HandleLocalPlayerEliminated();
            }
        }
    }
}


void ABlasterCharacter::UpdateHUDHealth()
{
    BlasterPlayerController = BlasterPlayerController==nullptr ? Cast<ABlasterPlayerController>(GetController()):BlasterPlayerController;
    if (BlasterPlayerController)
    {
        BlasterPlayerController->SetHUDHealth(Health,MaxHealth);
    }
}

void ABlasterCharacter::PollInit()
{
    if (BlasterPlayerState == nullptr)
    {
        BlasterPlayerState = GetPlayerState<ABlasterPlayerState>();
        if (BlasterPlayerState)
        {
            BlasterPlayerState->AddToScore(0.f);
            BlasterPlayerState->AddToDefeats(0);
        }
    }
}


void ABlasterCharacter::SetOverlappingWeapon(AWeapon* weapon)
{
    if (OverlappingWeapon)
    {
        OverlappingWeapon->ShowPickupWidget(false);
    }
    OverlappingWeapon = weapon;
    if (IsLocallyControlled())
    {
        if (OverlappingWeapon)
        {
            OverlappingWeapon->ShowPickupWidget(true);
        }
    }
}

void ABlasterCharacter::SetCarriedAmmo(int32 Ammo)
{
    CarriedAmmo = Ammo;
}

void ABlasterCharacter::SetCombatState(ECombatState CombatStateTemp)
{
    CombatState = CombatStateTemp;
}


bool ABlasterCharacter::IsWeaponEquipped()
{
    return EquippedWeapon!=nullptr;
}

bool ABlasterCharacter::IsAiming()
{
    return bAiming;
}

AWeapon* ABlasterCharacter::GetEquippedWeapon()
{
    if (!Combat)return nullptr;
    return EquippedWeapon;
}

FVector ABlasterCharacter::GetHitTarget() const
{
    if (!Combat)return FVector(1,1,1);
    return Combat->HitTarget.ImpactPoint;
}

ECombatState ABlasterCharacter::GetCombatState()
{
    if (Combat == nullptr)return ECombatState::ECS_MAX;
    return CombatState;
}


void ABlasterCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (UEnhancedInputComponent* EnhancedInputComponent = CastChecked<UEnhancedInputComponent>(PlayerInputComponent))
    {

        if (MoveAction)
        {
            EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ABlasterCharacter::Move);
        }
        if (LookAction)
        {
            EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ABlasterCharacter::Look);
        }
        if (JumpAction)
        {
            EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ABlasterCharacter::Jump);
        }
        if (EquipButtonAction)
        {
            EnhancedInputComponent->BindAction(EquipButtonAction, ETriggerEvent::Started, this, &ABlasterCharacter::Equip);
        }
        if (PlayerInputComponent)
        {
            PlayerInputComponent->BindKey(EKeys::F10, IE_Pressed, this, &ABlasterCharacter::DebugNetworkState);
            PlayerInputComponent->BindKey(EKeys::F9, IE_Pressed, this, &ABlasterCharacter::ToggleAgentPressed);
        }
        if (CrouchAction)
        {
            EnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Started, this, &ABlasterCharacter::BlasterCrouch);
        }
        if (AimAction)
        {
            EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &ABlasterCharacter::AimStart);
        }
        if (AimAction)
        {
            EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Completed, this, &ABlasterCharacter::AimEnd);
        }
        if (FireAction)
        {
            EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &ABlasterCharacter::FireStart);
        }
        if (FireAction)
        {
            EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &ABlasterCharacter::FireEnd);
        }
        if (ReloadAction)
        {
            EnhancedInputComponent->BindAction(ReloadAction, ETriggerEvent::Started, this, &ABlasterCharacter::Reload);
        }
        if (ThrowGrenadeAction)
        {
            EnhancedInputComponent->BindAction(ThrowGrenadeAction, ETriggerEvent::Started, this, &ABlasterCharacter::ThrowGrenade);
        }
        if (InteractAction)
        {
            EnhancedInputComponent->BindAction(InteractAction, ETriggerEvent::Started, this, &ABlasterCharacter::Interact);
            EnhancedInputComponent->BindAction(InteractAction, ETriggerEvent::Completed, this, &ABlasterCharacter::InteractCancel);
        }
        if (DropAction)
        {
            EnhancedInputComponent->BindAction(DropAction, ETriggerEvent::Started, this, &ABlasterCharacter::Drop);
        }
        if (DashAction)
        {
            EnhancedInputComponent->BindAction(DashAction, ETriggerEvent::Started, this, &ABlasterCharacter::Dash);
        }
        if (PrimarySlotAction)
        {
            EnhancedInputComponent->BindAction(PrimarySlotAction, ETriggerEvent::Started, this, &ABlasterCharacter::SelectPrimary);
        }
        if (SecondarySlotAction)
        {
            EnhancedInputComponent->BindAction(SecondarySlotAction, ETriggerEvent::Started, this, &ABlasterCharacter::SelectSecondary);
        }
        if (MeleeSlotAction)
        {
            EnhancedInputComponent->BindAction(MeleeSlotAction, ETriggerEvent::Started, this, &ABlasterCharacter::SelectMelee);
        }
        if (SpikeSlotAction)
        {
            EnhancedInputComponent->BindAction(SpikeSlotAction, ETriggerEvent::Started, this, &ABlasterCharacter::SpikePressed);
            EnhancedInputComponent->BindAction(SpikeSlotAction, ETriggerEvent::Completed, this, &ABlasterCharacter::SpikeReleased);
        }
        if (BuyAction)
        {
            EnhancedInputComponent->BindAction(BuyAction, ETriggerEvent::Started, this, &ABlasterCharacter::Buy);
        }
    }

}

void ABlasterCharacter::DebugNetworkState()
{
    UE_LOG(LogTemp, Warning, TEXT("=== NETWORK DEBUG INFO ==="));
    UE_LOG(LogTemp, Warning, TEXT("LocalRole: %d"), GetLocalRole());
    UE_LOG(LogTemp, Warning, TEXT("RemoteRole: %d"), GetRemoteRole());
    UE_LOG(LogTemp, Warning, TEXT("HasAuthority: %d"), HasAuthority());

    if (Combat)
    {
        UE_LOG(LogTemp, Warning, TEXT("PrimaryWeapon: %s"),
            Combat->PrimaryWeapon!=nullptr ? *Combat->PrimaryWeapon->GetName() : TEXT("None"));
        UE_LOG(LogTemp, Warning, TEXT("SecondaryWeapon: %s"),
            Combat->SecondaryWeapon!=nullptr ? *Combat->SecondaryWeapon->GetName() : TEXT("None"));
        UE_LOG(LogTemp, Warning, TEXT("EquippedWeapon: %s"),
            EquippedWeapon!=nullptr ? *EquippedWeapon->GetName() : TEXT("None"));
    }

    UE_LOG(LogTemp, Warning, TEXT("OverlappingWeapon: %s"),
        OverlappingWeapon ? *OverlappingWeapon->GetName() : TEXT("None"));


    UE_LOG(LogTemp, Warning, TEXT("%s =========================="), *GetName());
}

void ABlasterCharacter::ToggleAgentPressed()
{
    if (bDisableGameplay) return;
    if (ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(GetController()))
    {
        PC->ServerToggleAgent();
    }
}

void ABlasterCharacter::ServerReceiveFlashBlind(const FVector& Origin, float Duration)
{
    // 只在服务器写（观战者/队友客户端读复制值；拥有者自己仍靠 GE 复制 + ASC 读剩余）。
    if (!HasAuthority() || Duration <= 0.f) return;

    const float EndServerTime = GetWorld() ? (float)GetWorld()->GetTimeSeconds() + Duration : 0.f;
    if (EndServerTime <= ReplicatedBlindServerEndTime)
    {
        return; // 已有更晚的被闪结束时间，保持旧的（来源爆炸点也保留）
    }
    ReplicatedBlindServerEndTime = EndServerTime;
    ReplicatedBlindOrigin = Origin;
}

void ABlasterCharacter::RefreshReplicatedSkills()
{
    if (!AbilitySystemComponent) return;

    // 观战技能条数据源：服务器每帧用权威 ASC 重建。值与上帧一致 → 引擎不重发，零流量；
    // 只在冷却层到期 / 施法 / 武装切换等瞬间变化时复制出去一次。
    ReplicatedSkills.Reset();

    const float ServerNow = GetWorld() ? (float)GetWorld()->GetTimeSeconds() : 0.f;

    // 收集要显示在技能条的技能（镜像 SkillBarWidget::CollectVisibleSkills：实例优先，CDO 兜底）
    TArray<UBlasterGameplayAbility*> DisplayAbilities;
    for (const FGameplayAbilitySpec& Spec : AbilitySystemComponent->GetActivatableAbilities())
    {
        UBlasterGameplayAbility* Ability = Spec.GetPrimaryInstance()
            ? Cast<UBlasterGameplayAbility>(Spec.GetPrimaryInstance())
            : Cast<UBlasterGameplayAbility>(Spec.Ability.Get());
        if (Ability && Ability->bShowInSkillBar)
        {
            DisplayAbilities.Add(Ability);
        }
    }
    DisplayAbilities.Sort([](const UBlasterGameplayAbility& A, const UBlasterGameplayAbility& B)
    {
        const int32 AIdx = A.SkillSlotIndex == INDEX_NONE ? MAX_int32 : A.SkillSlotIndex;
        const int32 BIdx = B.SkillSlotIndex == INDEX_NONE ? MAX_int32 : B.SkillSlotIndex;
        return AIdx < BIdx;
    });

    for (UBlasterGameplayAbility* Ability : DisplayAbilities)
    {
        FBlasterReplicatedSkill Entry;
        Entry.bValid = 1;
        Entry.SkillType = Ability->SkillType;
        Entry.MaxCharges = FMath::Max(0, Ability->MaxCharges);

        const FBlasterAbilityCooldownInfo Info = Ability->GetCooldownInfo(AbilitySystemComponent);
        Entry.Charges = FMath::Clamp(Info.Charges, 0, Entry.MaxCharges);
        Entry.bCooldownValid = Info.bValid ? 1 : 0;
        Entry.CooldownDuration = Info.bValid ? Info.CooldownDuration : 0.f;

        // 有正在冷却的层：记录最早到期那层结束的绝对服务器时间 → 客户端用 GetServerTime 平滑倒数。
        // 量化到 0.1s：避免每帧 float 抖动让整块数组反复重发。
        // （无限时长冷却如 Sage 每回合一次：TimeUntilNextCharge=0 → 这里不写，行为与本地灰色点一致）
        if (Info.bValid && Entry.Charges < Entry.MaxCharges && Info.TimeUntilNextCharge > 0.05f)
        {
            Entry.NextChargeServerEndTime = FMath::RoundToFloat((ServerNow + Info.TimeUntilNextCharge) * 10.f) / 10.f;
        }

        // 武装（逐风）窗口：镜像能力实例的武装态 + 绝对结束时间
        if (Ability->IsArmed())
        {
            Entry.bArmed = 1;
            Entry.ArmedWindowDuration = Ability->ArmedWindowDuration;
            const float ArmedRemaining = Ability->GetArmedTimeRemaining();
            if (ArmedRemaining > 0.f)
            {
                Entry.ArmedServerEndTime = FMath::RoundToFloat((ServerNow + ArmedRemaining) * 10.f) / 10.f;
            }
        }

        ReplicatedSkills.Add(Entry);
    }
}
