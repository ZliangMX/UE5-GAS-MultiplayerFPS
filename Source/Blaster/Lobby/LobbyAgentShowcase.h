#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "LobbyAgentShowcase.generated.h"

class UCameraComponent;
class USkeletalMeshComponent;
class UStaticMeshComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UMaterialInterface;
class UAnimSequence;

/*
 * 手上一个挂件（刀 / 火球）怎么挂在角色骨架上。
 *
 * 为什么做成"属性"而不是让你在视口里拖组件：
 *   挂点 R_WeaponPoint 是**骨骼**，姿势由动画逐帧驱动 —— 在视口里拖出来的相对变换
 *   会被下一次刷新原样覆盖掉，存不住（和本文件相机那四个数是同一个理由）。
 *
 * 调法：选中 BP_LobbyAgentShowcase 的 **actor 本身**（不是组件），
 * 细节面板 "Lobby Showcase|Hand Prop" 分类下改 KnifePlacement / FireballPlacement。
 */
USTRUCT(BlueprintType)
struct FLobbyHandPropPlacement
{
	GENERATED_BODY()

	// 挂在哪根骨/哪个 socket 上。**骨骼名和 socket 名引擎都认** ——
	// 选人这两套骨架（CS_Wushu / CS_Phoenix）上 R_WeaponPoint 是**骨骼**不是 socket，
	// 名字里没 "Socket" 也照样能挂（实测四套骨架里都有这根骨）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FName Socket = TEXT("R_WeaponPoint");

	// 在挂点局部空间里的额外位移（cm）。挂点位置不理想时靠它微调。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FVector Location = FVector::ZeroVector;

	// 额外旋转（度）。刀的朝向一般要转一下才像个"握着"的样子。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FRotator Rotation = FRotator::ZeroRotator;

	// **世界口径**的大小（1 = 静态网格资产原大小）。
	//
	// ⚠️ 不是组件的相对缩放：挂在骨骼上会连**父级缩放一起继承**，选人骨架带 0.01
	//    这类整体缩放的话，直接写 1 会让 19cm 的刀变成 0.19cm（肉眼看不见，
	//    BP 面板上也看不出任何异常）。所以相对缩放按挂点的世界缩放反算，
	//    让这个值等于"你在引擎里看到的实际大小"。换算和 AJettCharacter::RefreshKnifeMeshes、
	//    AWeapon::AttachDecorationToSocket 是同一套。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float Scale = 1.f;
};

// Lobby 中间那个"我选的英雄"。
//
// 是**每个客户端本地 spawn 一个**（ULobbyOverlay 里建、随 overlay 一起销毁），
// 不复制、也不摆在 Lobby.umap 里 —— 因为大厅里每个人看的是自己选的角色，
// 别人选什么跟自己这块画面无关。这样也完全不用碰 GameMode/Pawn 的复制。
//
// 造型走 Content/ValorantAssets/CharSelect 下那套专用的选人网格：
//   出场 Intro 播一次 → 定时器到点接到 Idle 循环。
// 两条都是 **AnimSequence**（不是 Montage），所以不需要槽轨道那套东西，直接
// PlayAnimation 就行（Montage 的 Python/C++ 读写坑在这里完全绕开了）。
//
// ★ 顺带一条：**这条路上动画通知照常会响**，不需要为了通知去做蒙太奇。
//   PlayAnimation 走的是单节点动画实例（UAnimSingleNodeInstance），而它内部就是拿
//   一个 FAnimTickRecord 进同步组 → UAnimSequenceBase::TickAssetPlayer(..., NotifyQueue, ...)
//   → UAnimInstance::TriggerAnimNotifies —— 和普通动画图是同一条派发链
//   （引擎源码 AnimSingleNodeInstanceProxy.cpp 的 FAnimNode_SingleNode::Update_AnyThread、
//   AnimInstanceProxy.cpp 那两处 TickAssetPlayer 调用）。
//   所以"他什么时候把火球搓出来"这种时机可以放心交给动画通知。
//
// 相机是本 actor 的一个组件，由 ULobbyOverlay 在 spawn 后 SetViewTarget 过去 ——
// 这就是"固定机位"：玩家照样能操作自己的 Pawn，但视角锁在展示位上，
// 屏幕上只有选人画面。
UCLASS()
class BLASTER_API ALobbyAgentShowcase : public AActor
{
	GENERATED_BODY()

public:
	ALobbyAgentShowcase();

