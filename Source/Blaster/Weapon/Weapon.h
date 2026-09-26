// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponTypes.h"
#include "Sound/SoundCue.h"
#include "Weapon.generated.h"

struct FHitResult;

UENUM(BlueprintType)
enum class EWeaponState : uint8
{
	EWS_Initial UMETA(DisplayName = "Initial State"),
	EWS_Equipped UMETA(DisplayName = "Equipped"),
	EWS_Dropped UMETA(DisplayName = "Dropped"),

	// 收在背/腰挂点上的**没拿在手上**的那几把。以前这种情况也用 EWS_Equipped，
	// 于是"这把枪是不是在手上"从武器状态里根本读不出来（角色那边被迫自己记一份，
	// 见 ABlasterCharacter::FPViewmodelHiddenWeapon 的注释）。
	// 现在分出来，顺带承担一件事：**收起来的枪不渲染**（瓦里背后看不到枪，
	// 而且原来的 PrimaryHolsterSocket / SecondaryHolsterSocket / MeleeHolsterSocket
	// 在瓦的骨架上本来也不全，挂上去只会出现飘在身上的枪）。
	// 物理/碰撞/拾取提示这些和 EWS_Equipped 完全一样，只有显隐不一样。
	//
	// ★ 新加枚举值一律**加在 EWS_MAX 前面**（放中间会给已序列化的老值换含义）。
	EWS_Holstered UMETA(DisplayName = "Holstered (Not In Hand)"),

	EWS_MAX UMETA(DisplayName = "DefaultMAX")
};

UCLASS()
class BLASTER_API AWeapon : public AActor
{
	GENERATED_BODY()

public:
	AWeapon();
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// virtual：近战没有弹药，HUD 上那个数字要清空（见 AMeleeWeapon::SetHUDAmmo）
	virtual void SetHUDAmmo();
	virtual void OnRep_Owner() override;
	void ShowPickupWidget(bool bShowWidget);
	virtual void Fire(const FVector& HitTarget);

	/*
	 *开火时要不要让角色**身体**（第三人称）播那条通用的 FireWeaponMontage
	 *（ABlasterCharacter::PlayFireMontage，里面跳 RifleAim/RifleHip）。
	 *
	 *默认 true —— 枪械的身体后坐就靠它。近战覆盖成 false：它自己按段播挥刀动画
	 *（见 AMeleeWeapon::PerformAttack 里的 ThirdPersonAnimation），两条蒙太奇
	 *会抢同一个 Slot，谁后调谁赢，表现是挥刀动作被步枪后坐顶掉。
	 */
	virtual bool ShouldPlayBodyFireMontage() const { return true; }

	// 服务器回溯判定出来的命中，交回武器结算（伤害/爆头/击杀确认音/命中反馈）。
	// 只有开了 bUseServerSideRewind 的武器会走这里；默认什么都不做。
	virtual void ProcessServerRewindHit(const FHitResult& Hit) {}

	/*
	 *这把武器的伤害是不是**自己**结算的 —— 也就是"服务器不需要为它跑回溯"。
	 *
	 *三种武器三条路：
	 *  · 投射物武器（AProjectileWeapon）—— 伤害由飞出去的那个投射物命中时自己结算
	 *  · 近战 / Jett 飞刀        —— 伤害由 MulticastFire 那一遍 Fire() 在服务器上结算
	 *  · 普通枪械                —— 伤害由服务器回溯（ULagCompensationComponent）结算
	 *
	 *UCombatComponent::ServerFire_Implementation 用它来分派：前三者在服务器这边什么都不用做，
	 *只有最后一种要发起回溯。**别用 "bUseServerSideRewind == false" 来代替它** ——
	 *那个条件是把"没人结算伤害"这个危险情况检出来报警的，而上面三种武器恰好也都是 false，
	 *拿它当分派条件会让完全正常的开火每枪刷一条假警告（这段历史见 ServerFire 里的注释）。
	 */
	virtual bool AppliesOwnDamage() const { return false; }
	void Dropped();
	void AddAmmo(int32 AmmoToAdd);
	/**
 *Textures for the weapon crosshairs
 */
	
	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairCenter;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairRight;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairLeft;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairTop;

	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* CrosshairBottom;

	/*
	 *瞄准（ADS）时叠在准星底下的贴图 —— 就是"瞄准镜边框"那张图，比如步枪那个白色六边形。
	 *留空 = 瞄准时不额外画东西，屏幕上还是只有一个普通准星。
	 *画法：HUD 按贴图的**原始尺寸**居中画（和准星共用 DrawCrosshair，先画框再画准星），
	 *所以贴图导出多大它就显示多大 —— 想要多大就做多大的图。
	 *注意它是**叠加**不是替换：瞄准时普通准星照常显示，准星该变红还变红，框自己不跟着变色。
	 */
	UPROPERTY(EditAnywhere,Category=Crosshairs)
	UTexture2D* AimTexture;

	/*
	 *瞄准镜框的颜色。默认白色。
	 *单独给一个颜色、而不是跟着准星的 CrosshairsColor：后者在准星瞄到敌人时会变红，
	 *框跟着一起闪红很怪（Valorant 里那个框自始至终是同一个颜色）。
	 */
	UPROPERTY(EditAnywhere,Category=Crosshairs)
	FLinearColor AimTextureColor=FLinearColor::White;

	/*
	 *是否可以右键瞄准（ADS）
	 *
	 *false：右键完全不进瞄准 —— 不聚焦（FOV 不动）、不显示 AimTexture、移速也不降到瞄准速度。
	 *默认 true 是**故意**的：加这个开关之前所有武器都能右键瞄准，默认值必须保持原行为，
	 *不然每把枪的手感会在没人注意的情况下集体变一次。不想让某把枪能瞄（霰弹枪/刀之类）
	 *在它的武器蓝图里取消勾选就行。
	 */
	UPROPERTY(EditAnywhere,Category="Combat")
	bool bCanAim = true;

	/*
	 *Zoomed FOV while aiming
	 */
	UPROPERTY(EditAnywhere)
	float ZoomedFOV=30.f;

	UPROPERTY(EditAnywhere)
	float ZoomInterpSpeed=20.f;

	/*
	 * 这把枪的「击杀确认图标」—— 本回合第 1~6 杀各自画哪张图。
	 *
	 * 指向一个 UWeaponKillIconSet 数据资产（每把枪一份，建在 Content 里，
	 * 六个槽怎么填、留空什么语义都见那个类的头文件注释）。
	 *
	 * 留空 = 击杀确认仍用 C++ 里那套矢量造型（X / 菱形 / 五星…），也就是没配之前的表现。
	 * 配不配都不影响功能，可以一把枪一把枪慢慢补。
	 *
	 * 谁读它：ABlasterGameMode 判定击杀时从**击杀者**当时手上那把枪取，
	 * 随击杀确认的 RPC 一起发给客户端；ABlasterHUD::DrawKillMarker 按本回合击杀数挑对应的那张，
	 * 那一档没填才回落到矢量造型。
	 */
	UPROPERTY(EditAnywhere, Category = "Kill Icons")
	class UWeaponKillIconSet* KillIconSet;

