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
#include "Blaster/BlasterComponent/LagCompensationComponent.h"
#include "Blaster/BlasterComponent/BlasterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Blaster/Blaster.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkeletalMesh.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Spike/Spike.h"
#include "Blaster/Spike/SpikeAnimSet.h"
#include "Blaster/Pickup/UltOrb.h"
#include "AnimNotify_EmptyHandFinished.h"
#include "Blaster/PlantZone/PlantZone.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "TimerManager.h"
// GetThrowGrenadeDuration 要读蒙太奇的 GetPlayLength()（定义在 UAnimSequenceBase 上），
// 头里那个 UAnimMontage* 只是前向声明，这里得要完整定义。
#include "Animation/AnimMontage.h"
// 空手那三条动画：基类型（GetPlayLength）和"按槽名现造动态蒙太奇"用的混合设置
//（FMontageBlendSettings / CreateSlotAnimationAsDynamicMontage_WithBlendSettings）
#include "Animation/AnimSequenceBase.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "Blaster/Weapon/WeaponTypes.h"
#include "Blaster/Weapon/JettKnives.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayAbilitySpec.h"
#include "GameplayTagContainer.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "Blaster/Abilities/BlasterGameplayTags.h"
#include "Engine/EngineTypes.h"
#include "EngineUtils.h"
#include "Blaster/HUD/OverheadWidget.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Animation/WidgetAnimation.h"
// FindFProperty / FObjectPropertyBase：按"类上的同名属性"去捞 UMG 动画要用
#include "UObject/UnrealType.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
// 手上那个占位火球的材质（LoadObject 拿 M_GlowSphere_01，见 UpdateHeldThrowableVisual）
#include "Materials/MaterialInterface.h"
#include "Blaster/Abilities/CloveSmoke.h"

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

    // —— 第一人称：手模挂在第三人称身体下，位置/朝向每帧由 UpdateFPRig() 直接写世界变换 ——
    // 挂 GetMesh() 是为了**对齐空间约定**：手模骨架和第三人称身体一样原点在脚底
    //（相机骨骼在参考姿态里约在脚底上方 149cm，正好是眼高），挂在同一层才不用手填高度。
    // 注意这只是"参照物"：手模的相对变换每帧都会被 UpdateFPRig 覆盖掉，
    // 第三人称身体的动画（root motion 之类）带不动它。
    FPArmsMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("FPArmsMesh"));
    FPArmsMesh->SetupAttachment(GetMesh());
    // 只本人看得见：别人的客户端看不到你的手臂（他们看的是你的第三人称身体）
    FPArmsMesh->SetOnlyOwnerSee(true);
    FPArmsMesh->bCastDynamicShadow = false;
    FPArmsMesh->CastShadow = false;
    FPArmsMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FPArmsMesh->SetGenerateOverlapEvents(false);
    // 手臂是纯装饰，不参与命中判定（命中走第三人称身体 + 服务器回溯）
    FPArmsMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);

    // 资源和动画类在这里给默认值 —— 两个都能在 BP_BlasterCharacter 里覆盖。
    // 动画类若报加载失败（ABP 被改名/删掉），手臂会停在参考姿态，不影响运行。
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> FPArmsMeshAsset(
        TEXT("/Game/ValorantAssets/Hands/FP_Wushu_S0_Mesh"));
    if (FPArmsMeshAsset.Succeeded())
    {
        FPArmsMesh->SetSkeletalMesh(FPArmsMeshAsset.Object);
    }
    static ConstructorHelpers::FClassFinder<UAnimInstance> FPArmsAnimClass(
        TEXT("/Game/Blueprints/Character/Animation/ABP_WushuFP"));
    if (FPArmsAnimClass.Succeeded())
    {
        FPArmsMesh->SetAnimInstanceClass(FPArmsAnimClass.Class);
    }

    /*
     * 第一人称的枪械副本。这里只建组件、挂在手模上（先随便挂，具体挂点由武器给）：
     * 显示哪套网格、挂哪根骨骼、要不要显示，全在 UpdateFPWeaponMesh() 里按当前武器刷新 ——
     * 构造期拿不到武器（枪是运行时才 Spawn/复制的），只能用默认值占位。
     *
     * 沿用手模那一套「只本人可见 + 纯装饰」的设置，理由一样：
     * 别人的客户端看不到你手上的枪副本（他们看的是你的第三人称身体 + 真枪），
     * 副本也不参与任何命中判定。
     */
    FPWeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("FPWeaponMesh"));
    FPWeaponMesh->SetupAttachment(FPArmsMesh);
    FPWeaponMesh->SetOnlyOwnerSee(true);
    FPWeaponMesh->bCastDynamicShadow = false;
    FPWeaponMesh->CastShadow = false;
    FPWeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FPWeaponMesh->SetGenerateOverlapEvents(false);
    FPWeaponMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
    // 默认不显示：装备到武器时由 UpdateFPWeaponMesh 打开（而且还得武器 bUseFPViewModel 打开）
    FPWeaponMesh->SetVisibility(false);

    // 相机挂在手模的 Camera 骨骼上，相对变换为零（位置/旋转）。
    // 零偏移就够用：子组件的世界变换 = 相对变换 × 骨骼世界变换，相对变换是单位阵时
    // 相机的位姿**就是**骨骼的位姿 —— 位置精确落在骨骼上，不用去猜骨架那套米制单位。
    //
    // 缩放这里必须补一个 0.01：骨骼那一级带着 FP 骨架根骨骼的 100 倍（瓦的米→厘米），
    // 不补的话相机的世界缩放会是 100（相机本身不在乎缩放，但留个正常值，
    // 免得以后往相机下挂任何东西时又踩一次"挂上去大 100 倍"的坑）。
    FPCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FPCamera"));
    FPCamera->SetupAttachment(FPArmsMesh, FPCameraBone);
    FPCamera->bUsePawnControlRotation = false;
    FPCamera->SetRelativeScale3D(FVector(0.01f));

    /*
     * 第一人称手上那个包的副本（按 4 掏出包时才显示）。
     *
     * 真包（ASpike）挂在第三人称身体的挂点上 —— 而本人看自己的身体是关掉的
     *（RefreshFPRig 里那句 GetMesh()->SetOwnerNoSee(true)），挂在上面的东西本人一样看不见
     *（OwnerNoSee 是渲染标志、不会传给子组件，也不会传给附着在上面的 actor）。
     * 所以本人手里这个必须另开一份副本，和 FPWeaponMesh / HeldFireballFP 完全同一个套路：
     * 只本人可见 + 纯装饰（不参与任何命中判定）。
     *
     * 网格资产**不在这里给**（也不在角色上另配一份）：运行时从真包上取，见 UpdateFPSpikeVisual。
     * 这里只建组件、先挂在手模上（具体挂点由 FPSpikeSocket 决定，那次运行时重挂才算数）。
     */
    FPSpikeMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("FPSpikeMesh"));
    FPSpikeMesh->SetupAttachment(FPArmsMesh, FPSpikeSocket);
    FPSpikeMesh->SetOnlyOwnerSee(true);
    FPSpikeMesh->bCastDynamicShadow = false;
    FPSpikeMesh->CastShadow = false;
    FPSpikeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FPSpikeMesh->SetGenerateOverlapEvents(false);
    FPSpikeMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
    // 默认不显示：掏出包时由 UpdateFPSpikeVisual 打开
    FPSpikeMesh->SetVisibility(false);

    // 第一人称：角色朝向跟视角走，不再跟移动方向走。
    // 原来那套（yaw 跟控制器关掉 + 朝移动方向转）是第三人称的转向模型，
    // 第一人称下留着会变成「视角和身体打架」。
    bUseControllerRotationYaw = true;
    GetCharacterMovement()->bOrientRotationToMovement = false;

    OverheadWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("OverheadWidget"));
    OverheadWidget->SetupAttachment(RootComponent);

    Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("CombatComponent"));
    Combat->SetIsReplicated(true);

    // 服务器回溯：记录本角色的历史位置/骨骼，服务器用「客户端开火那一刻」的位置重算命中
    LagCompensation = CreateDefaultSubobject<ULagCompensationComponent>(TEXT("LagCompensationComponent"));

    // GAS 技能组件：冷却/充能/网络全走它。挂角色 → 每回合角色重建时充能自然重置（符合 Valorant）
    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AbilitySystemComponent->SetIsReplicated(true);
    AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

    GetCharacterMovement()->NavAgentProps.bCanCrouch = true;
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
    GetMesh()->SetCollisionObjectType(ECC_SkeletalMesh);
    GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
    GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);
    // 回溯读取的是骨架骨骼位置：角色没被渲染（背对镜头 / 离线视角）时也要照常求值骨架，
    // 否则拿到的是过期姿态（默认 OnlyTickPoseWhenRendered 会跳过不可见角色的 pose 更新）
    GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

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

    /*
     * 持火球时手上那个占位火球（用户要求：火球先上占位视觉、逻辑优先）。
     *
     * 两个组件服务两类观众，缺一不可：
     *   · HeldFireballFP 挂手模 → 只本人可见（第一人称"我手上拿着火球"）
     *   · HeldFireballTP 挂第三人称身体 → 只别人可见（对手得看得见你要丢火球了）
     *
     * 两者的可见性标志必须显式设：本人的第三人称身体是用 OwnerNoSee 隐藏的，
     * 而 OwnerNoSee/OnlyOwnerSee 是**每个组件的渲染标志**、不会传给子组件 ——
     * 不设的话本人会在第一人称画面里看到一个飘在自己身上的球。
     *
     * 大小/材质不在这里定：挂点的局部缩放差着两个数量级（手模 0.01、骨架 100），
     * 构造函数阶段也读不到 BP 里改的值 —— 交给 UpdateHeldThrowableVisual 在第一次
     * 真正要用的时候按世界缩放配齐（见那里的注释）。
     */
    HeldFireballFP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeldFireballFP"));
    HeldFireballFP->SetupAttachment(FPArmsMesh, HeldFireballFPSocket);
    HeldFireballFP->SetOnlyOwnerSee(true);
    HeldFireballFP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeldFireballFP->SetGenerateOverlapEvents(false);
    HeldFireballFP->SetCanEverAffectNavigation(false);
    HeldFireballFP->bCastDynamicShadow = false;
    HeldFireballFP->CastShadow = false;
    HeldFireballFP->SetVisibility(false);

    HeldFireballTP = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeldFireballTP"));
    HeldFireballTP->SetupAttachment(GetMesh(), HeldFireballTPSocket);
    HeldFireballTP->SetOwnerNoSee(true);
    HeldFireballTP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeldFireballTP->SetGenerateOverlapEvents(false);
    HeldFireballTP->SetCanEverAffectNavigation(false);
    HeldFireballTP->bCastDynamicShadow = false;
    HeldFireballTP->CastShadow = false;
    HeldFireballTP->SetVisibility(false);

    // 占位网格：引擎自带球（半径 50）。发光材质在 UpdateHeldThrowableVisual 里给。
    static ConstructorHelpers::FObjectFinder<UStaticMesh> HeldFireballMeshAsset(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (HeldFireballMeshAsset.Succeeded())
    {
        HeldFireballFP->SetStaticMesh(HeldFireballMeshAsset.Object);
        HeldFireballTP->SetStaticMesh(HeldFireballMeshAsset.Object);
    }

    /*
     * 手上那颗球的特效（Niagara），和上面两个占位球一一对应：同样的挂点、同样的可见性规则。
     *
     * 挂点和占位球用**同一个** HeldFireballFPSocket / HeldFireballTPSocket —— 用户要的是
     * "socket 变量留给我"，一套变量管两套表现，改一处两边都跟着动。
     * （BP 里改的值同样要等 UpdateHeldThrowableVisual 那次运行时重挂才生效，理由见那里的注释。）
     *
     * bAutoActivate=false：先不激活。两件事都靠 BeginPlay 之后的信息 ——
     * 用户配的是哪个资产（构造函数读不到 BP 的值），以及"现在手里到底有没有东西"。
     */
    HeldFireballFX_FP = CreateDefaultSubobject<UNiagaraComponent>(TEXT("HeldFireballFX_FP"));
    HeldFireballFX_FP->SetupAttachment(FPArmsMesh, HeldFireballFPSocket);
    HeldFireballFX_FP->SetOnlyOwnerSee(true);
    HeldFireballFX_FP->SetAutoActivate(false);
    HeldFireballFX_FP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeldFireballFX_FP->bCastDynamicShadow = false;
    HeldFireballFX_FP->SetVisibility(false);

    HeldFireballFX_TP = CreateDefaultSubobject<UNiagaraComponent>(TEXT("HeldFireballFX_TP"));
    HeldFireballFX_TP->SetupAttachment(GetMesh(), HeldFireballTPSocket);
    HeldFireballFX_TP->SetOwnerNoSee(true);
    HeldFireballFX_TP->SetAutoActivate(false);
    HeldFireballFX_TP->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeldFireballFX_TP->bCastDynamicShadow = false;
    HeldFireballFX_TP->SetVisibility(false);
}

void ABlasterCharacter::RefreshFPRig()
{
    // 「这台机器上我是不是本人」是整条第一人称链子的**唯一开关**：
    //   相机归属（FPCamera / FollowCamera 二选一激活）、手模与副本的显隐和 tick、
    //   以及"本人的第三人称身体永远对自己隐藏"。
    // 所以它只写在这一处，别的函数不再各自去 SetVisibility / SetComponentTickEnabled。
    //
    // ⚠ 判据**不能只在 BeginPlay 里问一次**：pawn 的 BeginPlay 总是早于控制器就位
    //（服务器 RestartPlayer 是先 SpawnActor 再 Possess；客户端是先收到复制的 pawn 再收到控制器），
    // 那会儿连本机玩家都判成"不是本人"。一旦照那一刻的答案把 tick 关掉、又没人再打开，
    // 手模就永久停在参考姿势（动画实例不更新，UWushuFPAnimInstance 那几个变量全停在 CDO 默认值
    // —— 监听服务器上"服务器那台的 WeaponType 不对"就是这么来的）。
    //
    // 现在每帧都问，答案变了才重新铺一遍：稳态下每帧只是一个 bool 比较。
    // 换句话说，IsLocallyControlled() 什么时候才变成最终值都不重要了 —— 下一帧会自己纠正。
    const bool bLocal = IsLocallyControlled();
    if (bFPRigStateKnown && bLocal == bFPRigIsLocal)
    {
        return;
    }
    bFPRigStateKnown = true;
    bFPRigIsLocal = bLocal;

    // 相机：本人用手模骨骼上的 FPCamera，其余情况（别人看你、你观战别人）用第三人称弹簧臂。
    // 两台都激活的话引擎会按组件顺序取第一台，行为不可控 —— 所以永远只开一台。
    if (FPCamera)
    {
        FPCamera->SetActive(bLocal);
    }
    if (FollowCamera)
    {
        FollowCamera->SetActive(!bLocal);
    }

    if (FPArmsMesh)
    {
        FPArmsMesh->SetVisibility(bLocal);
        // 不是本人就不求值动画：省一次骨架求值 + 一次动画蓝图求值
        FPArmsMesh->SetComponentTickEnabled(bLocal);
    }
    // 副本的**显隐**不在这里定（还要看当前这把武器开没开副本、有没有网格，且远端机器上
    // 副本也不渲染）—— 那是 UpdateFPWeaponMesh 的活，这里只管 tick。
    if (FPWeaponMesh)
    {
        FPWeaponMesh->SetComponentTickEnabled(bLocal);
    }

    // 本人的第三人称身体永远隐藏。用 OwnerNoSee 而不是 SetVisibility ——
    // 后者会把别人看到的你一起隐藏。
    // 注意身体上的枪/尖刺/手雷是**独立 actor**，可见性不跟着走，各自处理
    //（枪由 UpdateFPWeaponMesh 管，见那里）。
    if (GetMesh())
    {
        GetMesh()->SetOwnerNoSee(bLocal);
    }

    // 身份变了 → 副本该不该显示也跟着变（它内部自己判是不是本人）。换武器时另有一处调用。
    UpdateFPWeaponMesh();
}

void ABlasterCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME_CONDITION(ABlasterCharacter, OverlappingWeapon,COND_OwnerOnly);
    DOREPLIFETIME(ABlasterCharacter, bSpikeDrawn);
    // 只有掏包的人自己要用它（第一人称那个副本取网格资产），所以不发给别人
    DOREPLIFETIME_CONDITION(ABlasterCharacter, CarriedSpike, COND_OwnerOnly);
    DOREPLIFETIME(ABlasterCharacter, EquippedWeapon);
    DOREPLIFETIME(ABlasterCharacter, bAiming);
    DOREPLIFETIME(ABlasterCharacter, ReplicatedAO_Yaw);
    DOREPLIFETIME(ABlasterCharacter, Health);
    DOREPLIFETIME(ABlasterCharacter, Armor);
    DOREPLIFETIME(ABlasterCharacter, CombatState);
    DOREPLIFETIME(ABlasterCharacter, bElimmed);
    DOREPLIFETIME(ABlasterCharacter, bElimmedHidden);
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
    DOREPLIFETIME(ABlasterCharacter, ReplicatedThrowableKind);
    // 空手蒙太奇那三条（远端机靠它们重播：他们那边不跑能力的表现逻辑）
    DOREPLIFETIME(ABlasterCharacter, EmptyHandMontageFP);
    DOREPLIFETIME(ABlasterCharacter, EmptyHandMontageUB);
    DOREPLIFETIME(ABlasterCharacter, EmptyHandMontageLB);
    // 这次空手跳哪个方向的分段（各端要跳到同一段，否则看别人放技能方向是乱的）
    DOREPLIFETIME(ABlasterCharacter, EmptyHandDirection);
    // 空手中的"再来一次"世代号（E 二段在 Q 的空手里放出去时靠它让远端机重播，见头文件）
    DOREPLIFETIME(ABlasterCharacter, EmptyHandRestartCount);
    // 逐风云（C）的"按住控云"标志。**不能 COND_SkipOwner**：射手本机正是要靠它
    //（而不是靠松手那一刻的本地调用）去跳手模那段的 Outro，见 OnRep_CloudburstHold。
    DOREPLIFETIME(ABlasterCharacter, bCloudburstHoldActive);
    // 大招生效状态：是哪一个 + 结束时间（HUD 倒计时 / 观战看队友）
    DOREPLIFETIME(ABlasterCharacter, ActiveUltimate);
    DOREPLIFETIME(ABlasterCharacter, UltimateEndTime);
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
    // 近战槽的武器不 Dropped() 而是**销毁** —— 上面那两个主副武器
    // 是掉在地上给别人捡的，近战（自带的刀 / 大招的飞刀）不该被别人捡走。
    //
    // 这一条不只是"收干净"：角色每回合结束都会被销毁重建（AGameMode::RespawnAllPlayers），
    // 而近战武器是挂在角色骨骼网格上的独立 actor。不销毁的话它会变成一个挂在已销毁角色上的
    // 孤儿 actor 永久留在世界里，每回合漏一个。
    //
    // bRestoreStashed=false：这时候**被大招顶掉收着的那把刀也一并销毁**。
    // 默认那个 true 是给"大招收招"用的（把刀放回槽位）—— 在这里放回去没人接管，
    // 一样是每回合漏一个。下一回合 RestorePlayerWeapons 会重新发一把。
    if (Combat)
    {
        Combat->DestroyMeleeWeapon(/*bRestoreStashed=*/false);
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

    // 注意：不要在这里（或 BeginPlay / OnRep_Controller）判"是不是本人"去开关第一人称那套 ——
    // 那些时机都可能是"控制器还没到位/马上要换人"，判错了会把状态锁死。
    // 统一交给 Tick 里的 RefreshFPRig()，它每帧问一次、答案变了才动手。

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

    // 头顶 id 牌上的文字。原来在 BP_BlasterCharacter 的 Event BeginPlay 里
    //（OverheadWidget->GetWidget() → Cast<UOverheadWidget> → ShowPlayerNetRole(self)），
    // 英雄 BP 的父类换成英雄 C++ 类之后就继承不到了，所以搬到这里。
    // BP_BlasterCharacter 那张图还留着并会继续跑 —— 同一个字符串被设两遍，看不出差别。
    if (OverheadWidget)
    {
        if (UOverheadWidget* NameplateWidget = Cast<UOverheadWidget>(OverheadWidget->GetWidget()))
        {
            NameplateWidget->ShowPlayerNetRole(this);
        }
    }

    // 第一人称那一套（相机归属 / 手模显隐 / 各组件 tick）不在这里初始化 ——
    // BeginPlay 时控制器还没到位，此刻判"是不是本人"一定是错的。
    // 交给 Tick 里的 RefreshFPRig()，它每帧重问一次，控制器一到就自己纠正。

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

    // 蹲下视角下移：先刷新"要降多少"（幅度），再推进"移到哪了"（过渡进度）。
    // 放在 UpdateFPRig 之前 —— 这一帧的眼位就要用它（UpdateFPRig 和 GetFPEyeWorldLocation
    // 都是问 GetFPCrouchDrop 要这个值）。
    //
    // 幅度：蹲着的时候胶囊一直是矮的，随时采都一样，所以每帧采一次；起身时胶囊长回站高、
    // 采出来是 0，这里**刻意不覆盖**上一次采到的值 —— 起身那半段过渡还要用 48 往下落，
    // 覆盖了的话视角会在起身第一帧就直接弹回站高（蹲下那半段有过渡、起身没有，很怪）。
    //
    // 推进用 FInterpConstantTo（匀速），不是 FInterpTo：后者是指数逼近，理论上永远到不了终点，
    // 尾巴拖很长，蹲下和起身的观感还会不一样。匀速正好 FPCrouchInterpTime 秒走完 0↔1。
    {
        const float LiveDelta = ComputeFPCapsuleCrouchDelta();
        if (LiveDelta > 0.f)
        {
            FPCrouchDropDistance = LiveDelta;
        }
        const float TargetAlpha = bIsCrouched ? 1.f : 0.f;
        FPCrouchDropAlpha = (FPCrouchInterpTime > 0.f)
            ? FMath::FInterpConstantTo(FPCrouchDropAlpha, TargetAlpha, DeltaTime, 1.f / FPCrouchInterpTime)
            : TargetAlpha;
    }

    // 第一人称 rig：先铺状态（相机归属/显隐/tick），再按状态摆位。
    // 顺序不能反 —— UpdateFPRig 靠 FPCamera 激没激活判断该不该干活。
    RefreshFPRig();
    UpdateFPRig(DeltaTime);

    // 掏在手上了没有 / 背的是哪个包 / 网格是哪一套 —— 三件事分别在三个地方翻转
    //（bSpikeDrawn 复制、CarriedSpike 复制、网格资产挂在 BP_Spike 上），
    // 靠 RepNotify 挂钩子得挂三处还会漏掉"复制到达顺序"这种边角，这里每帧全量重算一次。
    UpdateFPSpikeVisual();

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
    if (AnimInstance == nullptr) return;

    // 优先用这把武器自己的第三人称开火蒙太奇（逐武器差异化）。
    // 没配就退回角色上那条共用的（老行为：一条蒙太奇靠 section 分武器类型）。
    UAnimMontage* Montage = EquippedWeapon->GetThirdPersonFireMontage();
    FName SectionName;

    if (Montage != nullptr)
    {
        SectionName = bAiming ? EquippedWeapon->GetThirdPersonFireAimSection()
                              : EquippedWeapon->GetThirdPersonFireHipSection();
    }
    else
    {
        Montage = FireWeaponMontage;
        SectionName = bAiming ? FName("RifleAim") : FName("RifleHip");
    }

    if (Montage == nullptr) return;

    AnimInstance->Montage_Play(Montage);

    /*
     * 段名填 NAME_None（单段蒙太奇）就跳过 —— Montage_JumpToSection(NAME_None) 会失败。
     *
     * 段名在蒙太奇里不存在时引擎只是静默留在第 0 段、不会出错，但**会刷一条含糊的警告**，
     * 而这是有正常用法的：按次数分段播的武器（飞刀：段名 "1".."5"，不是 RifleAim/RifleHip）
     * 走的就是这条路，它靠后面的 JumpThirdPersonFireMontageToSection 跳段 ——
     * 每次开火都刷一条"找不到 RifleAim"纯属噪音，所以这里先查一遍再跳。
     */
    if (SectionName != NAME_None && Montage->IsValidSectionName(SectionName))
    {
        AnimInstance->Montage_JumpToSection(SectionName, Montage);
    }
}

void ABlasterCharacter::PlayEquipMontage()
{
    if (Combat == nullptr || EquippedWeapon == nullptr)return;

    // 这把武器自己的第三人称掏枪蒙太奇。留空就**什么都不播**：
    // 老版本这里本来就没有这条（切枪只有枪自己的 EquipAnimation + 手模那条），
    // 所以"留空"= 保持原样，不会因为这次改动让任何一把现有武器变样。
    UAnimMontage* Montage = EquippedWeapon->GetThirdPersonEquipMontage();
    if (Montage == nullptr) return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance == nullptr) return;

    AnimInstance->Montage_Play(Montage);
}

bool ABlasterCharacter::IsMontageCompatibleWithMesh(const UAnimSequenceBase* Asset,
                                                    const USkeletalMeshComponent* MeshComp)
{
    if (Asset == nullptr || MeshComp == nullptr) return true;

    const USkeleton* AssetSkeleton = Asset->GetSkeleton();
    const USkeletalMesh* MeshAsset = MeshComp->GetSkeletalMeshAsset();
    if (AssetSkeleton == nullptr || MeshAsset == nullptr) return true;

    return AssetSkeleton == MeshAsset->GetSkeleton();
}

FName ABlasterCharacter::ResolveAnimSlotName(const UAnimSequenceBase* SlotNameSource, FName Fallback)
{
    // 只有蒙太奇带槽轨；裸 AnimSequence 没有"这条动画走哪个槽"这个信息。
    const UAnimMontage* Montage = Cast<UAnimMontage>(SlotNameSource);
    if (Montage != nullptr && Montage->SlotAnimTracks.Num() > 0)
    {
        const FName SlotName = Montage->SlotAnimTracks[0].SlotName;
        if (!SlotName.IsNone()) return SlotName;
    }
    return Fallback;
}

float ABlasterCharacter::GetAnimRemainingTime(const USkeletalMeshComponent* MeshComp,
                                              const UAnimSequenceBase* Asset) const
{
    if (MeshComp == nullptr || Asset == nullptr) return 0.f;

    /*
     * 裸序列：按资产全长算。
     *
     * 它没有分段，走 PlayAnimAssetOnInstance 那条路只会被**整条**播（现造动态蒙太奇），
     * 所以"剩余"就是全长。而那条临时蒙太奇是 NewObject 出来的、事后拿不到指针，
     * 也就没法问"你现在播到哪了"—— 全长是这里唯一拿得到的准确值。
     */
    const UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
    if (Montage == nullptr) return Asset->GetPlayLength();

    const UAnimInstance* AnimInstance = MeshComp->GetAnimInstance();
    if (AnimInstance == nullptr) return 0.f;

    // 没在播（已经播完 / 被别的动作顶掉了 / 这条根本没起播）→ 不用等
    if (!AnimInstance->Montage_IsPlaying(Montage)) return 0.f;

    return FMath::Max(Montage->GetPlayLength() - AnimInstance->Montage_GetPosition(Montage), 0.f);
}

bool ABlasterCharacter::PlayAnimAssetOnInstance(UAnimSequenceBase* Asset, UAnimInstance* AnimInstance,
                                                FName DynamicMontageSlot, int32 DynamicMontageLoopCount,
                                                bool bStopAllMontages)
{
    if (Asset == nullptr || AnimInstance == nullptr) return false;

    /*
     * 蒙太奇：它自带槽名和通知，直接播。
     *
     * bStopAllMontages 默认 true：连发时每枪从第一帧重播，
     * 不会出现"第二枪接着上一枪的尾巴往下走"这种半截动作。
     */
    if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
    {
        return AnimInstance->Montage_Play(Montage, 1.f, EMontagePlayReturnType::MontageLength, 0.f,
                                          bStopAllMontages) > 0.f;
    }

    /*
     * 裸序列（AnimSequence）—— 按槽名现造一条**动态蒙太奇**再播。
     *
     * 为什么不直接 PlayAnimation / SetAnimation：单节点动画没有"槽"这个概念，会被动画机的结果
     * 整个覆盖掉，播了也看不见。为什么不用 UAnimInstance::PlaySlotAnimationAsDynamicMontage
     * 那几个现成封装：它们内部写死了 Montage_Play(...)（bStopAllMontages 走默认的 true）而混合
     * 时长是引擎默认的 0.25 秒 —— 和 PlayEmptyHandTrack 里那段说明是同一回事，这里要的是
     * **明确的 bStopAllMontages=true + 短混合**。
     *
     * ⚠ CreateSlotAnimationAsDynamicMontage_WithBlendSettings 传蒙太奇进去会被它自己拒掉
     *（"If Montage, please use Montage_Play"），所以上面那个分支是必须的，不是优化。
     *
     * 现造的蒙太奇是 NewObject 出来的临时对象（Outer = transient package），不落盘：
     * 播放期间由 FAnimMontageInstance 持有引用，停了之后自然被 GC 掉。
     * ⚠ 引擎派发通知时读的是**序列自己**的通知列表（FAnimMontageInstance::HandleEvents），
     *   所以挂在裸序列上的动画通知照样会响。
     */
    if (DynamicMontageSlot.IsNone()) return false;

    // 混合 0.1 秒。短一点是刻意的 —— 走这条路的都是"一按下就得出动作"的即时反馈
    //（右键全扔 / 攻击），留 0.25 秒（引擎默认）会肉。
    const FMontageBlendSettings BlendIn(0.1f);
    const FMontageBlendSettings BlendOut(0.1f);
    UAnimMontage* Dynamic = UAnimMontage::CreateSlotAnimationAsDynamicMontage_WithBlendSettings(
        Asset, DynamicMontageSlot, BlendIn, BlendOut, 1.f, FMath::Max(DynamicMontageLoopCount, 1),
        /*InBlendOutTriggerTime=*/-1.f);
    if (Dynamic == nullptr) return false;

    /*
     * 这一行日志是**刻意留的**：序列走槽位播是"配错了就静默不出画"的那类问题
     *（槽名和动画图里 Slot 节点的名字对不上一声不吭），出问题时有这一行就能直接比对：
     * 日志里的槽名 vs 动画蓝图里那个 Slot 节点的名字。每次右键一行，量很小。
     */
    UE_LOG(LogTemp, Log,
        TEXT("[动画] %s：%s 是裸序列 → 按槽 \"%s\" 现造一条动态蒙太奇播放")
        TEXT("（槽名必须和该网格动画图里那个 Slot 节点的名字一致，否则什么都不出画且不报错）。"),
        *GetNameSafe(AnimInstance->GetOwningComponent()), *Asset->GetName(), *DynamicMontageSlot.ToString());

    // bStopAllMontages = true：这条要**顶掉**刚播起来的那条（比如 UCombatComponent 在
    // weapon->Fire() 之前播的身体开火蒙太奇），不能两条叠着。
    // 传 false 的唯一用途是"同一个身体上两条不同名的槽要同时播"（下包/拆包的上半身 + 下半身，
    // 见 PlayPlantStartPair）：那种情况下第二条必须留住第一条。
    if (AnimInstance->Montage_Play(Dynamic, 1.f, EMontagePlayReturnType::MontageLength, 0.f,
                                   bStopAllMontages) <= 0.f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[动画] %s 上的动态蒙太奇（%s，槽 \"%s\"）播不起来 —— 这条动作不会出画。"),
            *GetNameSafe(AnimInstance->GetOwningComponent()), *Asset->GetName(),
            *DynamicMontageSlot.ToString());
        return false;
    }

    return true;
}

