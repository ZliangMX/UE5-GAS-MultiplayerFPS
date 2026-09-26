#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Blaster/BlasterTypes/SpikeState.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPtr.h"
#include "Spike.generated.h"

class UAudioComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;
class UAnimMontage;
class USkeletalMesh;
class UAnimInstance;

/** spike 该挂在携带者身上的哪一套挂点上（具体槽位名是 ASpike 上的 EditDefaultsOnly 属性）*/
UENUM()
enum class ESpikeAttachSlot : uint8
{
	ESAS_Carry, // 背在携带者身上（常态隐藏，见 ApplyMeshVisibility）
	ESAS_Draw,  // 掏在手上（右手）
	ESAS_Plant  // 下包中：跟下包动画驱动的武器锚点走
};

UCLASS()
class BLASTER_API ASpike : public AActor
{
	GENERATED_BODY()

public:
	ASpike();
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// --- Components ---
	// 根组件 = 方块碰撞体。物理掉落要求「模拟物理的组件必须是根组件」，所以根从 SpikeMesh 换成盒子，
	// SpikeMesh 降为它的子件（相对位移见 PostInitializeComponents）。
	// 平时 NoCollision；Pickup 时 QueryOnly + Pawn 重叠（拾取/拆包判定）；掉包时 QueryAndPhysics 真模拟物理。
	UPROPERTY(VisibleAnywhere)
	class UBoxComponent* CollisionBox;

	// 视觉载体（子件）。安包后由代码驱动 Core 转角、发光变色；动画机 ABP_Spike 负责把 CoreSpinDegrees
	// 转成骨旋转并表现拆包进度
	UPROPERTY(VisibleAnywhere, Category = "Spike")
	class USkeletalMeshComponent* SpikeMesh;

	/*
	 *方块碰撞体尺寸（**半长**，cm —— 是「中心到面」的距离，不是全长）。
	 *
	 *零向量（默认）= 自动取骨骼网格的导入包围盒（约 22 × 24 × 40），方块紧贴模型外观。
	 *非零 = 以你填的值为准；填三个相等值就是严格正立方体。
	 *
	 ***只有一个自由度：盒子多大。底部对齐是自动的** —— 网格子件按 -包围盒中心 平移过，
	 *所以「方块中心 = 网格几何中心」，方块底天然和网格底齐平。注意贴地摆放（FreezeAtLocation）
	 *用的是**网格底**、不是方块底：盒子比模型大时，多出来的部分会扎进地面，这是正常的。
	 *
	 *怎么调（和命中盒 bDrawDebugRewind 是同一套流程）：
	 *  · 先读下面的「自动尺寸」只读值 —— 那是填零时真正生效的数，照着它微调最省事
	 *  · 在关卡里选中 Spike（或它的 CollisionBox / SpikeMesh 组件）→ 视口出现**橙色线框**
	 *    = 方块实际大小，青色扁框 = 网格底面（贴地基准）；直接改这个属性线框**实时**跟手，
	 *    不用重开关卡（PostEditChangeProperty 会立刻重算）
	 *  · 想在 PIE 里看真实效果勾 `bDrawDebugCollisionBox`
	 *
	 *改这里会同时影响三件事：拾取重叠范围（人能凑多近捡起来）、掉包物理的翻滚/卡位、
	 *拆包判定范围。碰撞通道和响应在 BeginPlay 里设，与尺寸无关，不用跟着改。
	 */
	UPROPERTY(EditAnywhere, Category = "Physics|Collision")
	FVector CollisionBoxExtentOverride = FVector::ZeroVector;

	// 只读：按网格导入包围盒算出来的「自动尺寸」（= Override 填零时真正生效的那个值）。
	// 每次 SetupCollisionBoxFromMesh 重算一次。想微调就先读这个数，再往 Override 里填绝对值。
	UPROPERTY(VisibleAnywhere, Category = "Physics|Collision")
	FVector AutoCollisionBoxExtent = FVector::ZeroVector;

	// 调试：PIE 里画出方块碰撞体（橙）+ 网格底面（青色扁框）。
	// 和命中盒的 bDrawDebugRewind 同款用途 —— 平时关，调尺寸/查拾取范围时打开。
	// EditAnywhere：PIE 运行中也能在 Details 面板勾选，不用停。
	UPROPERTY(EditAnywhere, Category = "Physics|Collision|Debug")
	bool bDrawDebugCollisionBox = false;

	// 当前该用的方块尺寸（半长）：Override 非零取 Override，否则取网格包围盒自动值。
	// 编辑器可视化器也调它，保证「视口画的」和「运行时真正设的」永远是同一个数。
	FVector ComputeCollisionBoxExtent() const;

	// --- Config: socket name for attaching to character ---
	UPROPERTY(EditDefaultsOnly, Category = "Attachment")
	FName CarryAttachSocket = FName("SpikeSocket");

	// 掏出后握在右手时的角色骨骼网格 socket（默认 "RightHandSocket"）
	UPROPERTY(EditDefaultsOnly, Category = "Attachment")
	FName DrawAttachSocket = FName("RightHandSocket");