	/**
	 * 「这一杀是本回合第几杀」（1 起）—— 击杀反馈（图标 / 音效）靠它选档位。
	 *
	 * 就是把 PlayerState 上那个计数原样读出来（**不要 +1**）。原因：叫它的地方是武器判定
	 * "这一发把人打死了"的那一瞬间，而伤害是 `ApplyDamage` **同步**结算的 ——
	 * 它内部走 Character::ReceiveDamage →（血量归零）GameMode::PlayerEliminated → AddRoundKill()，
	 * 所以武器在下一行调过来时计数里已经含这一杀了。详见 .cpp 里那段注释。
	 *
	 * 必须在**服务器**算（四个开火点本来就都在服务器权威分支里），算好了随 RPC 一起发下去。
	 * 让客户端自己数不可靠：RPC 到达时它那边的计数复制到第几了说不准，
	 * 表现会是"图标显示第 3 杀、音效却响第 2 段"。
	 */
	static int32 ComputeThisKillIndex(const class AController* KillerController);
	
	/*
	 *Automatic fire
	 */
	UPROPERTY(EditAnywhere,Category="Combat")
	float FireDelay=.15f;

	UPROPERTY(EditAnywhere,Category="Combat")
	bool bAutomatic=true;

	/*
	 *手持这把武器时的**跑动上限**（cm/s）。
	 *
	 *0 或负数 = 「没单独配过」→ 用 UCombatComponent 里按 WeaponType 查到的默认值
	 *（手枪 650 / 步枪 600 / 霰弹 575 / 狙击 550 / 近战 700）。填了正数就只认这里。
	 *
	 *为什么默认值走 WeaponType 而不是逼着人在每张武器蓝图的 Details 里填：
	 *  武器蓝图是一堆派生（BP_Pistol / BP_Shotgun / BP_SniperRifle / BP_Melee …），
	 *  一个个手填，漏掉一个的表现是「这把枪跑起来和步枪一样快」—— 无声无息，很难查。
	 *  所以 C++ 先按类型给一套完整默认，蓝图里想给某一把枪单独调快慢时再覆盖它。
	 *
	 *这是**跑动**（不瞄准、不蹲）的上限。瞄准/开镜的速度不再是各自的绝对值，而是按比例
	 *跟着它降（见 UCombatComponent::AimSpeedRatio / ScopedSpeedRatio）—— 否则一把配得很慢
	 *的枪会出现「瞄准比跑还快」这种荒唐结果。
	 */
	UPROPERTY(EditAnywhere, Category = "Movement")
	float MaxWalkSpeed = 0.f;

	/*
	 *服务器回溯（lag compensation）
	 *
	 *开了之后，这一枪的伤害不再由服务器「按现在的位置」在 Fire() 里结算，而是由
	 *UCombatComponent::ServerFire 拿「客户端开火那一刻」的位置回溯重算（见 ULagCompensationComponent）。
	 *会飞的子弹（AProjectileWeapon）构造里关掉：命中由投射物自己碰撞决定。
	 */
	UPROPERTY(EditAnywhere, Category = "Net")
	bool bUseServerSideRewind = true;

	UPROPERTY(EditAnywhere,Category="Combat")
	USoundCue* EquipSound;

	// 购买价格（经济系统，BuyPhase 时用 Credits 购买）
	UPROPERTY(EditAnywhere, Category = "Economy")
	int32 WeaponCost = 0;

	/*
	 *Enable or disable custom depth
	 */
	void EnableCustomDepth(bool bEnable);

	// 真枪网格上的枪口 socket 位置（MuzzleFlashSocket）。socket 名是 protected，所以取用入口放这里。
	// **不再是命中射线的起点** —— 起点是眼睛（AHitScanWeapon::GetShotOrigin / ABlasterCharacter::GetFPEyeWorldLocation）。
	// 现在只剩两个用处：① 别人屏幕上给这一发补表现时的射线起点 ② 未开回溯武器/投射物的发射位置。
	// 拿不到枪口 socket 时返回 false。
	bool GetMuzzleLocation(FVector& OutLocation) const;

	/*
	 * 本人**实际看得见**的那个枪口：开了第一人称副本时，真枪（WeaponMesh）对本人是隐藏的，
	 * 看得见的是 GetViewMesh()。所以凡是"要画在枪口上给本人看"的东西（枪口火光、弹道轨迹）
	 * 都得取这个，而不是 GetMuzzleLocation（那个永远取真枪，射手自己会看到火光/轨迹从空中的枪上冒出来）。
	 * 拿不到枪口 socket（网格还没挂上 / 没这个 socket）时返回 false。
	 */
	bool GetViewMuzzleTransform(FTransform& OutTransform) const;
	
protected:
	virtual void BeginPlay() override;

	// 开火特效（每枪统一）：枪口火光 + 枪声。在基类 AWeapon::Fire 里播放，
	// 所以命中扫描武器和投影武器（步枪）都有开火反馈。
	UPROPERTY(EditAnywhere, Category = "Combat")
	class UParticleSystem* MuzzleFlash;

	UPROPERTY(EditAnywhere, Category = "Combat")
	USoundCue* FireSound;

	// —— Socket 名（武器骨骼网格上）——
	// 枪口火光 / 弹道轨迹 / 弹壳抛出口的 socket。默认 "MuzzleFlash"/"AmmoEject"，
	// 换不同模型（如 Valorant 网格）时在武器蓝图里改。
	// **socket 名和骨骼名都行**（取用入口两个都认）。实测各枪网格：
	//   · 手枪/AK/狙击/SMG：真有 MuzzleFlash socket（就在 MuzzlePoint 骨骼那个位置）
	//   · ★ 霰弹枪的 Slim_Mesh：**没有** MuzzleFlash socket，只有一个 Muzzle 骨骼
	//     → 在 BP_Shotgun 的 Details 里把这一格改成 "Muzzle"，否则霰弹枪的枪口火光和弹道轨迹
	//       都会找不到枪口（轨迹会退回"从眼睛出发"）。
	UPROPERTY(EditAnywhere, Category = "Socket")
	FName MuzzleFlashSocket = TEXT("MuzzleFlash");

	UPROPERTY(EditAnywhere, Category = "Socket")
	FName AmmoEjectSocket = TEXT("AmmoEject");

	// —— 逐武器差异化后坐力（四把枪在 Details 面板各自调）——
	// 每枪的相机后坐力俯仰量（度）：开火时本地镜头向上抬，随后自动回稳。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilPitch = 2.f;

	// 每枪的相机水平后坐力（度）：每枪在 ±RecoilYaw 间随机，制造弹道晃动感。
	// 步枪/霰弹用，狙击设 0（纯垂直大跳）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilYaw = 0.5f;

	// 本枪后坐力累积上限（度）：连发时镜头最多抬到这个角度（步枪连射的上限）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float MaxRecoilPitch = 6.f;

	// 本枪回稳速度：越大回正越快（手枪利落、狙击慢沉）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float RecoilRecoverySpeed = 8.f;

	// —— 可学习压枪弹道（可选，主要给全自动步枪配）——
	// 非空时，本次连发的第 N 发用序列第 N 个值（超出取最后一个），
	// 序列是固定的 → 弹道可记忆、可压枪（Valorant 式）。留空则退回上面 RecoilPitch/随机 Yaw。
	UPROPERTY(EditAnywhere, Category = "Combat")
	TArray<float> RecoilPitchPattern;