void ABlasterCharacter::PlayFPArmsMontage(UAnimSequenceBase* Montage,
                                          UAnimSequenceBase* SlotNameSource,
                                          FName FallbackSlotName,
                                          int32 DynamicMontageLoopCount)
{
    if (Montage == nullptr) return;

    // 只有本地玩家这台机器播：远端角色的手模既不渲染、组件 tick 也关着
    //（连骨架求值都不走，见 RefreshFPRig），往那儿播等于白发一条指令。
    if (!IsLocallyControlled()) return;
    if (FPArmsMesh == nullptr) return;

    /*
     * ⚠⚠ **骨架对不上就绝对不能播**，这一条是踩出来的，别删。
     *
     * 引擎的蒙太奇是按**骨名**把轨道绑到目标骨架上的：名字对得上的骨会被照样改写，
     * 对不上的骨静默跳过（只在 log 里留一行很含糊的 warning）。而手模骨架
     *（FP_Wushu_S0_Skeleton）和刀骨架（AB_Wushu_S0_X_Skeleton）**都有叫 Skeleton 和 Root 的骨**，
     * 又都是相机那条链的祖先 —— 于是"把刀的蒙太奇填进 FPFireMontage"这种手滑，
     * 后果是手模的 Skeleton / Root 被改写成刀骨架的值：
     *
     *     手模 Skeleton(骨内 P90/Y-135/R0) + Root(P0/Y-90/R-90) → 合起来是单位阵
     *     刀   的同两骨                                      → 合起来是 P0/Y-90/R-90
     *     两者差 **120°**
     *
     * 而 FPCamera 就挂在手模 Camera 骨（链末端）上，UpdateFPRig 每帧只重写**手模组件自己的**
     * 世界变换、并不补偿骨内的旋转 —— 结果就是**整个第一人称视角被拧转 120°**，
     * 且因为"绑定失败"是静默的，画面上的表现（视角歪了）和配置错误（填错蒙太奇）
     * 之间没有任何线索能对上。这一句把静默变响。
     *
     * 判定放行的情况见 IsMontageCompatibleWithMesh：任一侧取不到骨架就照播。
     */
    if (!IsMontageCompatibleWithMesh(Montage, FPArmsMesh))
    {
        const USkeletalMesh* ArmsAsset = FPArmsMesh->GetSkeletalMeshAsset();
        UE_LOG(LogTemp, Warning,
            TEXT("[手模] %s 的动画 %s 不是手模骨架的动画（手模骨架 = %s，动画骨架 = %s）→ 这一条不播。")
            TEXT("填错了会静默改写手模 Skeleton/Root 两根同名骨（差 120°），把整个第一人称视角拧掉 ——")
            TEXT("手模那几条（FPFireMontage / FPEquipMontage / FPReloadMontage / FPInspectMontage /")
            TEXT("FPRightClickMontage）必须用**手模骨架**（FP_Wushu_S0_*）的动画，")
            TEXT("刀骨架的 AB_Wushu_S0_X_* 是给 KnifeRigMesh 用的（AJettCharacter::KnifeAttack）。"),
            *GetName(),
            *Montage->GetName(),
            ArmsAsset && ArmsAsset->GetSkeleton() ? *ArmsAsset->GetSkeleton()->GetName() : TEXT("<无>"),
            Montage->GetSkeleton() ? *Montage->GetSkeleton()->GetName() : TEXT("<无>"));
        return;
    }

    // 注意是**手模自己的**动画实例（ABP_WushuFP），不是 GetMesh() 那个身体动画机 ——
    // 两者是两套独立骨架、两个独立 AnimInstance，播错了画面上什么都不会发生。
    UAnimInstance* AnimInstance = FPArmsMesh->GetAnimInstance();
    if (AnimInstance == nullptr) return;

    PlayAnimAssetOnInstance(Montage, AnimInstance,
                            ResolveAnimSlotName(SlotNameSource, FallbackSlotName),
                            DynamicMontageLoopCount);
}

bool ABlasterCharacter::PlayThirdPersonMontage(UAnimSequenceBase* Asset,
                                               UAnimSequenceBase* SlotNameSource,
                                               FName FallbackSlotName,
                                               int32 DynamicMontageLoopCount,
                                               bool bStopAllMontages)
{
    if (Asset == nullptr) return false;

    // 和手模那条不同：**不判本地控制**。身体是所有人眼里的你，每台机器都要播
    //（本地预测一遍、多播一遍，各自播各自的）。
    USkeletalMeshComponent* BodyMesh = GetMesh();
    if (BodyMesh == nullptr) return false;

    if (!IsMontageCompatibleWithMesh(Asset, BodyMesh))
    {
        // 身体这条填错骨架不会像手模那样拧视角（没有相机挂在这条链上），
        // 表现只是"动作不对/不像本人"，所以这里只留一行短警告。
        const USkeletalMesh* BodyAsset = BodyMesh->GetSkeletalMeshAsset();
        UE_LOG(LogTemp, Warning,
            TEXT("[第三人称] %s 的动画 %s 和身体网格不是同一套骨架（身体骨架 = %s，动画骨架 = %s）→ 不播。"),
            *GetName(), *Asset->GetName(),
            BodyAsset && BodyAsset->GetSkeleton() ? *BodyAsset->GetSkeleton()->GetName() : TEXT("<无>"),
            Asset->GetSkeleton() ? *Asset->GetSkeleton()->GetName() : TEXT("<无>"));
        return false;
    }

    UAnimInstance* AnimInstance = BodyMesh->GetAnimInstance();
    if (AnimInstance == nullptr) return false;

    return PlayAnimAssetOnInstance(Asset, AnimInstance,
                                   ResolveAnimSlotName(SlotNameSource, FallbackSlotName),
                                   DynamicMontageLoopCount, bStopAllMontages);
}

/*
 * 第三人称身体上**上半身 + 下半身两条一起播**。
 *
 * ── 为什么需要这个（不是"想播两条"，是"只播上半身根本不够"）───────────────────
 * 现役 ABP_BlasterCharacter 的动画图是这么接的：
 *     LB_Rifle(状态机) → Slot 'LowerBody' → 分层混合的 **BasePose**
 *     UB_Rifle(状态机) → Slot 'UpperBody' → 分层混合的层[0]（遮罩 Body）
 *     Hand_Rifle       → Slot 'UpperBody' → 分层混合的层[1]（遮罩 Arm）
 * 两个遮罩按 BlendMask 语义只有**列进去的骨头**有权重（FAnimationRuntime::CreateMaskWeights，
 * AnimationRuntime.cpp:2243，没列进去的骨保持 BasePose）：Body = Spine1-4/Collar/Neck/Head 七根，
 * Arm = 两条手臂 53 根。**腿和 Root/pelvis 都不在里面**。
 * 所以只往 UpperBody 槽里喂资产时，腿和胯永远来自 LowerBody 槽（LB_Rifle 的站立姿势）——
 * 这就是"第三人称蹲不下去"的真正原因（往下包/拆包那几条 _UB 序列里写腿也没用，那些轨道压根没权重）。
 *
 * ── 引擎的蹲为什么补不上这一课 ─────────────────────────────────────────
 * ACharacter::OnStartCrouch 把身体网格的相对 Z 往上补了"胶囊矮下去的那一段"
 *（本工程 BlasterCharacter.h:788 那段注释里写明了：一降一升正好抵消，
 *  **身体网格的世界变换根本不动**），所以 Crouch()/UnCrouch() 只改胶囊、眼高和玩法，
 * 对第三人称身体是**零表现**。而瓦的 TP 资产里又没有蹲姿序列
 *（只有 TP_Core_Crouch_Jump_LB 一条，且是跳），**唯一带蹲姿的下半身数据就是 Bomb 那三条 LB**。
 * 结论：第三人称要看见蹲，只能把 LB 序列喂给 LowerBody 槽。
 *
 * ── 顺序和 bStopAllMontages ──────────────────────────────────────────────
 * 上半身先播（它走默认的 bStopAllMontages=true，负责把这一帧之前的东西清干净），
 * 下半身**必须后播且传 false**，否则 Montage_Play 的 bStopAllMontages 是真·全停，
 * 会把刚起播的上半身那条一起顶掉（PlayThrowableSet 里那条注释说的是同一件事）。
 * 用"上半身到底播出去没有"来决定谁来清场，而不是写死顺序：上半身留空 / 播不出去时，
 * 下半身那条得接着当清场的那一条。
 */
bool ABlasterCharacter::PlayThirdPersonUpperLower(UAnimSequenceBase* UpperAsset, UAnimSequenceBase* LowerAsset,
                                                 int32 UpperLoopCount, int32 LowerLoopCount)
{
    // 槽名写死成两句字面量：必须是动画图里 Slot 节点的名字，配错了引擎一声不吭（不出画、不报错）。
    const bool bUpperPlayed = PlayThirdPersonMontage(UpperAsset, nullptr, TEXT("UpperBody"), UpperLoopCount);

    PlayThirdPersonMontage(LowerAsset, nullptr, TEXT("LowerBody"), LowerLoopCount,
                           /*bStopAllMontages=*/!bUpperPlayed);

    return bUpperPlayed;
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
        const float MontageDuration = AnimInstance->Montage_Play(ElimMontage);

        // 只有权威机决定「什么时候收尸」，结果随 bElimmedHidden 复制给各端。
        // AddUniqueDynamic：万一重复调用也不会重复绑定。
        if (HasAuthority() && !bElimmedHidden)
        {
            if (MontageDuration > 0.f)
            {
                AnimInstance->OnMontageEnded.AddUniqueDynamic(this, &ABlasterCharacter::OnElimMontageEnded);
            }
            else
            {
                // 蒙太奇没播起来（权重 0 / 被立刻打断）：退回按 ElimDelay 兜底
                ScheduleCorpseHide(ElimDelay);
            }
        }
    }
    else if (HasAuthority() && !bElimmedHidden)
    {
        // 没配死亡蒙太奇资产也得收尸，否则尸体一直躺在那
        ScheduleCorpseHide(ElimDelay);
    }
}

void ABlasterCharacter::OnElimMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
    // OnMontageEnded 是「任意蒙太奇播完」都会来的，只认死亡蒙太奇
    if (Montage != ElimMontage) return;

    ScheduleCorpseHide(CorpseHideDelay);
}

void ABlasterCharacter::ScheduleCorpseHide(float Delay)
{
    GetWorldTimerManager().SetTimer(
        ElimHideTimer,
        this,
        &ABlasterCharacter::HideElimmedCorpse,
        FMath::Max(0.f, Delay),
        false);
}

void ABlasterCharacter::HideElimmedCorpse()
{
    if (!HasAuthority() || bElimmedHidden) return;

    // 权威机不会收到自己的 OnRep，这里手动应用一次
    bElimmedHidden = true;
    ApplyElimmedHidden();
}

void ABlasterCharacter::OnRep_ElimmedHidden()
{
    if (bElimmedHidden)
    {
        ApplyElimmedHidden();
    }
}

void ABlasterCharacter::ApplyElimmedHidden()
{
    // 只隐藏、不销毁：PC 这时可能还 possess 着这具尸体（死亡镜头/观战切换），
    // 销毁会连带牵动视图目标与输入栈；隐藏同样达到「人已经没了」的效果，
    // 且下一回合 GameMode 会整批 Destroy + 重新生成，不需要再恢复显示。
    SetActorHiddenInGame(true);
    SetActorEnableCollision(false);
    GetCharacterMovement()->DisableMovement();

    // 死亡时刷的处决特效是独立 actor，一并停掉
    if (ElimBotComponent)
    {
        ElimBotComponent->Deactivate();
        ElimBotComponent = nullptr;
    }
}

void ABlasterCharacter::PlayReloadMontage()
{
    if (Combat == nullptr || EquippedWeapon == nullptr)return;

    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance == nullptr) return;

    // 优先用这把武器自己的第三人称换弹蒙太奇 —— 单段动作，不跳 section。
    if (UAnimMontage* WeaponMontage = EquippedWeapon->GetThirdPersonReloadMontage())
    {
        AnimInstance->Montage_Play(WeaponMontage);
        return;
    }

    // 退回角色上那条共用蒙太奇：一条资产靠 section 名分武器类型。
    if (ReloadMontage)
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
        default:
            // 没有换弹段的武器类型（近战飞刀）→ 直接不播。
            // 少了这个 default，SectionName 会保持 FName 默认值（NAME_None），Montage_JumpToSection
            // 会失败、蒙太奇从头播一遍别人的动作。以前这还会连带把玩家**永久**卡在 ECS_Reloading
            // 里（那时换弹结束只能靠这条蒙太奇尾部的动画通知），现在换弹结束由服务器计时器兜底
            // （见 AWeapon::ReloadTime），所以这里只剩"别播错动作"这一个理由了。
            return;
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

float ABlasterCharacter::GetThrowGrenadeDuration() const
{
    // 显式配了就用配的
    if (ThrowGrenadeTime > 0.f) return ThrowGrenadeTime;

    // 没配就按蒙太奇自己的长度。**注意**：这里只是读资产上写的时长，
    // 蒙太奇能不能在当前骨架的网格上播出来（绑错骨架会被拒播）不影响这个值 ——
    // 也就是说"看不看得见动画"和"什么时候能再开枪"从此解耦了。
    if (ThrowGrenadeMontage) return FMath::Max(ThrowGrenadeMontage->GetPlayLength(), 0.1f);

    // 兜底。绝不能返回 0：那会立刻结束扔雷状态。
    return 1.5f;
}

const USpikeAnimSet* ABlasterCharacter::GetSpikeAnimSet(const TCHAR* Where)
{
    if (SpikeAnims) return SpikeAnims.Get();

    if (!bWarnedMissingSpikeAnimSet)
    {
        bWarnedMissingSpikeAnimSet = true;
        UE_LOG(LogTemp, Warning,
            TEXT("[Spike动画] %s 的 SpikeAnims 没配（%s）→ 掏出/下包/拆包一个动画都不会播，")
            TEXT("而且除了这一行不会有任何别的提示。去该角色的蓝图上把 SpikeAnims 指到")
            TEXT("一个 USpikeAnimSet 资产（工程里五个角色蓝图都该指同一个）。"),
            *GetName(), Where);
    }
    return nullptr;
}

void ABlasterCharacter::MulticastPlantAnimation_Implementation(bool bStart)
{
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("下包"));
    if (Anims == nullptr) return;

    // 两套网格各播各的：身体那份所有机器都要播（身体是别人眼里的你），
    // 手模那份只有本人这台有（PlayFPArmsMontage 内部会挡掉非本地控制）。
    // 两个入口都自带骨架检查 —— 填错骨架（比如把手模骨架的动画填进 PlantFP）
    // 会打一行指名道姓的警告，而不是静默出事。
    if (bStart)
    {
        // SlotNameSource 传 nullptr：那个参数是给"裸序列要现造动态蒙太奇"时取槽名用的，
        // 这里直接给目标的槽名（身体 UpperBody / 手模 DefaultSlot，两个 ABP 里 Slot 节点的名字）。
        //
        // 身体这条走 PlayThirdPersonUpperLower（上半身 + 下半身**成对**播）：下包的"蹲"在下半身，
        // 而动画蓝图的两个遮罩都不含腿和 Root —— 只播上半身的话是"上身探下去、腿站着"。
        PlayThirdPersonUpperLower(Anims->PlantTP, Anims->PlantLB);
        PlayFPArmsMontage(Anims->PlantFP, nullptr, TEXT("DefaultSlot"));
    }
    else
    {
        // 用 StopAnimAssetOnMesh 而不是 Montage_Stop(资产)：这两条槽里填的可能是**裸序列**
        //（实际在播的是现造的动态蒙太奇，拿序列本身去停匹配不上）。理由见函数上的注释。
        // 下半身那条一起淡出（0.2 秒）—— 站起来的过渡全靠它，停太快腿是"啪"地弹回站姿。
        StopAnimAssetOnMesh(GetMesh(), Anims->PlantTP, 0.2f);
        StopAnimAssetOnMesh(GetMesh(), Anims->PlantLB, 0.2f);
        StopAnimAssetOnMesh(FPArmsMesh, Anims->PlantFP, 0.2f);
    }
}

/*
 * 掏出包（按 4 切到 spike）那两条装备动画。
 *
 * 只调两个 Play* 就行：骨架检查、槽名、本地控制门禁全在那两个函数里
 *（身体那条所有机器都播，手模那条只本人那台播）。
 *
 * 两个触发点都要挂 —— 服务器那条路 SetSpikeDrawn 里直接调，客户端那条路等复制到了在
 * OnRep_SpikeDrawn 里调。漏了 OnRep 那个的话，**只有 listen server 本人看得到装备动画**，
 * 远端客户端和模拟代理身上是硬切。
 */
void ABlasterCharacter::PlaySpikeEquipAnimations()
{
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("掏出包"));
    if (Anims == nullptr) return;

    PlayThirdPersonMontage(Anims->EquipTP, nullptr, TEXT("UpperBody"));
    PlayFPArmsMontage(Anims->EquipFP, nullptr, TEXT("DefaultSlot"));
}

void ABlasterCharacter::MulticastDefuseAnimation_Implementation(bool bStart)
{
    if (bStart)
    {
        PlayDefuseDrawAnimations();
    }
    else
    {
        PlayDefuseStopAnimations();
    }
}

/*
 * 自动蹲下 —— 安包/拆包期间的"替玩家按 Ctrl"。
 *
 * 三条路都只认权威机 + 本人那台（理由：蹲下是 CMC 客户端预测状态，见头文件
 * MulticastSpikeAutoCrouch 的注释）；别的机器上调 Crouch 只会让模拟代理的 CMC 打架。
 */
void ABlasterCharacter::SetAutoCrouch(bool bEnable)
{
    if (!HasAuthority() && !IsLocallyControlled()) return;

    if (bEnable)
    {
        // 玩家本来就蹲着（自己按的）→ 不记成"这次是我按的"，收尾也就不该替他站起来
        if (bIsCrouched) return;

        bAutoCrouched = true;
        Crouch();
    }
    else if (bAutoCrouched)
    {
        bAutoCrouched = false;
        UnCrouch();
    }
}

void ABlasterCharacter::MulticastSpikeAutoCrouch_Implementation(bool bEnable)
{
    SetAutoCrouch(bEnable);
}

void ABlasterCharacter::UpdateFPSpikeVisual()
{
    if (FPSpikeMesh == nullptr) return;

    /*
     * 只有三个条件同时成立才显示：本人那台机器 + 包在手上（bSpikeDrawn）+ 知道是哪个包
     *（CarriedSpike —— 网格资产从它身上取）。
     *
     * 每帧全量重算，不挂 RepNotify：这三件事分别在三个地方翻转（bSpikeDrawn 复制、
     * CarriedSpike 复制、BP_Spike 上换网格），挂三个钩子也还会漏掉"复制到达顺序"这类边角
     *（比如 bSpikeDrawn 先到、CarriedSpike 后到，钩子跑的时候网格还是空的）。
     * 代价就是下面三次判断，且真正会改组件状态的那几行都有缓存挡着，不会每帧重复设。
     */
    USkeletalMesh* SpikeAsset = (CarriedSpike && CarriedSpike->SpikeMesh)
        ? CarriedSpike->SpikeMesh->GetSkeletalMeshAsset()
        : nullptr;
    const bool bShow = IsLocallyControlled() && IsSpikeDrawn() && SpikeAsset != nullptr;

    if (bShow)
    {
        // 换包了（或者第一次掏）→ 重新配一遍网格 + 挂点
        if (FPSpikeMesh->GetSkeletalMeshAsset() != SpikeAsset)
        {
            FPSpikeMesh->SetSkeletalMesh(SpikeAsset);
            bFPSpikeAttached = false;
        }

        /*
         * ★ 1P 这份包**自己不跑动画机**：姿势整个照搬真包（CarriedSpike->SpikeMesh，3P 那份）。
         *
         * 不这么做的话它停在骨骼网格的 ref pose 上 —— 而 EQ_Bomb_S0_Mesh 的 ref pose 是**展开**状态
         * （绑定姿势就是张开的，导入包围盒 81cm 高），于是手里的包一直是大开的：
         * 既不跟 ABP_Spike 的 Idle（收拢，Cap 才 21.6cm），也不跟下包展开 / 拆包那些蒙太奇。
         * 看起来就像"手里的包没走动画机"。
         *
         * SetLeaderPoseComponent（旧名 SetMasterPoseComponent）= 每帧复制对方的骨骼姿势：
         * ABP 状态机、ModifyBone、DefaultSlot 槽上叠的 montage（开始下包展开 / 安包完成展开 /
         * 拆包收拢）全都自动跟过来，不用在这里再养第二套动画机。
         * 同一个 leader 重复调它自己会跳过（bForceUpdate 默认 false），所以放这儿不用额外记状态。
         *
         * ⚠ 前提：真包那份在**本地玩家眼里是隐藏的**（Carrier 自己看不到 3P 包，见 ASpike::
         *   UpdateMeshVisibility），而隐藏的网格默认根本不 tick 姿势（OnlyTickPoseWhenRendered），
         *   leader pose 会跟着一起僵住。BP_Spike 的 SpikeMesh 组件已经显式设成
         *   AlwaysTickPoseAndRefreshBones，那个默认值就是为这件事留的，别改回去。
         */
        FPSpikeMesh->SetLeaderPoseComponent(CarriedSpike->SpikeMesh);

        if (!bFPSpikeAttached && FPArmsMesh)
        {
            bFPSpikeAttached = true;

            /*
             * ★ 必须**运行时重挂一次**，构造函数里那次 SetupAttachment 不作数 ——
             * 那一刻 BP 的类默认值还没套到这个对象上，挂点名字被永久钉死在 C++ 默认值上，
             * 在 BP 里改 FPSpikeSocket 会一点效果都没有（整套理由见 UpdateHeldThrowableVisual
             * 里那段注释，火球那边踩过一模一样的坑）。
             */
            FPSpikeMesh->AttachToComponent(FPArmsMesh,
                FAttachmentTransformRules::SnapToTargetNotIncludingScale, FPSpikeSocket);

            if (!FPArmsMesh->DoesSocketExist(FPSpikeSocket))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[尖刺包] 手模上找不到挂点 '%s'（socket 和骨骼都没有），第一人称手里的包会挂在手模原点")
                    TEXT("（请检查 ABlasterCharacter::FPSpikeSocket）"), *FPSpikeSocket.ToString());
            }

            /*
             * ★ 用**世界缩放**归一大小：手模那条链的根骨带 100 倍（瓦的米→厘米），
             *   挂上去继承下来就是 100 倍大的包。SetWorldScale3D 按父链反算相对缩放，
             *   于是这里填 1 就是"和真包一样大"（和 FPCamera 补 0.01、火球补世界缩放是同一件事）。
             *   ⚠ 顺序：先挂点再设缩放 —— SnapToTargetNotIncludingScale 会保持世界缩放、
             *     反算相对缩放；放在设缩放之后会把刚设好的值再改一遍。
             */
            FPSpikeMesh->SetWorldScale3D(FVector(1.f));

            // 挂点上的微调（默认零 = 网格原点正落在挂点上，和真包那边同一个约定）。
            // 只用位移和旋转 —— 缩放上面那行说了算，这里再乘一次会两边打架。
            FPSpikeMesh->SetRelativeLocationAndRotation(
                FPSpikeMeshOffset.GetLocation(), FPSpikeMeshOffset.GetRotation());
        }
    }
    else
    {
        // 收起来时把"挂好了"的标记清掉：下次掏出来重新挂一遍（挂点/网格都可能已经变了）
        bFPSpikeAttached = false;
    }

    if (bFPSpikeVisible != bShow)
    {
        bFPSpikeVisible = bShow;
        FPSpikeMesh->SetVisibility(bShow, /*bPropagateToChildren=*/true);
    }
}

/*
 * ——— 拆包那串蒙太奇：掏出 → 待命（循环） → 收起 ———
 *
 * 三个槽各自是独立的一条资产，中间靠这里接。为什么不合成一条蒙太奇：用户要的是
 * "蒙太奇槽位留给我填"，三段分开填最灵活（也正好对应导入的那三条瓦资产
 * FP_Core_Bomb_S0_Defusal_Start / _Loop / _Stop）。
 *
 * ⚠ "掏出"接"待命"的等待时间量的是**这个网格上实际播起来的那条**还剩多久
 *  （GetAnimRemainingTime），不是资产全长：裸序列会被包成动态蒙太奇播，两者在
 *   跳段/被顶掉之后能差出好几秒。查不到剩余（压根没播起来，比如骨架对不上被拒播）
 *   就直接接待命 —— 那种情况下死等一个不会播的动画，画面上就是僵住。
 */
void ABlasterCharacter::PlayDefuseDrawAnimations()
{
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("拆包·掏出"));
    if (Anims == nullptr) return;

    // —— 第一人称（手模）——
    if (IsLocallyControlled() && FPArmsMesh && FPArmsMesh->GetAnimInstance())
    {
        GetWorldTimerManager().ClearTimer(DefuseIdleTimerFP);

        if (Anims->DefuseDrawFP)
        {
            PlayFPArmsMontage(Anims->DefuseDrawFP, nullptr, TEXT("DefaultSlot"));

            const float DrawRemain = GetAnimRemainingTime(FPArmsMesh, Anims->DefuseDrawFP);
            if (DrawRemain > 0.f)
            {
                GetWorldTimerManager().SetTimer(DefuseIdleTimerFP, this,
                    &ABlasterCharacter::PlayDefuseIdleFP, DrawRemain, false);
            }
            else
            {
                PlayDefuseIdleFP();
            }
        }
        else
        {
            PlayDefuseIdleFP();
        }
    }

    // —— 第三人称（身体）——
    if (GetMesh() && GetMesh()->GetAnimInstance())
    {
        GetWorldTimerManager().ClearTimer(DefuseIdleTimerTP);

        if (Anims->DefuseDrawTP || Anims->DefuseDrawLB)
        {
            // 上半身 + 下半身成对播（"掏出拆包器"那一下的蹲姿在下半身）——
            // 只播上半身的话腿一直是站姿，见 PlayThirdPersonUpperLower。
            PlayThirdPersonUpperLower(Anims->DefuseDrawTP, Anims->DefuseDrawLB);

            // 等**两条里长的那个**播完再接待命：只按上半身算的话，下半身那条会被提前切掉
            //（腿还没收到位就开始循环，接缝上会闪一下）。
            const float DrawRemain = FMath::Max(GetAnimRemainingTime(GetMesh(), Anims->DefuseDrawTP),
                                                GetAnimRemainingTime(GetMesh(), Anims->DefuseDrawLB));
            if (DrawRemain > 0.f)
            {
                GetWorldTimerManager().SetTimer(DefuseIdleTimerTP, this,
                    &ABlasterCharacter::PlayDefuseIdleTP, DrawRemain, false);
            }
            else
            {
                PlayDefuseIdleTP();
            }
        }
        else
        {
            PlayDefuseIdleTP();
        }
    }
}

void ABlasterCharacter::PlayDefuseIdleFP()
{
    /*
     * 待命是**循环**的（拆多久就待命多久）。
     *
     * 传一个大 LoopCount 是给"裸序列"那条路用的：PlayAnimAssetOnInstance 默认 LoopCount=1，
     * 现造出来的动态蒙太奇播一遍就完了 —— 拆包后半程手上就没动作了。
     * 填的是**蒙太奇**的话这里不起作用，循环要写在蒙太奇自己里面（段的 Next Section 指向自己）。
     */
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("拆包·待命(1P)"));
    if (Anims == nullptr) return;

    PlayFPArmsMontage(Anims->DefuseIdleFP, nullptr, TEXT("DefaultSlot"),
                      /*DynamicMontageLoopCount=*/1000);
}

void ABlasterCharacter::PlayDefuseIdleTP()
{
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("拆包·待命(3P)"));
    if (Anims == nullptr) return;

    // 上半身 + 下半身一起循环（蹲着拆多久就待命多久）。LoopCount 只有"裸序列"那条路用得上：
    // 上半身那条是蒙太奇（循环写在它自己的段里），下半身那条是裸序列，靠这个 1000 循环。
    PlayThirdPersonUpperLower(Anims->DefuseIdleTP, Anims->DefuseIdleLB,
                              /*UpperLoopCount=*/1000, /*LowerLoopCount=*/1000);
}