	// 切到该英雄的造型并重播出场动画。
	// 表里没有该英雄（Sage / Clove 现在就没导资源）→ 整个 actor 隐藏，大厅中间留空。
	void SetAgent(EBlasterAgent Agent);

	FORCEINLINE UCameraComponent* GetShowcaseCamera() const { return Camera; }
	FORCEINLINE EBlasterAgent GetCurrentAgent() const { return CurrentAgent; }

	// --- 相机摆位（蓝图子类里可直接调）---
	// 在 BP_LobbyAgentShowcase 里选中 **actor 本身**（不是 ShowcaseCamera 组件），
	// 细节面板的 "Lobby Showcase|Camera" 分类下就能改这几个数；OnConstruction
	// 会立刻重新摆一次，所以编辑器视口里改动是即时可见的。
	//
	// 网格朝哪：UE 角色惯例是面朝 +X；CharSelect 网格的包围盒也是 X 最薄（前后向），
	// 所以按面朝 +X 做，相机默认放 +X 一侧回头看（yaw=180）。
	// ⚠️ 注意：值守恒的是这四个数 —— 直接在视口里拖 ShowcaseCamera 组件得到的
	//    变换会被 OnConstruction 覆盖掉，要调就改这里的值。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Camera")
	float CameraForwardOffset = 260.f;   // 相机在角色前方多远（cm，沿 +X）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Camera")
	float CameraHeight = 185.f;          // 相机高度（cm）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Camera")
	float CameraPitch = -2.f;            // 相机俯仰（负 = 略微俯视）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Camera")
	float CameraYaw = 180.f;             // 相机朝向（180 = 回头看角色）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Camera")
	float CameraFOV = 45.f;              // 相机视场角

	// --- 手上那个**常驻**挂件：Jett 的刀 ---
	// 网格路径写在 FindShowcaseEntry 的表里（哪个英雄拿什么），这里只管怎么挂。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Hand Prop")
	FLobbyHandPropPlacement KnifePlacement;

	// --- 手上那个**由动画通知开关**的火球：火男的闪光弹 ---
	// 什么时候出现由你摆在动画上的 UAnimNotify_LobbyHeldFireball 说了算
	//（"哪一帧搓出火球"只有动画知道，代码按秒数猜是猜不准的）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Hand Prop")
	FLobbyHandPropPlacement FireballPlacement;

	/*
	 * 火球的**特效**（Niagara）。填了就用它，下面那个占位球自动藏掉；留空就还是占位球。
	 * 和角色那边（ABlasterCharacter::HeldFireballEffect）完全同一个规矩 ——
	 * 想和外面那颗长得一样，把 APhoenixFireball::FlightEffect 用的同一个资产填过来即可。
	 *
	 * ⚠️ FireballPlacement.Scale 是给半径 50 的引擎球换算的，**不要**套到特效上
	 *    （那等于把特效整体缩 3.3 倍）。特效按资产自己的大小播，要调有下面这个。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Hand Prop")
	TObjectPtr<UNiagaraSystem> HandFireballEffect;

	// 特效的缩放（1 = 特效资产原大小）。只在填了 HandFireballEffect 时有意义。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Hand Prop")
	float HandFireballEffectScale = 1.f;

	// 占位球的材质。留空 = 项目里那个发光球材质（M_GlowSphere_01），和角色手上那颗一致。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lobby Showcase|Hand Prop")
	TObjectPtr<UMaterialInterface> HandFireballMaterial;

	/*
	 * 手上火球的显隐。**由动画通知调用**（UAnimNotify_LobbyHeldFireball），
	 * 也可以从蓝图里直接调 —— 排通知之前想先看一眼效果的话，这是最快的路子。
	 *
	 * 换英雄（SetAgent）时会自动复位成"没有火球"，上一个英雄搓出来的球不会留在下一个人手上。
	 */
	UFUNCTION(BlueprintCallable, Category = "Lobby Showcase|Hand Prop")
	void SetHandFireballVisible(bool bShow);