	/*
	 * 下包（按住安包）**期间**用的挂点，默认 "SpikeSocket"。
	 *
	 * 为什么要单独一套：这套 Valorant 骨架里 SpikeSocket 挂在
	 * `MasterWeaponAim → MasterWeapon → L_WeaponMaster` 这条**武器/道具锚点链**上，
	 * 而这条链是**被动画驱动**的（不是固定的手/背挂点）——
	 *
	 *   · 常态/握持（ABP 那些 TP_Wushu_S0_AO_* 基础姿势）：锚点落在**角色脚底**
	 *     （2026-09-25 在大厅真人角色上实测 L_WeaponMaster = 网格空间 (-15.96,-3.72,-8.15)，
	 *      网格原点就是脚底、世界 Z 还在地面下 6.5cm）⇒ 拿它当 DrawAttachSocket
	 *     表现就是「按 4 掏出包，包看不见」（包被塞进地板/脚里）。
	 *   · 下包动画 TP_Core_Bomb_S0_Activate_UB 恰恰是「把这条锚点摆到地面」的动画：
	 *     139 帧里锚点 Z 从 111 → 74 → 59 → **5.9cm**、X 到正前方 84cm
	 *     （同一动画里右手 R_WeaponPoint 全程停在 Z≈80~110cm）⇒ **下包时它才是对的位置**，
	 *     包会跟着动画一路压到地面，接得上 FreezeAtLocation 的落地摆放。
	 *
	 * 所以：携带 = CarryAttachSocket（常态隐藏），握持 = DrawAttachSocket（右手），
	 * 下包中 = 这里（跟动画锚点），下包完成走 FreezeAtLocation，取消则换回 DrawAttachSocket。
	 * 名字无效时一律退到 FallbackAttachPoint（不会静默挂在网格原点）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Attachment")
	FName PlantAttachSocket = FName("SpikeSocket");

	/*
	 * 上面这些名字（以及 DefuseKitSocketTP / DefuseKitSocketFP）如果找不到，就退到这一个。
	 *
	 * 2026-09-22 实测 TP_Wushu_S0 / FP_Wushu_S0：RightHandSocket / Spike / GrenadeSocket
	 * 这三个名字**既不是 socket 也不是骨骼**（是照别的骨架写的），
	 * 真实可用的是 R_WeaponMaster 这根右手骨、TP 身体上的
	 * SpikeSocket / WeaponSocket_Rifle / _Pistol / _Boltsniper，以及 1P 手模上的
	 * MasterWeaponSocket / MeleeSocket / L_WeaponPointSocket（同名槽在两块网格上不通用）。
	 * 名字无效时 AttachToComponent 会静默落到网格原点（包贴在脚底），日志里一个字都没有
	 * —— 兜底 + 打警告，别让它静默。
	 *
	 * ⚠ 注意 TP 上的 SpikeSocket 虽然是**真槽**，但它父骨是上面那条动画锚点链，
	 *   常态下落在脚底（见 PlantAttachSocket 的注释）—— 「名字有效」不等于「位置能用」。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Attachment")
	FName FallbackAttachPoint = FName("R_WeaponMaster");

	/*
	 * 把配置里的挂点名解析成这套骨架上真的能用的名字（socket 和骨骼都算能用，
	 * 因为 AttachToComponent 两者都吃）。无效时警告一次并返回 FallbackAttachPoint。
	 * 返回 NAME_None 表示连兜底都没有 —— 那就挂在网格原点上（老行为）。
	 */
	FName ResolveAttachPoint(USkeletalMeshComponent* Mesh, FName Desired, const TCHAR* What) const;

	// 按给定挂点名把 actor 重挂到角色网格上：解析名字 + AttachToComponent + 补网格位移。
	// 三处附着（携带/掏出/下包）都走它，规则和补偿只此一处，别再各写一份。
	void AttachToCharacterAtPoint(ABlasterCharacter* Character, FName Desired, const TCHAR* What);