void ABlasterCharacter::PlayDefuseStopAnimations()
{
    const USpikeAnimSet* Anims = GetSpikeAnimSet(TEXT("拆包·收起"));
    if (Anims == nullptr) return;

    // 打断和拆完走的都是这一条（用户："拆包打断或者拆完了再播stop"）。
    GetWorldTimerManager().ClearTimer(DefuseIdleTimerFP);
    GetWorldTimerManager().ClearTimer(DefuseIdleTimerTP);

    /*
     * 先把**在播的那两段**都掐掉，再播收尾那一段。
     *
     * 掐"掏出"是因为它可能还在播（刚掏出来一下就被打断/就走完进度），
     * 它和收尾那条占同一个槽位 —— 不掐掉的话谁赢取决于引擎收集蒙太奇的顺序，
     * 表现是"有时候收得住、有时候手上还是掏出那个姿势"。
     * 混合时间 0 = 立刻断，收尾紧接着开始，不能两条叠着。
     */
    StopAnimAssetOnMesh(FPArmsMesh, Anims->DefuseDrawFP);
    StopAnimAssetOnMesh(FPArmsMesh, Anims->DefuseIdleFP);
    StopAnimAssetOnMesh(GetMesh(), Anims->DefuseDrawTP);
    StopAnimAssetOnMesh(GetMesh(), Anims->DefuseIdleTP);

    // 下半身那两条也掐掉，但**留 0.2 秒混合**：站起来全靠这一下，
    // 传 0（上面那几条的默认）腿会"啪"地弹回站姿。3P 的"收起"在瓦的资产里没有下半身版本
    //（连上半身都没有），所以这里没有"播收尾那一段"这回事，只有"淡着站起来"。
    StopAnimAssetOnMesh(GetMesh(), Anims->DefuseDrawLB, 0.2f);
    StopAnimAssetOnMesh(GetMesh(), Anims->DefuseIdleLB, 0.2f);

    PlayFPArmsMontage(Anims->DefuseStopFP, nullptr, TEXT("DefaultSlot"));
    PlayThirdPersonMontage(Anims->DefuseStopTP, nullptr, TEXT("UpperBody"));
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
    // Phoenix 大招「再来一次」：生效期间"阵亡"不真的死 —— 满血满甲回到标记点，然后扣大招点。
    //
    // 拦在这里是因为所有致死路径（枪械伤害 / 爆能器爆炸 / 坠落）最终都汇到 ServerElim。
    // ⚠️ GameMode 在调 Elim() 之前**已经**给击杀者记了分/钱/大招点/击杀播报，这是有意保留的：
    // Valorant 里打死一个开着大的不死鸟同样算对方的击杀，只是不死鸟本人不倒。
    // 死亡掉枪 / 掉包 / 溶解 / 关碰撞 / 进观战 全在下面，这里提前 return 就都跳过了。
    //
    // 注意判的是 IsRunItBackActive() 而不是笼统的 IsUltimateActive()：只有再来一次会拦死亡。
    if (IsRunItBackActive())
    {
        // 走通用出口 → 分派到 ServerReturnToRunItBack()：拉回标记点 + 满血满甲 + 扣大招点
        ServerEndUltimate(/*bFromDeath=*/true);
        return;
    }

    // Jett 大招「刃风暴」：阵亡 = 大招作废，把飞刀销毁掉（销毁而不是掉落 —— 敌人不该捡到对手大招的刀）。
    // 顺带扣掉大招点 —— 活着的 Jett 是"把刀扔完"才扣，死在刀没扔完时同样得付这次大招的钱，
    // 收尾走的是同一个出口（ServerEndBladeStorm），所以不会漏也不会双扣。
    // 这里**不 return**：该掉枪掉枪、该掉包掉包、该进观战进观战，只是顺手收刀。
    if (ActiveUltimate == EActiveUltimate::EUA_BladeStorm)
    {
        // bFromDeath=true → 不掏枪（下面 DropAllWeapons 紧接着就会把枪全掉光，掏一下纯属白播音效）
        ServerEndUltimate(/*bFromDeath=*/true);
    }

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

// --- 大招「生效中」状态（通用）---

void ABlasterCharacter::ServerStartUltimate(EActiveUltimate Kind, float Duration)
{
    if (!HasAuthority()) return;
    if (Kind == EActiveUltimate::EUA_None) return;

    // 已经有一个大招在跑就别启动第二个 —— 两个大招叠着的话收尾只能收回其中一个，
    // 另一个的场面（标记点/飞刀）会永久留在场上。能力那边的 CanActivateAbility 也挡了，
    // 这里是服务器侧的最后一道。
    if (IsUltimateActive())
    {
        UE_LOG(LogTemp, Warning, TEXT("[大招] %s 已有大招在生效中，忽略这次启动"), *GetName());
        return;
    }

    // 大招自己该做的前置动作由调用方在**调这个之前**做完（Phoenix 记标记点 / Jett 生成飞刀），
    // 这里只管"进入生效状态"这一件通用的事。
    if (Kind == EActiveUltimate::EUA_RunItBack)
    {
        // 记标记点 = "开大那一刻站的位置"。只有再来一次需要它。
        RunItBackLocation = GetActorLocation();
        RunItBackRotation = GetActorRotation();
    }

    ActiveUltimate = Kind;

    // Duration <= 0 = 不自动收（刀留到扔完/阵亡/回合结束）。
    // 绝对服务器时间（= PC->GetServerTime() 的时间轴），客户端拿它算倒计时。
    UltimateEndTime = (Duration > 0.f) ? GetWorld()->GetTimeSeconds() + Duration : 0.f;

    // 权威机改自己的复制属性不会触发 OnRep，手动刷一次
    OnRep_ActiveUltimate();

    if (Duration > 0.f)
    {
        GetWorldTimerManager().SetTimer(UltimateTimer, this,
            &ABlasterCharacter::OnUltimateTimerFired, Duration, false);
    }
}

void ABlasterCharacter::OnUltimateTimerFired()
{
    // 到期 = 正常收招，不是阵亡导致的作废（所以 bFromDeath=false：
    // Jett 收刀后会把枪掏回来）
    ServerEndUltimate(/*bFromDeath=*/false);
}

void ABlasterCharacter::ServerEndUltimate(bool bFromDeath)
{
    if (!HasAuthority()) return;
    if (!IsUltimateActive()) return;

    // 先抓住"结束的是哪一个"，再清状态 —— 下面的收尾逻辑里要开新武器、要读角色状态
    const EActiveUltimate Ending = ActiveUltimate;

    ActiveUltimate = EActiveUltimate::EUA_None;
    UltimateEndTime = 0.f;
    GetWorldTimerManager().ClearTimer(UltimateTimer);
    // 飞刀扔空排的那个"下一帧收招"也要一起撤掉 —— 收招已经发生了，它再来一次是多余的
    //（回调里有 IsUltimateActive 守卫，不会真出事，但没必要留个悬着的定时器）
    GetWorldTimerManager().ClearTimer(BladeStormDepleteTimer);
    OnRep_ActiveUltimate();

    switch (Ending)
    {
    case EActiveUltimate::EUA_RunItBack:
        ServerReturnToRunItBack();
        break;

    case EActiveUltimate::EUA_BladeStorm:
        // 活着收刀（到期）→ 掏回枪；阵亡收刀 → 不掏（死亡流程马上就掉枪了）
        ServerEndBladeStorm(/*bReEquipWeapon=*/!bFromDeath);
        break;

    default:
        break;
    }
}

void ABlasterCharacter::OnRep_ActiveUltimate()
{
    // 大招生效/结束的视觉都在 HUD 里按 ActiveUltimate + UltimateEndTime 每帧现算
    //（技能条大招槽倒计时），这里没有需要主动推的东西。
    // 留这个空实现是为了：① 复制属性必须有 RepNotify，否则 HUD 那侧的变化要等下一帧轮询才注意到；
    // ② 以后要加"开大/收招"的音效、特效、镜头抖动，挂这里最自然。
}

// --- Phoenix 大招「再来一次 / Run It Back」收尾 ---

void ABlasterCharacter::ServerReturnToRunItBack()
{
    if (!HasAuthority()) return;

    // 回到标记点。伤害那一发**已经结算过了**（ReceiveDamage 里扣过血），这里直接覆盖回满，
    // 不是"回血"而是"回到开大那一刻的状态"。
    SetActorLocation(RunItBackLocation, false, nullptr, ETeleportType::TeleportPhysics);
    SetActorRotation(RunItBackRotation);
    if (UCharacterMovementComponent* Move = GetCharacterMovement())
    {
        // 清掉传送前的速度/下落状态，否则会带着死亡那一刻的动量飞出去
        Move->StopMovementImmediately();
    }

    Health = MaxHealth;
    // 甲量和血量共用同一条 HUD 推送链路，SetArmor 内部会 UpdateHUDHealth，不用再刷一次
    SetArmor(MaxArmor);

    // ⚠️ 扣点就在这里 —— **不在按下 X 的时候**（用户明确要求"回到原点之后再扣"）。
    // 阵亡回程（ServerElim 拦下来）和到期回程（定时器）都经过 ServerEndUltimate 来到这里，
    // 所以扣点只写在这一处：两条路都扣、也只扣一次。
    if (ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>())
    {
        PS->SpendAllUltPoints();
    }
}

// --- Jett 大招「刃风暴 / Blade Storm」 ---

void ABlasterCharacter::ServerStartBladeStorm(TSubclassOf<AJettKnives> KnivesClass, float Duration, int32 KnifeCount)
{
    if (!HasAuthority()) return;
    if (!KnivesClass || !Combat) return;
    if (IsUltimateActive()) return;

    UWorld* World = GetWorld();
    if (!World) return;

    // 现生成一把新的飞刀塞进近战槽。
    // 不复用上一把：刀数和伤害是每个 Jett 各自的，上一轮/上一个大招的刀也不该被继承。
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.Instigator = this;
    // 出生点可能被别的东西占着，挪开就行，别生成失败
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

    AJettKnives* Knives = World->SpawnActor<AJettKnives>(
        KnivesClass,
        GetActorLocation(),
        GetActorRotation(),
        SpawnParams
    );
    if (!Knives) return;

    // 刀数：能力上填了就用它，没填（<=0）用武器蓝图自己的 MagCapacity —— 数值只在一处调
    if (KnifeCount > 0)
    {
        Knives->SetAmmo(KnifeCount);
    }

    /*
     * 换弹中开大招 → **先把换弹取消掉**，再掏刀。
     *
     * 不取消的话这条大招会**半生效**：EquipSlotWeapon 有 CanChangeWeapon() 门禁，
     * 而它明确把 ECS_Reloading 挡在外面（那条限制是对的：换弹中不该能切枪）。
     * 于是刀生成了、大招也进生效状态了，人手上却还是那把枪 —— 后续每一次开火
     * 走的都是手枪（伤害、弹药、动画全是手枪的），飞刀只能看不能用，直到大招到期。
     * 用户报的就是这个（2026-09-18：手枪换弹时按 X，手枪不换、后续还是手枪开枪）。
     *
     * 为什么是"取消"而不是"拒绝开大"：开大本来就是"换手上的东西"，
     * 玩家按 X 的意思就是立刻要刀 —— 和瓦里一致，手感也对。
     * 注意 CancelReload 不回填弹药（换到一半就是没换完），且必须**紧接**掏刀，
     * 好让下面那条掏刀蒙太奇把换弹动画顶掉。
     */
    Combat->CancelReload();

    // 塞进近战槽并掏出来（内部走 EquipSlotWeapon：收回手上的枪 + 挂好 + HUD 弹药 + 掏枪音效）
    Combat->EquipMeleeWeapon(Knives);

    /*
     * 掏没掏成功，**验一下再进生效状态**。
     *
     * 上面那句是"能掏的都会掏"，但掏失败的路不止换弹一条（空手锁、状态不对、以后加的新门禁……）。
     * 而这里一旦往下走到 ServerStartUltimate，大招就是"生效中"了 ——
     * 刀不在手上、HUD 却在倒计时、飞刀挂架还亮着，是最难查的那种半生效。
     * 所以拿"手上到底是哪把"当唯一判据：没换成功就把刚生成的刀销毁、这条大招整个不发。
     */
    if (GetEquippedWeapon() != Knives)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[大招] %s 的刃风暴没能把飞刀掏上手（当前手上是 %s）→ 这一条大招不发，刀已销毁"),
            *GetName(), GetEquippedWeapon() ? *GetEquippedWeapon()->GetName() : TEXT("<空手>"));
        Knives->Destroy();
        return;
    }

    // 进入大招生效状态，放最后 —— 它内部会判 IsUltimateActive()，先放了会被自己挡掉
    ServerStartUltimate(EActiveUltimate::EUA_BladeStorm, Duration);
}

void ABlasterCharacter::ServerToggleBladeStormKnives()
{
    if (!HasAuthority() || !Combat) return;

    // 只有刃风暴生效期间才有"呼出/收起"这回事 —— 没开大时按 X 是**开始**大招（能力那条路），
    // 收招之后按 X 又是新一轮开大。这里被别的大招（Phoenix）调到也要一声不响地返回。
    if (!IsBladeStormActive()) return;

    // 生效期间近战槽里就是这把飞刀（ServerStartBladeStorm 塞进去的）。
    // 拿不到 = 状态不对（比如刚被别的路径销毁），什么都不做比瞎切安全。
    AWeapon* Knives = Combat->GetMeleeWeapon();
    if (Knives == nullptr) return;

    if (Knives == GetEquippedWeapon())
    {
        /*
         * 正拿着 → 收起来，掏回枪。
         *
         * 用 EquipBestOwnedWeapon()（主武器优先、没有主武器才副武器）而不是"记住上次那把"：
         * 这条链上没有"上次"这个可靠的记录 —— 玩家可能是从枪切刀、也可能从枪切到别的枪再切刀，
         * 而且收招那边（ServerEndBladeStorm）用的也是同一个函数，两处行为一致最好预测。
         *
         * ⚠ 一把枪都没有时那个函数什么都不做（它是 HasAuthority + EquipSlotWeapon 的薄封装），
         *   于是会变成"按 X 没反应"。所以兜一手：退回角色自带的那把刀。
         *   两把都没有（理论上不会：近战槽里那把是天生自带的）就维持现状。
         */
        if (Combat->PrimaryWeapon || Combat->SecondaryWeapon)
        {
            Combat->EquipBestOwnedWeapon();
        }
        else if (Combat->StashedMeleeWeapon)
        {
            Combat->EquipSlotWeapon(Combat->StashedMeleeWeapon);
        }
        return;
    }

    /*
     * 没拿着 → 掏出来。
     *
     * 刀已经扔空了就别掏了：那一刻大招正在收（AJettKnives::NotifyIfDepleted 排的下一帧收招），
     * 掏出来只会让玩家看到"手上一把刀、但立刻又没了"。
     */
    if (Knives->IsEmpty()) return;

    // 和按 3 那条（SwitchWeapon→EquipSlotWeapon）同一套：收枪、挂点、HUD 弹药、掏枪音效、
    // 手模/身体的掏枪蒙太奇全在里面。刀骨骼那 5 把刀的显隐和掏出动画不在武器身上 ——
    // 由 AJettCharacter 在"已掏出"的上升沿自己播（见 SyncKnivesToAmmo）。
    Combat->EquipSlotWeapon(Knives);
}

void ABlasterCharacter::ServerEndBladeStorm(bool bReEquipWeapon)
{
    if (!HasAuthority()) return;

    /*
     * 掏枪前先量一下"最后那一刀的表演还剩多久"。
     *
     * 必须在这里量（DestroyMeleeWeapon 之后飞刀那个 actor 就没了），也必须在**收招这一刻**
     * 现量（不能在"上报扔空"时量了存起来：扔空那一下要是打死了人，击杀刷新会把 Ammo 补满、
     * 这次收招就作废了，存下来的那个延迟会一直挂着，等真正到期收招时把枪晚掏好几秒）。
     */
    const float EquipDelay = bReEquipWeapon ? ResolveBladeStormEquipDelay() : 0.f;

    // 销毁飞刀（销毁而不是掉落 —— 敌人不该捡到对手大招的刀）。
    // DestroyMeleeWeapon 内部会顺手清空手上武器、HUD 弹药归零。
    if (Combat)
    {
        Combat->DestroyMeleeWeapon();

        // 刀没了手上就空了，把枪掏回来。
        // 阵亡时不掏（bReEquipWeapon=false）：紧接着的死亡流程就会把枪全掉光。
        //
        // 判一下"手上是不是真空了"：玩家可以在最后一把刀还在飞的时候按 1/2 掏枪，
        // 收招时 DestroyMeleeWeapon 动的是**收在挂点上的**那把刀，不会碰他手上的枪 ——
        // 这时候再 EquipBestOwnedWeapon 一次会把他刚掏出来的手枪顶掉、换回主武器。
        if (bReEquipWeapon && !GetEquippedWeapon())
        {
            // 表演还在播就先等它播完 —— 立刻掏枪会用枪的 equip 蒙太奇把最后那一刀顶掉
            //（右键"一次全扔"那条最长，用户 2026-09-18："没杀掉人 montage 会直接被掏枪打断，这里要播完"）。
            // 这段时间里手上是空的（刀确实全扔出去了），玩家自己按 1/2/3 挑枪也照常能用 ——
            // 那属于主动打断，回调里会判掉，不会掏两把。
            if (EquipDelay > 0.f)
            {
                GetWorldTimerManager().SetTimer(BladeStormReEquipTimer, this,
                    &ABlasterCharacter::OnBladeStormReEquipDelayElapsed, EquipDelay, false);
            }
            else
            {
                Combat->EquipBestOwnedWeapon();
            }
        }
    }

    // ⚠️ 扣大招点就在这里 —— **不在按下 X 的时候**（用户要求："飞刀用完大招充能开始重新算"）。
    // 和 Phoenix 把扣点写在 ServerReturnToRunItBack 里是同一套路：时机按技能定。
    //
    // 三条收招路径（刀扔空 / Duration 到期 / 阵亡）都汇到 ServerEndUltimate 再进这里，
    // 所以扣点只写在这一处：三条路都扣、也只扣一次。
    //
    // 到期/阵亡也要扣 —— 刀是"用掉了"不是"没放出来"，Valorant 里死在大招上同样消耗。
    // 清零后大招点从 0 开始重攒；大招生效期间（点数是满的）攒的点会被上限夹掉，一并清在这里。
    if (ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>())
    {
        PS->SpendAllUltPoints();
    }
}

void ABlasterCharacter::ServerRequestBladeStormEnd()
{
    if (!HasAuthority()) return;
    if (ActiveUltimate != EActiveUltimate::EUA_BladeStorm) return;

    // 一次开火里 Fire() 会被调不止一次（本地预测一次 + 多播一次），别排出一串定时器
    if (GetWorldTimerManager().IsTimerActive(BladeStormDepleteTimer)) return;

    GetWorldTimerManager().SetTimerForNextTick(this, &ABlasterCharacter::OnBladeStormDepletedNextTick);
}

float ABlasterCharacter::ResolveBladeStormEquipDelay() const
{
    // 近战槽里那把就是飞刀（大招生效期间 ServerStartBladeStorm 塞进去的，收招前一直在）。
    const AJettKnives* Knives = Combat ? Cast<AJettKnives>(Combat->GetMeleeWeapon()) : nullptr;
    if (Knives == nullptr) return 0.f;

    return Knives->GetAttackPresentationRemainingTime();
}

void ABlasterCharacter::OnBladeStormReEquipDelayElapsed()
{
    if (!HasAuthority() || !Combat) return;

    /*
     * 等这段表演的工夫里，玩家可能已经自己做了别的选择，逐条判掉：
     *   · 手上已经有武器了（他自己按了 1/2/3 挑枪）→ 再掏一次会把他挑的那把顶掉
     *   · 又有大招在生效（重新开了一次刃风暴）→ 这时候掏枪等于把他的飞刀收走
     *   · 死了 → 枪已经全掉光，EquipBestOwnedWeapon 反正也找不到东西，但早点走更清楚
     */
    if (GetEquippedWeapon()) return;
    if (IsUltimateActive()) return;
    if (bElimmed) return;

    Combat->EquipBestOwnedWeapon();
}

void ABlasterCharacter::OnBladeStormDepletedNextTick()
{
    if (!HasAuthority()) return;
    if (ActiveUltimate != EActiveUltimate::EUA_BladeStorm) return;

    // 又补上刀了？——**最后一把刀打死人**就是这种情况：飞刀改成射线之后，命中结算和
    // 击杀刷新（Ammo 补回 5）都发生在同一帧、就在排这个定时器那次 Fire() 里面。
    // 少了这一句，"扔空的那一下正好杀了人"会被判成收招：人死了、刀也确实补回来了，
    // 大招却当场结束。以后要是加了"捡回飞刀"之类的机制，这一句同样兜得住。
    if (AWeapon* Melee = Combat ? Combat->GetMeleeWeapon() : nullptr)
    {
        if (!Melee->IsEmpty()) return;
    }

    // bFromDeath=false：这不是阵亡作废而是"刀打完了"→ 正常收招，该把枪掏回来
    ServerEndUltimate(/*bFromDeath=*/false);
}

void ABlasterCharacter::ServerSettleActiveUltimate()
{
    if (!HasAuthority()) return;
    if (!IsUltimateActive()) return;

    // bFromDeath=true：这次收尾不是"正常打完"，别做"活着才该做的事"（Jett 不掏枪 ——
    // 枪都还在槽里没掉，掏一下纯属白播音效）。
    //
    // ⚠️ Phoenix 那条路无视 bFromDeath 照做传送 + 满血满甲，这里无害：传送后的坐标没人看得见
    //（角色下一行就被销毁），满血满甲也存不下来 —— GameMode 是在调本函数**之前**读走甲量的
    //（见 RespawnAllPlayers）。
    // 两边都一定要走到的是**扣大招点** —— 那才是回合切换非要补这一下的原因。
    ServerEndUltimate(/*bFromDeath=*/true);
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
    case ECombatState::ECS_EmptyHand:
        /*
         * 空手蒙太奇（技能放完那一段）。
         *
         * 为什么这里才播、技能那边不直接调：
         *   · 服务器那边自己已经在 BeginEmptyHand 里播过了（房主还要兼顾自己那台的手模）
         *   · 其余每台机器（包括**射手本人**）都在这一刻统一播 —— 和换弹 / 掏枪 / 扔雷
         *     走的是同一条路（服务器改状态、其余各端跟 RepNotify）。
         *     射手本人不预测的理由和换弹一样：预测会让本地状态先变成 ECS_EmptyHand，
         *     服务器那次复制到达时 RepNotify 照样会响（引擎是按 bunch 里的属性触发、
         *     不比对当前值），同一条蒙太奇会被从头播第二遍 —— 手会抖一下。
         *
         * 枪的收回不用在这里做：它走的是武器自己的 EWS_Holstered + EquippedWeapon 复制
         * （服务器 BeginEmptyHand 里调 Combat->HolsterEquippedWeapon()），和这里同批到达。
         */
        PlayEmptyHandMontages();

        /*
         * 逐风云补一刀：如果"松手"这一下比本次状态复制**先到**（按得比一个 RTT 还短，
         * 或者云还没飞完就被打断），上面那次播放会让手模从头演 Intro —— 而这边的空手
         * 其实马上就要收了（服务器已经走了 EndCloudburstHold，只等 Outro 的长度）。
         * 直接跳到 Outro，别演一遍起手。
         *
         * bCloudburstHoldActive 是复制属性，和 CombatState 在同一个 bunch 里写完才回调，
         * 所以这里读到的一定是**和这次状态同时**的那个值，不会读到中间态。
         */
        if (!bCloudburstHoldActive)
        {
            JumpEmptyHandToCloudburstOutro();
        }
        break;
    case ECombatState::ECS_Unoccupied:
        /*
         * 离开空手之后一定会走到这儿（收尾顺序是 EmptyHand → Unoccupied → Equip → …
         * → EquipFinish → Unoccupied），在这一台机器上把空手那几条蒙太奇兜底停掉。
         *
         * 为什么本机那份停放在这里、而不是放在 EmptyHandFinish 那次（可能是本机的动画通知
         * 触发的）调用里：**通知可能比状态复制早整整一个 RTT** —— 第一人称那条蒙太奇只在
         * 射手本机播，它的尾帧通知响在"没有权威"的那台机器上，那时状态还是 ECS_EmptyHand，
         * 停掉蒙太奇 = 手模退回 ABP 的**空手**姿势（"冲刺动画播完露一下空手动画机"就是这么来的）。
         *
         * 停在这里而不是"离开空手的那一刻"（ECS_Equip）：掏枪蒙太奇自己会用
         * Montage_Play(bStopAllMontages=true) 把冲刺那条按 0.25s **交叉淡化**接手过来，
         * 那时候插手去硬停只会把这个接头砸掉。等到这儿，掏枪蒙太奇已经播完，
         * 这一停要么是空转（配了掏枪蒙太奇的武器 —— 那条早被停掉了），
         * 要么就是把"没配掏枪蒙太奇、一直定格在末帧"的那条摘掉（兜底）。
         */
        StopEmptyHandMontages();
        break;
    }
}

// ——— 空手蒙太奇（Jett 放完 E / Q 那一段）———

void ABlasterCharacter::BeginEmptyHand(const FBlasterEmptyHandMontages& InMontages, const FVector& InWorldDirection)
{
	ApplyEmptyHand(InMontages, InWorldDirection, /*bRestart=*/false);
}

void ABlasterCharacter::RestartEmptyHand(const FBlasterEmptyHandMontages& InMontages, const FVector& InWorldDirection)
{
	ApplyEmptyHand(InMontages, InWorldDirection, /*bRestart=*/true);
}

void ABlasterCharacter::ApplyEmptyHand(const FBlasterEmptyHandMontages& InMontages,
	const FVector& InWorldDirection, bool bRestart)
{
	// 只有服务器改状态：其余各端跟着 CombatState 复制走（OnRep_CombatState 里播动画）。
	// 这里**故意不做本地预测** —— 预测会让客户端先变成 ECS_EmptyHand，服务器那次复制到达时
	// RepNotify 照样会响（引擎按 bunch 触发、不比对当前值），同一条蒙太奇会从头播第二遍。
	if (!HasAuthority()) return;

	// ⚠ 这里**故意不判"三条全空就返回"**：要不要进空手由技能决定
	//（UBlasterGameplayAbility::IsEmptyHandEnabled），角色只管执行。
	// 走 ABP 状态机路线时传进来的就是三条空槽 —— 那种情况下 PlayEmptyHandMontages() 全部空转，
	// 但"收枪 + 不可打断 + 到点自动掏最强武器"这套照样要生效。

	// 死了 / 大厅里（还没进对局）不折腾。重入那一路也挡：给死人重开一段动画没有意义。
	if (IsElimmed() || bDisableGameplay) return;

	// 这些状态下手上拿的根本不是枪，硬进空手会和它们自己的收尾逻辑打架：
	//   · Planting / Defusing —— 尖刺在手上（安包/拆包/吃球那套自己管收枪和收尾）
	//   · 持闪光 / 治疗选中 —— 枪已经收起来了，退出时它们要掏回自己记的那把
	// 挡住它们 = 这些情况下技能照放、只是不播空手蒙太奇（降级，不是报错）。
	// 重入那一路不用判：这些状态本身就不可能是 ECS_EmptyHand（它们自己的入口挡着）。
	if (bThrowableHolding || bSageHealSelecting) return;

	const bool bAlreadyEmptyHanded = IsEmptyHandLocked();

	if (bAlreadyEmptyHanded)
	{
		// 已经在空手里：
		//   · 普通入口（E 冲完紧接 Q 这种撞车）→ **不重入**：别让第二条蒙太奇把第一条顶掉
		//     （那样状态机的时间对不上，收尾会提前）
		//   · 重入入口（E 二段在 Q 的空手里放出去）→ 把旧的顶掉：停蒙太奇 + 收尾重新计时，
		//     见头文件 RestartEmptyHand 的注释。状态和手上都不动（枪早就收好了）。
		if (!bRestart) return;

		// 旧那条停掉，连带它尾帧的收尾通知一起作废 —— 不会和新那条抢收尾权。
		// 保险丝也清掉：它是恒起的备胎（只是正常会被通知清掉），留着就走的是**旧**那条的长度，
		// 清掉之后下面 StartEmptyHandTimer 会按新那条重算。
		StopEmptyHandMontages();
		GetWorldTimerManager().ClearTimer(EmptyHandTimer);
	}
	else
	{
		const ECombatState State = GetCombatState();
		if (State == ECombatState::ECS_Planting || State == ECombatState::ECS_Defusing) return;

		// 先退瞄准再收枪：UCombatComponent::SetAiming 要求手持武器非空，收完再调就退不掉了
		if (bAiming && Combat) Combat->SetAiming(false);

		// 收枪 —— 手上留空，蒙太奇才是"空手"那套。和拆包 / 吃大招球走同一个函数
		//（EWS_Holstered + 挂回闲置 socket + EquippedWeapon 置空，之后动画机走空手姿态）。
		if (Combat) Combat->HolsterEquippedWeapon();

		SetCombatState(ECombatState::ECS_EmptyHand);
	}

	// 三条指针随 CombatState 同一批复制给远端机（他们靠这个重播）
	EmptyHandMontageFP = InMontages.FirstPerson;
	EmptyHandMontageUB = InMontages.ThirdPersonUpper;
	EmptyHandMontageLB = InMontages.ThirdPersonLower;

	// 方向同样要复制：远端机得跳到**同一个分段**（JumpToSection 是本地操作，
	// 不做这一步的话每个人各播各的方向，看别人放技能就是"他朝右冲、动画却在往前走"）
	EmptyHandDirection = ResolveEmptyHandDirection(InWorldDirection);

	// ★ 重入那一路要把世代号推一下：状态没变（EmptyHand → EmptyHand）、三条蒙太奇也可能正好
	//   没变（Q 和 E 填同一条资产时），而复制是**按值比对**的 —— 远端机什么都收不到，
	//   他们会继续放 Q 那套动画，人却已经冲出去了。世代号是唯一可靠的"重播"信号，
	//   见头文件里那段。普通入口不动它（首次进入由 OnRep_CombatState 负责，两边都发会播两遍）。
	if (bAlreadyEmptyHanded)
	{
		++EmptyHandRestartCount;
	}

	PlayEmptyHandMontages();
	StartEmptyHandTimer();
}

void ABlasterCharacter::OnRep_EmptyHandRestart()
{
	/*
	 * 远端机（含射手本机）：空手中又放了个技能，用**刚复制到**的那三条 + 方向重播一遍。
	 *
	 * 只调 PlayEmptyHandMontages，不去显式停旧的那条（Q 的）：它和新的那条在同一批槽上
	 *（八向蒙太奇都装在 DefaultSlot / UpperBody / LowerBody 上）⇒ 同一个 Slot Group，
	 * 而 PlayEmptyHandMontages 里第一条轨道是带 bStopAllMontages=true 播的 ——
	 * 引擎会先 StopAllMontagesByGroupName 把同组那条收掉。这和**首次进入空手**是同一条路
	 *（那边也是靠它清掉身上残留的冲刺动画），两处保持一致。
	 *
	 * 前提是"还在空手里"：空手已经收尾时不要再把动画播起来（收尾那次状态复制可能先到）。
	 * 所有复制属性在这一整个 bunch 里都写完了才回调 RepNotify，所以这里读到的一定是最新的状态值。
	 */
	if (!IsEmptyHandLocked()) return;

	PlayEmptyHandMontages();
}

