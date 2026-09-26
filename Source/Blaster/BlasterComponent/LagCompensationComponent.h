#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LagCompensationComponent.generated.h"

class ABlasterCharacter;

/**
 * 一帧历史：某个角色在某一时刻的关键骨骼世界位置。
 * 只活在服务器内存里，不参与复制（UPROPERTY 只是为了让 USTRUCT 能存进数组）。
 */
USTRUCT()
struct FBlasterRewindFrame
{
	GENERATED_BODY()

	float Time = 0.f;

	// 角色位置 + 朝向（只用 Yaw）：命中盒绕它旋转
	FVector RootLocation = FVector::ZeroVector;
	FRotator RootRotation = FRotator::ZeroRotator;

	// 近似命中盒的骨骼位置
	FVector HeadLocation = FVector::ZeroVector;
	FVector ChestLocation = FVector::ZeroVector;
	FVector PelvisLocation = FVector::ZeroVector;

	// 射手侧：第一人称眼位（= 那一时刻这一枪的射线起点）。
	// 见 ABlasterCharacter::GetFPEyeWorldLocation —— 起点是眼睛不是枪口，理由在那里。
	FVector EyeLocation = FVector::ZeroVector;
};

/**
 * 服务器回溯（lag compensation / server-side rewind）。
 *
 * 为什么需要：客户端看到的世界比服务器「新」了半个 RTT。射手对着屏幕上的敌人开枪，
 * 服务器收到时敌人已经走开了 → 服务器自己重算这条射线就会打空（"我明明打中了"）。
 * 做法：客户端开枪时带上「开火那一刻的服务器时间」（PC->GetServerTime()），
 * 服务器把每个角色还原到那个时刻的位置，用还原后的命中盒重算这一枪，命中才结算伤害。
 *
 * 记录：只有权威机记录（每个角色记自己的一份，30Hz 环形缓冲，默认 0.5s 窗口）。
 * 判定：由射手的组件查询所有候选目标的历史，射线取最近命中；墙/箱子挡在更前面时判为没打中
 *      （顺带补上了原来「服务器完全不校验视线」的洞）。
 * 命中盒：头/胸/胯/腿四个「带朝向的盒子」近似 —— 不做逐骨骼物理命中盒，够用且便宜。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BLASTER_API ULagCompensationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULagCompensationComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/*
	 *服务器：把各角色还原到 HitTime（服务器时间轴上的时刻）后重算这一枪。
	 *
	 *起点 = **服务器自己记下的、射手那一刻的眼位**（回溯帧里的 EyeLocation）。
	 *客户端在起点上没有任何发言权 —— 它只提供方向（HitTarget 是准星那条射线的落点，射线本体由
	 *客户端的目标点拉出来）。以前起点还要靠客户端上报的「手 → 枪口」偏移来复原，现在不需要了：
	 *眼位是服务器每帧自己算的（ABlasterCharacter::GetFPEyeWorldLocation），作弊者动不了它。
	 *
	 *命中才填 OutHit 并返回 true。Shooter 必须是本组件的拥有者。
	 */
	bool ServerSideRewind(ABlasterCharacter* Shooter, const FVector& HitTarget,
		float HitTime, FHitResult& OutHit);

	// 本角色在 Time 时刻的插值位置（一点历史都没有时会先就地记一帧兜底）
	bool GetRewoundFrame(float Time, FBlasterRewindFrame& OutFrame);