	// --- 网格显隐（"常态隐藏"）---
	/*
	 * 包的网格**平时是藏起来的**：背在携带者身上（ESS_Carried）那段时间不显示，
	 * 只有三种情况显形 ——
	 *   · 被携带者按 4 掏到手上（bCarrierDrawn，见 Draw()/Holster()）
	 *   · 掉在地上（ESS_Dropped，含物理掉落全程）
	 *   · 被安放 / 已拆除 / 已爆炸（ESS_Planted / Defused / Exploded）
	 *
	 * 服务器权威、复制给所有客户端：客户端上拿不到 CurrentCarrier（那个指针没复制），
	 * 也拿不到"掏出来了没有"以外的任何线索，所以这一位必须复制。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_CarrierDrawn)
	bool bCarrierDrawn = false;

	UFUNCTION()
	void OnRep_CarrierDrawn();

	// 按当前状态 + bCarrierDrawn 把网格显隐摆正（所有改这两个输入的地方都要调它）。
	// 顺带处理"携带者本人不该看到自己身上那个包"（本人手里那个是角色上的副本组件）。
	void ApplyMeshVisibility();

	// 现在背着/拿着这个包的角色。服务器上直接读 CurrentCarrier；客户端上那个指针不复制，
	// 退回"看它现在挂在哪"（附着本身是复制的，两边的答案一致）。
	const class ABlasterCharacter* GetCarryingCharacter() const;

	UPROPERTY(EditDefaultsOnly, Category = "Timing")
	float PlantDuration = 4.f;

	UPROPERTY(EditDefaultsOnly, Category = "Timing")
	float DefuseDuration = 7.f;

	// 两段式拆包：第一段占拆包总进度的比例（默认 50% = 3.5s）
	// 拆满第一段后中断会保留进度，下次从半程继续；第一段未满则重新开始
	UPROPERTY(EditDefaultsOnly, Category = "Timing")
	float FirstDefuseSegmentFraction = 0.5f;

	// 安包到爆炸的倒计时（Valorant 标准 45s）
	UPROPERTY(EditDefaultsOnly, Category = "Timing")
	float ExplodeCountdown = 45.f;

	// 安包/拆包时，水平速度超过该阈值（或跳跃）会打断进度（站定才能安/拆）
	UPROPERTY(EditDefaultsOnly, Category = "Timing")
	float MovementCancelSpeed = 50.f;

	// --- 安包后视觉：Core 转动加速 + 发光变红（阶段/配色逻辑全在代码）---
	// 基准转速：plant_intro 尾段实测 ~56-70°/s（收尾帧 55.7°，帧62-70 平均 ~66°/s），取 ~60°/s
	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	float CoreSpinSpeed = 60.f;

	// 剩余 ≤ AlertRemainingTime：加速到 CoreSpinAlertSpeed + 发光 AlertGlowColor
	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	float CoreSpinAlertSpeed = 180.f;

	// 剩余 ≤ CriticalRemainingTime：更快 CoreSpinCriticalSpeed + 更红 CriticalGlowColor
	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	float CoreSpinCriticalSpeed = 360.f;

	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	float AlertRemainingTime = 30.f;

	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	float CriticalRemainingTime = 10.f;

	// 各阶段自发光颜色（对应材质参数 GlowColor；槽 Bomb_S0_Temp_Core_Emissive / Temp_Emissive）
	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	FLinearColor NormalGlowColor = FLinearColor(0.75f, 9.0625f, 15.f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	FLinearColor AlertGlowColor = FLinearColor(6.f, 0.9f, 0.25f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	FLinearColor CriticalGlowColor = FLinearColor(11.f, 0.55f, 0.2f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "PlantedVisual")
	FName GlowColorParamName = TEXT("GlowColor");

	// 下包完成后播放的倒计时音频（时长需与 ExplodeCountdown 一致）
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	USoundBase* PlantedSound;

	// 倒计时 beep 素材（单发，安包后按剩余时间阶梯加速重复播放，机制见 UpdateCountdownBeep）。
	// 软引用默认路径指向导入的 CS C4 单发 beep；BP 可覆盖换素材。
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	TSoftObjectPtr<USoundBase> CountdownBeepSound;

	// 爆炸音效（单发，OnExplode 时各端播一次完整 boom）。
	// 软引用默认路径指向导入的爆炸音；BP 可覆盖换素材。
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	TSoftObjectPtr<USoundBase> ExplosionSound;

	// 安包音：角色开始安包（按下安包）瞬间播一次（单发、非循环）；
	// 中途松开/被打断（取消、移动打断、死亡掉包）会立刻掐断，安包完成也停。
	// 空间音：从 spike 位置（安包者手上）发声，能听出方位。BP Details 填素材，留空不响。
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	USoundBase* PlantArmSound;

	// 成功安包（spike 落地安放完成）的全局广播音：不分方位、无距离衰减，所有玩家同时听到。
	// 接口槽：BP Details 填素材，留空不响。
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	USoundBase* PlantSuccessSound;

	/*
	 * 拆包音：拆包一开始响**一次**，拆完/被打断/回合复位立刻停（不循环、不重播）。
	 *
	 * 素材（导入的瓦拆包音）本身 1.008s 的单发，播完就静了 —— 这是有意为之：
	 * 10 秒的拆包过程配一个 1 秒的音，重复播会变成"哒哒哒"的机关枪。
	 *
	 * 空间音：从 spike（包所在处）发声，能听出方位（同安包音）。
	 * 软引用默认路径指向导入的拆包音；BP 可覆盖换素材。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	TSoftObjectPtr<USoundBase> DefuseArmSound;

	// 空间音频衰减（作用于 spike 自带的 3D 音：安包音 / 倒计时 beep / 安包后持续音 / 爆炸）。
	// 留空用代码默认：球 400cm 内满音量、之后约 4000cm 渐弱到无声，并开启 3D 定位（能听出方位）。
	// 填一个 USoundAttenuation 资产可整体调距离/曲线。
	UPROPERTY(EditDefaultsOnly, Category = "Audio")
	class USoundAttenuation* SpikeSpatialAttenuation;

	// ==================== 爆炸球：视觉 + 范围伤害 ====================
	//
	// spike 爆炸后从爆点胀开的一个纯黑球：外面看是不透明的黑（看不到"里面"），
	// 半径 2 秒胀到 ExplosionSphereRadius，停 3 秒后消失。
	// 球面覆盖到的角色持续掉血 —— 靠近爆点的人基本当场必死，**包括进攻方自己**
	// （原作就是这样，也是这个效果存在的意义：安完包必须撤）。
	//
	// 网络分工（和倒计时视觉同一套路数）：
	//   · 球体**不复制**，各端按 MulticastExplode 到达的时刻自己推演大小；
	//   · 伤害**只在服务器**结算（ApplyExplosionSphereDamage），客户端一根血都不碰。

	// 视觉载体：引擎自带球体，纯装饰（无碰撞、不投影）。初始隐藏，爆炸时才显形。
	UPROPERTY(VisibleAnywhere, Category = "Spike|ExplosionSphere")
	class UStaticMeshComponent* ExplosionSphere;

	// 球的最大半径（cm）。2000 = 20m。
	UPROPERTY(EditAnywhere, Category = "Spike|ExplosionSphere")
	float ExplosionSphereRadius = 2000.f;

	// 从 0 胀到最大半径用多久（秒）
	UPROPERTY(EditAnywhere, Category = "Spike|ExplosionSphere")
	float ExplosionSphereGrowTime = 2.f;

	// 胀满后保持多久再消失（秒）
	UPROPERTY(EditAnywhere, Category = "Spike|ExplosionSphere")
	float ExplosionSphereHoldTime = 3.f;

	// 球里的角色**每秒**掉多少血。900 意味着满血(100)站进去一秒内必死。
	UPROPERTY(EditAnywhere, Category = "Spike|ExplosionSphere")
	float ExplosionDamagePerSecond = 900.f;

	// 伤害结算间隔（秒）。900/s 被拆成「每 0.25s 扣 225」四跳 —— 血条是滑下去的，
	// 而不是"进圈一年后突然一下空掉"。间隔越小越平滑，但服务器遍历角色的次数也越多。
	UPROPERTY(EditAnywhere, Category = "Spike|ExplosionSphere")
	float ExplosionDamageTickInterval = 0.25f;

	// 球的材质。留空 = 用工程里的 M_SpikeExplosionSphere（Unlit 纯黑，不参与光照，
	// 任何角度看都是同一个纯黑剪影）。想换成带边缘光/扭曲的效果就填自己的材质。
	UPROPERTY(EditDefaultsOnly, Category = "Spike|ExplosionSphere")
	class UMaterialInterface* ExplosionSphereMaterial;


	// 安包完成瞬间，Spike 自己在原地播放的 montage（展开动画）。
	// EditDefaultsOnly → 在 BP_Spike Details 里配置；留空则不播。
	// 注意：montage 的 Slot 必须在 ABP_Spike 动画图里有对应的 Slot 节点（一般叫 DefaultSlot）才会叠上去显示。
	UPROPERTY(EditDefaultsOnly, Category = "Spike|Animation")
	UAnimMontage* PlantUnfoldMontage;

	/*
	 * **开始**下包瞬间，Spike 自己播放的 montage（在安包者手上把包**展开/激活**的那条，一般 = PlantDuration 4s）。
	 * EditDefaultsOnly → 在 BP_Spike Details 里配置；留空则不播。
	 *
	 * 和上面 PlantUnfoldMontage 的分工（两条是**接力**，可以同时填，不会重复展开）：
	 *   · PlantStartMontage  —— 开始下包（MulticastStartPlant）时播：收拢 → 展开。
	 *     现填 EQ_Core_Bomb_S0_Activate_Montage（4.0s，Cap 22.2cm → 70.7cm）。
	 *   · PlantUnfoldMontage —— 安包完成、包已经贴地摆放好之后播：从**已展开**状态接着往下演
	 *     （现填 ..._Plant_Intro_Montage：全程 Cap 都是 66.6cm，它演的是展开之后的机构动作，
	 *      不是再展开一遍；收拢那条 `..._Plant_Outro` 是拆包完成槽在用的）。
	 * 取消下包 / 安包完成会主动 Montage_Stop（见 StopPlantStartMontageLocal），
	 * 免得半展开的包僵在手上不回 Idle。
	 *
	 * 注意：montage 的 Slot 必须和 ABP_Spike 动画图里的 Slot 节点同名（DefaultSlot）才会叠上去显示。
	 * 客户端能看见的前提是那条 montage 播在 SpikeMesh（3P 包）上 —— 本人第一人称手里那份
	 * （FPSpikeMesh）不跑动画机，靠 SetMasterPoseComponent 照搬真包的姿势，所以自动跟着一起动。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Spike|Animation")
	UAnimMontage* PlantStartMontage;

	// 拆包完成瞬间，Spike 自己播放的 montage（收拢/关闭动画）。
	// 留空：拆完立即隐藏进入 Defused；设置：先播 montage，播完（montage 长度）再隐藏。
	UPROPERTY(EditDefaultsOnly, Category = "Spike|Animation")
	UAnimMontage* DefuseCompleteMontage;

	// 安包贴地后的手工 Z 补偿（cm）：底部贴紧地面后可整体抬/降微调
	UPROPERTY(EditDefaultsOnly, Category = "Spike|PlacedVisual")
	float PlantedGroundZOffset = 0.f;

	// Spike 默认外观资产（软引用，EditDefaultsOnly → BP_Spike Details 的 Spike|Mesh 可覆盖）。
	// ⚠️ 故意不用硬引用/不在构造器里同步加载：构造器加载 SkeletalMesh 会让编辑器启动阶段的
	// 类加载触发异步 skinned 资产编译并死锁（卡 "Waiting for skinned assets to be ready ..."）。
	// 改为实例化后（PostInitializeComponents）按需加载赋给 SpikeMesh。
	UPROPERTY(EditDefaultsOnly, Category = "Spike|Mesh")
	TSoftObjectPtr<USkeletalMesh> SpikeMeshAsset;

	UPROPERTY(EditDefaultsOnly, Category = "Spike|Mesh")
	TSoftClassPtr<UAnimInstance> SpikeAnimClass;

	// --- 拆包器（defuser kit）：拆包时出现在拆包者手上，停止/完成拆包即消失 ---
	// 拆包器 skeletal mesh（软引用，懒加载，模式同 SpikeMeshAsset 避开启动期死锁）。
	// 默认指向已导入的 Valorant 拆包器 EQ_Bomb_Defuser_S0_Mesh；BP Details 可换。
	UPROPERTY(EditDefaultsOnly, Category = "Defuse|Kit")
	TSoftObjectPtr<USkeletalMesh> DefuserKitMesh;

	/*
	 * 拆包器挂在拆包者手上的挂点 —— 第三人称 / 第一人称各一个槽，默认值照武器（AK / BP_ProjectileWeapon）填。
	 *
	 * 为什么要两个：这两套网格是**两套不同的骨架**，挂点名字本来就不一样 ——
	 *   3P 身体（TP_Wushu_S0）→ WeaponSocket_Rifle   （AK 的 ThirdPersonAttachSocket）
	 *   1P 手模（FP_Wushu_S0）→ MasterWeaponSocket   （AK 的 FPWeaponSocket）
	 * 而且拆包的人**自己也必须看得见手里这个**：3P 那份挂在 3P 身体上，而本人那台机器会
	 * 专门跳过它（ShowDefuserKit 里判 IsLocallyControlled —— 不跳的话本人第一人称会同时
	 * 看到两个拆包器）。所以本人手里那份得单独挂在手模上，挂点在上面那套骨架里。
	 *
	 * socket 名和骨骼名都认；名字无效会退到 FallbackAttachPoint 并打一行警告（见 ResolveAttachPoint）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Defuse|Kit")
	FName DefuseKitSocketTP = FName("WeaponSocket_Rifle");

	UPROPERTY(EditDefaultsOnly, Category = "Defuse|Kit")
	FName DefuseKitSocketFP = FName("MasterWeaponSocket");

	// 第一人称那份拆包器在挂点上的微调（默认零 = 网格原点正落在挂点上，和 3P 那边同一个约定）。
	// 只管位移和旋转 —— 缩放由代码按世界缩放归一（手模那条链根骨带 100 倍），往这里填会打架。
	UPROPERTY(EditDefaultsOnly, Category = "Defuse|Kit")
	FTransform DefuserKitFPMeshOffset = FTransform::Identity;

	// --- 给动画机/蓝图读的运行值 ---
	// 安包后 Core 累计转角（度）。动画机 ABP_Spike 用 Transform(Modify) Bone 把 Core 绕自身 Y 旋转这个角度。
	UPROPERTY(BlueprintReadOnly, Category = "PlantedVisual")
	float CoreSpinDegrees = 0.f;

	// 阶段：0=正常(基准速/青色)；1=剩余≤30s(加速/红)；2=剩余≤10s(更快/更红)。各端独立推演（不复制）。
	UPROPERTY(BlueprintReadOnly, Category = "PlantedVisual")
	int32 UrgencyStage = 0;

	// 拆包进度 0-100（服务器权威、复制）。中断拆包时若 ≥50 回落到 50，否则清零。
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Defuse")
	float DefuseProgress = 0.f;

	// --- Interaction (called by character) ---
	void PickUp(class ABlasterCharacter* Character);
	void Drop();
	void ResetSpike();
	void Draw(class ABlasterCharacter* Character);
	void Holster(class ABlasterCharacter* Character);
	void StartPlant(class ABlasterCharacter* Character);
	void StartDefuse(class ABlasterCharacter* Character);
	void CancelAction();

	UFUNCTION(BlueprintPure, Category = "Spike|State")
	ESpikeState GetSpikeState() const { return CurrentState; }
	FORCEINLINE bool IsPlanting() const { return bIsPlanting; }
	FORCEINLINE bool IsDefusing() const { return bIsDefusing; }
	float GetPlantProgress() const { return PlantProgress; }

	UFUNCTION(BlueprintPure, Category = "Defuse")
	float GetDefuseProgress() const { return DefuseProgress; }

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;

#if WITH_EDITOR
	// 编辑器里改 CollisionBoxExtentOverride / 换网格时立刻重算方块尺寸，
	// 让视口线框（FSpikeVisualizer）实时跟手，不用重开关卡或进 PIE 才知道调没调对。
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION()
	void OnPickupBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnPickupEndOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
	// 当前状态（服务器权威、复制到各端）。5 态：Dropped/Carried(未安)/Planted/Defused/Exploded。
	// 蓝图可读走 public 的 BlueprintPure GetSpikeState()（UE 不允许 private 成员挂 BlueprintReadOnly）
	UPROPERTY(ReplicatedUsing = OnRep_SpikeState)
	ESpikeState CurrentState = ESpikeState::ESS_Dropped;

	UFUNCTION()
	void OnRep_SpikeState();

	// 安包落地后的最终 transform（服务器设置，复制到客户端在 OnRep 里应用），
	// 保证客户端也能看到 spike 竖直摆放在地面上
	UPROPERTY(ReplicatedUsing = OnRep_PlantedTransform)
	FTransform PlantedTransform;

	UFUNCTION()
	void OnRep_PlantedTransform();

	// 丢弃落地后的 transform：服务器在物理落稳那一刻记一次，客户端在 OnRep 里吸附到同一落点。
	// （掉包途中由根组件的物理运动复制逐帧同步；这里是落稳兜底 + 后进玩家的定位）
	UPROPERTY(ReplicatedUsing = OnRep_DroppedTransform)
	FTransform DroppedTransform;

	UFUNCTION()
	void OnRep_DroppedTransform();

	// --- Planting ---
	bool bIsPlanting = false;
	float PlantProgress = 0.f;

	UFUNCTION(Server, Reliable)
	void ServerStartPlant(class ABlasterCharacter* Character);

	UFUNCTION(Server, Reliable)
	void ServerCancelPlant();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastStartPlant();

	// 换挂点：下包开始换到 PlantAttachSocket（跟动画锚点下到地面），取消换回 DrawAttachSocket。
	// 单开一条 multicast 而不是只在服务器侧换 —— 附着是**各端本地**行为
	//（和 MulticastCarrierAttach 同理），服务器换了客户端不换就停在旧槽上。
	UFUNCTION(NetMulticast, Reliable)
	void MulticastSetAttachSlot(class ABlasterCharacter* Character, ESpikeAttachSlot Slot);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlantComplete();

	// 松开/取消下包：各端把"开始下包"那条 montage 掐断，包回到 Idle（收拢）姿势。
	// 单独一条 multicast 而不是塞进 MulticastStopPlantArmSound：那条是音效的，语义别混
	//（安包完成不走这条 —— 那里直接在 MulticastPlantComplete 里停，见其注释）。
	UFUNCTION(NetMulticast, Reliable)
	void MulticastStopPlantStartMontage();

	// --- Defusing (进度已用 0-100 的 DefuseProgress 表示并复制) ---
	bool bIsDefusing = false;

	UFUNCTION(Server, Reliable)
	void ServerStartDefuse(class ABlasterCharacter* Character);

	UFUNCTION(Server, Reliable)
	void ServerCancelDefuse();

	// 拆包开始：把拆包器挂到拆包者手上（各端都需要知道是谁在拆才能挂对骨骼）
	UFUNCTION(NetMulticast, Reliable)
	void MulticastStartDefuse(class ABlasterCharacter* Character);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastDefuseComplete();

	// 拆包被中断（松开/移动打断/死亡）：各端摘除并隐藏手上的拆包器
	UFUNCTION(NetMulticast, Reliable)
	void MulticastStopDefuseKit();

	// 拆包音起/停。起的那条从 MulticastStartDefuse 里发，停的两条分别挂在
	// MulticastDefuseComplete（拆完）和 MulticastStopDefuseKit（被打断/回合复位）上。
	// 起那条只在开始拆时响一次（素材单发），中途不再重播。
	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlayDefuseArmSound();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastStopDefuseArmSound();

	// 拆包器可见网格：平时隐藏跟在 spike 上，拆包时改挂到拆包者手部 socket（第三人称那份）
	UPROPERTY()
	class USkeletalMeshComponent* DefuserKitMeshComp;

	/*
	 * 第一人称那份拆包器：同一套网格资产，**另开一个组件**，拆包时挂到拆包者本人的手模
	 *（ABlasterCharacter::GetFPArmsMesh）上 —— 和 FPSpikeMesh / FPWeaponMesh 完全同一个套路。
	 *
	 * ⚠ 两份是**互斥**的：本人那台只显示这一份，别人那台只显示 3P 那份（见 ShowDefuserKit）。
	 *   别指望"本人看不见自己的身体，所以也看不见挂在身上的 3P 拆包器" —— 那个 OwnerNoSee
	 *   只管身体**那一个组件**，附着上来的组件照渲染；两边都显示就是"第一人称看见两个拆包器"。
	 *
	 * ⚠ 这里**不能**用 SetOnlyOwnerSee：它的判据是"组件的 Owner 是不是观察者"，而这个 actor
	 *   的 Owner 是关卡，对所有人都成立 —— 一设就成了"谁都看不见"
	 *（同 ApplyMeshVisibility 里那条注释踩的是同一个坑）。改用代码判 IsLocallyControlled()：
	 *   只在本人那台机器上挂上去并显示，别人的机器上整个跳过。
	 */
	UPROPERTY()
	class USkeletalMeshComponent* DefuserKitFPMeshComp;