EMovementDirection8 ABlasterCharacter::ResolveEmptyHandDirection(const FVector& InWorldDirection) const
{
	// 1) 先看速度：不依赖任何人传参，服务器和本地机看到的是同一个（速度是复制的）。
	//    为什么不让调用方直接读输入：服务器读不到远端玩家的 GetLastMovementInputVector
	//   （LastControlInputVector 只在本地角色更新，远端恒 0 —— 冲刺方向就是踩过这个坑
	//    才另加了一条 RPC，见 Move() 里那段注释）。
	EMovementDirection8 Direction = EMovementDirection8::N;
	if (MovementDirection8::FromVelocity(GetVelocity(), GetActorForwardVector(),
		FMath::Square(EmptyHandDirectionMinSpeed), Direction))
	{
		return Direction;
	}

	// 2) 速度太小（原地站着放技能、或者冲刺刚停下速度已经在衰减）→ 用调用方给的提示。
	//    冲刺类能力会把它那一趟的 DashDirection 传进来，最准。
	//    这里**不看长度**（直接量化方向）：提示是个纯方向，没有"太小"这个说法，
	//    真为零向量时 QuantizeWorldDirection 自己会返回 false。
	if (MovementDirection8::QuantizeWorldDirection(InWorldDirection, GetActorForwardVector(), Direction))
	{
		return Direction;
	}

	// 3) 真的一点方向都没有 → N（朝面朝方向那一段）。
	//    留 N 而不是"随便挑一个"：N 是"站着不动"最自然的那一段，也是资产里最不会缺的那段。
	return EMovementDirection8::N;
}

bool ABlasterCharacter::PlayEmptyHandTrack(UAnimInstance* AnimInstance, UAnimSequenceBase* Asset,
	FName SlotName, EMovementDirection8 Direction, bool bStopAllMontages)
{
	if (AnimInstance == nullptr || Asset == nullptr || SlotName == NAME_None) return false;

	// 已经是蒙太奇 → 它自带槽名和通知，直接播（想用蒙太奇编辑器里的分段/通知也支持）
	if (UAnimMontage* AsMontage = Cast<UAnimMontage>(Asset))
	{
		if (AnimInstance->Montage_Play(AsMontage, 1.f, EMontagePlayReturnType::MontageLength, 0.f, bStopAllMontages) <= 0.f)
		{
			return false;
		}

		/*
		 * 八个方向 = 同一条蒙太奇里的 8 个 section，播完起手就跳到对应那一段。
		 *
		 * 为什么是"先 Play 再 Jump"而不是"从第 N 秒开始播"（Montage_Play 的 InTimeToStartMontageAt）：
		 *   · JumpToSection 走的是 FAnimMontageInstance::JumpToSectionName（AnimMontage.cpp:1722）
		 *     —— 它只改位置（SetPosition）+ 发一次 OnMontagePositionChanged，**不会重来一遍
		 *     blend-in**，所以先 Play 再 Jump 不会有"抖一下"或者混合被重置的问题。
		 *   · 传 InTimeToStartMontageAt 得自己算"这一段从第几秒开始"，资产一改就错；
		 *     分段名是自解释的，加/减一个方向不用动代码。
		 *
		 * 两个已经踩过的引擎细节：
		 *   · 跳过的那几帧上的**通知不会补响**（HandleEvents 的起点就是跳过去之后的位置，
		 *     AnimMontage.cpp:2450/2537）→ 所以走别的分段时，N 那段尾帧挂的收尾通知不会误触发。
		 *   · 分段之间**可能自动接续**（编辑器建段时 UAnimMontage::AddAnimCompositeSection
		 *     会把上一段指向下一段，AnimMontage.cpp:301-341）→ 跳到 SE 播完会顺着往下播 S、SW……
		 *     收尾那边有 StopEmptyHandMontages() 兜着（见 EmptyHandFinish）。
		 *     ★ 本工程这两条 dash 蒙太奇的 NextSectionName 实测都是空的（不接续），
		 *       但新做的资产务必把**每个 section 的 Next Section 手动设成 None**。
		 *
		 * ★ 挑段用的是 FBlasterEmptyHandMontages::ResolveSectionName —— **播放和算时长共用它**：
		 *   先按八向的段名精确找，找不到就退到**四向**那一段
		 *  （NE/SE → E，NW/SW → W，见 MovementDirection8::CollapseToCardinal）——
		 *   第一人称的手模只做了前后左右四个方向，斜着走时按横移那套播。
		 *   两边共用一个函数是刻意的：各写一遍最容易悄悄对不上（挑出不同的段 →
		 *   保险丝长度和实际播的段不一致）。
		 *
		 * 两步都没找到（资产里压根没分段 / 名字全对不上）时不跳、整条播，单方向的老资产照样能用。
		 * 真配错时打一条日志把实际分段名列出来，省得又是"没反应但什么都不报"。
		 */
		const FName SectionName = FBlasterEmptyHandMontages::ResolveSectionName(*AsMontage, Direction);
		if (SectionName != NAME_None)
		{
			AnimInstance->Montage_JumpToSection(SectionName, AsMontage);
		}
		else if (AsMontage->GetNumSections() > 0)
		{
			// 把资产里**实际**有的分段名列出来 —— 这条日志的全部价值就在这儿：
			// 只报"找不到 N"的话，人还得自己去资产里翻到底叫什么。
			FString ActualSections;
			for (int32 Index = 0; Index < AsMontage->GetNumSections(); ++Index)
			{
				if (Index > 0) ActualSections += TEXT("/");
				ActualSections += AsMontage->GetSectionName(Index).ToString();
			}
			// ⚠ 这是**静态函数**，没有 this —— 想报角色名不能写 GetName()（编译器会说
			//   "调用非静态成员函数需要对象"），得从动画实例的 Owner 绕一下。
			const AActor* Owner = AnimInstance->GetOwningActor();
			const FString Message = FString::Printf(
				TEXT("[空手] %s：%s 里找不到分段 '%s'（四向退到 '%s' 也没有；实际有：%s）")
				TEXT("—— 整条播，不会跳段。分段名请起成 N/NE/E/SE/S/SW/W/NW；只有四向的就用 N/E/S/W。"),
				Owner ? *Owner->GetName() : TEXT("<未知>"),
				*AsMontage->GetName(),
				*MovementDirection8::ToSectionName(Direction).ToString(),
				*MovementDirection8::ToSectionName(MovementDirection8::CollapseToCardinal(Direction)).ToString(),
				*ActualSections);

			/*
			 * ★ 分两种资产，日志级别不一样 —— 不然"时间轴型"蒙太奇每播一次就喷一条 Warning。
			 *
			 * 上面这条警告的用途只有一个：**方向型资产配错了方向名**（八段里少写一段、
			 * 或者名字拼错）。方向型资产的特征是"分段名是 N/E/S/W 那一套"。
			 *
			 * 而时间轴型蒙太奇（Intro → Loop → Outro 这种**流程段**，见 UJettCloudburstAbility）
			 * 压根不分方向：它本来就该从第 0 帧整条往下播，"不跳段"是**正确行为**而不是配错。
			 * 这种资产的分段名里一个方向名都找不到 —— 拿这个当判据降成 Verbose：
			 * 默认看不到，真要排查时把日志分类开到 Verbose 就能看到，信息一条没少。
			 */
			bool bLooksDirectional = false;
			for (int32 Index = 0; Index < MovementDirection8::Num; ++Index)
			{
				if (AsMontage->IsValidSectionName(
					MovementDirection8::ToSectionName(static_cast<EMovementDirection8>(Index))))
				{
					bLooksDirectional = true;
					break;
				}
			}

			if (bLooksDirectional)
			{
				UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
			}
			else
			{
				UE_LOG(LogTemp, Verbose, TEXT("%s（该资产没有任何方向名分段，按时间轴型处理属正常）"), *Message);
			}
		}
		return true;
	}

	/*
	 * 裸序列（AnimSequence）—— 按槽名现造一条**动态蒙太奇**再播。
	 *
	 * 为什么不直接 PlayAnimation：单节点动画没有"槽"这个概念，会被动画机的结果整个覆盖掉，
	 * 播了也看不见（和 Weapon.h 里那四条手模蒙太奇同一个坑）。
	 * 为什么不用 UAnimInstance::PlaySlotAnimationAsDynamicMontage_* 那几个现成封装：
	 * 它们内部写死了 `Montage_Play(NewMontage, InPlayRate, MontageLength, InTimeToStartMontageAt)`
	 *（AnimInstance.cpp:2062 附近），bStopAllMontages 用的是默认值 **true** —— 播第二条
	 *（下半身）时会把第一条（上半身）整个停掉，结果永远只有半身在动，而且不报错。
	 * 所以这里自己造、自己带着 bStopAllMontages 播。
	 *
	 * 现造的这条蒙太奇是 NewObject 出来的临时对象（Outer = transient package），不落盘；
	 * 播放期间由 FAnimMontageInstance 持有引用，停了之后自然被 GC 掉。
	 * ⚠ 引擎里 FAnimMontageInstance::HandleEvents 会把 SlotAnimTracks 里那条序列自己的
	 *   通知一起派发，所以挂在**序列**上的动画通知（UAnimNotify_EmptyHandFinished）照样会响。
	 *
	 * ★ 这条路的蒙太奇是现造的，只有一个叫 "Default" 的分段 —— **装不下八个方向**。
	 *   所以 Direction 在这里用不上（不分段、整条播，跳段那步有意省略）。
	 *   要八向就填**真蒙太奇**（8 个 section 那种），见上面蒙太奇分支里的说明。
	 */
	const FMontageBlendSettings BlendIn(0.08f);
	const FMontageBlendSettings BlendOut(0.08f);
	UAnimMontage* Dynamic = UAnimMontage::CreateSlotAnimationAsDynamicMontage_WithBlendSettings(
		Asset, SlotName, BlendIn, BlendOut, 1.f, /*LoopCount=*/1, /*InBlendOutTriggerTime=*/-1.f);
	if (Dynamic == nullptr) return false;

	return AnimInstance->Montage_Play(Dynamic, 1.f, EMontagePlayReturnType::MontageLength, 0.f, bStopAllMontages) > 0.f;
}

void ABlasterCharacter::PlayEmptyHandMontages()
{
	// 第一人称：只有射手本人这台机器有手模（别的机器上 FPArmsMesh 既不渲染、组件 tick 也关着，
	// 往那儿播等于白发一条指令 —— 和 PlayFPArmsMontage 里同一个判断）。
	// 注意是**手模自己的**动画实例（ABP_WushuFP），不是 GetMesh() 那个身体动画机。
	if (IsLocallyControlled() && FPArmsMesh != nullptr)
	{
		PlayEmptyHandTrack(FPArmsMesh->GetAnimInstance(), EmptyHandMontageFP, EmptyHandFPSlotName,
			EmptyHandDirection, /*bStopAllMontages=*/true);
	}

	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (AnimInstance == nullptr) return;

	// 第三人称：上半身 + 下半身两条**要同时播**。
	// 它们必须是同一条身体上两个**不同名**的 Slot 轨道，本来就互不覆盖 ——
	// 关键是第二条必须 bStopAllMontages=false，否则会把第一条顶掉（见 PlayEmptyHandTrack 里那段）。
	// 第一条为 true：进空手这一刻把身上正在播的东西清掉（冲刺残留之类），免得两个动作叠在一起。
	// 用 bFirst 而不是写死"UB 一定是第一条"：某一条留空时，下一条要接着当"清场的那一条"。
	//
	// ★ 两条同时播、槽名又一样时打一条警告：引擎按**名字**收集蒙太奇
	//（FAnimInstanceProxy::SlotEvaluatePose，AnimInstanceProxy.cpp:1841），
	// 动画蓝图里两个同名 Slot 节点会把这两条都收进来按权重归一化后 50/50 混成一条，
	// 上下半身都糊掉 —— 而不是"上半身播 A、下半身播 B"。
	// 不拦下来（用户可能就是要"两条混着用"），但**不能闷声糊**：这条日志是唯一的线索。
	if (EmptyHandUBSlotName == EmptyHandLBSlotName
		&& EmptyHandMontageUB != nullptr && EmptyHandMontageLB != nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[空手] %s：上下半身在同一个槽 '%s' 上各配了一条动画 —— "
			"引擎按槽名收集蒙太奇，两个同名 Slot 节点会把它们 50/50 混成一条。"
			"要上下半身各播各的，请给 EmptyHandLBSlotName 换一个槽名（如 \"LowerBody\"）。"),
			*GetName(), *EmptyHandUBSlotName.ToString());
	}

	bool bFirst = true;
	if (PlayEmptyHandTrack(AnimInstance, EmptyHandMontageUB, EmptyHandUBSlotName, EmptyHandDirection, true))
	{
		bFirst = false;
	}
	PlayEmptyHandTrack(AnimInstance, EmptyHandMontageLB, EmptyHandLBSlotName, EmptyHandDirection, bFirst);
}

/*
 * 在某个动画实例上，把"正在播这条资产"的蒙太奇停掉。
 *
 * 为什么要挨个 FAnimMontageInstance 找，而不是直接 Montage_Stop(资产)：
 *   填的若是**裸序列**，实际在播的是播放那一刻现造出来的动态蒙太奇（另一个对象），
 *   拿序列本身去 Montage_Stop 根本匹配不上 —— 表现就是"分段自动接续的那几段没人停，
 *   动画顺着往下播到蒙太奇末尾"。所以两种都要认：
 *     · 资产本身就是蒙太奇 → 蒙太奇实例的 Montage 指针直接相等
 *     · 资产是序列 → 看实例那条槽轨道里装的（那一条段）是不是它
 *
 * SlotName 传 NAME_None = **所有槽轨都翻一遍**（调用方说不出这条动画当时播进了哪个槽时用这个，
 * 比如尖刺包那几段 —— 填的是序列的话槽名是调用时给的，隔了一段时间再回来停未必还记得）。
 */
static void StopMontageInstancesUsingAsset(UAnimInstance* AnimInstance, const UAnimSequenceBase* Asset,
	FName SlotName, float BlendOutTime = 0.f)
{
	if (AnimInstance == nullptr || Asset == nullptr) return;

	// "这条资产被装在这个蒙太奇里"吗 —— 只看 SlotName 那条轨，SlotName 为空时全部翻一遍
	auto MontageContainsAsset = [Asset, SlotName](const UAnimMontage* Montage)
	{
		if (SlotName.IsNone())
		{
			for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
			{
				for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
				{
					if (Segment.GetAnimReference() == Asset) return true;
				}
			}
			return false;
		}

		const FAnimTrack* AnimTrack = Montage->GetAnimationData(SlotName);
		if (AnimTrack == nullptr) return false;
		for (const FAnimSegment& Segment : AnimTrack->AnimSegments)
		{
			if (Segment.GetAnimReference() == Asset) return true;
		}
		return false;
	};

	// 先收集、后停：Montage_Stop 会把元素从 MontageInstances 里摘掉（还会回调
	// OnMontageInstanceStopped），边遍历边删会漏掉后面的元素。
	TArray<UAnimMontage*, TInlineAllocator<2>> ToStop;
	for (FAnimMontageInstance* MontageInstance : AnimInstance->MontageInstances)
	{
		if (MontageInstance == nullptr || MontageInstance->Montage == nullptr) continue;

		if (MontageInstance->Montage == Asset)
		{
			ToStop.AddUnique(MontageInstance->Montage);
			continue;
		}
		if (MontageContainsAsset(MontageInstance->Montage))
		{
			ToStop.AddUnique(MontageInstance->Montage);
		}
	}

	// 混合时间 0 = 立刻断掉。收尾紧接着就要播掏枪，留混合时间只会让两条动画叠一下。
	//（持投掷物那几段动作现在也是这么停的，见 ABlasterCharacter::StopThrowableMontages。）
	for (UAnimMontage* Montage : ToStop)
	{
		AnimInstance->Montage_Stop(BlendOutTime, Montage);
	}
}

void ABlasterCharacter::StopAnimAssetOnMesh(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Asset,
	float BlendOutTime)
{
	if (MeshComp == nullptr || Asset == nullptr) return;

	// 一个网格一个动画实例（手模和身体是两个），所以从网格拿实例就够，不用调用方再传。
	StopMontageInstancesUsingAsset(MeshComp->GetAnimInstance(), Asset, NAME_None, BlendOutTime);
}

void ABlasterCharacter::StopEmptyHandMontages()
{
	// 本机局部操作（和播的时候一样，各端各自管自己那台 —— EmptyHandFinish 在每台机器上都会被调到）。
	//
	// 为什么必须停（不停的话是**功能性**问题，不只是不好看）：
	//   ① 这几条蒙太奇的 Enable Auto Blend Out 关着（要"停在末帧"），不会自己掉下来；
	//   ② 分段上万一留了 Next Section（编辑器建段时引擎会顺手把上一段指向下一段，
	//      UAnimMontage::AddAnimCompositeSection，AnimMontage.cpp:301-341），八个方向挤在
	//      一条蒙太奇里时这段自动接续意味着"跳到 SE 播完会接着播 S、SW……"，一路播到末尾 ——
	//      人已经掏完枪了动画还在空手挥。
	//   正确做法是每个 section 的 Next Section 手动设成 None（本工程这两条蒙太奇已经是空的），
	//   这里再兜一层：收尾时把这条轨道收到的头，谁来建的资产都不会漏。
	UAnimInstance* BodyInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	StopMontageInstancesUsingAsset(BodyInstance, EmptyHandMontageUB, EmptyHandUBSlotName);
	StopMontageInstancesUsingAsset(BodyInstance, EmptyHandMontageLB, EmptyHandLBSlotName);

	// 第一人称手模是**另一个**动画实例（ABP_WushuFP），得单独停
	StopMontageInstancesUsingAsset(FPArmsMesh ? FPArmsMesh->GetAnimInstance() : nullptr,
		EmptyHandMontageFP, EmptyHandFPSlotName);
}

// 这条资产上挂了空手收尾通知吗。蒙太奇的分段通知和裸序列自身的通知都在同一个数组里
//（UAnimSequenceBase::Notifies），所以两条路都覆盖得到。
static bool HasEmptyHandFinishNotify(const UAnimSequenceBase* Asset)
{
	if (Asset == nullptr) return false;

	for (const FAnimNotifyEvent& Event : Asset->Notifies)
	{
		if (Event.Notify && Event.Notify->IsA<UAnimNotify_EmptyHandFinished>())
		{
			return true;
		}
	}
	return false;
}

void ABlasterCharacter::StartEmptyHandTimer()
{
	if (!HasAuthority()) return;

	/*
	 * 保险丝 = 服务器上"没人收尾就把人放出来"的备胎。**恒起**，但正常情况根本不会响：
	 * 主路是身体那条（第三人称）蒙太奇上的收尾通知 UAnimNotify_EmptyHandFinished，
	 * 通知一到就调 EmptyHandFinish，那一函数顺手 ClearTimer(EmptyHandTimer) 把它清掉。
	 *
	 * ★ 为什么留着保险丝（而不是"有通知就不起"，那是上一版的写法）：通知是**资产数据**，
	 *   会丢 —— 换骨架 / 换 ABP、或者通知只挂在某一个分段上而这次播的偏偏是另一个分段，
	 *   通知就是不响。而 ECS_EmptyHand 是**不可打断**状态：没人收尾 = 这一整回合开不了火、
	 *   换不了弹、切不了枪，人就卡在空手里。备胎的作用只是把最坏情况从"卡死"降成
	 *   "多空手站 EmptyHandFuseExtraDelay 那零点几秒"。
	 *（顺带把它也覆盖了：万一身体那条蒙太奇在服务器上不 tick 动画 —— 那种部署下
	 *   通知永远不响，收尾全靠这条保险丝。）
	 *
	 * ★ 身体那条挂着通知时，保险丝**往后推** EmptyHandFuseExtraDelay。两者的时刻本来就挨得
	 *   很近（冲刺实测：通知 0.4998s、按分段长度算出来 0.6167s，只差 0.117s），不留余量就是
	 *   竞速 —— 谁先到全看同一帧里组件 tick 的次序，两台机器可以是两个结果。推后之后顺序
	 *   是确定的：通知先到（正常）→ 保险丝被清；通知没响 → 保险丝到点收尾。
	 *   两条路都收尾也没事：EmptyHandFinish 自带幂等门禁，后到的那次是空转。
	 *
	 * 为什么以**身体那条**为准（不是第一人称那条）：手模（FPArmsMesh）只在射手本机播，
	 * 服务器上那条动画根本不 tick。身体那条所有机器都播（含服务器），一台机器一个时刻。
	 * ⚠ 所以只挂在手模那条上的通知**不算数** —— UAnimNotify_EmptyHandFinished 里会把手模
	 *   那条滤掉，下面判的也是 UB / LB 两条（只挂手模那条 = 通知收不了尾，这时不该加余量：
	 *   保险丝就是唯一出口，按分段长度准点放人）。想靠通知收尾就挂在身体那条上。
	 *
	 * 时长 = **第一人称（手模）那条所跳的分段**有多长（只配了第三人称时退回三条里最长的）
	 * —— 这条是保险丝，总得有人把人从空手里放出来。
	 *
	 * ★ 按 FP 收尾而不是"三条里最长"：三条是给三块不同屏幕看的，而"这次动作为什么要等"
	 *   只影响射手本人那块（第三人称那具身体对他 SetOwnerNoSee）。取最长 = 让射手替他
	 *   看不见的那条动画等 —— 实测 N 方向 FP 0.6167s / TP 0.8333s，手模会在末帧上
	 *   定格 0.2167s 才等到掏枪（S 0.1061 / W 0.1333 / E 0.0333）。
	 *   代价见 FBlasterEmptyHandMontages::GetFuseDuration 的注释（别人的屏幕上冲刺尾巴被切）。
	 *
	 * ★ 必须是**分段**的长度，不能拿整条蒙太奇的长度：八个方向的动画装在同一个蒙太奇里，
	 *   GetPlayLength() 拿到的是八段加起来的总长 —— 用它当保险丝等于给了 8 倍的时间，
	 *   表现是"这段动画早播完了、人也还空着手站半天"。
	 */
	float Duration = FBlasterEmptyHandMontages::GetFuseDuration(
		EmptyHandMontageFP, EmptyHandMontageUB, EmptyHandMontageLB, EmptyHandDirection);

	// 一个槽都没填（走 ABP 状态机那条路）时算不出长度，用配置的兜底值。
	// 那个值要**不小于**状态机的长度，否则保险丝会在动画播到一半时把人拉回 ECS_Unoccupied。
	if (Duration <= 0.f) Duration = EmptyHandFallbackDuration;

	if (HasEmptyHandFinishNotify(EmptyHandMontageUB) || HasEmptyHandFinishNotify(EmptyHandMontageLB))
	{
		Duration += EmptyHandFuseExtraDelay;
	}

	GetWorldTimerManager().SetTimer(EmptyHandTimer, this, &ABlasterCharacter::EmptyHandTimerFinished, Duration, false);
}

void ABlasterCharacter::EmptyHandTimerFinished()
{
	EmptyHandFinish();
}

void ABlasterCharacter::EmptyHandFinish()
{
	// 幂等门禁：状态已经不是 ECS_EmptyHand（计时器先收的尾 / 通知重复响）就直接走人
	if (!IsEmptyHandLocked()) return;

	/*
	 * 只有服务器真的收尾；**非权威机器上这一整函数就是空转**（状态靠复制回来）。
	 * 客户端也改一遍的话会和服务器的值打架，而且它手上根本不知道"最强的武器"是哪把
	 *（EquipBestOwnedWeapon 是权威专用的）。
	 *
	 * ★ 这一句**必须排在停蒙太奇之前**。收尾通知会调到这里来，而非权威机器上这条通知响的
	 *   时刻是**本机自己那条蒙太奇**的播放进度，和服务器真正收尾那一刻差着一个复制延迟。
	 *   这时状态还是 ECS_EmptyHand，把蒙太奇停掉 = 手模立刻退回 ABP 的**空手**姿势，
	 *   一直到"状态离开空手"复制过来为止 —— 表现就是"动画播完露一下空手动画机"。
	 *   所以本机这份停不在这儿做：它由"状态真的离开空手"驱动（见 OnRep_CombatState）。
	 */
	if (!HasAuthority()) return;

	/*
	 * 停掉空手那几条蒙太奇。
	 *
	 * 为什么必须停：这几条蒙太奇的 Enable Auto Blend Out 是关掉的（要"停在末帧"不往回混），
	 * 它们不会自己掉下来；而且分段上万一留了 Next Section（编辑器建段时引擎会顺手把上一段
	 * 指向下一段），跳到中间某一段播完还会顺着往下播到末尾。
	 * ★ 本工程这两条 dash 蒙太奇实测四段/八段的 NextSectionName 都是空的（不接续），
	 *   所以"一路播到尾"目前不会发生 —— 但这里照样兜一层：收尾之后不该还有空手动画在动，
	 *   谁来建的资产都不漏。
	 */
	StopEmptyHandMontages();

	GetWorldTimerManager().ClearTimer(EmptyHandTimer);

	/*
	 * 顺序不能反：先放回 ECS_Unoccupied，再掏枪。
	 * EquipBestOwnedWeapon() 自己走 CanChangeWeapon()（只放行 Unoccupied / Equip），
	 * 状态还停在 ECS_EmptyHand 的话它会把自己挡掉 —— 表现就是"空手走一整回合"，
	 * 而且什么都不报。掏枪本身会再把状态推成 ECS_Equip（掏枪动画那段）。
	 */
	SetCombatState(ECombatState::ECS_Unoccupied);

	// 掏最强的武器：主武器优先，没主武器才掏副武器。
	// 两把都没有（比如刚丢完枪）就什么都不做，停在空手 —— 此时 CanFire 因为
	// EquippedWeapon==nullptr 也不会开火，不会崩。
	if (Combat) Combat->EquipBestOwnedWeapon();

	/*
	 * 兜底：空手收了尾，"按住控云"这一段也就结束了。
	 *
	 * 正常路径上它早就是 false 了（松手 / 云绽放都会先调 EndCloudburstHold）—— 走到这儿的
	 * 只有"空手被别的出口收掉的"那几种：保险丝到点、被打死、回合结束的重置。
	 * 不清的话云会一直以为你还按着 C（一直跟准心），下一次放云也会一出生就受控。
	 */
	bCloudburstHoldActive = false;
}

// ——— 逐风云（C）的"按住控云" ———

void ABlasterCharacter::BeginCloudburstHold()
{
	// 只有服务器置位：客户端那份靠复制过来（它要拿这个值决定跳不跳 Outro）。
	if (!HasAuthority()) return;

	bCloudburstHoldActive = true;
}

void ABlasterCharacter::EndCloudburstHold()
{
	if (!HasAuthority()) return;
	if (!bCloudburstHoldActive) return;		// 幂等：松手 / 云绽放 / 兜底可能都来一遍

	bCloudburstHoldActive = false;

	// 权威机器上的 RepNotify 不会响，所以本机这一跳得自己走
	// —— 客户端那台走 OnRep_CloudburstHold，两条路不会同时落在同一台机器上。
	JumpEmptyHandToCloudburstOutro();

	// 空手已经不在（被打死了之类）就到此为止：标志清掉就够了。
	if (!IsEmptyHandLocked()) return;

	/*
	 * 把保险丝改成"Outro 那一段有多长"，到点由 EmptyHandTimerFinished → EmptyHandFinish 掏枪。
	 *
	 * ★ 这一段**没有**别的出口：时间轴型蒙太奇的分段名（Intro/Loop/Outro）不是方向名，
	 *   走不到 HasEmptyHandFinishNotify 那条"靠身体蒙太奇上的通知收尾"的路，
	 *   所以保险丝在这儿是**主路**而不是备胎（对照 EmptyHandFinish 上面那段注释）。
	 *
	 * 取不到长度（没配 Outro 段 / 手模那条压根没填）→ 传 0，SetEmptyHandFuseDuration 会把
	 * 它夹成一个极小值 = 立刻收尾。降级成"没有 Outro 直接掏枪"，不会把人卡在空手里。
	 */
	float OutroLength = 0.f;
	if (const UAnimMontage* Montage = Cast<UAnimMontage>(EmptyHandMontageFP))
	{
		const int32 SectionIndex = Montage->GetSectionIndex(CloudburstOutroSection);
		if (SectionIndex != INDEX_NONE)
		{
			OutroLength = Montage->GetSectionLength(SectionIndex);
		}
	}

	SetEmptyHandFuseDuration(OutroLength);
}

void ABlasterCharacter::SetEmptyHandFuseDuration(float InDuration)
{
	if (!HasAuthority()) return;
	if (!IsEmptyHandLocked()) return;

	GetWorldTimerManager().ClearTimer(EmptyHandTimer);

	// 0 或负数 = "没有可等的动画了" → 换成极小值，下一拍就收尾。
	// 直接 SetTimer(0) 在语义上是"本帧结束就触发"，但那样过不了"至少等一帧"的直觉，
	// 也给不出一个能看出是降级的值。
	const float Duration = (InDuration > 0.f) ? InDuration : 0.01f;

	GetWorldTimerManager().SetTimer(EmptyHandTimer, this, &ABlasterCharacter::EmptyHandTimerFinished,
		Duration, false);
}

void ABlasterCharacter::ServerEndCloudburstHold_Implementation()
{
	EndCloudburstHold();
}

void ABlasterCharacter::OnRep_CloudburstHold()
{
	/*
	 * 远端机（含**射手本机**）收到"不按了" → 把空手那段跳到 Outro。
	 *
	 * 为什么射手本机也靠这条、而不是"松手那一刻本机直接跳"：
	 *   那条路要求客户端在松手这一拍就知道"服务器确实收尾了"—— 它不知道（可能这一下
	 *   云正好撞墙、或者这一下 RPC 丢了）。而 bCloudburstHoldActive 是服务器算完的结论，
	 *   复制过来就带上了。代价是手模的 Outro 晚一个 RTT 起手 —— 和**进空手**那一拍
	 *   走的是同一条路（FP 那条蒙太奇本来也是收到 ECS_EmptyHand 才开始播的，见 OnRep_CombatState），
	 *   两头的延迟一致，接缝不会错位。
	 */
	if (bCloudburstHoldActive) return;		// 只关心"按下 → 松开"这一个方向

	JumpEmptyHandToCloudburstOutro();
}