	// 每发水平偏移序列（正=右、负=左）。留空则退回随机 ±RecoilYaw。
	UPROPERTY(EditAnywhere, Category = "Combat")
	TArray<float> RecoilYawPattern;

	// —— 爆头判定 ——
	// 命中骨骼名 == HeadBoneName（默认 "head"）→ 伤害 × HeadshotMultiplier。
	// 手枪/狙击/步枪配倍率；霰弹枪设 1（弹丸多，不做爆头判定）。
	UPROPERTY(EditAnywhere, Category = "Combat")
	float HeadshotMultiplier = 2.f;

	UPROPERTY(EditAnywhere, Category = "Combat")
	FName HeadBoneName = TEXT("head");

	void PlayFireEffects();

	UFUNCTION()
	virtual void OnSphereOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult
	);

	UFUNCTION()
	void OnSphereEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex
	);

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	USkeletalMeshComponent* WeaponMesh;

	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class USphereComponent* AreaSphere;

	UPROPERTY(ReplicatedUsing = OnRep_WeaponState, VisibleAnywhere, Category = "Weapon Properties")
	EWeaponState WeaponState;

	UFUNCTION()
	void OnRep_WeaponState();

	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class UWidgetComponent* PickupWidget;

	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimationAsset* FireAnimation;

	/*
	 * ——— 枪械**模型自己**的动画（播在 WeaponMesh 上，**所有机器都播**）———
	 *
	 * 和下面那组"第一人称手模动画"的分工，别搞混：
	 *   · 这一组 → 枪这个模型自己的动画（工程里 Guns/<武器>/ 那套 GN_Core_* 就是，
	 *     挂在枪自己的骨架 AK_Skeleton / Slim_Skeleton … 上），远端玩家也看得见
	 *     （他们看得见你的枪在动、弹匣在掉）；开火那条就是上面的 FireAnimation。
	 *   · 下面 FPXxxMontage 那四条 → 你本人的**手臂**（FPArmsMesh），只在本机播。
	 *
	 * 类型故意写成 UAnimationAsset（和 FireAnimation 一致），所以**两种都能填**：
	 *   · 填 AnimSequence / AnimSequenceBase → 走 WeaponMesh->PlayAnimation（单节点，零配置，
	 *     不需要动画蓝图，播完停在最后一帧；后一条动画直接顶掉前一条，硬切）
	 *   · 填 AnimMontage → 走武器网格动画实例的 Montage_Play（能被打断、能混合、能查状态，
	 *     但**需要给武器网格挂一个带 Slot 节点的动画蓝图**；没挂的话会打一条 warning 并
	 *     退回用蒙太奇里那第一段序列单节点播，不会静默不动）
	 * 也就是说：先填序列就能跑，哪天需要打断/相位同步，把同一个资产转成蒙太奇填进来即可，
	 * C++ 不用改。
	 */
	// 掏出这把枪时枪自己的动画（拉栓/上膛那种）。走的是"所有机器"，和 FPEquipMontage 各播各的
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimationAsset* EquipAnimation;

	// 换弹时枪自己的动画（弹匣掉落/上膛）。只在换弹**开始**那一刻播一遍
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimationAsset* ReloadAnimation;

	// 检视时枪自己的动画。和 FPInspectMontage 同时播，一个是枪、一个是手
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimationAsset* InspectAnimation;

	/*
	 * ——— 换弹节奏：服务器权威，不许交给动画 ———
	 *
	 * 为什么时长必须由 C++ 说了算：换弹结束是 ECS_Reloading 的**唯一出口**，出不来就
	 * 开不了枪（CanFire 只认 ECS_Unoccupied）也切不了枪（EquipSlotWeapon/SwitchWeapon 直接 return）。
	 *
	 * 这件事以前由换弹蒙太奇尾部的动画通知驱动，而那条通知的实现挂在动画蓝图里（ABP_Blaster 系的
	 * AnimNotify_ReloadFinished）。2026-09-15 角色网格换成瓦的 Wushu 模型、anim_class 换成
	 * ABP_BlasterCharacter 之后，通知悄无声息地没了 —— 于是所有人永久卡在「换弹中」。
	 * 现在服务器起计时器把这件事做成确定性的，蒙太奇只负责好看（空着/播不出来都无所谓）。
	 *
	 * 2026-09-26 补充：那些"结束通知"后来又被挂回到了换弹/掏枪蒙太奇上（挂的是第三人称那条），
	 * 和计时器抢着收尾 —— 通知总是比动画末帧早响，而第三人称动画又和第一人称手模不等长，
	 * 于是"时长"变成了第三人称动画的长度、和玩家看到的差半拍。现已全部摘掉（备份在
	 * E:/Notion/claude-temp/backup_tp_notify_20260926/），时长只由计时器给。
	 */
	// 整段换弹要多久（秒）。0 = 自动：优先用 FPReloadMontage 的**实际**长度（原始长度 ÷ RateScale，
	// 见 Weapon.cpp 的 GetEffectivePlayLength），没挂就兜底 2 秒。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float ReloadTime = 0.f;

	/*
	 * 掏出这把枪要多久（秒）—— 也就是 ECS_Equip 要待多久。
	 *
	 * 和 ReloadTime 同一套道理（见上面那段）：掏枪结束是 ECS_Equip 的**唯一出口**，
	 * 出不来就开不了枪也换不了弹。所以这里同样由 C++ 起一条服务器计时器收尾。
	 *
	 * 2026-09-26 之前掏枪蒙太奇尾部还挂着 UAnimNotify_EquipFinished，和这条计时器抢着收尾：
	 * 它落在第三人称掏枪动画的末帧之前（比动画早 0.1~0.5 秒），实测**总是它先到** ——
	 * 于是"掏枪时长"实际是那条第三人称动画的长度，和玩家真正看到的第一人称手模对不上。
	 * 现在那条通知已从蒙太奇上摘掉，计时器是唯一判据（见 CombatComponent::StartEquipTimer）。
	 *
	 * 0 = 自动：优先用 FPEquipMontage 的**实际**长度（原始长度 ÷ RateScale；玩家掏枪时真正看到
	 * 的那条），没挂就兜底 0.6 秒。想做"比动画快一点/慢一点"的手感就直接填秒数。
	 *
	 * 注意 ECS_Equip **不锁切枪**（CanChangeWeapon 放行 ECS_Equip，和 ECS_Reloading 正相反）：
	 * 掏枪动画播到一半按 1/2 就是把新枪换上来 —— 这也是这条时长可以做得短、做错也不会把人卡住的原因。
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float EquipTime = 0.f;

	// 霰弹枪每压一发要多久（秒）。霰弹枪是「一发一发压」，不走上面那个整段时长。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float ShotgunShellTime = 0.4f;

	// 霰弹枪压满 / 没备弹之后留给动画收尾的时间（秒），到点才离开「换弹中」。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float ReloadRecoveryTime = 0.25f;

	// 上面三条 + FireAnimation 的公共实现：判空 + PlayAnimAssetOnMesh + 顺带让第一人称副本同步
	void PlayGunAnimation(class UAnimationAsset* Anim);

	/*
	 * ——— 第一人称手模动画：每个武器蓝图在这里挂自己那几条 ———
	 *
	 * 四条都是 **UAnimMontage**（不能是裸的 AnimSequence：手模靠 ABP_WushuFP 里的
	 * Slot 节点接住它，单节点动画没有槽位概念、会被动画机整个覆盖掉，播了也看不见）
	 * —— 唯一的例外是下面那个右键专用的 FPRightClickMontage，它是 UAnimSequenceBase，
	 * 填裸序列也行（角色那边会按槽名现造一条动态蒙太奇再播），见那条的注释；
	 * 四条都**只在射手本人的机器上播**（别人的手模根本不渲染），槽名要和 ABP 里的 Slot 节点一致。
	 * 留空 = 那条动作不播手模动画，但别的东西照旧（开火照样有枪口/枪声/弹壳、
	 * 换弹照样在 ReloadTime 到点后把弹匣填满）—— 所以可以先挂一条、其它慢慢补。
	 *
	 * ⚠⚠ **必须是手模骨架（FP_Wushu_S0_Skeleton）的蒙太奇**，不能把别的骨架的动画填进来。
	 *   引擎按**骨名**绑轨道，同名骨会被照样改写：手模和刀骨架都有叫 Skeleton/Root 的骨，
	 *   两者根链朝向差 120°，而第一人称相机就挂在手模的 Camera 骨上 ——
	 *   填错的表现是「整个视角被拧转」，不是"不播动画"。代码里已经拦了
	 *（ABlasterCharacter::IsMontageCompatibleWithMesh，播之前判骨架并打日志），但正确的是别填错：
	 *   刀那套 AB_Wushu_S0_X_* 是给 AJettCharacter::KnifeRigMesh 用的（角色上的 KnifeAttack/KnifeEquip），
	 *   手模要用另一套手模骨架的 FP_Wushu_S0_X_* 动画。
	 *
	 * 和上面 FireAnimation 的分工，别搞混：
	 *   · FireAnimation → **武器网格自己**（WeaponMesh），UAnimationAsset（单节点动画就行），
	 *     **所有人的机器上都播**（远端玩家要看你的枪口在动）。
	 *   · 下面这四条 → **你本人的手模**（FPArmsMesh），只在本机播。
	 *
	 * 都由武器自己播出去（PlayFPXxxMontage），调用点是各自的游戏逻辑：
	 *   FPFireMontage    ← AWeapon::Fire（每次开火；连发时每枪从第一帧重播）
	 *   FPEquipMontage   ← UCombatComponent::EquipSlotWeapon（切枪/捡枪/技能结束重新掏枪，
	 *                       所有"把这把枪拿到手上"的路都从这里过）
	 *   FPReloadMontage  ← UCombatComponent::HandleReload（换弹动作开始的时候，一次换弹播一遍）
	 *   FPInspectMontage ← 检视键（IA_Inspect）
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimMontage* FPFireMontage;

	// 切枪掏出这把武器时的手模动画（远端玩家的这个动作走的是他们的第三人称，看不见）
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimMontage* FPEquipMontage;

	// 换弹的手模动画。整段换弹只播一次（霰弹枪那种一发一发压的，也只在开始时播一次；
	// 想要"每压一发播一遍"得单独做，说一声）。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimMontage* FPReloadMontage;

	// 检视武器的手模动画（转枪/看弹匣那种）。触发键见 IA_Inspect。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimMontage* FPInspectMontage;

	/*
	 * **右键动作**的手模动画。单独一个字段（不跟开火那条共用），因为右键在这一把武器上
	 * 干的是另一件事：飞刀的右键是"一次全扔"，和一条一条甩的左键是两套动作。
	 *
	 * ⚠ 类型是 UAnimSequenceBase 而不是上面那四条用的 UAnimMontage —— **裸序列也能填**。
	 *   瓦的原始资产里右键大多就是序列（FP_Wushu_S0_X_RightClickAttack），想直接拖进来就能用。
	 *   序列没有"槽位"概念、Montage_Play 播不了，所以角色那边会按槽名现造一条动态蒙太奇再播
	 *（见 ABlasterCharacter::PlayFPArmsMontage）。要做分段/挂通知就做成蒙太奇，一样能填。
	 *
	 * 留空 = **退回开火那条**（FPFireMontage 从头播，也就是第 1 段），也就是右键看起来和左键一样。
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UAnimSequenceBase* FPRightClickMontage;

	// 上面四个 PlayFPXxxMontage 的公共实现：判空 + 把资产递给角色播。
	void PlayFPArmsMontage(class UAnimSequenceBase* Asset);

	/*
	 * ——— 第三人称：每个武器蓝图在这里挂自己那套 ———
	 *
	 * 到这个文件里一共有三条"播动画"的链子，别搞混：
	 *   · FireAnimation / EquipAnimation / ReloadAnimation / InspectAnimation
	 *       → **武器模型自己**（WeaponMesh，挂在武器骨架上），所有机器都播
	 *   · FPXxxMontage → **你本人的手模**（FPArmsMesh），只在本机播
	 *   · ↓ 下面这一组 → **角色第三人称身体**（ABlasterCharacter::GetMesh()），所有机器都播，
	 *       也就是"别人眼里的你"。开火/切枪/换弹各一条。
	 *
	 * 三条**都可以留空**：留空就退回角色身上那条共用蒙太奇（FireWeaponMontage / ReloadMontage，
	 * 里面按武器类型分 section），所以**不配也能跑**，可以一把武器一把武器慢慢补。
	 * 配了就优先用这里的（换弹/装备这类单段动作不再跳 section）。
	 *
	 * ⚠ 「配了却什么都不播」的头号原因是 **Slot 名对不上**：蒙太奇要出画，它内部用的槽名
	 *   必须和角色动画图里那个 Slot 节点的名字一致。现役 ABP_BlasterCharacter 的槽叫
	 *   **"UpperBody"**，而工程里现成的 Mon_FireWeapon / Mon_RifleReload / ThrowGrenade
	 *   用的还是老的 **"WeaponSlot"** —— 对不上时引擎**不报错**，只是静默不播。
	 *   配之前先确认这两个名字是一致的（要么改 ABP 的 Slot 节点，要么在蒙太奇里重命名槽）。
	 */
	// 第三人称开火蒙太奇。连发时每枪从头重播（bStopAllMontages = true）
	UPROPERTY(EditAnywhere, Category = "ThirdPerson")
	class UAnimMontage* ThirdPersonFireMontage;

	// 上面那条里「瞄准 / 腰射」两段的名字（和老的 FireWeaponMontage 用同一套命名，
	// 所以同一份蒙太奇可以直接挪过来）。单段蒙太奇就把这两个都留空 = 从第 0 段开始播。
	UPROPERTY(EditAnywhere, Category = "ThirdPerson", meta = (EditCondition = "ThirdPersonFireMontage"))
	FName ThirdPersonFireAimSection = TEXT("RifleAim");

	UPROPERTY(EditAnywhere, Category = "ThirdPerson", meta = (EditCondition = "ThirdPersonFireMontage"))
	FName ThirdPersonFireHipSection = TEXT("RifleHip");

	// 掏出这把武器时第三人称身体的蒙太奇（切枪/捡枪/技能结束重新掏枪都走这里）。
	// 单段动作，不跳 section。
	UPROPERTY(EditAnywhere, Category = "ThirdPerson")
	class UAnimMontage* ThirdPersonEquipMontage;

	// 换弹时第三人称身体的蒙太奇。单段动作，不跳 section。
	// 什么时候算换完**不看这条动画**，看 AWeapon::ReloadTime（服务器计时器），
	// 所以留空 / 播到一半被打断都不会把人卡在「换弹中」。
	UPROPERTY(EditAnywhere, Category = "ThirdPerson")
	class UAnimMontage* ThirdPersonReloadMontage;

	/*
	 * **右键动作**的第三人称身体动画（现在只有飞刀的"一次全扔"用）。
	 *
	 * 和上面三条的区别、以及为什么类型是 UAnimSequenceBase（裸序列也能填）见
	 * FPRightClickMontage 的注释。另外这条是 **UB（上半身）**那份：
	 * 瓦的右键资产是 TP_Wushu_S0_X_RightClickAttack_UB / _LB 两条，
	 * 身体动画图上的槽是 "UpperBody"，所以默认播在**上半身** —— 下半身那条要一起动就得
	 * 自己做成一条蒙太奇（或者再开一个字段，说一声）。
	 *
	 * 留空 = 身体不出手。注意这跟手模那条不一样 —— 对手模来说"不播"等于手不动，
	 * 所以那边留空会退回开火那条顶着；这一条留空就是真的什么都不播。
	 *
	 * ⚠ 和 ThirdPersonFireMontage 的分工（2026-09-18 改）：
	 *   单扔（左键）走 ThirdPersonFireMontage 的 "1".."5" 段；
	 *   全扔（右键）先被 UCombatComponent::Fire 播一遍上面那条（本类现在
	 *   ShouldPlayBodyFireMontage() 返回 true 了，见 AJettKnives.h），紧接着被这一条顶掉 ——
	 *   同一帧内的事，看不出闪。两条都留空时身体在扔刀期间完全不动。
	 */
	UPROPERTY(EditAnywhere, Category = "ThirdPerson")
	class UAnimSequenceBase* ThirdPersonRightClickMontage;

	/*
	 * 这把武器挂在**第三人称角色骨骼**的哪根骨骼 / socket 上。
	 *
	 * 默认 "RightHandSocket" —— Wushu 角色骨骼自带的那一个。想给某把武器单独调握位
	 *（狙击枪靠肩、手枪抬高一点之类），在角色骨骼上加个 socket 把名字填这里就行；
	 * 直接填骨骼名也可以（找不着 socket 时引擎会把名字当骨骼用，不报警告）。
	 *
	 * 读取点有两处，改这里两边一起生效：
	 *   UCombatComponent::EquipSlotWeapon（拿枪到手）
	 *   ABlasterCharacter::OnRep_EquipWeapon（远端机器收到复制）
	 */
	UPROPERTY(EditAnywhere, Category = "ThirdPerson")
	FName ThirdPersonAttachSocket = TEXT("RightHandSocket");

	/*
	 * ——— 弹匣（静态网格）———
	 *
	 * 为什么要单独挂：换弹动画里弹匣是跟着武器骨骼走的（掉下去/装上来），
	 * 做成一个挂在骨骼上的静态网格组件，骨骼一动它自己就跟着动 —— 不用 K 动画。
	 */
	// 弹匣用的静态网格（工程里 Guns/<武器>/ 下的 AK_Magazine / Bolt_Magazine 那些就是）。
	// 留空 = 不出弹匣（什么都不做，不影响别的）。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UStaticMesh* MagazineMesh;

	// 弹匣挂到武器网格的哪根**骨骼**（或 socket，找不着 socket 时引擎会把名字当骨骼用）。
	// 各枪的实际名字（从资产里读出来的）：
	//   AK / 手枪 / 大狙 → Magazine_Main（AK 的网格上还有个同位的 socket 叫 Magazine_MainSocket，
	//                        两个都行 —— 骨骼名和 socket 名引擎都认）
	//   Slim 霰弹枪 → Mag_Rotator
	// 填错/不存在的话弹匣会贴在武器原点，不会报错也不会崩 —— 一眼看得出来。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FName MagazineSocket = TEXT("Magazine_Main");

	// 弹匣的**世界缩放**。1 = 静态网格资产的原大小。
	// 为什么要有这个：枪骨架根骨骼是 ×100 的（瓦的米→厘米），而弹匣静态网格的局部数据是厘米
	//（AK_Magazine 局部量出来约 22cm 长），所以"相对缩放"必须是 1/100 = 0.01 才对 ——
	// 组件上的 Relative Scale 由代码按这个值算好写进去，**你填在组件缩放栏里的值会被覆盖**，
	// 要调大小请填这里（个别枪的弹匣网格单位不一样时才需要动，默认 1 就行）。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float MagazineWorldScale = 1.f;

	// 位置/旋转微调（世界厘米；0 = 正好贴在骨骼上）。
	// 弹匣网格的轴心不在卡口上时用这个挪，别去改静态网格资产的轴心。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FVector MagazineLocationOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FRotator MagazineRotationOffset = FRotator::ZeroRotator;

	// 弹匣组件本体。位置/旋转吸附到 MagazineSocket，缩放见 MagazineWorldScale。
	// 网格两种填法都认：① 填上面的 MagazineMesh 属性 ② 直接在树里给这个组件选 Static Mesh
	//（属性填了就以属性为准；属性留空时**不会**去清组件上已选的网格）。
	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class UStaticMeshComponent* MagazineMeshComp;

	/*
	 * ——— 枪上的"备用弹匣位"（MagazineExtraSocket）———
	 *
	 * 就是**常驻别在枪身上那块备用弹匣**的位置：填了 MagazineExtraMesh 就在这个位置上
	 * 一直挂一块弹匣（瓦的很多枪模上本来就别着一块备用弹匣）。不填 = 不显示，默认状态。
	 *
	 * 挂哪根骨骼由 UpdateMagazineExtraAttachment() 在 OnConstruction /
	 * PostInitializeComponents 两个时机去挂（和弹匣/瞄准镜同一套）。
	 *
	 * 曾经还有一条"换弹时把弹匣井里那块临时挪过来、靠动画通知驱动"的路子，2026-09-21
	 * 改选常驻方案后**已整体删除**（连同两个 AnimNotify 类和那时留下的兜底调用）。
	 */
	// 枪身上"备用弹匣位"那根 socket / 骨骼（名字是 socket 或骨骼都认）。
	// 默认 Magazine_ExtraSocket —— 目前的网格里**只有 AK_Mesh 上有这根 socket**
	//（它同时还有 Magazine_Extra 这根骨骼）。别的枪想用这个位置，得自己在网格上补一根同名的。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FName MagazineExtraSocket = TEXT("Magazine_ExtraSocket");

	// 挂在这个位置上时的微调（世界厘米 / 度；0 = 正好贴在 socket 原点上）。
	// 单独给一组、不跟上面那组共用：备用位和弹匣井的朝向常常不一样，十有八九要另外转一下。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FVector MagazineExtraLocationOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FRotator MagazineExtraRotationOffset = FRotator::ZeroRotator;

	/*
	 * 常驻在备用弹匣位上的那块备弹（静态网格）。
	 *
	 * 和 MagazineMesh 是两回事：MagazineMesh 是**弹匣井里那块**（会跟着换弹动画动），
	 * 这个是**一直别在枪身上那块**。留空 = 不显示（默认）—— 只有枪模上真有这个位置的枪才需要填。
	 * 填法和 MagazineMesh 一样两种都认：填这里，或者直接在组件树里给 MagazineExtra 组件选 Static Mesh。
	 */
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UStaticMesh* MagazineExtraMesh;

	// 备弹的**世界缩放**（1 = 静态网格资产原大小）。语义同 MagazineWorldScale。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float MagazineExtraWorldScale = 1.f;

	// 备弹组件本体（默认挂 MagazineExtraSocket，见上）。
	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class UStaticMeshComponent* MagazineExtraMeshComp;

	/*
	 * ——— 瞄准镜（静态网格）———
	 *
	 * 和弹匣同一套路：只建组件，**具体挂哪根骨骼**交给 UpdateScopeAttachment() 在
	 * OnConstruction / PostInitializeComponents 两个时机去挂（构造期读不到蓝图子类填的值）。
	 * 缩放/微调的坑也一样（枪骨架根骨骼 ×100）—— 组件上的相对变换由代码算好写进去，
	 * 要调就填下面这几个属性，别改组件、也别改静态网格资产的轴心。
	 */
	// 瞄准镜的静态网格资产。留空 = 不显示（什么都不做，不影响别的）。
	// 和弹匣一样两种填法都认：填这里，或者直接在组件树里给 ScopeMesh 组件选 Static Mesh。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class UStaticMesh* ScopeMesh;

	// 挂在武器网格的哪根 socket/骨骼上。默认 ReflexSocket（反射式瞄具那个位）；
	// 换模型/换挂点时在武器 BP 里改。名字不存在时引擎会把名字当骨骼用，不报警告，
	// 表现是瞄准镜贴在武器原点 —— 一眼看得出来。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FName ScopeSocket = TEXT("ReflexSocket");

	// 瞄准镜的**世界缩放**（1 = 静态网格资产原大小）。语义同 MagazineWorldScale。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	float ScopeWorldScale = 1.f;

	// 位置/旋转微调（世界厘米；0 = 正好贴在 socket 上）。网格轴心不在卡口上时用这个挪。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FVector ScopeLocationOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FRotator ScopeRotationOffset = FRotator::ZeroRotator;

	// 瞄准镜组件本体（默认挂 ReflexSocket，见 ScopeSocket）。
	UPROPERTY(VisibleAnywhere, Category = "Weapon Properties")
	class UStaticMeshComponent* ScopeMeshComp;

	/*
	 * ——— 第一人称挂载：把这把枪绑到我的手模上 ———
	 *
	 * 做法是**本机再显示一份副本**（角色那边的 FPWeaponMesh），真枪一动不动。
	 * 为什么不把真枪直接挪到手模上：① 真枪是**别人看到的那把枪** —— 挪到手模上等于把
	 * 「第三人称身体的手」这个信息扔了，远端玩家会看到枪飘在不属于它的位置；
	 * ② 真枪网格还挂着枪口 socket（枪口火光/抛壳/给别人补表现时的射线起点都读它），
	 * 手模的手和第三人称身体的手不在一个地方，挪过去这些位置全变。
	 * 所以真枪留在第三人称手上，第一人称单独显示一份副本。
	 */
	// 打开 = 本机第一人称显示挂在手模上的枪械副本；关着 = 保持原样（第一人称看到的是
	// 第三人称那只手上的枪）。按武器单独开关，先把手模那条链子调好再逐个打开。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	bool bUseFPViewModel = false;

	// 副本挂在手模的哪根 socket/骨骼上。默认 WeaponADSSocket —— FP_Wushu 手模自带的那唯一一个
	// socket，它自己的 local scale 是 0.01，正好抵掉手模骨架根骨骼那 100 倍
	//（所以挂上去尺寸自动是对的，不用手改缩放）。
	// 想换挂点可以填别的骨骼名（MasterWeapon / WeaponADS / R_Hand 都行 ——
	// 找不着 socket 时引擎会把名字当骨骼用，不报警告）。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	FName FPWeaponSocket = TEXT("WeaponADSSocket");

	// 第一人称副本用哪套网格。留空 = 直接用 WeaponMesh 那套（省事，FP 和 TP 共用一个模型）。
	// 想要 FP 专用模型（更精细/不同 LOD）就填这里。
	UPROPERTY(EditAnywhere, Category = "Weapon Properties")
	class USkeletalMesh* FPViewModelMesh;

	// 编辑器里改 MagazineMesh / MagazineSocket 立刻在 BP 预览里生效（不然要进 PIE 才看得见）
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;

	UPROPERTY(EditAnywhere)
	TSubclassOf<class ACasing> CasingClass;

	// —— 下面这几个从 private 挪到 protected ——
	// 派生武器要在 C++ 构造里设它们的默认值：
	// AJettKnives 必须把自己设成 EWT_Melee / 近战槽 / 一把刀不发弹匣空仓，
	// 光靠蓝图爸爸面板配的话，忘了配就是"按下开火没反应、日志里什么都没"。
	// 蓝图子类照样能在 Details 面板覆盖（依然是 EditAnywhere）。