	// 拆包时 kit 当前挂载的角色（各端本地记录，用于停止拆包时摘除；不复制）
	UPROPERTY(Transient)
	class ABlasterCharacter* LocalKitAttachedCharacter;

	// 各端本地：附加并显示拆包器 / 摘除并隐藏
	void ShowDefuserKit(class ABlasterCharacter* Character);
	void HideDefuserKit();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlayDefuseCompleteMontage();

	// 各端强制把 spike 重新附着到携带者背上（兜底：新回合先在出生点复位、立刻交给随机
	// 攻击者，远端可能丢一次自动 attachment 复制，导致防守端看不到携带的 spike，直到掏出）
	UFUNCTION(NetMulticast, Reliable)
	void MulticastCarrierAttach(class ABlasterCharacter* Character);

	// --- Explosion ---
	UPROPERTY(Replicated)
	float PlantStartTime = 0.f;

	FTimerHandle ExplodeTimerHandle;
	void StartExplodeTimer();
	void OnExplode();

	// 安包后每帧向所有玩家广播爆炸倒计时（"00:45" + 剩余比例）
	void BroadcastExplodeCountdown();

	// 清空所有玩家的 spike HUD（倒计时文字 + 进度条）
	void ClearSpikeUIForAll();

	// --- Planted audio ---
	UPROPERTY()
	UAudioComponent* PlantedAudio;

	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlayPlantedSound();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastStopPlantedSound();