void ABlasterCharacter::JumpEmptyHandToCloudburstOutro()
{
	// 空手已经收了尾（复制比这一下晚到 / 保险丝先响）就不要再把 Outro 播起来
	if (!IsEmptyHandLocked()) return;

	// 手模那条：只在本机播（别人的机器上 FPArmsMesh 既不渲染、组件也不 tick）
	if (IsLocallyControlled() && FPArmsMesh != nullptr)
	{
		JumpEmptyHandTrackToOutro(FPArmsMesh->GetAnimInstance(), EmptyHandMontageFP);
	}

	// 身体那两条：所有机器都在播
	UAnimInstance* BodyInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (BodyInstance != nullptr)
	{
		JumpEmptyHandTrackToOutro(BodyInstance, EmptyHandMontageUB);
		JumpEmptyHandTrackToOutro(BodyInstance, EmptyHandMontageLB);
	}
}

void ABlasterCharacter::JumpEmptyHandTrackToOutro(UAnimInstance* AnimInstance, UAnimSequenceBase* Asset)
{
	if (AnimInstance == nullptr) return;

	// 裸序列（现造动态蒙太奇那条路）只有一个 "Default" 分段，没有 Outro 可跳。
	// 不报错：那是"这条槽本来就只配了一段"的正常情况，不是配错。
	UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
	if (Montage == nullptr) return;
	if (!Montage->IsValidSectionName(CloudburstOutroSection)) return;

	// 这一条现在真的在播吗。没在播时 JumpToSection 本来就是空转，但先确认一下更稳：
	// 万一这条蒙太奇正好被别处（武器蒙太奇之类）播在同一个实例上，跳它会改错东西。
	if (!AnimInstance->Montage_IsPlaying(Montage)) return;

	// ★ 已经在那一段上就别再跳：JumpToSection 会把位置设回段首，
	//   重复跳 = Outro 从头又播一遍（表现是回位动作抖一下重来）。
	if (AnimInstance->Montage_GetCurrentSection(Montage) == CloudburstOutroSection) return;

	AnimInstance->Montage_JumpToSection(CloudburstOutroSection, Montage);
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

    // 护甲先吃伤害：按 ArmorAbsorptionRatio 把这一发的一部分划给护甲池，剩下的才落到血上。
    // 甲不够吃（被打穿）时，没吃下的部分原样回到血量上 —— 伤害总量守恒，不凭空蒸发。
    // 只有正伤害才轮得到甲（治疗/负数伤害直接走血量那条）。
    float HealthDamage = Damage;
    if (Damage > 0.f && Armor > 0.f)
    {
        const float ArmorShare = Damage * FMath::Clamp(ArmorAbsorptionRatio, 0.f, 1.f);
        const float Absorbed = FMath::Min(Armor, ArmorShare);
        Armor = FMath::Clamp(Armor - Absorbed, 0.f, MaxArmor);
        HealthDamage = Damage - Absorbed;
    }

    Health = FMath::Clamp(Health - HealthDamage, 0.0f, MaxHealth);
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

bool ABlasterCharacter::CanEKeyPickupWeapon() const
{
    // 正在游戏内、没在治疗选中/持闪光状态、脚下有可捡武器。
    // （函数名里的 "EKey" 是历史遗留 —— 捡枪现在的键位是 F，见 Pickup()）
    if (bDisableGameplay || bSageHealSelecting || bThrowableHolding) return false;
    if (!Combat || !OverlappingWeapon) return false;
    return true;
}

void ABlasterCharacter::Pickup(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bThrowableHolding) return;

    // 吃球还是捡枪由**服务器**判断，客户端不猜 —— 客户端上的 OverlappingOrb 是空的
    // （重叠判定只在服务器做，和 AWeapon 一样），所以客户端只能发个无参 RPC 让服务器去决定。
    if (HasAuthority()) ServerPickup_Implementation();
    else ServerPickup();
}

void ABlasterCharacter::ServerPickup_Implementation()
{
    // 1) 吃球优先：站在球里、球还在，就先试着蓄力
    // IsValid 不是多余的：OverlappingOrb 是 UPROPERTY 裸指针，球万一被销毁（关卡重载/手动删）
    // 指针不会自动置空，这里挡一下。
    if (IsValid(OverlappingOrb) && OverlappingOrb->IsAvailable())
    {
        if (OverlappingOrb->StartChannel(this)) return;

        // 没吃上（大招已满 / 已经有别人在吃）→ **继续往下走**去试捡枪，而不是直接 return。
        // 满大的玩家站在球旁边时，地上的枪仍然应该能捡起来。
    }

    // 2) 捡枪
    ServerEquipButtonPressed_Implementation();
}

void ABlasterCharacter::PickupCancel(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    // 一段式蓄力：松开 F 就是打断、进度清零（球留在原地可以重按）。
    // 没在吃球时这里是空操作（CancelOrbChannel 找不到 ChannelingOrb 就什么都不干）。
    CancelOrbChannel(true);
}

void ABlasterCharacter::CancelOrbChannel(bool bRestoreWeapon)
{
    if (HasAuthority())
    {
        if (ChannelingOrb) ChannelingOrb->CancelChannel(bRestoreWeapon);
    }
    else
    {
        ServerCancelOrbChannel(bRestoreWeapon);
    }
}

void ABlasterCharacter::ServerCancelOrbChannel_Implementation(bool bRestoreWeapon)
{
    // 服务器从自己这份 ChannelingOrb 找目标，不采信客户端传来的球
    if (ChannelingOrb) ChannelingOrb->CancelChannel(bRestoreWeapon);
}

void ABlasterCharacter::Reload(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bThrowableHolding)return;
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
    // 开着封烟地图时右键是"封烟"，不是开镜（理由同 FireStart 那道保险）。
    // 收尾的 AimEnd 不需要挡：AimStart 从没跑过，SetAiming(false) 是空操作，
    // 而且玩家按 E 开图时如果本来在开镜，松手这一下正好把镜收掉。
    if (const ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsSmokeMapOpen())
        {
            return;
        }
    }

    if (bDisableGameplay)return;

    // 手上拿着技能投掷物时右键的语义（一律不进瞄准）
    //   曲线闪光 → 右拐抛掷（左键=左拐，两个键各管一拐是它自己的约定）
    //   火球     → 什么都不做（火球只认左键，2026-09-21 用户改的；刻意留空，不是漏做）
    //   火墙     → 什么都不做（火墙只认左键。刻意留空，不是漏做）
    if (bThrowableHolding)
    {
        if (HeldThrowableKind == EBlasterThrowableKind::Flash)
        {
            ThrowCurveball(false);
        }
        return;
    }

    // Jett 刃风暴：手上是飞刀时右键 = 一次性把剩下的刀全甩出去（User 要求的 Valorant 同款），
    // 不进瞄准。TryThrowAllKnives 返回 true 就表示"这次右键已经被飞刀吃掉了"。
    // 放在 SetAiming 之前：飞刀本来就不能开镜（CanScope() 只对狙击为真），
    // 让它走 SetAiming 只会白白把移速降成瞄准速度。
    if (Combat && Combat->TryThrowAllKnives())
    {
        return;
    }

    // 近战（3 号槽的刀）：右键 = 重击，同样不进瞄准。
    // 和飞刀那条是同一套路数（吃掉右键 + 走正常的开火链路），放在 SetAiming 之前 ——
    // 刀本来就不能瞄准（bCanAim=false，SetAiming(true) 会被自己挡掉），
    // 让它走那一步只会白白把移速降成瞄准速度。
    if (Combat && Combat->TryMeleeHeavyAttack())
    {
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
    if (bThrowableHolding) return; // 手持投掷物时 AimStart 直接把它丢出去了、没进瞄准，忽略收尾
    if (Combat)
    {
        Combat->SetAiming(false);
    }
}

void ABlasterCharacter::FireStart(const FInputActionValue& Value)
{
    if (bDisableGameplay)return;

    // 开着封烟地图时左键是"选点"，不是开火。
    // 理论上左键已经被 CloveSmokeMapWidget::NativeOnMouseButtonDown 返回 Handled 吃掉了
    //（事件在 Slate 层就断掉，根本传不到 viewport），这里只是第二道保险 ——
    // 万一哪天改成 UIOnly 之外的输入模式、或者 widget 命中失败，代价是走火，太难看。
    // 和 bSageHealSelecting / bThrowableHolding 是同一套"左键被技能占用"的处理。
    if (const ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsSmokeMapOpen())
        {
            return;
        }
    }

    // 手上拿着技能投掷物：左键那一下的语义由种类决定（收枪状态下本就不会开枪）
    //   曲线闪光 → 左拐抛掷
    //   火球     → **丢出去**（和闪光一样是一按就出去，整条路数完全相同，只是不需要方向 RPC）
    //   火墙     → **发射并开始控球**（按住 = 球跟准心，松手 = 球自己直飞）
    if (bThrowableHolding)
    {
        if (HeldThrowableKind == EBlasterThrowableKind::Wall)
        {
            // 已经在飞了就不理会：按住期间的重复 Started（自动连发/输入重触发）没有语义
            if (!bBlazeFiring)
            {
                StartBlazeFire();
            }
        }
        else if (HeldThrowableKind == EBlasterThrowableKind::Fireball)
        {
            // ThrowFireball 自己会 ExitThrowableHoldLocal，所以这一下之后 bThrowableHolding 已经是 false；
            // 紧接着的那次 FireEnd（松左键）落在"手上没东西"那条路，是空转 —— 同闪光。
            ThrowFireball();
        }
        else
        {
            ThrowCurveball(true);
        }
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

    /*
     * 火墙：**松开左键 = 松手**，球从此自己直线飞完（用户的要求："和 jett 的 c 松手了一样
     * 让球自己飞"），人这边收尾掏枪。
     *
     * 放在 FireButtonPressed(false) 之前：这一段里手上没有枪，那一下本来就是空转，
     * 但"还按着左键"这件事只有这里知道 —— 球每帧读的就是它（见 IsBlazeFiring）。
     */
    if (bBlazeFiring)
    {
        StopBlazeFire();
        return;
    }

    if (Combat)
    {
        Combat->FireButtonPressed(false);
    }
}

void ABlasterCharacter::ThrowGrenade(const FInputActionValue& Value)
{

    if (bDisableGameplay || bSageHealSelecting || bThrowableHolding)return;
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
        // ★ 必须是 true。站着不动时让 Actor 朝向跟视角走，别人（以及服务器上的
        //   lag compensation / 小地图队友朝向锥）才看得到你在转。
        //
        // 这里曾经是 false —— 那是为"冻结 Actor、改由 Mesh 根骨骼旋转"那套方案写的：
        // 配套的 ABP 必须有 AnimGraphNode_RotateRootBone（吃 GetRootRotationYaw()）。
        // 旧的 ABP_Blaster1 有，换成瓦模型后新做的 ABP_BlasterCharacter **没有**，
        // 于是 AO_Rotation 算出来没人用，别人眼里就是完全不转。
        // 与其去给新 ABP 补一套 turn-in-place，不如把朝向交回 Actor：
        // 胶囊和命中盒的朝向必须和视觉一致，否则射击判定和小地图都是歪的。
        // （代价：转身从"骨骼平滑转"变成"Actor 直接对齐视角"，没有过渡动画。）
        bUseControllerRotationYaw = true;
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

void ABlasterCharacter::Inspect(const FInputActionValue& Value)
{
    // 纯表现动作，但时机不对会很难看：换弹/甩雷/掏着尖刺包时手是"忙"的，
    // 这时候插一条检视动画会把手从正在做的事里硬拽出来（Montage_Play 会停掉前一条蒙太奇，
    // 而换弹的计时器照常在跑 —— 看上去就是"手提前放下了，弹匣过了会儿自己满上"）。
    if (bDisableGameplay || IsElimmed()) return;
    // 和 Reload 一样的两个开关：拿着奶球/技能球时枪是收着的，这时候播检视手会乱
    if (bSageHealSelecting || bThrowableHolding) return;
    if (IsSpikeDrawn()) return;
    if (IsAiming()) return;
    if (GetCombatState() != ECombatState::ECS_Unoccupied) return;

    if (EquippedWeapon == nullptr) return;
    // 手（FPInspectMontage）和枪（InspectAnimation）各播各的：
    // 枪那条所有机器都播（远端看得见你在转枪），手那条只本机播。
    EquippedWeapon->PlayFPInspectMontage();
    EquippedWeapon->PlayInspectAnimation();
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

    // E 键和捡武器共用一个键：脚下有可捡武器时，E 只捡枪、不开技能（Equip 里会执行拾取）。
    // bSageHealSelecting / bThrowableHolding 时 CanEKeyPickupWeapon 为 false，下面技能照常走。
    if (CanEKeyPickupWeapon())
    {
        return;
    }

    // Clove 暮蝶：E = 开关封烟选点地图（界面和状态机在 PC 上，这里只转发）。
    // 放在 CanEKeyPickupWeapon 之后 —— 脚下有枪时 E 仍然是捡枪，和其他英雄一致。
    // 但"地图已经开着"这条要在捡枪之前处理：那时候 E 只可能是确认/退出，
    // 不该因为脚边恰好掉了一把枪就按不动。
    if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsSmokeMapOpen())
        {
            BPC->SmokeMapKeyPressed();
            return;
        }
    }
    if (HasSmokeAbility())
    {
        if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
        {
            BPC->SmokeMapKeyPressed();
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

const UBlasterGameplayAbility* ABlasterCharacter::GetAbilityCDOBySkillType(EBlasterSkillType SkillType) const
{
    for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
    {
        if (!AbilityClass) continue;
        const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
        if (CDO && CDO->SkillType == SkillType)
        {
            return CDO;
        }
    }
    return nullptr;
}

void ABlasterCharacter::SkillQPressed(const FInputActionValue& Value)
{
    // Lobby（选人）里禁用技能，和 E / X 同一个门槛
    if (bDisableGameplay) return;
    if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsInLobby()) return;
    }

    /*
     * Q 键有两个可能的语义，按"这个角色有没有那个技能"分派：
     *
     *   · Phoenix 火球 —— Q = 拿起/收起火球（丢出去是**左键**，见 FireStart）。它走的是持投掷物那套
     *     （收枪、1/2/3 可打断），所以**不能**在这里直接激活能力：拿起本身不消耗充能、
     *     也不生成任何东西，激活能力是"丢出去"那一拍的事（见 ThrowFireball）。
     *   · Jett 腾空 —— Q = 直接激活能力，一次性往上窜。
     *
     * 判据放在前面：一个角色不可能同时有两个 Q 键技能，先问火球更省事，
     * 而且读代码时"Q 是火球还是腾空"一眼就能看到。
     */
    if (HasFireballAbility())
    {
        FireballPressed();
        return;
    }

    // 按 SkillType 找这个角色有没有 Q 技能。找不到直接返回 —— 不是错误，
    // Sage/Phoenix/Clove 的 DefaultAbilities 里就没有 Updraft。
    const UBlasterGameplayAbility* Ability = GetAbilityCDOBySkillType(EBlasterSkillType::Updraft);
    if (!Ability || !AbilitySystemComponent) return;

    // 和 X 大招同一套：按类激活，不走 gameplay tag（类是从 DefaultAbilities 现查出来的，
    // 没必要为它再注册一个 tag）。充能够不够由 CanActivateAbility 自己挡。
    AbilitySystemComponent->TryActivateAbilityByClass(Ability->GetClass());
}

void ABlasterCharacter::SkillCPressed(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;
    if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsInLobby()) return;
    }

    /*
     * C 键有两个可能的语义，按"这个角色有没有那个技能"分派（和 Q 键火球/腾空同一个套路）：
     *
     *   · Phoenix 火墙 —— C = **武装**（收枪、进持投掷物态），左键才是发射。
     *     和火球同理：武装本身不消耗充能、也不生成任何东西，所以不能在这里激活能力。
     *   · Jett 逐风云 —— C = 放云 + 开始按住控云（下面那条）。
     *
     * 一个角色不可能同时有两个 C 技能，先问火墙更省事。
     */
    if (HasBlazeAbility())
    {
        BlazePressed();
        return;
    }

    const UBlasterGameplayAbility* Ability = GetAbilityCDOBySkillType(EBlasterSkillType::Cloudburst);
    if (!Ability || !AbilitySystemComponent) return;

    AbilitySystemComponent->TryActivateAbilityByClass(Ability->GetClass());

    /*
     * 技能放出去了，接下来就是"按住控云"（逐风云 C 专属，别的技能没有这一段：
     * 它们的空手是"动画播完 → 收尾"，逐风云是"松手/云绽放 → 收尾"）。
     *
     * ⚠ 这里**不**本机置 bCloudburstHoldActive，也不本机进空手：
     *   · 它是复制属性，客户端自己写会和服务器的值打架（GAS 那边 LocalPredicted
     *     同样只让服务器改状态，见 ApplyEmptyHand 里那段注释）；
     *   · 云和空手都是服务器那一拍的事（UJettCloudburstAbility::ExecuteSkillAction）。
     * 客户端这一下只是"请求放技能"，真正的"开始按住"由服务器在放云那一拍调
     * BeginCloudburstHold —— 时序上早不了也晚不了，云一出生就知道自己在不在受控状态。
     */
}

void ABlasterCharacter::SkillCReleased(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    /*
     * 火墙那条路：**松开 C 什么都不做**。
     *
     * 它和逐风云虽然都从 C 键进，但"松开"的语义完全不同：逐风云按住的**就是 C**
     *（按住控云、松手收尾），而火墙按 C 只是一次"武装/收起"的开关 ——
     * 真正按住的是**左键**（发射 + 控球），收尾也认左键松开。
     *
     * ⚠ 必须在这里就返回：IA_SkillC 上没有 Hold 触发器，**点一下 C 也会走一遍
     *   Started → Completed**（玩家眼里的"按一下"在输入层是"按下 + 抬起"）。
     *   要是让它落进下面逐风云那套，按一下 C 就会当场把刚武装好的火墙收掉 ——
     *   表现是"按 C 完全没反应"，很难往"松开事件把它撤了"上想。
     */
    if (HasBlazeAbility()) return;

    // 权威机器（房主 / 单机）自己就是判定方，直接收尾；客户端转给服务器。
    // 客户端不在这儿做任何"本地预演"：手模那段的 Outro 是靠 bCloudburstHoldActive
    // 复制回来跳的（见 OnRep_CloudburstHold 里为什么），早跳晚跳都会抖。
    if (HasAuthority())
    {
        EndCloudburstHold();
    }
    else
    {
        ServerEndCloudburstHold();
    }
}

void ABlasterCharacter::UltimatePressed(const FInputActionValue& Value)
{
    if (bDisableGameplay) return;

    // Lobby（选人）里禁用大招，和 E 键那套技能同一个门槛
    if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(GetController()))
    {
        if (BPC->IsInLobby()) return;
    }

    // 手被别的长按动作占着就放不了大招：
    //   · ECS_Defusing —— 拆包 / 吃大招球（两者共用这个状态），此时是空手长按动作中
    //   · 持闪光 / 治疗选中 —— 这两个状态已经把枪收起来了，"退出时掏回武器"和大招会互相打架
    //   · ECS_EmptyHand —— 技能收尾那段空手蒙太奇，**不可打断**（见 CombatState.h）。
    //     这条在技能基类的 CanActivateAbility 里也挡了一次；这里显式列出来是为了
    //     "不可打断的状态"在这个函数里一眼看全（本函数本来就是所有"手占着"的集合）。
    // 这里不替玩家强行打断：这些状态画面上都很显眼，自己取消再按 X 就行；
    // 在这条路径上处理交叉状态（收枪/掏枪顺序、球那边进度清零）只会更容易出错。
    if (GetCombatState() == ECombatState::ECS_Defusing) return;
    if (IsEmptyHandLocked()) return;
    if (bThrowableHolding || bSageHealSelecting) return;

    const UBlasterGameplayAbility* Ult = GetUltimateAbilityCDO();
    if (!Ult || !AbilitySystemComponent) return;

    // 按类激活（不走 gameplay tag）：大招类是从 DefaultAbilities 现查出来的，手上就有，
    // 没必要为它再注册一个 tag。点数没攒满 / 已经在生效中，都由能力自己的 CanActivateAbility 挡掉。
    AbilitySystemComponent->TryActivateAbilityByClass(Ult->GetClass());
}

// --- 大招（X 键）查找 ---

const UBlasterGameplayAbility* ABlasterCharacter::GetUltimateAbilityCDO() const
{
    const UBlasterGameplayAbility* Found = nullptr;

    for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
    {
        if (!AbilityClass) continue;
        const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
        if (!CDO || !CDO->IsUltimate()) continue;

        if (!Found)
        {
            Found = CDO;
            continue;
        }

        // 配了两个 bIsUltimate 的技能 —— 按 X 只会放**数组里靠前**的那个，另一个永远放不出来，
        // 而且看不出来是"没配"还是"被挡了"。典型的踩法：把大招放在父类 BP_BlasterCharacter 上，
        // 子类（Phoenix/Sage）又加了自己那个 → 子类同时继承两个大招。
        UE_LOG(LogTemp, Warning,
            TEXT("[大招] %s 的 DefaultAbilities 里配了不止一个大招（%s 和 %s），只会用靠前的那个。")
            TEXT("子类角色请在自己的 DefaultAbilities 里把父类那个大招删掉。"),
            *GetName(), *Found->GetClass()->GetName(), *CDO->GetClass()->GetName());
    }

    return Found;
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

// --- Phoenix 曲线球 / 火球：共用的"手里拿着技能投掷物"状态 ---
// 交互（用户定的）：
//   E 闪光：E 拿起（收枪）→ 左键=左拐抛 / 右键=右拐抛 → 掏回枪；再按 E 或 1/2/3 打断
//   Q 火球：Q 拿起（收枪）→ **左键丢出去**（2026-09-21 用户改的，之前是右键；右键现在不丢东西）
//           → 掏回枪；再按 Q 或 1/2/3 打断
// 两个技能共用 EnterThrowableHold / ExitThrowableHoldLocal 这一套，靠 HeldThrowableKind 区分。
// ★ 因此火球"丢出去"那一拍和闪光走的是**同一条输入路**（FireStart → Started），
//   区别只在 ThrowFireball 不发方向 RPC（火球方向就是准心朝向，服务器本来就有）。

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

    if (bThrowableHolding)
    {
        // 手上是闪光 → 再按 E = 打断（掏回枪退出）。
        // 手上是火球 → 不理会：两个状态是互斥的，这一下既不该丢火球也不该把火球收掉，
        // 玩家要换技能得先按 Q 收掉（各自管各自的键，出问题好查）。
        if (HeldThrowableKind == EBlasterThrowableKind::Flash)
        {
            ExitThrowableHoldLocal();
        }
        return;
    }

    // 第一段：进入持闪光状态（收枪；有充能才能进，进入本身不扣次数，1/2 可免费打断）
    if (!IsCurveballAvailable()) return;
    EnterThrowableHold(EBlasterThrowableKind::Flash);
}

void ABlasterCharacter::ThrowCurveball(bool bCurveLeft)
{
    if (!bThrowableHolding || HeldThrowableKind != EBlasterThrowableKind::Flash) return;

    // 抛掷方向 RPC（可靠 RPC 同通道有序，先于 CallServerTryActivateAbility 到达 → 服务器 spawn 时能读到）
    ServerSetPendingCurveballSide(bCurveLeft);

    if (AbilitySystemComponent)
    {
        FGameplayTagContainer TagContainer;
        TagContainer.AddTag(BlasterGameplayTags::Ability_Phoenix_Curveball);
        AbilitySystemComponent->TryActivateAbilitiesByTag(TagContainer);
    }

    /*
     * 丢出去这一下：手模那条**本机立刻播**（预测 —— 按下就出手），身体那条由服务器多播
     *（ServerNotifyThrowableThrown → MulticastThrowableThrown），每台机器只会走其中一条。
     *
     * ★ 顺序要紧：**先** BeginThrowableFinisher **再** ExitThrowableHoldLocal。
     *   退出那条路会来停动画（StopThrowableMontages），它看到"正在等收尾"才不动手 ——
     *   没先记上的话，这一下会把自己刚播出去的动作当场停掉，掏枪也不会等它了。
     */
    PlayThrowableThrow(/*bFirstPerson=*/true, EBlasterThrowableKind::Flash);
    BeginThrowableFinisher(EBlasterThrowableKind::Flash, /*bLift=*/false);
    ServerNotifyThrowableThrown(EBlasterThrowableKind::Flash);

    // 本地立即退出持球态（服务器掏枪经复制跟上；若充能已被打空，GA 不激活、只是回归可射击）
    ExitThrowableHoldLocal();
}

// --- Phoenix 火球（Q）：拿起 / 丢出 ---