protected:
	// —— 记录参数 ——
	// 历史窗口：最多能往回拉多久。越大越能补偿高延迟，也给高延迟作弊留更大空间。
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	float MaxRecordTime = 0.5f;

	// 记录频率：30Hz 足够插值
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	float RecordInterval = 1.f / 30.f;

	// —— 判定参数 ——
	// 超过这个距离的目标不参与回溯（打不到，也白算）
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	float MaxRewindDistance = 8000.f;

	/*
	 *近似命中盒：半尺寸（cm）+ 相对骨骼的偏移（cm，角色局部空间，随朝向一起转）。
	 *
	 *四个盒子（头/胸/胯/腿）并起来盖住整个身体。盒子以骨骼**原点**为中心，而骨骼原点不等于
	 *那一块的几何中心 —— 最典型的是 `head` 骨骼，它的原点是**脖子根**（颅底），真正的头在它上面
	 *一截。所以头盒的 Offset.Z 就是「骨骼原点到那一块几何中心」的距离。
	 *
	 ***下面这组值 2026-09-24 按当前角色（TP_Wushu_S0_Mesh / TP_Wushu_S0_Skeleton，5 个角色 BP 共用）**
	 *重新量过。量法（可复现，脚本在 E:/Notion/claude-temp/probe_lagcomp_char*.py）：
	 *在 headless 编辑器里 spawn 一个 BP_BlasterCharacter，用
	 *`SkeletalMeshComponent.get_bone_transform(name, RTS_WORLD)` 读骨骼世界坐标，
	 *减去角色位置得到「角色局部坐标」；骨骼的 ref pose 数值单位是**米**，运行时 ×100 变厘米
	 *（对照：脚底骨骼算出来离胶囊底 12.27cm，与 ref pose 的 0.12m 吻合，网格组件世界缩放 = 1.00）。
	 *
	 *当前角色的实测骨架（ref pose，单位 cm；H = 离地高，角色局部 Z = H − 88）：
	 *  · Head   H=192（角色局部 104）／Spine2 H=136（48）／Pelvis H=123（35）／胶囊中心 H=88（0）
	 *  · 下巴 H≈187 颅顶 H≈202 发梢 H≈211.5；脸最前 X≈+18 后脑 X≈-11；头宽 ≈16
	 *  · 肩关节 H=170 Y=±17；髋 H=118 Y=±12；膝 H=66；脚踝 H=12 脚趾 H=4
	 *
	 *所以四个盒子落成（H 区间是实际覆盖范围）：
	 *  头盒 H174~210 X-16~+18 Y±12 ／ 胸盒 H136~176 X-10~+22 Y±20
	 *  胯盒 H100~138 X -7~+19 Y±20 ／ 腿盒 H  4~118 X-16~+16 Y±26
	 *相邻两段都咬在一起（腿↔胯 100~118、胯↔胸 136~138、胸↔头 174~176），**全身没有漏区**。
	 *这一套是「贴合实测」版：爆头必须真打到头。
	 *
	 *历史（2026-09-11 那套 (43,30,30)/(‑14,0,12) 为什么被换掉）：
	 *  那套尺寸比现在这个角色大 2.5~3 倍，头盒中心还落在头骨上方 12cm、后方 14cm ——
	 *  实际罩住 X -55~+31、Y±35、H165~225，等于「站在敌人侧面 35cm 或头后 55cm 开枪也算爆头」。
	 *  同时胸盒因为 ChestBoneName 写的是 `spine_02`（这套骨架里根本没有，实际叫 `Spine2`）
	 *  一直在走兜底、钉死在「角色位置 + (0,0,42)」不跟身体走，腿盒和胯盒之间还漏了大腿根 17cm。
	 *
	 *要再调就在角色 BP 的组件面板改（`EditAnywhere`），配合 `bDrawDebugRewind` 彩框对着角色拉：
	 *  · 框整体偏了 → 调 Offset（Z 正数往上）
	 *  · 框够不到边缘 → 调 Extent（半尺寸，是「从中心到面」的距离，不是全长）
	 */
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector HeadExtent = FVector(17.f, 12.f, 18.f);

	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector HeadBoxOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector ChestExtent = FVector(16.f, 20.f, 20.f);

	// Spine2 在 H136，胸廓中心在 H156 → 往上抬 20
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector ChestBoxOffset = FVector(0.f, 0.f, 20.f);

	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector PelvisExtent = FVector(13.f, 20.f, 19.f);

	// Pelvis 在 H123，胯部几何中心在 H119 → 往下压 4
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector PelvisBoxOffset = FVector(0.f, 0.f, -4.f);

	// 腿盒挂在角色根（胶囊中心 H88）上：没有单独的腿骨骼也能覆盖脚踝到大腿根
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector LegsExtent = FVector(16.f, 26.f, 57.f);

	// H88 - 27 = H61 为中心，±57 → 覆盖 H4~118
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Hitboxes")
	FVector LegsBoxOffset = FVector(0.f, 0.f, -27.f);

	// 骨骼名：不同骨架叫法不同，BeginPlay 时解析；解析不到就退回「胶囊根 + 固定偏移」
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FName HeadBoneName = TEXT("head");

	// 当前骨架叫 Spine2（Valorant 原命名 spine_02 在导入时被改成 Spine1~4），别名链见 BeginPlay
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FName ChestBoneName = TEXT("Spine2");

	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FName PelvisBoneName = TEXT("pelvis");

	/*
	 *目标回溯要不要额外减掉「射手看世界的那份延迟」。
	 *
	 *客户端上报的 GetServerTime() 是「服务器此刻的时间」，服务器再一夹就正好是 Now ——
	 *也就是说当前这条链路的回溯窗口实际是 0：目标停在原地不往回拉。
	 *但射手屏幕上看到的目标是他「单向延迟 + 客户端插值缓冲」之前的状态，所以要按那个量往回拉。
	 *本地玩家（host）单向延迟 = 0 → 这条是恒等变换。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	bool bCompensateShooterLatency = true;

	// 客户端插值缓冲带来的额外延迟（秒）：远端角色在客户端是插值显示的，比真实时间还要旧一点。
	// 默认 0（没实测过 Blaster 这套的具体值），需要时再往上加。
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	float TargetInterpDelay = 0.f;

	/*
	 *兜底偏移（相对角色根位置 = 胶囊中心，离地 88cm）。
	 *只有骨骼名一根都没解析到才会用到 —— 正常情况走的是上面四个盒子的锚点骨骼。
	 *数值 = 对应几何中心离地高 − 88（头 H192→104、胸 H156→68、胯 H119→31）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FVector HeadFallbackOffset = FVector(0.f, 0.f, 104.f);

	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FVector ChestFallbackOffset = FVector(0.f, 0.f, 68.f);

	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	FVector PelvisFallbackOffset = FVector(0.f, 0.f, 31.f);

	/*
	 *调试：画出这一枪的回溯判定（诊断「打中不掉血」用）。默认关，调命中盒的时候打开。
	 *
	 *画三样东西，配合 [LagComp|未命中] 日志一起看：
	 *  射线本体：命中=红，没命中=蓝
	 *  命中盒  ：赢的那个用部位颜色（头红/胸黄/胯绿/腿蓝），被墙或更近命中挡住的=橙，射线几何上没碰到的=灰
	 *  遮挡点  ：被墙截断的位置画个黄球
	 *
	 *怎么读：
	 *  · 一个盒子都没有（只有线）      → 候选目标=0（队友/已阵亡/太远），看日志那行
	 *  · 盒子全灰                     → 盒子生成了但射线偏了，比对起点和日志里的坐标
	 *  · 有橙盒 + 黄球                → 被墙挡了，射线或起点位置有问题
	 *  · 有彩盒但日志说未命中         → 不可能，除非盒子在别的候选身上
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation")
	bool bDrawDebugRewind = false;

	/*
	 *常驻命中盒可视化：**每帧**把自己身上那四个盒子画出来，跟开不开火无关。
	 *
	 *和上面的 `bDrawDebugRewind` 分工不同：
	 *  · bDrawDebugRewind   → 只在服务器判定某一枪的瞬间画一次，画的是**回溯到过去**的盒子，2 秒后消失
	 *  · bDrawDebugHitboxes → 每个角色每帧都画，画的是**此刻**的盒子，靠每帧重画实现「一直可见」
	 *
	 *所以调盒子尺寸/位置用这个（拉一下就能看见框跟着动），查「这一枪为什么没打中」还是用上面那个。
	 *颜色就是四个部位色（头红/胸黄/胯绿/腿蓝），没有「灰=几何没碰到 / 橙=被墙挡住」那两种状态色 ——
	 *这条路径不跑射线判定，无从知道打没打中。
	 *
	 *客户端也画（组件在客户端上一样存在、骨骼位置也是现成的），不限于服务器窗口。
	 *只在 `ENABLE_DRAW_DEBUG` 为真的构建里有画出（编辑器 / Development）；Shipping 里是空函数。
	 */
	UPROPERTY(EditAnywhere, Category = "Lag Compensation|Debug")
	bool bDrawDebugHitboxes = true;

	/*
	 *诊断日志总开关。默认**关**（功能已经调通，正常玩的时候不该刷屏）；出问题时在
	 *BP_BlasterCharacter 的组件面板勾上即可，不用重编。
	 *
	 *打开后会有这三类（都是**每次开火**触发，所以关着才是常态）：
	 *
	 *[LagComp|输入年龄]（按 `LagLogInterval` 节流）：距上次收到该角色的 Move 多久 —— 注意这是
	 *  **客户端的 Move 发送周期**（实测 24~36ms），不是延迟。对比用的单向延迟另有其数
	 *  （PS->GetPingInMilliseconds()*0.5）。行尾 `if (InputAge > MaxRecordTime) return;` 挡掉静止角色的刷屏。
	 *[LagComp|命中] / [LagComp|未命中]：命中判定的结果。未命中时把四条例外路径各自的数值都打出来
	 *  （退化射线 / 候选目标=0 / 起点被几何体包住 / 打不进任何盒子），并附候选数、测试盒数、遮挡比例。
	 *  「打人不掉血」就是靠这几行定位的 —— 原来是静默 return，什么都看不到。
	 *
	 ***只有 `[LagComp|开火被拒]`（在 UCombatComponent 里）不受这个开关管**，它是无条件的：
	 *那一条只在服务器判定这发非法时触发（射速/弹药/状态/没开回溯），正常玩不该出现 ——
	 *一旦频繁出现，本身就是信号，不该被一个默认关掉的开关挡住。
	 *
	 *历史：2026-09-11 删掉了「起点校验 / 起点明细 / 根位置差」几行偏移诊断。结论已经拿到
	 *（两端偏移长度一致到 1%、差的是 23° 的动画相位；手骨偏差是摆臂不是滞后；服务器姿势不滞后，模型 B），
	 *留着只是每开一枪刷两行。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation|Debug")
	bool bLogLagDiagnostics = false;

	// 诊断输出间隔（秒）：别每帧刷屏
	UPROPERTY(EditDefaultsOnly, Category = "Lag Compensation|Debug")
	float LagLogInterval = 0.25f;

private:
	void SaveFramePackage();
	void FillFrame(FBlasterRewindFrame& OutFrame);

	// 常驻画出此刻的四个命中盒（每帧调用，不经过射线判定）
	void DrawDebugHitboxes();

	// 诊断：把「服务器手里的输入有多旧」打出来（最近一次收到 Move 到现在的时间 = 理论上最多落后多少）
	void LogInputLagDiagnostics();
	float LagLogAccumulator = 0.f;

	// 解析骨架里实际存在的骨骼名；都没有就返回 NAME_None（取值退回偏移）
	FName ResolveFirstValidBone(const TArray<FName>& Candidates) const;
	FVector BoneWorldLocation(FName Bone, const FVector& FallbackOffset) const;

	void CollectCandidates(ABlasterCharacter* Shooter, TArray<ABlasterCharacter*>& OutCandidates) const;

	// 射线 vs 带朝向的盒子：把射线变换到盒子局部空间做 slab 测试。
	// OutTime ∈ [0,1] 是命中点在 Start→End 上的比例。
	static bool RayIntersectsOrientedBox(const FVector& Start, const FVector& End, const FVector& Center,
		const FQuat& Rotation, const FVector& Extent, float& OutTime);

	UPROPERTY()
	ABlasterCharacter* Character = nullptr;

	// 环形缓冲：写满后从头覆盖
	TArray<FBlasterRewindFrame> FrameHistory;
	int32 NextFrameIndex = 0;
	int32 NumFramesRecorded = 0;
	float RecordAccumulator = 0.f;

	FName ResolvedHeadBone = NAME_None;
	FName ResolvedChestBone = NAME_None;
	FName ResolvedPelvisBone = NAME_None;
};