	// --- 倒计时 beep：独立音频组件，不干扰 PlantedAudio 的持续音 ---
	UPROPERTY()
	class UAudioComponent* CountdownAudio;

	// 按 Valorant 阶梯节奏播 beep：剩余 >35s 每秒1响 → >25s 每秒2响 → >10s 每秒4响 → ≤10s 每秒8响。
	// 各端本地用剩余时间驱动（同 UpdatePlantedVisual），无需网络同步。
	void UpdateCountdownBeep(float DeltaTime);
	bool bCountdownSoundLoaded = false;
	float BeepAccumulator = 0.f;

	// --- 爆炸音效：独立组件，让 boom 完整播完（拆包/重置不会截断它）---
	UPROPERTY()
	class UAudioComponent* ExplosionAudio;
	bool bExplosionSoundLoaded = false;

	/*
	 * --- 拆包音：独立组件，不干扰安包音（两条虽然互斥，但组件分开语义不混）---
	 *
	 * 开始拆那一刻响**一次**就完事，不循环不重播（早期版本在 Tick 里"播完再补一声"，
	 * 素材 1.008s 对着 10s 的拆包时长会响十来下，已去掉）。
	 * 停由 MulticastDefuseComplete / MulticastStopDefuseKit 各端自己掐 —— 不能拿 bIsDefusing
	 * 当"该不该响"的判据：那个只在服务器上被 ServerCancelDefuse 清掉，
	 * 客户端收到 MulticastStopDefuseKit 时它还是 true（历史上就这样），音会停不下来。
	 */
	UPROPERTY()
	class UAudioComponent* DefuseArmAudio;
	bool bDefuseArmSoundLoaded = false;