bool ABlasterCharacter::HasFireballAbility() const
{
    return GetFireballAbilityCDO() != nullptr;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetFireballAbilityCDO() const
{
    for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
    {
        if (!AbilityClass) continue;
        const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
        if (CDO && CDO->SkillType == EBlasterSkillType::Fireball)
        {
            return CDO;
        }
    }
    return nullptr;
}

bool ABlasterCharacter::IsFireballAvailable() const
{
    const UBlasterGameplayAbility* Fireball = GetFireballAbilityCDO();
    if (!Fireball || !Fireball->CooldownEffectClass || !AbilitySystemComponent) return false;
    const int32 ActiveCooldowns = AbilitySystemComponent->GetGameplayEffectCount(Fireball->CooldownEffectClass, nullptr);
    return ActiveCooldowns < Fireball->MaxCharges;
}

void ABlasterCharacter::FireballPressed()
{
    if (bDisableGameplay) return;

    if (bThrowableHolding)
    {
        // 手上是火球 → 再按 Q = 打断（掏回枪退出，和持闪光按 E 一致）
        if (HeldThrowableKind == EBlasterThrowableKind::Fireball)
        {
            ExitThrowableHoldLocal();
        }
        return;
    }

    if (!IsFireballAvailable()) return;
    EnterThrowableHold(EBlasterThrowableKind::Fireball);
}

void ABlasterCharacter::ThrowFireball()
{
    if (!bThrowableHolding || HeldThrowableKind != EBlasterThrowableKind::Fireball) return;

    /*
     * 和曲线闪光那条路的关键区别：**不需要先发方向 RPC**。
     *
     * 曲线闪光要客户端先告诉服务器"往左拐还是往右拐"，是因为那个选择只存在于输入里；
     * 火球的方向就是准心的完整朝向，而服务器本来就有这个射手最新的 ControlRotation
     *（移动同步的一部分），能力直接读即可 —— 少一条 RPC，也就少一处"顺序对不对"的担心。
     *
     * 按类激活（不走 gameplay tag）：火球类是从 DefaultAbilities 现查出来的，
     * 和 Q 腾空 / 大招 / 逐风云同一个套路。
     */
    if (const UBlasterGameplayAbility* Fireball = GetFireballAbilityCDO())
    {
        if (AbilitySystemComponent)
        {
            AbilitySystemComponent->TryActivateAbilityByClass(Fireball->GetClass());
        }
    }

    // 丢出去这一下：手模本机立刻播（预测），身体那条靠服务器多播 —— 同闪光那处（含顺序那条理由）
    PlayThrowableThrow(/*bFirstPerson=*/true, EBlasterThrowableKind::Fireball);
    BeginThrowableFinisher(EBlasterThrowableKind::Fireball, /*bLift=*/false);
    ServerNotifyThrowableThrown(EBlasterThrowableKind::Fireball);

    // 本地立即退出持球态（服务器掏枪经复制跟上；若充能已被打空，GA 不激活、只是回归可射击）
    ExitThrowableHoldLocal();
}

// --- Phoenix 火墙（C）：武装 → 按住左键发射 → 松手放它自己飞 ---
// 交互（用户定的）：
//   C 武装（收枪、手上什么都没有，球是隐形无碰撞的）
//   → **按住左键**发射：球朝准心飞、同时在地上立起烟墙，全程约 1 秒
//   → 松手 = 球自己直线飞完（同 Jett 逐风云松手），人这边收尾掏枪
//   → 武装期间再按 C = 收起来（还没发射才有效）
// 真正干活的在别处：球是 APhoenixBlazeBall、墙是 APhoenixFlameWall、施放是 UPhoenixBlazeAbility。

bool ABlasterCharacter::HasBlazeAbility() const
{
    return GetBlazeAbilityCDO() != nullptr;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetBlazeAbilityCDO() const
{
    // 和火球 / 闪光查同一张表：DefaultAbilities 里 SkillType == Wall 的那个 CDO。
    // ★ 因此 GA_Phoenix_Blaze 上的 SkillType 必须填 Wall —— 填错（或留默认）这里查不到，
    //   表现是"按 C 完全没反应"，而且不会有任何报错。
    return GetAbilityCDOBySkillType(EBlasterSkillType::Wall);
}

bool ABlasterCharacter::IsBlazeAvailable() const
{
    const UBlasterGameplayAbility* Blaze = GetBlazeAbilityCDO();
    if (!Blaze || !Blaze->CooldownEffectClass || !AbilitySystemComponent) return false;
    const int32 ActiveCooldowns = AbilitySystemComponent->GetGameplayEffectCount(Blaze->CooldownEffectClass, nullptr);
    return ActiveCooldowns < Blaze->MaxCharges;
}

void ABlasterCharacter::BlazePressed()
{
    if (bDisableGameplay) return;

    if (bThrowableHolding)
    {
        /*
         * 拿着火墙时再按 C：
         *   · 还在武装（没发射）→ 收起来（和按 Q 收火球、按 E 收闪光一致）
         *   · 球已经在飞了      → **不理会**。用户对这一段的要求是"松左键才放手"，
         *     技能键管不着正在飞的球；硬退的话本地会立刻走掏枪流程，而球还在受控，
         *     两边状态就对不上了（球以为射手还按着，射手屏幕上枪已经掏出来了）。
         * 拿着的是别的技能 → 也不理会（各自管各自的键，出问题好查，同 Q / E 那两处）。
         */
        if (HeldThrowableKind == EBlasterThrowableKind::Wall && !bBlazeFiring)
        {
            ExitThrowableHoldLocal();
        }
        return;
    }

    /*
     * 有充能才能武装。
     * 和火球那边同一个道理，但对火墙更要紧：武装之后还得按左键才真的发射，
     * 而"发射那一下没能激活能力"的兜底是 2.5 秒的保险丝 —— 没充能还不挡的话，
     * 玩家按一下 C 就会白站两秒半。
     */
    if (!IsBlazeAvailable()) return;

    EnterThrowableHold(EBlasterThrowableKind::Wall);
}

void ABlasterCharacter::StartBlazeFire()
{
    if (!bThrowableHolding || HeldThrowableKind != EBlasterThrowableKind::Wall) return;
    if (bBlazeFiring) return;

    bBlazeFiring = true;

    /*
     * 按住左键那一段的动画（Fire = 一次性）。
     *
     * ★ 这一段播完 C++ **什么都不做**（用户定的新分工）：按住期间举着控球的姿势本来就归动画蓝图
     *   （WeaponType = EWT_PhoenixBlaze），Fire 只是"按下去这一下"的一记动作。
     *   收尾（收起那段 Lift + 掏枪）只由"松手"和"球飞完"两件事触发 —— 都走 StopBlazeFire，
     *   保险丝则兜"球根本没生成出来"。所以 Fire 资产照那个技能最长按键时间做就够，
     *   不必再靠它来倒计时。
     *
     * 手模那条**本机立刻播**（预测 —— 按下左键这一刻手就该动），身体那条由服务器多播
     *（见 ServerStartBlazeFire_Implementation 里的 MulticastThrowableBlazePhase），
     * 和曲线闪光 / 火球抛掷那两处是同一套分工。
     *
     * ★ 火墙走的是 Fire / Lift，**不是** Throw：Throw 是"一按就出去"那两个技能的最后一段。
     */
    PlayThrowableFire(/*bFirstPerson=*/true);

    /*
     * 保险丝（本机也起一个）。
     * 正常路径永远不会响：松手、球飞完都会收尾，它兜的是"球根本没生成出来"。
     * 客户端上球没生成最可能的原因是服务器那边没充能 / 没激活成功，而那条 RPC
     * **不会有任何回执** —— 没有这根保险丝，射手会举着手一直卡在持墙态，连枪都掏不出来。
     */
    GetWorldTimerManager().SetTimer(BlazeFireFuseTimer, this, &ABlasterCharacter::StopBlazeFire,
        BlazeFireFuse, false);

    ServerStartBlazeFire();
}

void ABlasterCharacter::ServerStartBlazeFire_Implementation()
{
    if (!bThrowableHolding || HeldThrowableKind != EBlasterThrowableKind::Wall) return;

    /*
     * ★ 这里**不能**拿 bBlazeFiring 当"已经发射过了"的门禁 —— 和 ServerSetThrowableHolding 那处同一个坑：
     *   听服主机上 StartBlazeFire 已经在本地把这个标志置上了，紧接着这条 Server RPC 就在**同一个进程里**
     *   就地执行：一进门被挡掉的话，下面 TryActivateAbilityByClass 永远不跑 ——
     *   主机上按左键 **球根本不生成、墙也就立不起来**（手上有动作，世界里面什么都没有）。
     *   而用户的单机 PIE 测试就是听服主机，所以这条必须是"就地执行也照做"。
     *   "一次按键只激活一次"由调用方保证：StartBlazeFire 自己有 bBlazeFiring 门禁，
     *   客户端那条路本来也只发一次 RPC。
     */
    bBlazeFiring = true;
    GetWorldTimerManager().SetTimer(BlazeFireFuseTimer, this, &ABlasterCharacter::StopBlazeFire,
        BlazeFireFuse, false);

    // 别人屏幕上的"按住控球"（射手自己那台按下时手模已经先播过，身体那条只有这一条路会播）
    MulticastThrowableBlazePhase(/*bFiring=*/true);

    /*
     * 激活能力 = 生成那颗球 + 扣一层充能。
     * 按类激活（不走 gameplay tag）：火墙类是从 DefaultAbilities 现查出来的，
     * 和火球 / 逐风云 / 大招同一个套路。
     *
     * ★ 顺序要紧：bBlazeFiring 必须在激活**之前**置位 —— 球在 BeginPlay 里就开始了自己的
     *   第一帧，而它每帧读的就是服务器上这个标志（IsBlazeFiring）。晚一步的话球出生的
     *   头一帧会以为"射手没在按"，直接照初始方向直飞出去，玩家掰不动。
     */
    if (const UBlasterGameplayAbility* Blaze = GetBlazeAbilityCDO())
    {
        if (AbilitySystemComponent)
        {
            AbilitySystemComponent->TryActivateAbilityByClass(Blaze->GetClass());
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[火墙] ★ 查不到火墙技能 CDO（GetBlazeAbilityCDO 返回空）→ 球不会生成"));
    }
}

void ABlasterCharacter::StopBlazeFire()
{
    // 幂等门禁：松手、球飞完、保险丝，三条路先后都可能到
    if (!bBlazeFiring) return;
    bBlazeFiring = false;
    GetWorldTimerManager().ClearTimer(BlazeFireFuseTimer);

    if (bThrowableHolding && HeldThrowableKind == EBlasterThrowableKind::Wall)
    {
        /*
         * 收尾那一下的动画：Lift（松开左键 / 球飞完，都是这一段）。
         *
         * ★ 顺序要紧：**先**把它们推上去，**再**退出持投掷物态。
         *   退出那条路会来停动画（StopThrowableMontages），它看到"正在等收尾"才不动手 ——
         *   先记上才能让 Lift 活下来，掏枪也才会排在它后面（QueueOrEquipAfterThrow 认的就是它）。
         *   反过来先退出的话，Lift 会被那次停直接吞掉。
         *
         * 手模这条本机立刻播；**别人屏幕上那条身体动画**要过服务器 ——
         * 和按左键那一下同构（那边是服务器直接多播，这边射手先请求）。
         */
        PlayThrowableLift(/*bFirstPerson=*/true);
        BeginThrowableFinisher(EBlasterThrowableKind::Wall, /*bLift=*/true);
        ServerNotifyThrowableBlazePhase(/*bFiring=*/false);

        /*
         * 退出持投掷物态（掏枪那一步会等 Lift 播完，见 QueueOrEquipAfterThrow）。
         *
         * 不需要单独再发一条"停止控球"的 RPC：退出这件事本身就要告诉服务器，
         * 而 ServerExitThrowableHold 里会把服务器那份 bBlazeFiring 一起清掉 ——
         * 在权威机上下面这条 Server RPC 是就地执行，在客户端上它发出去的就是给服务器的通知。
         */
        ExitThrowableHoldLocal();
    }
    // else：手上已经不是火墙了（被别的路先收掉了）。什么都不做 —— 这里再推 Lift 反而会凭空播一段。
}

// --- 两者共用的持投掷物骨架 ---

void ABlasterCharacter::EnterThrowableHold(EBlasterThrowableKind Kind)
{
    // 已经拿着东西了（不管拿的是哪个）就不重复进 —— 换技能必须先收掉手上的，
    // 否则会丢掉上一次记下的那把枪（收枪记录只有一份）。
    if (bThrowableHolding || Kind == EBlasterThrowableKind::None) return;

    // 先退出瞄准再收枪（SetAiming 内部要求有手持武器）
    if (bAiming && Combat) Combat->SetAiming(false);

    bThrowableHolding = true;
    HeldThrowableKind = Kind;

    /*
     * ★ 上一发还在播"丢出去"那一下、枪还没掏回来，就又按了技能键（火球/火墙都能这么接）：
     *   这时 GetEquippedWeapon() 是空的 —— 那把枪正躺在 ThrowablePendingEquipWeapon 里等动画。
     *   直接取会让这一次**记不到枪**，收尾时就没人把它掏回来（表现是"这回合枪没了"）。
     *   所以先把等掏的那把接过来，并且这一下把它从"等掏"里摘出来。
     *
     * 玩家在等的这段时间里自己换过枪（1/2/3 是放行的）时 GetEquippedWeapon() 有值 ——
     * 那就按平常那条走，等掏的那把作废（它还在身上，按 1/2/3 随时能拿出来）。
     */
    AWeapon* WeaponToHolster = GetEquippedWeapon();
    if (WeaponToHolster == nullptr && ThrowablePendingEquipWeapon)
    {
        WeaponToHolster = ThrowablePendingEquipWeapon;
    }
    ThrowablePendingEquipWeapon = nullptr;

    // 本地预测收枪（服务器 RPC 兜底；收枪状态经 EquippedWeapon 复制给所有客户端）
    ThrowableHolsteredWeapon = WeaponToHolster;
    if (Combat && ThrowableHolsteredWeapon)
    {
        ThrowableHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
        Combat->AttachActorToSocket(ThrowableHolsteredWeapon, Combat->GetHolsterSocketForWeapon(ThrowableHolsteredWeapon));
        SetEquippedWeapon(nullptr);
    }

    UpdateHeldThrowableVisual();

    /*
     * 动画：手模那条**本机立刻播**（按下技能键这一刻手上就该有动作，预测，不等服务器）。
     * 身体那条不在这里 —— 它由服务器（ServerSetThrowableHolding）和远端机的 OnRep 驱动，
     * 两条路互斥，每台机器只会走其中一条（见 PlayThrowableSet 上头那段）。
     *
     * 拿起演完 C++ 什么都不做：之后就是动画蓝图那个"举着它待命"的姿势（WeaponType 报的值
     * 在这一刻已经跟着 bThrowableHolding / HeldThrowableKind 变了，见两个动画实例）。
     */
    PlayThrowableEquip(/*bFirstPerson=*/true, Kind);

    ServerSetThrowableHolding(true, Kind);
}

void ABlasterCharacter::ExitThrowableHoldLocal(bool bTellServer)
{
    if (!bThrowableHolding) return;

    // 收下手上那个种类的**前一份**：下面要拿它去停动画（清成 None 之后就不知道停谁的了）
    const EBlasterThrowableKind PreviousKind = HeldThrowableKind;

    bThrowableHolding = false;
    HeldThrowableKind = EBlasterThrowableKind::None;

    // 火墙那段"按住控球"到这儿一定结束（松手 / 球飞完 / 被别的输入打断，三条路都经过这里）
    bBlazeFiring = false;
    GetWorldTimerManager().ClearTimer(BlazeFireFuseTimer);

    /*
     * 本地预测掏枪（服务器 RPC 兜底）。
     *
     * ★ 注意这一下**不一定真的掏出来了**：手上那条正播着"丢出去"时它只是排队，
     *   要等那段动画播完（见 QueueOrEquipAfterThrow）—— 用户要求"技能放完要等 throw
     *   动画播完再切换状态掏枪"，这一段就是那条延时的入口。
     */
    if (AWeapon* PendingHold = ThrowableHolsteredWeapon)
    {
        QueueOrEquipAfterThrow(PendingHold);
    }
    ThrowableHolsteredWeapon = nullptr;

    UpdateHeldThrowableVisual();

    /*
     * 动画收掉：这个技能的几段动作全停（手模 + 身体一起；本机预测，别人的屏幕由复制的 None
     * → OnRep 走同一条路）。
     *
     * ★ 若这一下是"丢出去 / 收起"（收尾段正在演），StopThrowableMontages 会**整段不动手** ——
     *   让那一下自己演完、掏枪也排在它后面（它看的就是 BeginThrowableFinisher 记下的那个状态）。
     */
    StopThrowableMontages(PreviousKind);

    if (bTellServer)
    {
        ServerSetThrowableHolding(false, EBlasterThrowableKind::None);
    }
}

void ABlasterCharacter::QueueOrEquipAfterThrow(AWeapon* Weapon)
{
    if (Weapon == nullptr) return;

    // 手上还在演收尾段（丢出去 / 收起）→ 枪再等一会儿，等它演完兑现（见 ThrowableFinisherFinished）
    if (IsWaitingForThrowableFinisher())
    {
        ThrowablePendingEquipWeapon = Weapon;
        return;
    }

    // 不是"丢出去"那条路（再按一下技能键收起、按 1/2/3 打断）→ 立刻掏，和以前一样
    if (Combat)
    {
        Combat->EquipSlotWeapon(Weapon);
    }
}

void ABlasterCharacter::TryFlushPendingThrowEquip()
{
    if (ThrowablePendingEquipWeapon == nullptr) return;

    /*
     * 还在等收尾段（通知还没来、备胎也还没到）→ 接着等。
     *
     * 只有身体那一条会走到这儿来（通知摆在身体上），手模那条按约定配得比身体短 ——
     * 所以"身体演完"已经隐含了"手模也演完了"，不用再分别问两条轨道
     *（原来手模 / 身体各记各段位时是要两边都问一遍的）。
     */
    if (IsWaitingForThrowableFinisher())
    {
        return;
    }

    AWeapon* Weapon = ThrowablePendingEquipWeapon;
    ThrowablePendingEquipWeapon = nullptr;

    // 玩家在等的这段时间里自己换了枪（1/2/3 是放行的）→ 别把他刚选的那把顶掉
    if (Combat && GetEquippedWeapon() == nullptr)
    {
        Combat->EquipSlotWeapon(Weapon);
    }
}

void ABlasterCharacter::UpdateHeldThrowableVisual()
{
    /*
     * Q（火球）和 E（曲线闪光）拿在手上都是**同一个占位火球**。
     *
     * 用户要求："q 和 e 拿在手上的时候都该是一个火球"。两者本来就只有材质颜色该不一样，
     * 现在连占位表现都共用一套（同一个网格、同一个材质、同一个挂点、同一个缩放）——
     * 以后要给闪光换个颜色，在下面配材质那段按 HeldThrowableKind 分一份就行。
     *
     * C（火墙）**不显示**：它不是"手上拎着个球往外丢"，而是按住左键放出一道墙，
     * 拿球反而会误导玩家以为要丢。刻意留空，不是漏做。
     */
    /*
     * "手上拿着什么"要从哪一份读，分两种情况：
     *   · 本机（射手自己那台）：读本地预测的 bThrowableHolding / HeldThrowableKind。
     *   · 其他机器（观战者、以及权威机上别人的那份）：bThrowableHolding **永远是 false**
     *     （它只在本机按技能键那一刻被置位，不复制），只能读复制的 ReplicatedThrowableKind。
     *     少了这一层的话，别人屏幕上这只球根本不会出现 —— 第三人称那个组件
     *     （HeldFireballTP，SetOwnerNoSee）就是专门给这个视角准备的，白做了。
     */
    EBlasterThrowableKind EffectiveKind = HeldThrowableKind;
    bool bHolding = bThrowableHolding;
    if (!IsLocallyControlled())
    {
        EffectiveKind = ReplicatedThrowableKind;
        bHolding = ReplicatedThrowableKind != EBlasterThrowableKind::None;
    }

    const bool bShow = bHolding
        && (EffectiveKind == EBlasterThrowableKind::Fireball
            || EffectiveKind == EBlasterThrowableKind::Flash);

    /*
     * 挂点/材质第一次真正要用的时候才配齐，不在 BeginPlay 里做。
     *
     * 理由有两条，都是踩过的地方：
     *   · 手模（FPArmsMesh）和它的骨骼要等角色真正生成出来才就位，第一次持火球
     *     必然远晚于 BeginPlay —— 不用去猜构造/ BeginPlay / 控制器就位之间的先后；
     *   · 这个函数在每次进出持投掷物态时都会被调到，用 bHeldFireballVisualReady
     *     保证那段只跑一次（里面要查 socket、加载材质，不该每帧问）。
     */
    if (!bHeldFireballVisualReady)
    {
        bHeldFireballVisualReady = true;

        if (HeldFireballFP && FPArmsMesh)
        {
            /*
             * ★★ 这里必须**运行时重新挂一次**，构造函数里那次 SetupAttachment 不作数。
             *
             * SetupAttachment(FPArmsMesh, HeldFireballFPSocket) 是在 C++ 构造函数里跑的，
             * 那一刻 BP 的类默认值还没套到这个对象上 —— 于是挂点名字被**永久钉死在 C++ 默认值**上。
             * 表现：在 BP 里把 HeldFireballFPSocket 改成 R_WeaponPoint，保存、PIE，
             * **一点效果都没有**（实测：属性值是 R_WeaponPoint，而组件的
             * get_attach_socket_name() 仍然是 WeaponADSSocket），球还停在手模原点。
             *
             * 挂点名字可以是 socket 也可以是骨骼（引擎两样都认）：手模上 R_WeaponMaster /
             * R_WeaponPoint / MasterWeapon / WeaponADS 都是骨骼，名字里没 "Socket" 也照样能用。
             * 查不到时引擎自己退到"挂在网格原点"，第一人称就是一闭眼一团火糊在镜头上 ——
             * 所以下面那条 Warning 仍然保留，它是这个问题唯一的线索。
             */
            HeldFireballFP->AttachToComponent(
                FPArmsMesh,
                FAttachmentTransformRules::SnapToTargetNotIncludingScale,
                HeldFireballFPSocket);

            if (!FPArmsMesh->DoesSocketExist(HeldFireballFPSocket))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[火球] 手模上找不到挂点 '%s'（socket 和骨骼都没有），手上的火球会挂在手模原点（请检查 ABlasterCharacter::HeldFireballFPSocket）"),
                    *HeldFireballFPSocket.ToString());
            }
            // ★ 用世界缩放统一大小：手模那个挂点的局部缩放是 0.01（枪械副本那一套的米制），
            //   第三人称骨架则是 100 —— 继承下来差一万倍。SetWorldScale3D 会按父链反算出
            //   相对缩放，所以两边最后都是 HeldFireballScale 那么大。
            //   ⚠ 顺序：先挂点再设缩放 —— SnapToTargetNotIncludingScale 保持世界缩放，
            //     它会反算相对缩放；放在设缩放之后会把刚设好的值再改一遍。
            HeldFireballFP->SetWorldScale3D(FVector(HeldFireballScale));
        }
        if (HeldFireballTP && GetMesh())
        {
            // 同 FP：构造函数那次挂载用的是 C++ 默认值，BP 里改的不生效，必须在这儿重挂。
            HeldFireballTP->AttachToComponent(
                GetMesh(),
                FAttachmentTransformRules::SnapToTargetNotIncludingScale,
                HeldFireballTPSocket);

            if (!GetMesh()->DoesSocketExist(HeldFireballTPSocket))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[火球] 第三人称骨架上找不到挂点 '%s'（socket 和骨骼都没有），别人看到的火球会挂在身体原点（请检查 ABlasterCharacter::HeldFireballTPSocket）"),
                    *HeldFireballTPSocket.ToString());
            }
            HeldFireballTP->SetWorldScale3D(FVector(HeldFireballScale));
        }

        /*
         * 特效那对组件和占位球走**完全一样**的挂载流程（同一个 socket 变量、同样要运行时重挂、
         * 同样用世界缩放抹平两条骨架链的 100 倍差异）—— 理由和上面那段一字不差，不再重复。
         *
         * 只有一处不同：缩放用的是 HeldFireballEffectScale（默认 1），不是 HeldFireballScale(0.3)。
         * 0.3 是给"半径 50 的引擎球"换算的，而 Niagara 特效有自己的设计尺寸，
         * 拿它当世界缩放会把用户的特效整体缩小 3.3 倍。
         */
        if (HeldFireballFX_FP && FPArmsMesh)
        {
            HeldFireballFX_FP->AttachToComponent(
                FPArmsMesh,
                FAttachmentTransformRules::SnapToTargetNotIncludingScale,
                HeldFireballFPSocket);
            HeldFireballFX_FP->SetWorldScale3D(FVector(HeldFireballEffectScale));
        }
        if (HeldFireballFX_TP && GetMesh())
        {
            HeldFireballFX_TP->AttachToComponent(
                GetMesh(),
                FAttachmentTransformRules::SnapToTargetNotIncludingScale,
                HeldFireballTPSocket);
            HeldFireballFX_TP->SetWorldScale3D(FVector(HeldFireballEffectScale));
        }

        // 占位材质：BP 里指派了就用 BP 的；没指派就用项目里那个发光球材质。
        // 刻意**不**写成 ConstructorHelpers 的必需资产 —— 那是项目内容，哪天改名/移走，
        // 这里只是退回引擎默认材质（灰球，但依然看得见），不会把类的构造直接搞崩。
        UMaterialInterface* Mat = HeldFireballMaterial;
        if (!Mat)
        {
            Mat = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/Game/Assets/MilitaryWeapSilver/FX/Materials/M_GlowSphere_01.M_GlowSphere_01"));
        }
        if (Mat)
        {
            if (HeldFireballFP) HeldFireballFP->SetMaterial(0, Mat);
            if (HeldFireballTP) HeldFireballTP->SetMaterial(0, Mat);
        }
    }

    /*
     * 手上表现二选一：配了特效就开特效、把占位球收起来；没配就还是占位球（默认）。
     * 每次进出持投掷物态都会跑到这儿，所以这里必须是幂等的。
     *
     * ★ 藏特效时必须 **Deactivate()**，不能只 SetVisibility(false) —— 后者只是不画，
     *   Niagara 系统还在后台继续算粒子（画面看不见、性能照烧）。占位球没这个问题。
     */
    auto ApplyHeldVisual = [bShow](UNiagaraComponent* FXComp, UNiagaraSystem* Effect,
                                   UStaticMeshComponent* MeshComp)
    {
        // bShow 为假时短路，不调 ActivateNiagaraFX（它内部会 Activate(true)）
        const bool bUseFX = bShow && ActivateNiagaraFX(FXComp, Effect);

        if (FXComp)
        {
            FXComp->SetVisibility(bUseFX, /*bPropagateToChildren=*/true);
            if (!bUseFX) FXComp->Deactivate();
        }
        if (MeshComp)
        {
            MeshComp->SetVisibility(bShow && !bUseFX, /*bPropagateToChildren=*/true);
        }
    };

    // 第一人称那个只本人可见、第三人称那个只别人可见（本人看第三人称身体是被隐藏的，
    // 挂在它上面的火球却不受影响 —— OwnerNoSee 是渲染标志，不会传给子组件，
    // 不显式关掉的话本人会在第一人称画面里看到一个飘在自己身上的球）。
    ApplyHeldVisual(HeldFireballFX_FP, HeldFireballEffect, HeldFireballFP);
    ApplyHeldVisual(HeldFireballFX_TP, HeldFireballEffect, HeldFireballTP);
}

// ——— 持投掷物（Phoenix E 闪光 / Q 火球 / C 火墙）的动作动画 ———
// 资产挂在**技能**上（UBlasterGameplayAbility::ThrowableEquipMontages / ThrowMontages /
// FireMontages / LiftMontages，每组三段槽），这里只管"什么时候、往哪个动画实例播哪一组"。
//
// ★ 这一块 2026-09-20 按用户的意思重做过：**"举着待命"那一段交给动画蓝图**
//   （两个动画实例在持着东西期间把 WeaponType 报成 EWT_Phoenix*，ABP 按它切状态），
//   C++ 只播四段**一次性**动作：拿起 / 丢出去 / 按住（火墙）/ 收起（火墙）。
//   所以这里没有"当前演到哪一段"的状态、没有循环段、没有段与段之间的接续计时器 ——
//   原来的两条轨道 + 段位枚举 + 计时器接续那一套（以及它衍生出来的接缝混合、提前量、
//   认资产判据）全部删掉了。段与段的衔接变成了"动作播完 → 回到 ABP 那个待命姿势"。
//
// 交互（哪个键拿、哪个键丢）在文件上面那段，两件事分开看。

/*
 * ★ 排查"播了但看不见"时用得上的两件工具（留在这儿，别删）：
 *   · UAnimInstance::GetSlotMontageLocalWeight(槽名) —— 大于 0 说明槽上真有动画在出力。
 *     它等于 0 而蒙太奇在播，就是**动画蓝图里那个 Slot 节点没接进图**：播是播了，出不来。
 *   · UAnimInstance::MontageInstances —— 列出"现在到底在播哪几条"，一眼能看出有没有被别的
 *     （空手那套 / 换弹那套）顶掉：资产名会直接换掉，谁顶的一目了然。
 *
 * ★ 还有一个踩过的坑值得记着：**"停"要认准是哪一侧的实例**（见 StopThrowableMontages）。
 *   换段那会儿两条轨道同帧各停一次，谁跨界去停对方那侧的实例，就会把对方刚起播的下一段
 *   一起停掉 —— 表现是"那段动画播了但看不见"（日志里那条新蒙太奇权重 0、0.15 秒后消失）。
 */

bool ABlasterCharacter::PlayThrowableTrack(UAnimInstance* AnimInstance, UAnimSequenceBase* Asset,
	FName SlotName, bool bStopAllMontages, bool bHoldLastFrame)
{
	if (AnimInstance == nullptr || Asset == nullptr || SlotName == NAME_None) return false;

	/*
	 * 两者都是**播一遍就完**：这是"动作"那几段（拿起 / 丢出去 / 按住 / 收起），
	 * 演完就该回到动画蓝图那个"举着它待命"的姿势 —— 所以这里没有循环那一档。
	 *
	 * 走 Montage_Play 而不是带 blend settings 的那版：动作之间不再需要交叉混合
	 *（原来给混合时间是为了让"拿起→持着"这种同一套资产内部的接缝不闪，现在接缝的另一头
	 * 是 ABP 的状态，交给 ABP 自己的状态混合去管），用**资产上配的**淡入淡出的时长最省事，
	 * 用户想调就在蒙太奇资产里调。
	 */
	// 已经是蒙太奇 → 直接播。
	// ⚠ 这一支不受 bHoldLastFrame 影响（Enable Auto Blend Out 是**资产**上的属性，
	//   这里改它会写进共享资产）。现在这几个槽里填的全是裸序列，走不到这儿；
	//   哪天真往里填蒙太奇又要"停在末帧"，得在资产自己身上关掉（拆包那两条就是这么修的）。
	if (UAnimMontage* AsMontage = Cast<UAnimMontage>(Asset))
	{
		return AnimInstance->Montage_Play(AsMontage, 1.f, EMontagePlayReturnType::MontageLength, 0.f,
			bStopAllMontages) > 0.f;
	}

	/*
	 * 裸序列 → 和空手那条路一样按槽名现造一条动态蒙太奇（理由见 PlayEmptyHandTrack：
	 * 单节点动画没有"槽"的概念，会被动画机的结果整个覆盖掉，播了也看不见）。
	 *
	 * ★ LoopCount 传的是 1（播一遍）—— 别写成 0：**0 不是"无限"，是"零长度"**
	 *   （FAnimSegment::GetLength() = LoopingCount × 段长，AnimCompositeBase.h:149），
	 *   那样这条段长度是 0，蒙太奇瞬间就播完、什么都看不见。
	 *   （原来那段"按住"要循环，所以给它传过 10 万；现在待命姿势归 ABP，不需要循环段了。）
	 *
	 * 淡入淡出都用 0.08（不传就是资产默认，裸序列现造的动态蒙太奇默认也是这一档）——
	 * 和动画蓝图自身状态混合的时长量级一致，交接时看不出来。
	 */
	const int32 LoopCount = 1;
	const FMontageBlendSettings BlendIn(0.08f);
	const FMontageBlendSettings BlendOut(0.08f);
	UAnimMontage* Dynamic = UAnimMontage::CreateSlotAnimationAsDynamicMontage_WithBlendSettings(
		Asset, SlotName, BlendIn, BlendOut, 1.f, LoopCount, /*InBlendOutTriggerTime=*/-1.f);
	if (Dynamic == nullptr) return false;

	/*
	 * ★ "停在末帧"（bHoldLastFrame，只有"拿起"那一段会用，理由见 PlayThrowableSet）。
	 *
	 * 关掉 Auto Blend Out 之后（UAnimMontage::bEnableAutoBlendOut，AnimMontage.h:697；
	 * 现造的动态蒙太奇不会碰这个字段，取的是构造里的 true）引擎那句
	 * `if (!IsStopped() && bEnableAutoBlendOut)`（AnimMontage.cpp:2469）就不成立，
	 * 最后一段播到末尾不会自己 Stop —— 这条蒙太奇会一直压着槽，直到
	 * 丢出去那条（bStopAllMontages=true）或 StopThrowableMontages 来接管。
	 *
	 * 为什么要这样：不关的话它在末帧开始往 ABP 的持械姿势混，而 ABP 那条
	 *（TP_Phoenix_S0_*_Idle_UB / FP_Phoenix_S0_*_Idle）是另一条动画、有自己的播放进度 ——
	 * 混进去那一下在画面上就是"掏出动作又演了一遍"。这是**本机现造的对象**，
	 * 改它不会落盘、不会影响别的调用者。
	 */
	Dynamic->bEnableAutoBlendOut = !bHoldLastFrame;

	return AnimInstance->Montage_Play(Dynamic, 1.f, EMontagePlayReturnType::MontageLength, 0.f,
		bStopAllMontages) > 0.f;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetThrowableAbilityCDO(EBlasterThrowableKind Kind) const
{
	// 每个技能本来就是各按自己的 SkillType 查出来的（见 GetCurveballAbilityCDO），这里只是合起来
	switch (Kind)
	{
	case EBlasterThrowableKind::Flash:		return GetCurveballAbilityCDO();
	case EBlasterThrowableKind::Fireball:	return GetFireballAbilityCDO();
	// ★ 火墙不能漏：几段动画（拿起 / 持着 / 按住 / 收起）全是从这里查出 CDO 再去它的槽里拿资产的。
	//   漏掉的话 Wall 那一路 Set 恒为 nullptr —— 不报错、不进日志，就是"按 C 以后手上没动作"，
	//   而且"发射"那一下的轨道仍然会走（计时器按 0.1 秒兜底），所以连掏枪时机都是错的。
	case EBlasterThrowableKind::Wall:		return GetBlazeAbilityCDO();
	default:								return nullptr;
	}
}

void ABlasterCharacter::PlayThrowableSet(bool bFirstPerson, const FBlasterThrowableMontages& Set,
	bool bHoldLastFrame)
{
	if (bFirstPerson)
	{
		// 手模只有本机有（别人的机器上这组件既不渲染、tick 也关着，往那儿播等于白发一条指令）
		if (!IsLocallyControlled() || FPArmsMesh == nullptr) return;

		UAnimSequenceBase* Asset = Set.FirstPerson;
		if (Asset == nullptr) return;

		/*
		 * ⚠ 骨架对不上就绝对不能播 —— 手模骨架和角色骨架**都有叫 Skeleton / Root 的骨**，
		 * 绑定失败是静默的，后果是把第一人称视角整个拧掉（差 120°）。
		 * 判定和全套理由见 PlayFPArmsMontage 那段长注释，这里只是把同一个闸接到投掷物这几段上
		 *（同样是"要往手模槽里填资产"的地方，一样会填错）。
		 */
		if (!IsMontageCompatibleWithMesh(Asset, FPArmsMesh))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[投掷物] %s 的 %s 不是手模骨架的动画 → 第一人称这一段不播。")
				TEXT("（手模槽必须填 FP_Wushu_S0_* 那套；填成身体骨架的动画会静默改写手模的 Skeleton/Root，把视角拧掉）"),
				*GetName(), *Asset->GetName());
			return;
		}

		PlayThrowableTrack(FPArmsMesh->GetAnimInstance(), Asset, EmptyHandFPSlotName,
			/*bStopAllMontages=*/true, bHoldLastFrame);
		return;
	}

	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (AnimInstance == nullptr) return;

	/*
	 * 上半身 + 下半身两条**同时播**：它们是同一具身体上两个不同名的槽，本来就互不覆盖 ——
	 * 关键是第二条必须 bStopAllMontages=false，否则会把第一条顶掉
	 *（引擎的 Montage_Play 里 bStopAllMontages 是真·全停，见 PlayEmptyHandTrack 里那段）。
	 * 用"上一条播没播出去"来判断谁清场，而不是写死"UB 一定是第一条"：
	 * UB 那条留空 / 播不出去时，下半身那条得接着当"清场的那一条"。
	 */
	const bool bUpperPlayed = PlayThrowableTrack(AnimInstance, Set.ThirdPersonUpper, EmptyHandUBSlotName,
		/*bStopAllMontages=*/true, bHoldLastFrame);
	PlayThrowableTrack(AnimInstance, Set.ThirdPersonLower, EmptyHandLBSlotName,
		/*bStopAllMontages=*/!bUpperPlayed, bHoldLastFrame);
}