	FORCEINLINE bool IsHandFireballVisible() const { return bHandFireballVisible; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Lobby Showcase")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Lobby Showcase")
	TObjectPtr<UStaticMeshComponent> HandProp;

	// 占位球 / 特效二选一（和 ABlasterCharacter 那两颗球同样的分工）：
	// 填了 HandFireballEffect 就开这个 FX、把占位球收起来，没填就反过来。
	UPROPERTY(VisibleAnywhere, Category = "Lobby Showcase")
	TObjectPtr<UStaticMeshComponent> HandFireballMesh;

	UPROPERTY(VisibleAnywhere, Category = "Lobby Showcase")
	TObjectPtr<UNiagaraComponent> HandFireballFX;

	UPROPERTY(VisibleAnywhere, Category = "Lobby Showcase")
	TObjectPtr<UCameraComponent> Camera;

	// 出场动画播完 → 切 Idle 循环
	FTimerHandle IntroTimerHandle;
	void OnIntroFinished();

	// 一个英雄的一整套展示资源（全是 CharSelect 那套专用资产）
	struct FShowcaseEntry
	{
		const TCHAR* MeshPath;
		const TCHAR* IntroPath;    // 出场，播一次
		const TCHAR* IdlePath;     // 待机，循环
		// 手上**常驻**的挂件（静态网格）。留空 = 这个英雄手上不拿东西。
		// 走通知的那类（火男的火球）**不**填这儿，它由 UAnimNotify_LobbyHeldFireball 开关。
		const TCHAR* HandPropPath;
	};

	// 有资源的英雄在这里登记。查不到 = 大厅中间留空。
	static bool FindShowcaseEntry(EBlasterAgent Agent, FShowcaseEntry& OutEntry);

	// 把空材质槽（parent 为 None 的 MIC）换成引擎默认材质 —— Jett 的身体/头发三个槽
	// 就是这样：MIC 资产在，但 parent 是空的，而工程里没有对应的 M_TP_Wushu_* 源材质
	// 可以补，所以只能先顶个灰模。运行时覆盖，**不动资产**。
	void PatchBrokenMaterialSlots();

	// 按当前英雄把两个挂件的**网格 / 挂点 / 缩放**配齐（可见性单独说）。
	// OnConstruction 和 SetAgent 各调一次。
	void RefreshHandProps();

	// 只重算**可见性**（挂点那些不变），给通知用。
	void ApplyHandFireballVisual();

	// 把"挂点 + 世界口径大小 + 额外位移/旋转"套到某个组件上。
	// 形参是 USceneComponent*：静态网格和 Niagara 组件共用这一套换算，
	// 唯一不同的是 Scale 从哪来（占位球读 FireballPlacement.Scale，特效读 HandFireballEffectScale）。
	void ApplyPlacement(USceneComponent* Comp, const FLobbyHandPropPlacement& Placement);

	EBlasterAgent CurrentAgent = EBlasterAgent::None;

	// 火球现在该不该在手上。默认 false —— 靠动画通知开。
	bool bHandFireballVisible = false;

	// 挂点找不到只报一次（每次刷新都刷屏没意义）
	bool bLoggedMissingSocket = false;

	// 把上面那四个数/角度套到 Camera 组件上（构造和 OnConstruction 各调一次）
	void ApplyCameraPlacement();
};