	// 就地掐断（Stop 组件）。**不走 Multicast**：拆完/被打断分别发生在
	// MulticastDefuseComplete / MulticastStopDefuseKit 里，那两条本身就是 multicast，
	// 在里面再发一条 multicast 是多余的（同 MulticastPlantComplete 直接停 PlantArmAudio 的做法）。
	void StopDefuseArmSoundLocal();

	// --- 爆炸球：状态与结算 ---
	// 球正在胀/正在停。到 Grow+Hold 之后收掉（各端按本端时钟各自推演，不复制）
	bool bExplosionSphereActive = false;
	// 从开始胀起过了多少秒
	float ExplosionSphereElapsed = 0.f;
	// 服务器每 ExplosionDamageTickInterval 一次的伤害结算（客户端上永远不启动）
	FTimerHandle ExplosionSphereDamageTimer;
	// 半径 → 缩放 的换算基准：球网格资产自己的包围球半径（同 CloveSmoke 的做法，
	// 这样 BP 里换网格后 ExplosionSphereRadius 仍然是真实世界半径）
	float ExplosionSphereAssetRadius = 50.f;

	// 默认材质的软引用（**不加载**，同 ExplosionSound 的理由：构造期同步加载 /Game 资产
	// 会拖慢甚至卡住编辑器启动）。首次爆炸、真的要显示时才 LoadSynchronous，
	// 之后 ExplosionSphereMaterial 被填上就一直是它。
	TSoftObjectPtr<UMaterialInterface> ExplosionSphereMaterialAsset;
	bool bExplosionSphereMatLoaded = false;