void ABlasterCharacter::PlayThrowableEquip(bool bFirstPerson, EBlasterThrowableKind Kind)
{
	const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(Kind);
	// bHoldLastFrame = true："拿起"演完停在末帧，别让 ABP 的持械姿势混进来又演一遍掏出
	//（用户 2026-09-22 报的"放完 equip 蒙太奇之后有重新掏的效果"）。另外三段不传，
	// 它们演完就是要交给 ABP 的。
	if (CDO) PlayThrowableSet(bFirstPerson, CDO->ThrowableEquipMontages, /*bHoldLastFrame=*/true);
}

void ABlasterCharacter::PlayThrowableThrow(bool bFirstPerson, EBlasterThrowableKind Kind)
{
	const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(Kind);
	if (CDO) PlayThrowableSet(bFirstPerson, CDO->ThrowableThrowMontages);
}

// 按住 / 收起这两段是火墙专利（闪光和火球都是"一按就出去"），所以不用传种类，写死 Wall
void ABlasterCharacter::PlayThrowableFire(bool bFirstPerson)
{
	const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(EBlasterThrowableKind::Wall);
	if (CDO) PlayThrowableSet(bFirstPerson, CDO->ThrowableFireMontages);
}

void ABlasterCharacter::PlayThrowableLift(bool bFirstPerson)
{
	const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(EBlasterThrowableKind::Wall);
	if (CDO) PlayThrowableSet(bFirstPerson, CDO->ThrowableLiftMontages);
}

void ABlasterCharacter::BeginThrowableFinisher(EBlasterThrowableKind Kind, bool bLift)
{
	ThrowableFinisherAssetUB = nullptr;
	ThrowableFinisherAssetLB = nullptr;

	float Length = 0.f;

	// 身体上正在演的是哪两条 —— 认通知用（见 NotifyThrowableSkillFinished）。
	// 认不出是哪两条时它就退回"空 = 不挡"，所以查不到技能也不会卡住收尾。
	if (const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(Kind))
	{
		const FBlasterThrowableMontages& Set =
			bLift ? CDO->ThrowableLiftMontages : CDO->ThrowableThrowMontages;
		ThrowableFinisherAssetUB = Set.ThirdPersonUpper;
		ThrowableFinisherAssetLB = Set.ThirdPersonLower;
		// 备胎按**身体那条**算时长：通知摆在身体上，掏枪也是等身体那条
		Length = Set.GetLongestPlayLength();
	}

	bWaitingForThrowableFinisher = true;

	/*
	 * 备胎计时器：通知漏摆时唯一的门。
	 *
	 * 多留 ThrowableFinisherFallbackExtra 秒余量（0.15）—— 正门那个通知在蒙太奇**末尾**响
	 *（正好是 Length 那一帧），留点余量让它先到；同时这点时间也够资产自己 blend out 完。
	 * 下限 0.1 秒是防"这一段没配资产"（Length = 0）时计时器变成 0 ——
	 * 这一段的**责任**（掏枪）不能悬着，哪怕没动画可播也得走完这一次收尾。
	 *
	 * 和通知进的是同一个 ThrowableFinisherFinished，谁先到谁生效，后到的看到
	 * bWaitingForThrowableFinisher 已经是 false，天然是空操作 —— 所以这里不用去猜谁快。
	 */
	GetWorldTimerManager().SetTimer(ThrowableFinisherTimer,
		FTimerDelegate::CreateUObject(this, &ABlasterCharacter::ThrowableFinisherFinished),
		FMath::Max(Length, 0.1f) + ThrowableFinisherFallbackExtra, /*bLoop=*/false);
}

void ABlasterCharacter::ThrowableFinisherFinished()
{
	// 后到的那一条（通知 / 备胎计时器）：这一下没什么可做的
	if (!bWaitingForThrowableFinisher) return;

	bWaitingForThrowableFinisher = false;
	ThrowableFinisherAssetUB = nullptr;
	ThrowableFinisherAssetLB = nullptr;
	GetWorldTimerManager().ClearTimer(ThrowableFinisherTimer);

	/*
	 * ★ "等技能那一下演完再掏枪"的**兑现点**。
	 *
	 * 丢完 / 收起技能时枪没有立刻掏（存进了 ThrowablePendingEquipWeapon，见 QueueOrEquipAfterThrow）
	 * —— 现在这一下演完了，掏。
	 *
	 * ★ 为什么手模和身体只有这一次兑现就够：这里等的是**身体**那条（通知在身体上），
	 *   而按约定手模那条配得比身体短，所以身体演完时手模早就演完了 —— 不需要再分两条轨道各等一次。
	 */
	TryFlushPendingThrowEquip();
}

void ABlasterCharacter::StopThrowableMontages(EBlasterThrowableKind Kind)
{
	/*
	 * 收尾段正在演（丢出去 / 收起那一下）→ **不许停**。
	 * 停掉等于把丢出去的动作当场切一刀，而且"等它演完再掏枪"那件事也会跟着落空
	 *（同一次退出会从两条路上各来一次：本机预测一次 + 复制到达后一次，第二次不该把第一次的收尾切掉）。
	 */
	if (bWaitingForThrowableFinisher) return;
	if (Kind == EBlasterThrowableKind::None) return;

	const UBlasterGameplayAbility* CDO = GetThrowableAbilityCDO(Kind);
	if (CDO == nullptr) return;

	const FBlasterThrowableMontages* Sets[4] =
	{
		&CDO->ThrowableEquipMontages, &CDO->ThrowableThrowMontages,
		&CDO->ThrowableFireMontages, &CDO->ThrowableLiftMontages
	};

	/*
	 * 停的是这个技能的**四段动作**（认资产，不认"现在演到哪一段"）—— 和 StopEmptyHandMontages
	 * 一个路子：资产上关了自动融合的那条不会自己走，只有这里认得出它。
	 *
	 * ★ 手模和身体**一起停**在这里是安全的（原来是两条轨道各停各的，还为此踩过一个坑）：
	 *   那时候两条轨道会在同一帧各自换段，后停的那条会把对方**刚起播的下一段**一起停掉
	 *（表现是"那段动画播了但看不见"，详见进度记录八十）。现在段与段之间不再有这种同帧换段 ——
	 *   停只剩"退出持投掷物"这一处，两侧一起停正是想要的。
	 */
	for (const FBlasterThrowableMontages* Set : Sets)
	{
		StopMontageInstancesUsingAsset(FPArmsMesh ? FPArmsMesh->GetAnimInstance() : nullptr,
			Set->FirstPerson, EmptyHandFPSlotName, 0.f);
	}

	if (UAnimInstance* BodyInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
	{
		for (const FBlasterThrowableMontages* Set : Sets)
		{
			StopMontageInstancesUsingAsset(BodyInstance, Set->ThirdPersonUpper, EmptyHandUBSlotName, 0.f);
			StopMontageInstancesUsingAsset(BodyInstance, Set->ThirdPersonLower, EmptyHandLBSlotName, 0.f);
		}
	}
}

void ABlasterCharacter::NotifyThrowableSkillFinished(bool bFirstPerson, const UAnimSequenceBase* FinishedAsset)
{
	/*
	 * 用户摆在动画末尾的那个通知响的落点。只管一件事：这一下算不算"当前这条收尾动作演完了"，
	 * 算就把收尾交出去（掏枪等它，见 ThrowableFinisherFinished）。
	 */

	// 通知按约定摆在**第三人称**动画末尾 —— 手模上响的一律不理会
	//（掏枪是身体 + 手模一起的事，以身体那条为准，所以配资产时让手模那条短一点）
	if (bFirstPerson) return;

	// 没在等收尾（这一段不是收尾动作 / 已经收过了）→ 这一下没什么可切的
	if (!bWaitingForThrowableFinisher) return;

	/*
	 * 认资产（FinishedAsset = 响通知的那条资产）：
	 *   · 就是身体上现在这两条之一 → 认，切；
	 *   · 两条都是空的（这个技能没配身体那条）→ 认 —— 没东西可认就别挡着收尾；
	 *   · 是别的资产 → 不认。
	 *     最要紧的一处就是火墙：玩家按住左键时"按住"那一段的末尾通知，可能在他**已经松手**
	 *     （收起那段都起播了）之后才响 —— 不认资产的话，这条晚一拍的通知会把刚起播的收起动作
	 *     当场切掉、枪也提前亮出来。
	 */
	if (FinishedAsset != nullptr &&
		ThrowableFinisherAssetUB.Get() != FinishedAsset &&
		ThrowableFinisherAssetLB.Get() != FinishedAsset)
	{
		return;
	}

	ThrowableFinisherFinished();
}

void ABlasterCharacter::OnRep_ReplicatedThrowableKind()
{
	/*
	 * 远端机（**包括拥有者那台**）拿到"手上拿着什么"这个状态 → 播第三人称身体那条"拿起"。
	 *
	 * 只管身体：手模那条是射手本机按下技能键时自己播的（别人的角色上你根本没有手模组件）。
	 * 权威机不会走这儿（RepNotify 只在客户端响），它那边由 ServerSetThrowableHolding 自己叫。
	 *
	 * "丢出去"那一下不在这条路上 —— 这个属性是从 Kind 直接变成 None 的，
	 * 看不出中间那一下发生在什么时候，所以它走 ServerNotifyThrowableThrown → 多播。
	 *
	 * 待命姿势（持着期间站着的样子）不在这里播：它归动画蓝图，两个动画实例按
	 * WeaponType = GetThrowableWeaponType(Kind) 自己切（见 WushuFPAnimInstance /
	 * BlasterCharacterAnimInstance），拿到的 Kind 一变姿势就跟着变。
	 */

	/*
	 * ★ 先处理"服务器那边已经收手了、本机还以为自己手上拿着"这一种失配。
	 *
	 * 什么时候会这样：收尾不是玩家按出来的，而是服务器自己做的 —— 典型是火墙的球飞完
	 *（APhoenixBlazeBall::EndFlight → StopBlazeFire，那个函数在权威机上直接收）。
	 * 这时客户端本地那份 bThrowableHolding 还是 true，而下一帧它就会因为"手上有东西"
	 * 把左键/右键全部吞掉 —— 表现是"技能放完之后点不出去、也开不了枪"。
	 *
	 * 远端模拟机上 bThrowableHolding 永远是 false（它只在按技能键那台机器上被置位），
	 * 所以这一段实际只对**射手自己那台**生效。
	 *
	 * bTellServer = false：服务器本来就是发起方，回一条 RPC 只是打个来回。
	 */
	if (ReplicatedThrowableKind == EBlasterThrowableKind::None && bThrowableHolding)
	{
		ExitThrowableHoldLocal(/*bTellServer=*/false);
	}

	// 变 None 那一下不在这里停动画：退出会经过 ExitThrowableHoldLocal / ServerExitThrowableHold
	//（StopThrowableMontages 在那儿），那是唯一的停播点。
	if (ReplicatedThrowableKind != EBlasterThrowableKind::None)
	{
		PlayThrowableEquip(/*bFirstPerson=*/false, ReplicatedThrowableKind);
	}

	/*
	 * 手上那只球的显隐也要跟着变。
	 *
	 * 别人的屏幕上这是**唯一**的触发点：bThrowableHolding 只在本机按技能键那一刻置位、不复制，
	 * 远端机只能靠这个 RepNotify（见 UpdateHeldThrowableVisual 里那段"从哪一份读"）。
	 * 不补这一句的话表现是"别人拿技能你这边什么都看不到"——第三那个 TP 球白做了。
	 *
	 * 在射手自己那台机器上重复调一次无害：它读的是本地预测那份，结果和按下/松开那一刻算的一致。
	 */
	UpdateHeldThrowableVisual();
}

void ABlasterCharacter::ServerNotifyThrowableThrown_Implementation(EBlasterThrowableKind Kind)
{
	MulticastThrowableThrown(Kind);
}

void ABlasterCharacter::MulticastThrowableThrown_Implementation(EBlasterThrowableKind Kind)
{
	/*
	 * 别人屏幕上的"丢出去"。
	 * 射手自己那台也会收到这一条（多播在服务器和**所有客户端包括调用者**上执行）——
	 * 它那边手模已经播过了，这里补的正好是身体那条，两边各一条，不会重。
	 */
	PlayThrowableThrow(/*bFirstPerson=*/false, Kind);

	/*
	 * 收尾也要在**这一台**记上：掏枪要等这一段演完，而各台机器知道"他丢了"的途径就是这条多播
	 *（射手那台其实已经记过一次，重复记只是把备胎计时器重挂一遍，无害）。
	 */
	BeginThrowableFinisher(Kind, /*bLift=*/false);
}

void ABlasterCharacter::ServerNotifyThrowableBlazePhase_Implementation(bool bFiring)
{
	MulticastThrowableBlazePhase(bFiring);
}

void ABlasterCharacter::MulticastThrowableBlazePhase_Implementation(bool bFiring)
{
	/*
	 * 火墙的"按住 / 收起"在别人屏幕上的身体动画（手模那条只有射手本人有，本机按下时已经自己播过）。
	 * 和 MulticastThrowableThrown 同构，只是两段共用一条路。
	 */
	if (bFiring)
	{
		// 按住：这只是一记一次性动作 —— 按住期间"举着控球"的姿势由动画蓝图维持
		//（WeaponType = EWT_PhoenixBlaze），演完不会自动收起，收尾只认松手 / 球飞完。
		PlayThrowableFire(/*bFirstPerson=*/false);
		return;
	}

	// 收起：别人屏幕上这一下的收尾，同 MulticastThrowableThrown 那处
	PlayThrowableLift(/*bFirstPerson=*/false);
	BeginThrowableFinisher(EBlasterThrowableKind::Wall, /*bLift=*/true);
}

void ABlasterCharacter::ServerSetThrowableHolding_Implementation(bool bHolding, EBlasterThrowableKind Kind)
{
    if (bHolding)
    {
        if (Kind == EBlasterThrowableKind::None) return;

        // 手上拿着的是**别的**技能（乱序 / 连按）：不动，等下一次对齐
        if (bThrowableHolding && HeldThrowableKind != Kind) return;

        /*
         * ★ 这一边**不能**拿 bThrowableHolding 当"已经进过了"的门禁 —— 听服主机上
         *   EnterThrowableHold 已经在本地把这个标志置上了，紧接着这条 Server RPC 就在
         *   同一个进程里执行：一进门被挡掉的话，下面的 ReplicatedThrowableKind 永远不写，
         *   远端机收不到这个属性 → **主机手上的投掷物在别人屏幕上从头到尾都不存在**
         *  （身体那条动画也走这条线，主机的第三人称动作一并消失）。
         *   所以拆成两段：状态本来就已经是对的（本机预测过），只补复制 + 身体动画。
         */
        const bool bAlreadyHoldingLocally = bThrowableHolding;
        bThrowableHolding = true;
        HeldThrowableKind = Kind;
        ReplicatedThrowableKind = Kind; // 镜像给观战者技能条高亮对应槽位

        if (!bAlreadyHoldingLocally)
        {
            // 服务器权威收枪：记录 + 挂回闲置 socket（本机预测那条路已经做过了，别做第二遍）
            ThrowableHolsteredWeapon = GetEquippedWeapon();
            if (Combat && ThrowableHolsteredWeapon)
            {
                ThrowableHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
                Combat->AttachActorToSocket(ThrowableHolsteredWeapon, Combat->GetHolsterSocketForWeapon(ThrowableHolsteredWeapon));
                SetEquippedWeapon(nullptr);
            }
        }
        UpdateHeldThrowableVisual();

        // 动画：身体那条"拿起"由**权威**这边播（远端机走 OnRep_ReplicatedThrowableKind，
        // 两边互斥 —— 权威机不响 RepNotify，客户端不跑这个 RPC，所以每台机器只走一条路）。
        PlayThrowableEquip(/*bFirstPerson=*/false, Kind);
    }
    else
    {
        /*
         * 退出这一边同理：主机本地预测已经清掉了 bThrowableHolding，拿它当门禁的话
         * ReplicatedThrowableKind 会永远停在旧值上 —— 远端机看到的这个人**手里一直举着**，
         * 技能条高亮也不灭。ServerExitThrowableHold 本身重复跑没有副作用（枪已经交还、
         * 只是再设一遍 None），所以这里无条件走。
         */
        ServerExitThrowableHold();
    }
}

void ABlasterCharacter::ServerExitThrowableHold()
{
    // 同 ExitThrowableHoldLocal：停动画要用清掉之前那份种类
    const EBlasterThrowableKind PreviousKind = HeldThrowableKind;

    bThrowableHolding = false;
    HeldThrowableKind = EBlasterThrowableKind::None;
    ReplicatedThrowableKind = EBlasterThrowableKind::None;

    // 火墙的"按住控球"在服务器侧也到此为止（球那边每帧读的就是服务器上这一份）
    bBlazeFiring = false;
    GetWorldTimerManager().ClearTimer(BlazeFireFuseTimer);

    if (AWeapon* Weapon = ThrowableHolsteredWeapon)
    {
        /*
         * ★ 和本地那条（ExitThrowableHoldLocal）一样：手上那一下还在演"丢出去"的话，
         *   枪只是**排队**，等动画播完再掏 —— 这条是服务器上的那份，远端观战者看到的
         *   这个人掏枪时机也跟它走，两边得是同一套规则。
         */
        QueueOrEquipAfterThrow(Weapon);
    }
    ThrowableHolsteredWeapon = nullptr;
    UpdateHeldThrowableVisual();

    // 身体那条收掉（远端机靠 ReplicatedThrowableKind 变 None 的 RepNotify 在 OnRep 里走同一条路；
    // "丢出去 / 收起"那一下正在演时 StopThrowableMontages 会整段不动手，留着它演完）
    StopThrowableMontages(PreviousKind);
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
        SageHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
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
            SageHolsteredWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
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

    // 扣掉这一格（给 ASC 挂冷却 GE → 技能条开始走 45s 回充倒计时）
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

// ============================ Clove 暮蝶：封烟 ============================

bool ABlasterCharacter::HasSmokeAbility() const
{
	return GetSmokeAbilityCDO() != nullptr;
}

const UBlasterGameplayAbility* ABlasterCharacter::GetSmokeAbilityCDO() const
{
	// 和 GetSageHealAbilityCDO 完全同一套路：SkillType 就是"这个英雄有没有这个技能"的
	// 唯一判据。GA_Clove_Smoke 里把 SkillType 配成 Smoke 即可，C++ 不需要认识"Clove"。
	for (const TSubclassOf<UBlasterGameplayAbility>& AbilityClass : DefaultAbilities)
	{
		if (!AbilityClass) continue;
		const UBlasterGameplayAbility* CDO = AbilityClass->GetDefaultObject<UBlasterGameplayAbility>();
		if (CDO && CDO->SkillType == EBlasterSkillType::Smoke)
		{
			return CDO;
		}
	}
	return nullptr;
}

int32 ABlasterCharacter::GetCloveSmokeCharges() const
{
	const UBlasterGameplayAbility* Smoke = GetSmokeAbilityCDO();
	// 没这个技能 / 没配冷却 GE / 没 ASC → 0。
	// 这和改之前 IsCloveSmokeAvailable() 直接 return false 是同一件事
	//（那个函数现在就是「这个函数 > 0」），只是把"还剩几层"这个数也暴露出来了。
	if (!Smoke || !Smoke->CooldownEffectClass || !AbilitySystemComponent) return 0;

	// 冷却 GE 是**非叠加的 Duration 效果**，每施放一次就是**一个新的活跃实例**，
	// 实例数 == 已用掉的充能。（不是靠 StackCount 叠 —— UE 的 FindStackableActiveGameplayEffect
	// 在 StackingType==None 时直接返回 nullptr，所以每次应用都会新建一条，GetGameplayEffectCount
	// 把它们数出来。这条是这套"用冷却 GE 当次数"能成立的前提。）
	// 实例会随时间自然到期（Duration 走完自动移除），这正是"充能回复"。
	return Smoke->GetCharges(AbilitySystemComponent);
}

void ABlasterCharacter::ServerPlaceCloveSmoke_Implementation(const FVector& WorldLocation)
{
	if (!HasAuthority()) return;

	// 单个落点就是批量版的退化情形，走完全同一条路 —— 免得"单点"和"齐放"两套代码
	// 在充能扣减/射线判定上慢慢长歪。
	ServerPlaceCloveSmokes({ WorldLocation });
}

void ABlasterCharacter::ServerPlaceCloveSmokes_Implementation(const TArray<FVector>& WorldLocations)
{
	if (!HasAuthority()) return;

	// 服务器权威复核（客户端报的坐标和数组长度一概按"可能是改过的"处理）
	//
	// ⚠️ 这里**故意没有 bElimmed 检查** —— 死后放烟是暮蝶的招牌（用户明确要求）。
	//    注意是"故意不加"，不是漏了。唯一还成立的约束是充能，所以下面就查它。
	//
	// 注意是**先读一次充能、整批共用这一个数**，而不是每个落点各读一次：
	// PlaceCloveSmokeAt 每成功一次就会挂一层冷却，循环里再读就是"已经扣过的"值了。
	const int32 Allowed = FMath::Min(WorldLocations.Num(), GetCloveSmokeCharges());
	if (Allowed <= 0) return;
	if (!CloveSmokeClass) return;

	for (int32 i = 0; i < Allowed; ++i)
	{
		// 某一个点落在虚空上（地图边界外）只是那一个不作数，**不能 break** ——
		// 玩家选的第一个点完全可能是坏的、第二个是好的，提前退出等于把好的那个也吞了。
		// PlaceCloveSmokeAt 内部失败时不扣充能，所以这里不用管返回值。
		PlaceCloveSmokeAt(WorldLocations[i]);
	}
}

bool ABlasterCharacter::PlaceCloveSmokeAt(const FVector& WorldLocation)
{
	if (!HasAuthority()) return false;
	if (!IsCloveSmokeAvailable()) return false;
	if (!CloveSmokeClass) return false;

	// 落点高度：从施法者所在高度往上抬一点，再往下打。
	// 起点跟着施法者而不是固定高度 —— 多层建筑（A 大庭上下层）才能封到"我这一层"，
	// 从地图顶往下打只会永远命中最高那层的天花板。
	const float StartZ = GetActorLocation().Z + CloveSmokeTraceUpOffset;

	const FVector TraceStart(WorldLocation.X, WorldLocation.Y, StartZ);
	const FVector TraceEnd(WorldLocation.X, WorldLocation.Y, StartZ - CloveSmokeTraceDownDepth);

	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	// 把所有角色都排除掉：站在落点上的人（或自己还躺着的尸体）不该改变烟的高度 ——
	// 这条射线要回答的是「这个 XY 的地板在哪」，不是「路上第一个挡路的是谁」。
	// 不排的话，有人站在落点上烟就会浮在他头顶 90 公分。
	for (TActorIterator<ABlasterCharacter> It(GetWorld()); It; ++It)
	{
		Params.AddIgnoredActor(*It);
	}

	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params))
	{
		// 往下打不到任何东西 = 这个 XY 底下是虚空（点到了地图边缘外的区域）。
		// **什么都不做、也不扣充能** —— 玩家没损失，可以重新选点。
		return false;
	}

	// 抬一点离地，免得球壳一半埋在地板里看着像半个球
	const FVector SpawnLocation = Hit.ImpactPoint + FVector(0.f, 0.f, 10.f);

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	GetWorld()->SpawnActor<ACloveSmoke>(CloveSmokeClass, SpawnLocation, FRotator::ZeroRotator, SpawnParams);

	// 只有真落地了才扣充能。扣减放在最后一行就是这个意思 —— 前面任何一条 return false
	// 都是"这次没放成"，玩家一层都不该损失。
	ApplyCloveSmokeUsed();
	return true;
}