protected:
	UPROPERTY(ReplicatedUsing = OnRep_Ammo,EditAnywhere)
	int32 Ammo;

	// virtual：AJettKnives 覆盖它把"刀数变了"同步给角色的飞刀显隐
	//（基类这份里那个 JumpToShotGunEnd 对飞刀是误伤 —— IsFull() 恒真，见那边的注释）
	UFUNCTION()
	virtual void OnRep_Ammo();

	void SpendRound();

	UPROPERTY(EditAnywhere)
	int32 MagCapacity;

	UPROPERTY(EditAnywhere)
	EWeaponType WeaponType;

private:
	UPROPERTY()
	class ABlasterCharacter* BlasterOwnerCharacter;
	UPROPERTY()
	class ABlasterPlayerController* BlasterOwnerController;

public:
	void SetWeaponState(EWeaponState State);

	// 整把枪（枪身 + 弹匣/瞄准镜 + 拾取提示）的显隐，走 SetActorHiddenInGame。
	// 只有状态机该调它 —— 单独调会和 EWS_Holstered/EWS_Equipped 的语义打架。
	void SetWeaponVisible(bool bVisible);

	// ——— 第一人称手模动画的播放入口 ———
	// 各自对应的蒙太奇就是上面那四个属性，留空则什么都不做。
	// 内部统一判空 + 只认本地控制的那台机器（远端手模不渲染），所以调用点不用重复判断。
	void PlayFPFireMontage();
	void PlayFPEquipMontage();
	void PlayFPReloadMontage();
	void PlayFPInspectMontage();

	// 右键动作的两条（手模 / 第三人称身体），对应的资产就是上面 FPRightClickMontage 和
	// ThirdPersonRightClickMontage 那两个字段。调用点见 AJettKnives::ThrowAllKnives。
	// 两条都自带"留空退回老行为"，所以可以先只配一条。
	void PlayFPRightClickMontage();
	void PlayThirdPersonRightClickMontage();

	/*
	 *把**正在播**的那条手模开火蒙太奇跳到某一段。
	 *
	 *存在的理由：AWeapon::Fire 里那条 PlayFPFireMontage() 永远是"从头播"，
	 *而一条蒙太奇分了好几段、每次开火该播不同段的武器（Jett 飞刀：5 段）需要跳段。
	 *  · 只在本机 + 手模在 + FPFireMontage 正在播的时候才动（那三个条件 PlayFPArmsMontage
	 *    已经保证了；这里再判一次是因为调用点在 Fire() 之后，理论上那条蒙太奇可能已经结束）
	 *  · 段名对不上什么都不做 —— 退回"完整的从头播"，不会播错别人的动作
	 */
	void JumpFPFireMontageToSection(FName Section);

	/*
	 * 和上面那条一模一样，只是打在**第三人称身体**（GetMesh()）上、作用的蒙太奇是
	 * ThirdPersonFireMontage（也就是 ABlasterCharacter::PlayFireMontage 刚播起来的那条）。
	 *
	 * 存在的理由也一样：PlayFireMontage 永远是"从头播"，而按次数分段播的武器
	 * （飞刀：5 段，第 N 次扔刀播第 N 段）需要跳段 —— 而且**跳段必须等到扣完弹匣才知道是第几次**
	 * （AWeapon::Fire 末尾的 SpendRound），所以只能在 Fire 之后的 AJettKnives::PlayThrowPresentation 里跳。
	 *
	 * ⚠ 和 FP 那条的区别：**不判本地控制**。身体是所有人眼里的你，每台机器都在播同一条蒙太奇，
	 *   所以每台机器都得跟着跳（跳的是各自本地的动画实例，本来就各跳各的）。
	 */
	void JumpThirdPersonFireMontageToSection(FName Section);

	// 把一条动画播到某个网格上：蒙太奇走槽位（需要那个网格有带 Slot 的动画蓝图），
	// 其它类型走单节点 PlayAnimation。枪自己的网格和第一人称那份副本共用这一套规则，
	// 所以是 public static —— 角色那边的 PlayFPWeaponAnimation 也要调它。
	static void PlayAnimAssetOnMesh(class USkeletalMeshComponent* Mesh, class UAnimationAsset* Anim);

	// ——— 枪械模型自己动画的播放入口 ———
	// 对应上面 EquipAnimation / ReloadAnimation / InspectAnimation（开火那条就是 FireAnimation，
	// 已经在 Fire() 里播了）。留空则什么都不做；内部会自动识别序列/蒙太奇，并让第一人称副本同步。
	void PlayFireAnimation();
	void PlayEquipAnimation();
	void PlayReloadAnimation();
	void PlayInspectAnimation();

	// ——— 换弹节奏的读取口（UCombatComponent 的服务器计时器要用）———
	// 三个属性都在 private 区，编辑器里照样能配 —— 访问修饰符不影响 EditAnywhere 的详情面板，
	// 这里只是把它们从 C++ 侧暴露出去。
	float GetReloadDuration() const;
	FORCEINLINE float GetShotgunShellTime() const { return ShotgunShellTime; }
	FORCEINLINE float GetReloadRecoveryTime() const { return ReloadRecoveryTime; }

	// ——— 掏枪节奏的读取口（ECS_Equip 的兜底计时器要用）———
	// 和 GetReloadDuration 一个形状：EquipTime 填了用它，没填就按 FPEquipMontage 的**实际**长度
	//（原始长度 ÷ RateScale），都没有兜 0.6 秒。两条都**不含 RateScale 的坑**见 Weapon.cpp 的
	// GetEffectivePlayLength。
	float GetEquipDuration() const;

	/*
	 * 这一次「攻击表现」还剩多久播完（秒）。没在播 / 没什么可等的 → 0。
	 *
	 * 存在的理由：收招掏枪时那几条 equip 蒙太奇播在**同一批槽位**上，一上来就把刚播起来的
	 * 攻击动画顶掉（bStopAllMontages=true）。用户 2026-09-18："右键如果没杀掉人 montage 会
	 * 直接被掏枪打断，这里要播完"。角色侧据此把掏枪往后挪
	 *（ABlasterCharacter::ResolveBladeStormEquipDelay / ServerEndBladeStorm）。
	 *
	 * 四条通道都算，取还剩得最久的那条 —— 两条同时在被播的时候才可能被截断，
	 * 只要有一条没播完就得等（少算的表现是"手模那条完整、身体那条被砍掉半截"）：
	 *   手模（FP）  FPRightClickMontage（右键）/ FPFireMontage（左键）
	 *   身体（TP）  ThirdPersonRightClickMontage / ThirdPersonFireMontage
	 * 对普通枪械来说，没配的那两条天然不算数；飞刀是四条都在用的那一个。
	 *
	 * 为什么不用资产全长一把算：右键那两条是从第 0 帧整条播的，全长就是剩余，没问题；
	 * 但**单扔**那条是蒙太奇**跳段**后往下播的 —— 5 段的蒙太奇跳到第 5 段时资产全长还有 5 秒，
	 * 实际只剩最后一段，按全长等会让人物空着手站好几秒。
	 * 所以走实例的播放进度（ABlasterCharacter::GetAnimRemainingTime）。
	 */
	float GetAttackPresentationRemainingTime() const;

	// ——— 给第一人称那份副本（角色的 FPWeaponMesh）用的读取口 ———
	FORCEINLINE FName GetFPWeaponSocket() const { return FPWeaponSocket; }

	// ——— 第三人称那组的读取口 ———
	// 全在 private/protected 区也照样能在 Details 面板配（访问修饰符不影响 EditAnywhere），
	// 这里只是把值递给角色 / CombatComponent 用。
	FORCEINLINE FName GetThirdPersonAttachSocket() const { return ThirdPersonAttachSocket; }
	FORCEINLINE class UAnimMontage* GetThirdPersonFireMontage() const { return ThirdPersonFireMontage; }
	FORCEINLINE FName GetThirdPersonFireAimSection() const { return ThirdPersonFireAimSection; }
	FORCEINLINE FName GetThirdPersonFireHipSection() const { return ThirdPersonFireHipSection; }
	FORCEINLINE class UAnimMontage* GetThirdPersonEquipMontage() const { return ThirdPersonEquipMontage; }
	FORCEINLINE class UAnimMontage* GetThirdPersonReloadMontage() const { return ThirdPersonReloadMontage; }

	// 本人**实际看到**的那个网格：开了第一人称副本就是副本（手模上那份），否则是真枪。
	// **只给纯表现用**（枪口火光、抛壳）—— 因为真枪在开了副本时对本人是隐藏的，
	// 火光/弹壳还按真枪算就会飘在看不见的地方。
	// ⚠ 命中判定**不要**用这个：射线的起点是眼位（AHitScanWeapon::GetShotOrigin），
	// 和武器挂在哪个网格上没有关系；这里返回的是「哪份模型在被渲染」，不是「子弹从哪出发」。
	class USkeletalMeshComponent* GetViewMesh() const;
	FORCEINLINE bool ShouldUseFPViewModel() const { return bUseFPViewModel; }
	// 第一人称该显示哪套网格：FPViewModelMesh 优先，没填就退回武器网格本身
	class USkeletalMesh* GetFPViewModelMesh() const;
	FORCEINLINE class UStaticMeshComponent* GetMagazineMeshComp() const { return MagazineMeshComp; }
	FORCEINLINE class UStaticMeshComponent* GetMagazineExtraMeshComp() const { return MagazineExtraMeshComp; }
	FORCEINLINE class UStaticMeshComponent* GetScopeMeshComp() const { return ScopeMeshComp; }

	/*
	 * 枪身上「另外挂的静态网格装饰」（弹匣、瞄准镜）统一从这里刷新。
	 *
	 * 编辑器（OnConstruction）和运行时（PostInitializeComponents）各调一次；
	 * **角色侧换枪、第一人称副本显隐之后也要再调一次** —— 副本一显示，真枪对本人就隐藏了，
	 * 这些装饰不跟着挪到副本上就会留在看不见的地方（弹匣消失、红点飘在半空）。
	 *
	 * 新增同类装饰时：写一个 UpdateXxxAttachment()，在这里加一行就行，调用点不用动。
	 *
	 * ViewMeshOverride：传「本人此刻实际看到的那个网格」。角色那边第一人称副本正在显示这把枪时，
	 * 把它自己的 FPWeaponMesh 传进来 —— 它**明确知道**副本上摆的是哪把枪，不用武器去猜。
	 *
	 * ⚠ 为什么不能全靠默认的 GetViewMesh() 反推：那个是靠 GetOwner() + 角色当前装备的枪算出来的，
	 * 而捡枪时 UCombatComponent::EquipSlotWeapon 里 SetEquippedWeapon()（→ 走到这里）跑在
	 * SetOwner(Character) **前面**，那一刻 GetOwner() 还是 nullptr，反推只能得到真枪 ——
	 * 装饰就留在第三人称那把枪上了（表现：第一人称里真枪已被 SetOwnerNoSee 隐藏，
	 * 弹匣/瞄准镜却孤零零飘在第三视角枪位上）。
	 * 远端机器上副本永远隐藏，角色侧传进来的是 nullptr，行为跟以前一致。
	 */
	void UpdateAttachedMeshes(class USkeletalMeshComponent* ViewMeshOverride = nullptr);

	// 各自只负责自己那一个组件（缩放/微调换算两边共用 Weapon.cpp 里的
	// AttachDecorationToSocket()）。外部一般不用直接调，走 UpdateAttachedMeshes()。
	void UpdateMagazineAttachment(class USkeletalMeshComponent* ViewMeshOverride = nullptr);
	// 常驻在 MagazineExtraSocket 上的那块备弹（MagazineExtraMesh），同一套挂载逻辑。
	void UpdateMagazineExtraAttachment(class USkeletalMeshComponent* ViewMeshOverride = nullptr);
	void UpdateScopeAttachment(class USkeletalMeshComponent* ViewMeshOverride = nullptr);

	FORCEINLINE USphereComponent* GetAreaSphere() const { return AreaSphere; }
	FORCEINLINE USkeletalMeshComponent* GetWeaponMesh() const { return WeaponMesh; }
	FORCEINLINE float GetZoomFOV() const { return ZoomedFOV; }
	FORCEINLINE float GetZoomInterpSpeed() const { return ZoomInterpSpeed; }
	// virtual：给"没有弹药概念"的武器覆盖（AMeleeWeapon 恒 false → CanFire 不再被弹匣卡住）
	virtual bool IsEmpty();
	// virtual：给"没有换弹概念"的武器覆盖（AJettKnives 恒真 → 整条换弹链路自动短路，见其覆盖注释）
	virtual bool IsFull();
	FORCEINLINE EWeaponType GetWeaponType() const {return WeaponType;};
	/*
	 *是不是**占近战槽**的武器（3 号槽）。用它来判断"不能丢、不能被捡"，而不是 Cast<AMeleeWeapon> ——
	 *判断点：UCombatComponent::DropEquippedWeapon（丢枪输入）和 EquipWeapon（捡枪）。
	 *
	 *⚠️ 判的是"槽位"而不是"哪种武器"：Jett 大招的飞刀是另一个类型（EWT_BladeStorm）但占同一个槽，
	 *   所以这里认两个值。要问"是普通刀还是飞刀"用 GetWeaponType()。
	 */
	FORCEINLINE bool IsMeleeWeapon() const
	{
		return WeaponType == EWeaponType::EWT_Melee || WeaponType == EWeaponType::EWT_BladeStorm;
	}
	// 手枪和霰弹枪都占副武器槽位（出生自带手枪，霰弹枪是可选副武器）
	FORCEINLINE bool IsSecondaryWeapon() const
	{
		return WeaponType == EWeaponType::EWT_Pistol || WeaponType == EWeaponType::EWT_Shotgun;
	}
	// 是否可开镜：目前只有狙击枪能开镜（右键出镜圈 + 强倍率）
	FORCEINLINE bool CanScope() const
	{
		return WeaponType == EWeaponType::EWT_SniperRifle;
	}
	// 是否可右键瞄准（ADS）—— 见 bCanAim 的注释。CanScope() 的枪当然也能瞄准
	FORCEINLINE bool CanAim() const { return bCanAim; }
	// 瞄准时屏幕中央的贴图；没配就是 nullptr（HUD 那边以此决定要不要换掉普通准星）
	FORCEINLINE UTexture2D* GetAimTexture() const { return AimTexture; }
	// 瞄准镜框的颜色（默认白）。和准星的 CrosshairsColor 是两码事，见 AimTextureColor 的注释
	FORCEINLINE FLinearColor GetAimTextureColor() const { return AimTextureColor; }
	FORCEINLINE int32 GetAmmo() const { return Ammo; }
	FORCEINLINE int32 GetMagCapacity() const { return MagCapacity; }
	FORCEINLINE int32 GetWeaponCost() const { return WeaponCost; }
	FORCEINLINE float GetRecoilPitch() const { return RecoilPitch; }
	FORCEINLINE float GetRecoilYaw() const { return RecoilYaw; }
	FORCEINLINE float GetMaxRecoilPitch() const { return MaxRecoilPitch; }
	FORCEINLINE float GetRecoilRecoverySpeed() const { return RecoilRecoverySpeed; }
	FORCEINLINE const TArray<float>& GetRecoilPitchPattern() const { return RecoilPitchPattern; }
	FORCEINLINE const TArray<float>& GetRecoilYawPattern() const { return RecoilYawPattern; }
	FORCEINLINE float GetHeadshotMultiplier() const { return HeadshotMultiplier; }

	// 命中是否为爆头（BoneName == HeadBoneName）。要拿到 BoneName，
	// 武器 trace 必须打中骨骼网格（角色 mesh 已设 Visibility Block，满足）。
	bool IsHeadshot(const FHitResult& Hit) const;
	// 按命中部位算最终伤害：爆头 × HeadshotMultiplier，否则原伤害。
	float GetDamageForHit(float BaseDamage, const FHitResult& Hit) const;

	// 跨回合继承时恢复弹匣弹药（服务器调用，复制到客户端）
	void SetAmmo(int32 NewAmmo);
};