	// 开始胀（各端都调；只有服务器会额外起伤害定时器）
	void StartExplosionSphere();
	// Tick 里推进大小/寿命（各端本地推演，不复制）
	void UpdateExplosionSphere(float DeltaTime);
	// 服务器：按**当前**半径对球内的角色结算一跳伤害
	void ApplyExplosionSphereDamage();
	// 收球：隐藏 + 停伤害定时器（幂等）
	void StopExplosionSphere();

	// --- 安包音：独立组件，从 spike（安包者手上）发单发音；
	//     开始安包(MulticastStartPlant)播一次，松开/取消/完成(MulticastStopPlantArmSound / MulticastPlantComplete)掐断 ---
	UPROPERTY()
	class UAudioComponent* PlantArmAudio;

	UFUNCTION(NetMulticast, Reliable)
	void MulticastStopPlantArmSound();

	// --- 成功安包全局广播（2D、不分方位），安包完成 MulticastPlantComplete 时各端播 ---
	UFUNCTION(NetMulticast, Reliable)
	void MulticastPlayPlantSuccessSound();

	// 统一给 spike 的 3D 音频组件套空间衰减（保证能听出方位）；Success 音不走这里（2D 广播）
	void ApplySpatialAttenuation(class UAudioComponent* Comp) const;

	// --- 安包后每帧推演 Core 转角与发光阶段（服务器/客户端各自按剩余时间独立算）---
	void UpdatePlantedVisual(float DeltaTime);
	void SetupGlowMaterials();

	// 直接给自发光槽设色（黑 = 关灯）
	void SetGlowColor(const FLinearColor& Color);
	// 安包后按阶段（0 正常/1 预警/2 危急）变色
	void ApplyGlowColor(int32 Stage);
	// 关灯：自发光变黑（未安/拆包后/爆炸后都走这里，光条只在 Planted 亮）
	void TurnOffGlow();

	int32 LastAppliedGlowStage = -1;

	// 回到“未安/常态”的安包后视觉：Core 转角归零、光条关掉（回合切换/掉落复位用）
	void ResetPlantedVisualState();

	// --- 方块碰撞体 ↔ 视觉网格的对位 ---
	// 骨骼网格导入包围盒的原点不在模型中心（SpikeMesh origin.Z≈31.5，底面 Z≈-8.95），
	// 而方块碰撞体永远以自己的 transform 居中 → 直接把网格挂到盒子下面会让模型整体下沉 31.5cm
	// （掉下去会陷进地面 / 拿在手上位置也不对）。
	// 做法：把网格按 -Bounds.Origin 平移，让「包围盒中心」和「actor 原点 = 方块中心」重合；
	// 这样方块底 = 网格底，落地时模型正好站在地面上。
	// 代价是 actor 原点的语义从「网格原点」变成「网格几何中心」，所以凡是把 actor 摆到
	// 「网格原点」位置的地方（挂到角色 socket、关卡初始摆放）都要用 MeshAttachPivotOffset 反向补回来。
	FVector MeshRelativeOffset = FVector::ZeroVector;    // 网格相对 actor 原点的位移（= -Bounds.Origin，含网格缩放）
	FVector MeshAttachPivotOffset = FVector::ZeroVector; // = -MeshRelativeOffset：附着/摆放时补回「网格原点」位置