void ABlasterCharacter::ApplyCloveSmokeUsed()
{
	const UBlasterGameplayAbility* Smoke = GetSmokeAbilityCDO();
	if (!Smoke || !Smoke->CooldownEffectClass || !AbilitySystemComponent) return;

	// 和 ApplySageHealUsed 一模一样：UE5.4 的 ApplyGameplayEffectToSelf 传效果 CDO
	UGameplayEffect* EffectCDO = Smoke->CooldownEffectClass->GetDefaultObject<UGameplayEffect>();
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
    // 空手那一段不可打断。正常情况这里本来就丢不了东西（枪已经收回挂点、EquippedWeapon 是空，
    // spike 也被一起收回去了），但"看不见的空操作"不该靠副作用保证 —— 显式挡掉，
    // 以后谁改了收枪顺序也不会突然冒出"空手里把枪丢在地上"。
    if (IsEmptyHandLocked()) return;

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

    // 吃球中按 1/2 = 打断吃球并掏枪（进度清零重来）。
    // bRestoreWeapon=false：紧接着下面自己就切武器了，让球那边先 EquipBestOwnedWeapon 再切
    // 等于连切两把，画面上会抖一下。
    CancelOrbChannel(false);

    // 持闪光/治疗选中态：1/2 = 打断并掏枪（恢复正常可射击），不切武器
    if (bThrowableHolding)
    {
        ExitThrowableHoldLocal();
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

    // 吃球中按 1/2 = 打断吃球并掏枪（同 SelectPrimary）
    CancelOrbChannel(false);

    // 持闪光/治疗选中态：1/2 = 打断并掏枪（恢复正常可射击），不切武器
    if (bThrowableHolding)
    {
        ExitThrowableHoldLocal();
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
    if (bDisableGameplay || !Combat) return;

    // 和 1/2 一致：吃球中按 3 也是"打断吃球并掏武器"（进度清零重来）
    CancelOrbChannel(false);

    // 持闪光/治疗选中态：按 3 是"打断并掏武器"，不切槽
    if (bThrowableHolding)
    {
        ExitThrowableHoldLocal();
        return;
    }
    if (bSageHealSelecting)
    {
        ExitSageHealSelectLocal();
        return;
    }

    // 近战槽里平时是角色自带的那把刀（每次重生由 RestorePlayerWeapons 发），
    // Jett 大招生效期间被飞刀顶掉（收招后刀会放回来）。
    // 槽里是空的时候（被顶掉且还没收招）SwitchWeapon 那条分支什么都不做 —— 切 3 不会把手上的枪收走。
    if (HasAuthority()) Combat->SwitchWeapon(EWeaponSlot::ESlot_Melee);
    else ServerEquipSlot(EWeaponSlot::ESlot_Melee);
}

void ABlasterCharacter::SpikePressed(const FInputActionValue& Value)
{
    if (bDisableGameplay || bSageHealSelecting || bThrowableHolding || !Combat) return;
    // 空手那一段不可打断：连"把尖刺掏出来准备安包"都不行。
    // 挡在这一句就够 —— 长按计时器是下面才起的，所以不会出现"按下被挡了、松开却触发了安包"。
    //（掏尖刺本身也走 CanChangeWeapon()，这里是为了连计时器一起不启动。）
    if (IsEmptyHandLocked()) return;
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
    // 空手那一段不可打断：按下时（SpikePressed）还好好的，阈值到点之前状态被技能改成空手了
    // → 这一次长按整段作废。
    // ★ 门禁放在置标志**之前**：置了标志的话，松开时会走去 CancelSpikePlant /
    //   CancelAction，那是"取消正在进行的安包/拆包" —— 别人正在包的也会被一起取消。
    if (IsEmptyHandLocked()) return;

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
    if (bSageHealSelecting || bThrowableHolding) return;
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

bool ABlasterCharacter::IsInPlantZone() const
{
	// 服务器判定：查询所有 APlantZone 多边形（任意形状），角色位置点内即算在区域内
	return APlantZone::IsPointInAnyZone(this, GetActorLocation());
}

void ABlasterCharacter::SetEquippedWeapon(AWeapon* WeaponToEquip)
{
    EquippedWeapon = WeaponToEquip;

    // 换枪（含收枪时传 nullptr）就把第一人称那份枪械副本刷新一遍：
    // 显示哪套网格、挂在手模哪根骨骼上、要不要显示，全按这把武器的设置来。
    // 内部先判断是不是本地玩家，所以远端机器上白跑一下而已。
    UpdateFPWeaponMesh();

    // 换枪 = 换跑动上限（每把武器一个，见 AWeapon::MaxWalkSpeed）。
    // 挂在这里而不是 EquipSlotWeapon：收枪/丢枪走的是同一个函数、传的是 nullptr，
    // 移速必须跟着回到"空手"那一档，否则收了枪还沿着上一把枪的速度跑。
    // 这次的瞄准状态用本机自己那份（此刻刚好是对的，还没被复制覆盖）。
    if (Combat) Combat->ApplyMaxWalkSpeed(IsAiming());

    // 放最后：新武器已经就位，覆写的英雄子类（Jett 飞刀显隐）读到的才是最终值。
    // 客户端那条路不经过本函数（见 OnRep_EquipWeapon），那边的调用点也补了一次。
    OnEquippedWeaponChanged();
}

void ABlasterCharacter::OnEquippedWeaponChanged()
{
    // 基类没事可做。这里刻意留一个**非纯虚**的空实现：绝大多数角色/武器不关心这件事，
    // 让每个子类都被迫实现一遍没有意义（而且 base 是 ACharacter，不是接口）。
}

void ABlasterCharacter::UpdateFPWeaponMesh()
{
    if (FPWeaponMesh == nullptr) return;

    // 先把上一次为副本隐藏起来的真枪还原：换枪时它可能被挂回背后闲置 socket 或丢在地上，
    // 那两种情况下本人都该看得见它（隐藏只针对"正在手上、第一人称用副本代替"的那一把）。
    if (IsValid(FPViewmodelHiddenWeapon) && FPViewmodelHiddenWeapon != EquippedWeapon)
    {
        if (USkeletalMeshComponent* OldMesh = FPViewmodelHiddenWeapon->GetWeaponMesh())
        {
            OldMesh->SetOwnerNoSee(false);
        }
        // 那把枪的弹匣/瞄准镜刚才挂在副本上，现在副本要给别人用了 → 挂回它自己的真网格
        // 显式传 nullptr = "本人看到的就是真枪"，不让武器去反推
        FPViewmodelHiddenWeapon->UpdateAttachedMeshes(nullptr);
        FPViewmodelHiddenWeapon = nullptr;
    }

    // 只有本地玩家的手模会渲染 —— 别人身上这份副本从没显示过，也不用管
    if (!IsLocallyControlled())
    {
        FPWeaponMesh->SetVisibility(false);
        return;
    }

    // 没拿枪 / 这把武器没开第一人称副本 / 副本网格还没填 → 收起来。
    // 没开副本时第一人称看到的是**真枪**（它还挂在第三人称手上），也就是原来的样子。
    USkeletalMesh* ViewModel = EquippedWeapon ? EquippedWeapon->GetFPViewModelMesh() : nullptr;
    if (EquippedWeapon == nullptr
        || !EquippedWeapon->ShouldUseFPViewModel()
        || ViewModel == nullptr)
    {
        FPWeaponMesh->SetVisibility(false);
        // 副本收起来了 → 手上这把的弹匣/瞄准镜挂回真枪。显式传 nullptr（本人看到的就是真枪），
        // 不去依赖"副本当前可见不可见"这种会变的内部状态。
        if (EquippedWeapon != nullptr)
        {
            EquippedWeapon->UpdateAttachedMeshes(nullptr);
        }
        return;
    }

    // 副本要显示了 → 把真枪对本人隐藏，不然第一人称会同时看到两把枪
    //（副本在手模的挂点上，真枪还挂在第三人称身体的手上 —— 那具身体本身已经是
    // SetOwnerNoSee，但枪是另一个 actor，得单独处理）。
    // 用 OwnerNoSee 而不是 SetVisibility：后者会把远端玩家看到的枪一起隐藏。
    // 这个开关是纯渲染的、不走复制，各机器各算各的 —— 别人那边本来就不隐藏（他们不是 owner）。
    if (USkeletalMeshComponent* RealMesh = EquippedWeapon->GetWeaponMesh())
    {
        RealMesh->SetOwnerNoSee(true);
        FPViewmodelHiddenWeapon = EquippedWeapon;
    }

    if (FPWeaponMesh->GetSkeletalMeshAsset() != ViewModel)
    {
        FPWeaponMesh->SetSkeletalMesh(ViewModel);

        // 动画类直接抄真枪那个网格的：武器模型要播的动画（GN_Core_*）本来就挂在枪自己的骨架上，
        // 副本用的是同一套骨架，所以同一个动画蓝图能直接用 —— 不用在角色这边再填一遍。
        // 想用蒙太奇打断的话，那个 ABP 里要有 Slot 节点（见 AWeapon::PlayAnimAssetOnMesh）。
        //
        // 注意这里抄的是**组件上**的 Anim Class（武器 BP 里 WeaponMesh 组件 -> Animation Mode =
        // Animation Blueprint -> Anim Class）。要是 ABP 填在**网格资产**自己身上，这里读不到，
        // 副本就没有动画蓝图 —— 那种情况下蒙太奇播不了（会打 warning 并退回单节点播第一段序列），
        // 想让蒙太奇生效就把 ABP 在武器 BP 的 WeaponMesh 组件上再填一遍。
        // 抄不到时保持原样（传 nullptr 给 SetAnimInstanceClass 会把已有动画类清掉，别那么干）。
        USkeletalMeshComponent* SourceMesh = EquippedWeapon->GetWeaponMesh();
        UClass* SourceAnimClass = SourceMesh ? SourceMesh->GetAnimClass() : nullptr;
        if (SourceAnimClass)
        {
            FPWeaponMesh->SetAnimInstanceClass(SourceAnimClass);
        }
    }

    // 位置/旋转吸附到挂点、缩放保持世界缩放。手模骨头里带着 100 倍（瓦的米→厘米），
    // 而 WeaponADSSocket 自带的 local scale 是 0.01，正好抵掉 —— 枪的尺寸自动就是对的，
    // 不用像挂普通组件那样手动把相对缩放改成 0.01（那就是 KeepRelative 的坑）。
    FPWeaponMesh->AttachToComponent(
        FPArmsMesh,
        FAttachmentTransformRules::SnapToTargetNotIncludingScale,
        EquippedWeapon->GetFPWeaponSocket());

    FPWeaponMesh->SetVisibility(true);

    // 副本露出来了 → 这把枪的弹匣/瞄准镜挪到副本上（真枪对本人隐藏了，留在真枪上就等于看不见）。
    // **必须把 FPWeaponMesh 传进去**：武器自己反推不出来 —— 捡枪时 SetEquippedWeapon()（→ 这里）
    // 跑在 SetOwner(Character) 前面，那个瞬间武器的 GetOwner() 还是 nullptr，
    // 它反推只会得到真枪，瞄准镜就留在第三人称那个枪位上了。
    EquippedWeapon->UpdateAttachedMeshes(FPWeaponMesh);
}

USkeletalMeshComponent* ABlasterCharacter::GetVisibleFPWeaponMesh() const
{
    // IsVisible 就够判断了：副本只在本地玩家机器上被打开显示（UpdateFPWeaponMesh 里判过
    // IsLocallyControlled，远端机器上那一支直接把它隐藏并返回）。
    if (FPWeaponMesh == nullptr || !FPWeaponMesh->IsVisible()) return nullptr;
    return FPWeaponMesh;
}

void ABlasterCharacter::PlayFPWeaponAnimation(UAnimationAsset* Anim)
{
    if (Anim == nullptr || FPWeaponMesh == nullptr) return;

    // 副本只在本地玩家身上渲染（远端那台连 tick 都关了），别在那些机器上白播
    if (!IsLocallyControlled() || !FPWeaponMesh->IsVisible()) return;

    // 播法和真枪完全一样（同一个静态函数）：蒙太奇走槽位、序列走单节点。
    // 副本是同一个骨架 + 同一个动画蓝图，所以两边会不会被打断的行为也是一致的。
    AWeapon::PlayAnimAssetOnMesh(FPWeaponMesh, Anim);
}

void ABlasterCharacter::SetSpikeDrawn(bool bDrawn)
{
    bSpikeDrawn = bDrawn;

    // 这是**服务器那条路**（UCombatComponent::DrawSpike 调的）。客户端上 bSpikeDrawn 是靠复制
    // 过来的，OnRep 里另挂一次 —— 两处都要，理由见 PlaySpikeEquipAnimations 上的注释。
    if (bDrawn) PlaySpikeEquipAnimations();
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
        // 挂点由武器自己给（和 EquipSlotWeapon 同一处逻辑，远端机器收到复制后也走这里）
        Combat->AttachActorToSocket(EquippedWeapon, EquippedWeapon->GetThirdPersonAttachSocket());
        GetCharacterMovement()->bOrientRotationToMovement = false;
        bUseControllerRotationYaw = true;
    }
    if (EquippedWeapon && LastWeapon && LastWeapon != EquippedWeapon)
    {
        // 切换武器：旧武器挂回闲置 socket（丢弃/切出 spike 由武器自身状态或 OnRep_SpikeDrawn 处理）
        // 注意是 EWS_Holstered（收起来 = 不渲染），不是 EWS_Equipped。
        // 这一句和第 2666 行那句（新枪 EWS_Equipped）是**成对**的：谁少了谁，
        // 画面上就是"切完枪手上空的"或者"背后还飘着一把"。
        LastWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
        Combat->AttachActorToSocket(LastWeapon, Combat->GetHolsterSocketForWeapon(LastWeapon));
    }

    // 第一人称那份枪械副本也得跟着换：SetEquippedWeapon() 里会刷（服务器那条路径），
    // 但客户端走到 OnRep 这个分支时**不会**再经过它 —— 不补这一句，客户端第一人称永远看不到副本。
    // 放在最后是有意的：先让旧枪挂回闲置 socket，再刷副本，换枪时新副本正好接上。
    UpdateFPWeaponMesh();

    // 客户端这一份移速也得跟着换。MaxWalkSpeed 不复制，它是"本机模拟用的参数"：
    // 只有本机自己按新武器设过，本地预测才和服务器算的一致。
    // 服务器那条路径不会走到这里（SetEquippedWeapon 里已经设过），所以不会重复设两次。
    if (Combat) Combat->ApplyMaxWalkSpeed(IsAiming());

    // ── 切枪动画：复制到这一端之后才播（本机手模 + 远端看得见的第三人称身体）──────
    //
    // 这两条以前只在 UCombatComponent::EquipSlotWeapon 里调，而那条路径**只有服务器会跑**
    //（客户端按 1/2/3 走的是 ABlasterCharacter::ServerEquipSlot 这个 RPC）。于是：
    //   · 客户端第一人称手模的掏枪动画永远不播 —— 服务器上 ABlasterCharacter::PlayFPArmsMontage
    //     开头就被 IsLocallyControlled() 挡掉（那不是服务器本地控制的 pawn），
    //     而客户端自己这台机器压根没调过；
    //   · 别人眼前也没有第三人称掏枪蒙太奇 —— UAnimInstance::Montage_Play 是纯本地表现，
    //     **不参与复制**。服务器只是在"它自己那份 pawn"上播了一遍，其他机器上的那份没人调。
    //    （开火没有这个毛病，是因为 Fire() 在射手本机是本地预测跑一遍、其余机器由
    //     UCombatComponent::MulticastFire 那个 NetMulticast 补上；切枪当时漏了这一环。）
    //
    // 挂在 OnRep 上和换弹是同一个思路（换弹走 UCombatComponent::HandleReload ← OnRep_CombatState）：
    // EquippedWeapon 本来就是 ReplicatedUsing，属性复制到的那一刻正好是"该起手"的那一刻，
    // 而且此时 EquippedWeapon 已经是最新值 —— 不会出现"动画到了、武器还没换过来"的错位。
    //
    // 不会和服务器那条路径重复播：OnRep **不在服务器上跑**，服务器由 EquipSlotWeapon 直接调。
    // LastWeapon != EquippedWeapon 的判断把那两条"值没变"的回调挡掉（收枪时 EquippedWeapon
    // 是 nullptr，也会被它挡掉 —— 收枪没有掏枪动画）。
    if (EquippedWeapon && EquippedWeapon != LastWeapon)
    {
        // 第三人称身体：本机自己看不见（那具身体是 SetOwnerNoSee），远端玩家看的就是它
        PlayEquipMontage();

        // 第一人称手模：只有本机自己这台会真的播，函数内部判 IsLocallyControlled。
        // 必须放在 UpdateFPWeaponMesh() 之后 —— 这里已经把新枪的副本换好了。
        EquippedWeapon->PlayFPEquipMontage();
    }

    // 「手上换东西了」—— 服务器那条路走 SetEquippedWeapon 里的调用，客户端这条路只能在这儿补，
    // 少了它客户端永远不知道"手上的飞刀被换成枪了"（Jett 那 5 把刀就藏不掉）。
    // ⚠ 无条件调（不放在上面那个 if 里）：收枪（EquippedWeapon == nullptr）同样是"手上换了"，
    //   飞刀显隐正需要在那时跟着关掉。
    OnEquippedWeaponChanged();
}

void ABlasterCharacter::OnRep_SpikeDrawn()
{
    if (!Combat) return;

    if (bSpikeDrawn)
    {
        // 切出 spike：把当前手持枪挂回闲置（spike 挂手由 attachment 复制完成）
        if (EquippedWeapon)
        {
            EquippedWeapon->SetWeaponState(EWeaponState::EWS_Holstered);
            Combat->AttachActorToSocket(EquippedWeapon, Combat->GetHolsterSocketForWeapon(EquippedWeapon));
        }

        // 客户端这条路的装备动画（服务器那条在 SetSpikeDrawn 里）。
        // 这里所有机器都会跑到：本人那台由 PlayFPArmsMontage 出手模那份，
        // 别人的机器由 PlayThirdPersonMontage 出身体那份。
        PlaySpikeEquipAnimations();
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

void ABlasterCharacter::AddRecoil(float Pitch, float Yaw, float MaxPitch)
{
    if (!IsLocallyControlled() || Pitch <= 0.f) return;
    // 累加并限幅：连发会小幅累积（上限来自当前武器，如步枪 5.5°），松手后由回稳拉回
    RecoilPitchOffset = FMath::Min(RecoilPitchOffset + Pitch, MaxPitch);
    RecoilYawOffset = FMath::Clamp(RecoilYawOffset + Yaw, -MaxPitch, MaxPitch);
}

float ABlasterCharacter::GetFPCrouchDrop() const
{
    // 幅度由 Tick 采样（蹲着时每帧刷新），这里只乘过渡进度。
    // 为什么不在这里实时算幅度：起身时胶囊立刻长回站高、"实时差值"当场变 0，
    // 「落回站高」那半段过渡就没了 —— 见 Tick 里那段注释。
    return FPCrouchDropDistance * FPCrouchDropAlpha;
}

float ABlasterCharacter::ComputeFPCapsuleCrouchDelta() const
{
    const UCapsuleComponent* Capsule = GetCapsuleComponent();
    if (Capsule == nullptr) return 0.f;

    // 站立（默认）胶囊半高从 CDO 取 —— 引擎自己也是这么算的：
    // UCharacterMovementComponent::Crouch 里那句
    //   HalfHeightAdjust = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - ...
    // 实时相减而不是写死 48：胶囊半高在 BP 里可以改，蹲下值也可能被 CharacterMovement 调过，
    // 写死的话两边会悄悄对不上（眼位一个值、胶囊一个值）。
    const ABlasterCharacter* DefaultCharacter = GetDefault<ABlasterCharacter>(GetClass());
    const UCapsuleComponent* DefaultCapsule = DefaultCharacter ? DefaultCharacter->GetCapsuleComponent() : nullptr;
    if (DefaultCapsule == nullptr) return 0.f;

    return FMath::Max(0.f,
        DefaultCapsule->GetScaledCapsuleHalfHeight() - Capsule->GetScaledCapsuleHalfHeight());
}

FVector ABlasterCharacter::GetFPEyeWorldLocation() const
{
    // 本机：相机就在眼位上（UpdateFPRig 每帧把它摆过去），直接问它 —— 这是玩家实际透过来看世界
    // 的那个点，准星对应的射线就是从这儿出发的（见 CombatComponent::TraceUnderCrosshairs）。
    if (IsLocallyControlled() && FPCamera)
    {
        return FPCamera->GetComponentLocation();
    }

    // 其余情况（服务器给远端角色记回溯帧）：按「身体网格变换 × 固定眼高」算。
    // 和 UpdateFPRig 用的是同一套约定（骨架原点在脚底），所以算出来是同一个点。
    // 和本机那个值的差别只有手臂呼吸带着相机骨骼浮动的那几厘米 —— 服务器不可能知道远端玩家的
    // 动画相位，这点误差换的是「起点完全由服务器说了算」（客户端的 RPC 参数里已经没有起点了）。
    const USkeletalMeshComponent* BodyMesh = GetMesh();
    const FVector EyeWorld = BodyMesh
        ? BodyMesh->GetComponentTransform().TransformPosition(FPEyeLocalOffset)
        : GetActorTransform().TransformPosition(FPEyeLocalOffset);

    // 蹲下再往下移一段（理由见 GetFPCrouchDrop 的注释）：身体网格在蹲下时**不会动**，
    // 所以这一步必须显式减 —— 否则服务器给蹲着的射手记的回溯眼位还是站高，
    // 他那一枪的判定射线会从站立高度出发（蹲在掩体后也照打）。
    return EyeWorld - FVector(0.f, 0.f, GetFPCrouchDrop());
}

void ABlasterCharacter::UpdateFPRig(float DeltaTime)
{
    // 相机没激活 = 这台机器上不是本人在用第一人称（相机归属由 RefreshFPRig 定，它就在本函数前一行跑）。
    // 远端角色的手模既不渲染、tick 也关着，这里直接不干活。
    if (FPArmsMesh == nullptr || FPCamera == nullptr || !FPCamera->IsActive()) return;

    // ——— ① 后坐力回稳 ———
    // 回稳速度按当前手持武器取（手枪利落回正、狙击慢沉），没枪时用默认值。
    float RecoverySpeed = 8.f;
    if (AWeapon* Equipped = GetEquippedWeapon())
    {
        RecoverySpeed = Equipped->GetRecoilRecoverySpeed();
    }
    RecoilPitchOffset = FMath::FInterpTo(RecoilPitchOffset, 0.f, DeltaTime, RecoverySpeed);
    RecoilYawOffset = FMath::FInterpTo(RecoilYawOffset, 0.f, DeltaTime, RecoverySpeed);

    // ——— ② 把手模摆到眼睛上，朝向 = 视角 × 后坐力 ———
    //
    // 手模骨架的原点在**脚底**，而相机骨骼（FPCameraBone）离它有 ≈149cm。
    // 想让「抬头低头」是原地转而不是画圆弧，旋转枢轴就必须落在眼睛上 ——
    // 而枢轴永远是手模自己的原点。所以把原点挪到眼睛后面去：
    // 手模原点摆在「眼睛位置退开 C」，骨骼正好补回来落在眼睛上，
    // 于是「绕原点转」= 「绕眼睛转」。两行就是全部：位置对齐到眼睛 + 朝向跟着视角。
    //
    // 那 149cm 绝不写死：每帧从**当前姿势**读骨骼位置（idle 的呼吸会让它上下浮动）。
    // 读的是 ComponentSpace —— 只跟姿势有关、跟手模被摆到哪儿无关，
    // 所以不存在「读位置 → 写位置 → 位置又变」的追尾反馈环。
    const FVector CameraBoneLoc =
        FPArmsMesh->GetBoneLocation(FPCameraBone, EBoneSpaces::ComponentSpace);

    // 朝向 = 视角朝向 × 后坐力偏移。后坐力乘在**局部**（先转视角再叠后坐），
    // 和以前"加在中间节点上"是同一件事，只是不再需要一个节点来当支点。
    // 后坐力只改朝向 —— 相机在枢轴上，枢轴不动它就不平移，不会有"镜头平移十几厘米"的滑移感。
    const FQuat RigRotation =
        FQuat(GetViewRotation()) * FQuat(FRotator(RecoilPitchOffset, RecoilYawOffset, 0.f));

    // 眼睛的世界位置：把手模骨骼空间里的那个点，用身体网格的变换换算到世界。
    // 用 GetMesh() 当参照是因为手模和第三人称身体共用同一套空间约定（原点都在脚底），
    // 这样身体网格在 BP 里被挪动/缩放时手模也跟着对。
    const USkeletalMeshComponent* BodyMesh = GetMesh();
    const FVector BodyEye = BodyMesh
        ? BodyMesh->GetComponentTransform().TransformPosition(CameraBoneLoc)
        : GetActorTransform().TransformPosition(CameraBoneLoc);

    // 蹲下：整条手模（连带相机）跟着往下移一段。
    // 参照物 GetMesh() 在蹲下时**一动不动** —— 引擎把胶囊往下挪了多少，就把网格的相对 Z 往上补了多少
    //（见 GetFPCrouchDrop 的注释），所以必须在这里自己减，否则蹲下在第一人称里完全没有表现。
    // 相机在枢轴上，减这段等于相机和手臂一起降；朝向不受影响。
    const FVector EyeWorld = BodyEye - FVector(0.f, 0.f, GetFPCrouchDrop());

    FPArmsMesh->SetWorldLocationAndRotation(
        EyeWorld - RigRotation.RotateVector(CameraBoneLoc),
        RigRotation);

    // 结果是：相机世界位置 = 手模原点 + 旋转后的 C = EyeWorld，恒在眼睛上，
    // 只有朝向随视角/后坐力变；手臂的呼吸则通过 C 带着镜头微微起伏。
    // 手模被摆到哪儿不影响骨骼的**局部**位置，所以上面那次读值下一帧依然有效。
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
        // 护甲和血量同一条 HUD 推送链路（血条 + 甲条/甲文字一起刷），少一条独立路径就少一处漏刷
        BlasterPlayerController->SetHUDHealth(Health, MaxHealth, Armor, MaxArmor);
    }
}

void ABlasterCharacter::OnRep_Armor()
{
    UpdateHUDHealth();
}

void ABlasterCharacter::SetArmor(float NewArmor)
{
    Armor = FMath::Clamp(NewArmor, 0.f, MaxArmor);

    // 权威机改自己的复制属性不会触发 OnRep，得手动刷一次 HUD（客户端靠 OnRep_Armor 走同一条）
    UpdateHUDHealth();
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

void ABlasterCharacter::SetOverlappingOrb(AUltOrb* Orb)
{
    OverlappingOrb = Orb;
}

void ABlasterCharacter::ClearOverlappingOrb(AUltOrb* Orb)
{
    // 只在"离开的正好是我现在记着的那颗球"时才清空。
    // 直接无条件置空会有这个 bug：站在 A、B 两颗球之间时，走出 A 的 EndOverlap 会把
    // 还没离开的 B 的引用一并清掉，人还站在 B 里却按 F 没反应。
    if (OverlappingOrb == Orb)
    {
        OverlappingOrb = nullptr;
    }
}

void ABlasterCharacter::SetChannelingOrb(AUltOrb* Orb)
{
    ChannelingOrb = Orb;
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

void ABlasterCharacter::HideSniperScopeIfAiming()
{
    // 镜圈是 AddToViewport 挂在视口上的 widget，**不跟着角色一起销毁** —— 回合切换时角色是
    // 被 Reset()+Destroy() 掉的，如果不显式收一次，上回合开着的镜就留在屏幕上了
    //（阵亡那条路有"手上还得是把狙击枪"的条件，收枪/丢枪之后那条路就不再成立，
    //  这就是真正漏掉的那种情况）。
    //
    // 只认「本机 + 正开着镜」：bAiming 是那个 widget 被打开过的唯一来源
    //（ShowSniperScopeWidget 全项目只在 UCombatComponent::SetAiming 里被调用），
    // 加这道判是为了不对手模从没开过镜的实例去调 —— 那会白建一个 widget 出来。
    if (IsLocallyControlled() && bAiming)
    {
        ShowSniperScopeWidget(false);
    }
}

UWidgetAnimation* ABlasterCharacter::FindScopeAnimation() const
{
    if (SniperScopeWidgetInstance == nullptr || SniperScopeAnimationName.IsNone())
    {
        return nullptr;
    }

    // UMG 动画不是组件，而是**挂在控件类上的对象**，两种取法：
    // ① 类上的同名属性 —— 蓝图里那条动画就是以属性形式挂在控件类上的
    //   （BP 里 PlayAnimation 的动画引脚连的那个 get 节点，取的正是这个属性），
    //   所以这条最贴原行为；
    UClass* WidgetClass = SniperScopeWidgetInstance->GetClass();
    if (FObjectPropertyBase* AnimationProperty = FindFProperty<FObjectPropertyBase>(WidgetClass, SniperScopeAnimationName))
    {
        if (UWidgetAnimation* Animation = Cast<UWidgetAnimation>(
                AnimationProperty->GetObjectPropertyValue_InContainer(SniperScopeWidgetInstance)))
        {
            return Animation;
        }
    }

    // ② 兜底：生成类里那份动画清单。
    if (const UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetClass))
    {
        for (UWidgetAnimation* Animation : GeneratedClass->Animations)
        {
            if (Animation && Animation->GetFName() == SniperScopeAnimationName)
            {
                return Animation;
            }
        }
    }
    return nullptr;
}

void ABlasterCharacter::ShowSniperScopeWidget_Implementation(bool bShowScope)
{
    // 镜框只画在本机上（调用方 UCombatComponent::SetAiming 那道门也只放本地控制的角色进来，
    // 这里再兜一道：服务器/别人身上没必要建 widget）。
    if (!IsLocallyControlled())
    {
        return;
    }

    // ① 头一回开镜才建，建完一直挂在视口上 —— 跟原 BP 一样，不销毁、不开镜时也不摘。
    if (SniperScopeWidgetInstance == nullptr)
    {
        if (SniperScopeWidgetClass == nullptr)
        {
            return; // 没配控件类：什么都不做（没法画镜，但也不该崩）
        }
        if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
        {
            SniperScopeWidgetInstance = CreateWidget<UUserWidget>(PlayerController, SniperScopeWidgetClass);
        }
        else
        {
            SniperScopeWidgetInstance = CreateWidget<UUserWidget>(GetWorld(), SniperScopeWidgetClass);
        }
        if (SniperScopeWidgetInstance == nullptr)
        {
            return;
        }
        SniperScopeWidgetInstance->AddToViewport(0);
    }

    // ② 开关镜 = 那条动画正放 / 倒放各一次（镜框的显隐是动画自己做的，不是 SetVisibility）
    if (UWidgetAnimation* ScopeAnimation = FindScopeAnimation())
    {
        SniperScopeWidgetInstance->PlayAnimation(
            ScopeAnimation,
            0.f,                                                     // StartAtTime
            1,                                                       // NumLoopsToPlay
            bShowScope ? EUMGSequencePlayMode::Forward : EUMGSequencePlayMode::Reverse,
            1.f,                                                     // PlaybackSpeed
            false);                                                  // bRestoreState
    }

    // ③ 音效：2D 的 UI 音，跟着开/关镜各响一次
    if (USoundBase* Sound = bShowScope ? SniperScopeZoomInSound : SniperScopeZoomOutSound)
    {
        UGameplayStatics::PlaySound2D(this, Sound, 1.f, 1.f, 0.f, nullptr, nullptr, true);
    }
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
            // EquipButtonAction 现在承担「捡枪 + 吃大招球」两件事（键位在 IMC 里是 F）。
            // 属性名保持 EquipButtonAction 没改 —— 改名会让 BP_BlasterCharacter 里
            // 已经连好的资产引用断掉，得手动重连一次，不值当。
            EnhancedInputComponent->BindAction(EquipButtonAction, ETriggerEvent::Started, this, &ABlasterCharacter::Pickup);
            EnhancedInputComponent->BindAction(EquipButtonAction, ETriggerEvent::Completed, this, &ABlasterCharacter::PickupCancel);
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
        if (UltAction)
        {
            EnhancedInputComponent->BindAction(UltAction, ETriggerEvent::Started, this, &ABlasterCharacter::UltimatePressed);
        }
        if (SkillQAction)
        {
            EnhancedInputComponent->BindAction(SkillQAction, ETriggerEvent::Started, this, &ABlasterCharacter::SkillQPressed);
        }
        if (SkillCAction)
        {
            EnhancedInputComponent->BindAction(SkillCAction, ETriggerEvent::Started, this, &ABlasterCharacter::SkillCPressed);
            /*
             * 松开 C = 逐风云"控云"结束（见 ABlasterCharacter::EndCloudburstHold）。
             *
             * 为什么 Completed 能拿到"松手"：IA_SkillC 在 IMC 里挂的是默认的 Down 触发器 ——
             * 按下那一帧发 Started + Triggered，**松手**那一帧从 Triggered 回到 None，
             * 增强输入这时发的就是 Completed（Canceled 是"还没触发就断了"，比如窗口失焦）。
             * 所以 Q / E / X 那些"按一下就完事"的技能只绑 Started，C 需要多这一条。
             *
             * 这里**不判**"到底在不在按住"就直接转给服务器：客户端手上那份 bCloudburstHoldActive
             * 是复制来的，按得比一个 RTT 还短的话它还是 false —— 拿它当门禁会把这次松手吞掉，
             * 人一直空着手到保险丝到点。服务器那边 EndCloudburstHold 自带幂等门禁，多发无害。
             * （bDisableGameplay 那道门还是要的：大厅里松个 C 没必要发 RPC。）
             */
            EnhancedInputComponent->BindAction(SkillCAction, ETriggerEvent::Completed, this, &ABlasterCharacter::SkillCReleased);
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
        if (InspectAction)
        {
            EnhancedInputComponent->BindAction(InspectAction, ETriggerEvent::Started, this, &ABlasterCharacter::Inspect);
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

EBlasterAgent ABlasterCharacter::GetAgent() const
{
    // 直接问 PlayerState（唯一数据源），不缓存 —— 缓存就得处理"换英雄"时的失效，
    // 而这里每帧被动画蓝图问一次，问一次 O(1)，不值得省。
    // PlayerState 为 null 是正常情况（刚 spawn / 编辑器预览），返回 None 让调用方判。
    const ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
    return PS ? PS->GetAgent() : EBlasterAgent::None;
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
        Entry.SkillSlotIndex = Ability->SkillSlotIndex;
        Entry.MaxCharges = FMath::Max(0, Ability->MaxCharges);
        // 图标也跟着复制：观战者拿不到队友的 ASC 能力，只能从这份快照里取贴图
        Entry.SkillIcon = Ability->SkillIcon;

        const FBlasterAbilityCooldownInfo Info = Ability->GetCooldownInfo(AbilitySystemComponent);
        Entry.Charges = FMath::Clamp(Info.Charges, 0, Entry.MaxCharges);
        Entry.bCooldownValid = Info.bValid ? 1 : 0;
        Entry.CooldownDuration = Info.bValid ? Info.CooldownDuration : 0.f;

        // 有正在冷却的层：记录最早到期那层结束的绝对服务器时间 → 客户端用 GetServerTime 平滑倒数。
        // 量化到 0.1s：避免每帧 float 抖动让整块数组反复重发。
        // （无限时长冷却如 Jett 腾空/Clove 的两个技能：TimeUntilNextCharge=0 → 这里不写，行为与本地灰色点一致）
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