	// 附着到角色 socket（或关卡初始摆放）后调用：沿 actor 局部轴偏移，抵消网格子件的位移，
	// 让「网格原点贴在 socket 上」的旧表现完全不变
	void ApplyMeshPivotCompensation();

	// 按网格包围盒定方块尺寸 + 摆好网格子件的位置（PostInitializeComponents 里调一次）
	void SetupCollisionBoxFromMesh();

	// --- 丢弃后的物理落地 ---
	// 用方块碰撞体真跑物理（自由落体 + 翻滚 + 落稳），服务器权威，落包期间把物理运动复制给客户端。
	// 旧实现是「骨骼网格没物理资产 → 服务器端运动学自由落体」，现在不需要了。
	void EnablePhysicsDrop();
	void StopPhysicsDrop();
	bool bPhysicsDropping = false;
	float PhysicsDropElapsed = 0.f;

	// 落稳判定阈值：线速度/角速度都小于此值（或刚体已休眠）即视为停稳，记一次落点
	UPROPERTY(EditDefaultsOnly, Category = "Physics")
	float DropSettleLinearSpeed = 2.f;

	UPROPERTY(EditDefaultsOnly, Category = "Physics")
	float DropSettleAngularSpeed = 5.f;

	// 落稳判定兜底：超过这个时长还没停稳（掉进地图外/无地面）就不再等，直接收掉标记
	UPROPERTY(EditDefaultsOnly, Category = "Physics")
	float DropMaxDuration = 10.f;

	// 按网格包围盒定尺寸后，网格本地最底点在 actor 坐标系下的 Z（负值），供贴地摆放
	float MeshBottomInActorZ = 0.f;

	UPROPERTY(Transient)
	TArray<UMaterialInstanceDynamic*> GlowMIDs;

	// --- Spike 自身 montage（安包完成展开 / 拆包完成收拢）---
	// 网格最底点在 actor 坐标系下的 Z（负值）：网格相对位移 + 包围盒底点。贴地摆放用
	// （actor 原点抬到「地面Z - 这个值」，网格底面正好贴地）
	float GetMeshBottomLocalZ() const;

	// 安包完成：服务器在 FreezeAtLocation 后直接播，客户端在 OnRep_PlantedTransform 里播
	void PlayPlantUnfoldMontageLocal();

	// 开始下包 montage：MulticastStartPlant 里各端播；取消/完成时各端掐断回 Idle
	void PlayPlantStartMontageLocal();
	void StopPlantStartMontageLocal();

	// 拆包完成 montage 的实际播放（Multicast 各端调这里）
	void PlayDefuseCompleteMontageLocal();
	void HideAfterDefuseMontage();

	FTimerHandle DefuseMontageHideTimer;

public:
	float GetRemainingTimer() const;

	UFUNCTION(NetMulticast, Reliable)
	void MulticastExplode();

	// --- Helpers ---
	void AttachToCharacter(class ABlasterCharacter* Character);
	void DetachFromCharacter();
	void FreezeAtLocation();
	void SetSpikeState(ESpikeState NewState);

	/*
	 * 下包完成前抓一帧「包此刻的世界变换」（它挂在 PlantAttachSocket 上，姿态来自下包动画），
	 * 安放位置/朝向就用它。
	 *
	 * 为什么要抓：下包动画本身就是"把包放到地上"的动画 —— 实测 TP_Core_Bomb_S0_Activate_UB
	 * 第 120 帧（= PlantDuration 4s 那一下）挂点停在**身前 83.7cm、左右居中、离脚面 5.9cm**，
	 * 之后到末帧（138）一动不动；那一帧挂点自身还是**直立**的（up = (0,0,1)）。
	 * 所以"包最后落在哪"根本不用另配参数，安放前读一次挂点就行。
	 *
	 * 时机：必须在 MulticastPlantComplete()（它会停掉下包 montage）**之前**读，
	 * 不然拿到的是停完动画之后回落的姿态。
	 * 快照在服务器抓、经 PlantedTransform 复制给各端，客户端不自己算。
	 */
	void CapturePlantPlacement();
	FTransform PlantPlacementTransform = FTransform::Identity;
	bool bHasPlantPlacement = false;

	UPROPERTY()
	class ABlasterCharacter* CurrentCarrier;

	// 刚丢弃 spike 的角色，防止 Drop 后立即被同一角色重新拾取
	UPROPERTY()
	class ABlasterCharacter* JustDroppedCharacter;

	UPROPERTY()
	class ABlasterCharacter* CurrentPlanter;

	UPROPERTY()
	class ABlasterCharacter* CurrentDefuser;

	UPROPERTY()
	class ABlasterPlayerController* PlanterController;

	UPROPERTY()
	class ABlasterPlayerController* DefuserController;

	// 关卡里初始出生点，新回合用 ResetSpike 复位到这里
	FVector InitialSpawnLocation;

	// 初始朝向，安包时恢复竖直摆放
	FRotator InitialSpawnRotation;

	class ABlasterGameState* GetBlasterGameState() const;
};
