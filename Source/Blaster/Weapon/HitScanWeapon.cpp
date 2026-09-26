// Fill out your copyright notice in the Description page of Project Settings.


#include "HitScanWeapon.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Weapon/BulletTracer.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "DrawDebugHelpers.h"
#include "Kismet/KismetMathLibrary.h"
#include "WeaponTypes.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/BlasterTypes/Team.h"

FVector AHitScanWeapon::GetShotOrigin() const
{
	/*
	 *起点必须是**眼睛**：
	 *
	 *准星不是从枪口投出去的，是从相机投出去的 —— UCombatComponent::TraceUnderCrosshairs 把屏幕中心
	 *反投影成一条从相机出发的射线，玩家瞄的是那条线。射线起点若改到枪口，就多出一段
	 *「枪口 → 眼睛」的基线（~30cm），近距离时这段基线带来的夹角足够让子弹从准星旁边擦过去。
	 *
	 *这里用的是 ABlasterCharacter::GetFPEyeWorldLocation()：本机 = FPCamera 的世界位置，
	 *正是 TraceUnderCrosshairs 那条射线的原点；其他机器 = 身体网格位置 + FPEyeLocalOffset，
	 *和回溯缓冲区里记的 EyeLocation 是同一个算法（见 ULagCompensationComponent::FillFrame）。
	 */
	if (const ABlasterCharacter* OwnerCharacter = Cast<ABlasterCharacter>(GetOwner()))
	{
		/*
		 *本机（射手自己那台机器，本地预测）和**权威机**都走眼位。
		 *
		 *权威机为什么也要走眼位：MulticastFire 在服务器上跑的这一遍 Fire() 不结算伤害
		 *（开了回溯的武器由 ProcessServerRewindHit 单独结算），但它会 ProcessHit → 给射手转发
		 *命中标记和伤害数字（ClientShowHitMarker / ClientShowDamageNumber）。
		 *那条射线要是从别的地方起算，就会拿一条**和回溯射线无关的线**去给射手画反馈 ——
		 *「准星有反馈、跳了伤害数字，但一点血都没掉」正好是这个形状。
		 *所以这一遍必须和 ULagCompensationComponent::ServerSideRewind 用同一条线（同一起点、同一终点）。
		 */
		if (OwnerCharacter->IsLocallyControlled() || HasAuthority())
		{
			return OwnerCharacter->GetFPEyeWorldLocation();
		}
	}

	/*
	 *剩下的就是旁观者机器（非本机 + 非权威）—— MulticastFire 给其他人补的那一份，
	 *只放枪口火光/枪声/弹着特效，没人结算伤害也没人看准星。这里用**大家看得见的那个枪口**：
	 *第一人称手模在别人屏幕上是没有的，从眼位起算会把枪口火光和弹着点拉开一段看得出来的距离。
	 */
	FVector MuzzleLocation;
	if (GetMuzzleLocation(MuzzleLocation)) return MuzzleLocation;
	return GetActorLocation();
}

void AHitScanWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget);

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn == nullptr) return;
	AController* InstigatorController = OwnerPawn->GetController();
	if (InstigatorController == nullptr) return;

	// 起点不再要求「武器网格上必须有枪口 socket」：本机这条路走的是眼位，跟 socket 无关；
	// 只有给其他机器补表现时才需要它（GetShotOrigin 内部拿不到就退到武器自身位置）。
	// 以前这里 socket 缺失会直接跳过整枪，本地预测的表现就全没了。
	FHitResult FireHit;
	const FVector TraceStart = GetShotOrigin();
	WeaponTraceHit(TraceStart, HitTarget, FireHit);

	// 弹道轨迹 + 弹孔。放在这里（而不是某个只在权威机上跑的分支里）的原因和 ImpactParticles 一样：
	// Fire() 由 MulticastFire 在每台机器上各跑一遍，表现就地生成即可，不用再加 RPC；
	// 而射手自己那台机器走的是本地预测这一次（MulticastFire 在射手机器上直接 return，不会放两遍）。
	SpawnTracerFX(TraceStart, HitTarget, FireHit);
	SpawnImpactDecal(FireHit);

	ProcessHit(FireHit, InstigatorController, /*bRewindConfirmation=*/false);
}

namespace
{
	/*
	 * Niagara 用户参数的全名是 "User.XXX"，而 Details 面板上显示的是不带前缀的短名 ——
	 * 在蓝图里填的时候很容易只写短名。名字里没有点就当成用户参数补上前缀；
	 * 带点的（User.xxx / System.xxx）原样使用。
	 * （补前缀的规则以前只在这里一份，现在 ABulletTracer 那条路也要用同一个名字，抽出来共享。）
	 */
	FName ResolveNiagaraParameterName(FName InName)
	{
		if (InName == NAME_None) return NAME_None;

		const FString ParameterString = InName.ToString();
		if (ParameterString.Contains(TEXT("."))) return InName;

		return FName(*FString::Printf(TEXT("User.%s"), *ParameterString));
	}
}

void AHitScanWeapon::SpawnTracerFX(const FVector& TraceStart, const FVector& HitTarget, const FHitResult& FireHit)
{
	// 没填特效 = 这条功能关着（默认状态），直接返回，不做任何事。
	if (TracerEffect == nullptr && TracerParticles == nullptr) return;

	UWorld* World = GetWorld();
	if (World == nullptr) return;

	/*
	 * 起点取**看得见的枪口**，不是判定起点（眼睛）。
	 *
	 * 判定必须从眼睛出发（否则近处会整体偏一条「枪口→眼睛」的基线，见 GetShotOrigin 的长注释），
	 * 但画出来的轨迹要从枪口出发：从眼睛出发的话射手自己看到的是"从镜头里射出去的光柱"。
	 * GetViewMuzzleTransform 给的是本人实际看得见的那个网格（开了第一人称副本时真枪是隐藏的）。
	 *
	 * 枪口 socket 拿不到（网格还没挂上 / 换了模型没这个 socket）时退回判定起点 ——
	 * 宁可轨迹起点偏一点，也别整条轨迹不出现。
	 */
	FVector Start = TraceStart;
	FTransform MuzzleTransform;
	if (GetViewMuzzleTransform(MuzzleTransform)) Start = MuzzleTransform.GetLocation();

	FVector End;
	if (FireHit.bBlockingHit)
	{
		End = FireHit.ImpactPoint;

		// 贴脸开枪（枪口和弹着点几乎重合）时整条轨迹长度约等于 0，画出来是个点，还容易和枪口火光打架。
		// 直接不画这一发。
		if (FVector::DistSquared(Start, End) < FMath::Square(1.f)) return;
	}
	else
	{
		// 打空：沿这一枪的方向走到上限为止。方向用判定起点→HitTarget 算 ——
		// HitTarget 是射手算好的（含散布）800m 处那个点，方向就是真实的弹道方向。
		const FVector AimDirection = (HitTarget - TraceStart).GetSafeNormal();
		if (AimDirection.IsNearlyZero()) return;
		End = Start + AimDirection * TracerMaxDistance;
	}

	const FVector FlightDirection = (End - Start).GetSafeNormal();
	if (FlightDirection.IsNearlyZero()) return;

	// 让特效的 +X 对准弹道方向：朝前发射型的特效（Beam、拉长的面片）直接就能用。
	const FRotator SpawnRotation = FlightDirection.Rotation();
	const FName EndParameterName = ResolveNiagaraParameterName(TracerEndParameter);

	/*
	 * "跟着子弹飞"那条路（默认）：生成一颗假子弹带着特效走。
	 *
	 * 为什么必须这样：拖尾型特效（本项目的 P_*_Tracer_01）的粒子是沿**发射体走过的路**
	 * 一个个丢下来的，发射体不动的结果就是粒子全堆在枪口 —— 表现是"枪口一坨不动的粒子"。
	 * 详见 ABulletTracer 的头文件。
	 */
	if (bTracerFliesWithBullet)
	{
		FActorSpawnParameters SpawnParams;
		// 出生点就在枪口，可能和射手自己/枪身重叠 —— 默认的碰撞处理会判"生成失败"，
		// 表现是"偶尔某一发没有轨迹"（极难复现）。这里明确总是生成。
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ABulletTracer* Tracer = World->SpawnActor<ABulletTracer>(
			ABulletTracer::StaticClass(), Start, SpawnRotation, SpawnParams);
		if (Tracer == nullptr) return;

		Tracer->Speed = TracerSpeed;
		Tracer->InitTracer(TracerParticles, TracerEffect, EndParameterName, End,
			Start, FlightDirection, FVector::Dist(Start, End));
		return;
	}

	// —— 下面这条是"原地生成、特效自己飞"：自带速度的"一颗光点"型特效 / Beam 型特效 ——
	if (TracerEffect)
	{
		UNiagaraComponent* TracerComp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World,
			TracerEffect,
			Start,
			SpawnRotation
		);

		// 终点参数（可选）：把弹着点的世界坐标喂进去，Beam 型的轨迹就能拿它接末端。
		if (TracerComp && EndParameterName != NAME_None)
		{
			// 用 FName 这一版：FString 那一版（SetNiagaraVariableVec3）从 5.3 起标了 UE_DEPRECATED
			//（编译会用 C4996 警告刷屏；本项目 /W4 但没开 /WX，所以它编得过，只是不该用）。
			TracerComp->SetVariableVec3(EndParameterName, End);
		}
	}
	else
	{
		// 老式 Cascade 粒子：没有参数可设，方向靠生成时的旋转带出去。
		UGameplayStatics::SpawnEmitterAtLocation(
			World,
			TracerParticles,
			Start,
			SpawnRotation
		);
	}
}

void AHitScanWeapon::SpawnImpactDecal(const FHitResult& FireHit)
{
	if (ImpactDecalMaterial == nullptr) return;

	// ★ 必须先判 bBlockingHit：没打中的 FHitResult 里 ImpactPoint 是零向量 (0,0,0)，
	// 直接拿去生成贴花的话，每打空一枪就在**世界原点**贴一个弹孔。
	if (!FireHit.bBlockingHit) return;

	// 打在角色身上默认不贴：人的网格一直在动，贴花留在原地就成了"飘在半空中的弹孔"。
	if (!bSpawnDecalOnPawn && Cast<APawn>(FireHit.GetActor()) != nullptr) return;

	/*
	 * 朝向：让贴花的 +X（厚度方向）沿**表面法线**朝外，贴图才是正对着墙面贴上去的。
	 * 法线理论上可能是零向量（个别自定义碰撞体），那种情况下 Rotation() 会给出零旋转、
	 * 贴花躺在地上 —— 不会崩，也不值得为它写特例。
	 */
	UGameplayStatics::SpawnDecalAtLocation(
		GetWorld(),
		ImpactDecalMaterial,
		ImpactDecalSize,
		FireHit.ImpactPoint,
		FireHit.ImpactNormal.Rotation(),
		ImpactDecalLifeSpan
	);
}

void AHitScanWeapon::ProcessServerRewindHit(const FHitResult& Hit)
{
	// 回溯判定已经算完了，这里只补结算（伤害 + 击杀确认音）：
	// 表现部分各机器已经由多播放过，射手本地预测时也出过命中标记，重复放会闪两下。
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	AController* InstigatorController = OwnerPawn ? OwnerPawn->GetController() : nullptr;
	if (InstigatorController == nullptr) return;

	ProcessHit(Hit, InstigatorController, /*bRewindConfirmation=*/true);
}

void AHitScanWeapon::ProcessHit(const FHitResult& FireHit, AController* InstigatorController, bool bRewindConfirmation)
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(FireHit.GetActor());
	// 爆头：命中头骨（BoneName == "head"）→ 伤害 × HeadshotMultiplier。
	// 用同一个 FinalDamage 造成伤害 + 显示伤害数字，保证数字和实际掉血一致。
	const float FinalDamage = GetDamageForHit(Damage, FireHit);

	// 开了回溯的武器：伤害由 ProcessServerRewindHit 单独结算，多播这一份不再扣血（否则一次开枪扣两次）
	const bool bApplyAuthoritativeDamage = bRewindConfirmation || !bUseServerSideRewind;
	if (BlasterCharacter && bApplyAuthoritativeDamage && HasAuthority() && InstigatorController)
	{
		const float HealthBefore = BlasterCharacter->GetHealth();
		UGameplayStatics::ApplyDamage(
			BlasterCharacter,
			FinalDamage,
			InstigatorController,
			this,
			UDamageType::StaticClass()
		);

		// 击杀确认音效：这一发把目标打死（之前血量>0，现在<=0）→ 给击杀者客户端播"叮"
		if (BlasterCharacter->GetHealth() <= 0.f && HealthBefore > 0.f)
		{
			ABlasterPlayerController* KillerPC = Cast<ABlasterPlayerController>(InstigatorController);
			if (KillerPC)
			{
				const bool bHeadshot = IsHeadshot(FireHit);
				if (KillerPC->IsLocalController())
				{
					KillerPC->PlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
				}
				else
				{
					KillerPC->ClientPlayKillSound(bHeadshot, KillIconSet, ComputeThisKillIndex(InstigatorController));
				}
			}
		}
	}

	// 表现部分（命中特效/命中音效/命中标记）：回溯补结算那次一律跳过（bRewindConfirmation），
	// 那些已经由多播和射手的本地预测放过了，这里再来一遍就是两次。
	//
	// ★ bBlockingHit 判断是必须的：打空时 FHitResult 的 ImpactPoint/ImpactNormal 都是零向量，
	//   少了这一句，每打空一枪就会在**世界原点**（地图中央地面 (0,0,0)）放一发弹着特效和音效。
	//   AJettKnives 扔空的那些刀也走这条 ProcessHit，同样受益。
	if (FireHit.bBlockingHit && ImpactParticles && !bRewindConfirmation)
	{
		UGameplayStatics::SpawnEmitterAtLocation(
			GetWorld(),
			ImpactParticles,
			FireHit.ImpactPoint,
			FireHit.ImpactNormal.Rotation()
		);
	}

	// 命中反馈：命中不同队敌人时，射手视角准星闪现命中标记 + 命中点飘出伤害数字。
	// 本机（射手客户端）直接画，服务器转发给射手客户端；旁观者机器（非本机+非权威）什么都不做。
	// 本地预测会在射手这边先出一次，所以服务器只给「不是本机」的射手转发（IsLocalController 分支）。
	if (BlasterCharacter && !bRewindConfirmation)
	{
		ABlasterPlayerState* ShooterPS = OwnerPawn ? OwnerPawn->GetPlayerState<ABlasterPlayerState>() : nullptr;
		ABlasterPlayerState* HitPS = BlasterCharacter->GetPlayerState<ABlasterPlayerState>();
		if (ShooterPS && ((HitPS && HitPS != ShooterPS && HitPS->Team != ShooterPS->Team) || BlasterCharacter->IsTestBot()))
		{
			ABlasterPlayerController* ShooterPC = Cast<ABlasterPlayerController>(InstigatorController);
			if (ShooterPC)
			{
				if (ShooterPC->IsLocalController())
				{
					ShooterPC->ShowHitMarker();
					ShooterPC->ShowDamageNumber(FinalDamage, FireHit.ImpactPoint);
				}
				else if (HasAuthority())
				{
					ShooterPC->ClientShowHitMarker();
					ShooterPC->ClientShowDamageNumber(Damage, FireHit.ImpactPoint);
				}
			}
		}
	}

	if (FireHit.bBlockingHit && HitSound && !bRewindConfirmation)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			HitSound,
			FireHit.ImpactPoint
		);
	}
}

FVector AHitScanWeapon::ComputeTraceEnd(const FVector& HitTarget)
{
	if (!bUseScatter) return HitTarget;

	// 散布必须从**这一枪的实际起点**算起（GetShotOrigin，和 Fire 里那次 WeaponTraceHit 的起点是同一个），
	// 否则散布锥的锥尖和射线起点不重合，弹着点会整体偏一份基线。
	return TraceEndWithScatter(GetShotOrigin(), HitTarget);
}

void AHitScanWeapon::WeaponTraceHit(const FVector& TraceStart, const FVector& HitTarget, FHitResult& OutHit)
{
	UWorld* World = GetWorld();
	if (World)
	{
		/*
		 *忽略开枪者本人和手上这把枪 —— 起点改到眼位之后**必须**加这一步。
		 *
		 *眼位在角色身体里面，而枪就悬在眼前 30cm 处：武器网格是带碰撞的（服务器回溯那边的遮挡
		 *测试也专门 AddIgnoredActor 了它，注释写的就是「别让它把自己这一枪挡了」）。
		 *不忽略的话本地这条射线第一下就打在枪身上，命中特效糊在枪口、伤害一个都结算不了。
		 *以前起点是枪口、本来就在枪体外面，所以不需要这份参数。
		 */
		FCollisionQueryParams Params(TEXT("WeaponTrace"), /*bTraceComplex=*/false, GetOwner());
		Params.AddIgnoredActor(this);

		/*
		 * 再忽略**挂在射手身上的一切**：收在挂点上的主/副武器、刀、爆能器、手里的技能道具……
		 *
		 * 起点在眼位，这些东西全在射线起点附近几十厘米内。武器类自己那份 mesh 有状态机管着碰撞
		 *（背上/手里都是 NoCollision，只有掉在地上才开），但只要有**任何一个**没关干净、
		 * 或者以后新加一个挂在身上的东西忘了关，表现就是「子弹打在离脸特别近的地方打不出去」——
		 * 而且只有它正好落在射线上时才出现，复现极难。
		 * 这里一次性按"挂在射手身上的 actor 一律不吃自己这一枪"处理，跟 `AProjectile::BeginPlay`
		 * 对投射物做的是同一件事（那边是 MoveIgnoreActors，理由见那儿的注释）。
		 */
		if (const AActor* OwnerActor = GetOwner())
		{
			TArray<AActor*> AttachedToOwner;
			OwnerActor->GetAttachedActors(AttachedToOwner, /*bResetArray=*/true, /*bRecursivelyIncludeAttachedActors=*/true);
			Params.AddIgnoredActors(AttachedToOwner);
		}

		// 终点由射手算好（含散布）后一路传下来，这里不再二次随机 ——
		// 否则服务器/其他客户端各自随一次，「我这边看到打中、服务器那边算没打中」。
		FVector End = TraceStart + (HitTarget - TraceStart)*1.25;
		World->LineTraceSingleByChannel(
					OutHit,
					TraceStart,
					End,
					ECollisionChannel::ECC_Visibility,
					Params
				);

		/*
		 * ⚠ 临时排查桩（定位「子弹打在离脸特别近的地方打不出去」的元凶，确认后删掉）。
		 *
		 * 正常开枪不该有 1.5m 以内的首发命中 —— 出现就把"打到了谁"直接写进日志：
		 *   打到自己/自己身上的东西 → 忽略清单还漏了它（actor 名字直接给出是哪一件）
		 *   起点被包裹=1             → 眼位本身在那个碰撞体里面（命中点会落在起点上，即"贴脸"）
		 */
		if (OutHit.bBlockingHit)
		{
			const float NearHitDistance = FVector::Dist(TraceStart, OutHit.ImpactPoint);
			if (NearHitDistance < 150.f)
			{
				UE_LOG(LogTemp, Warning, TEXT("[近距弹着] 距离 %.1fcm | 命中 %s / 组件 %s | 起点被包裹=%d"),
					NearHitDistance, *GetNameSafe(OutHit.GetActor()), *GetNameSafe(OutHit.GetComponent()),
					OutHit.bStartPenetrating ? 1 : 0);
			}
		}
	}
}

FVector AHitScanWeapon::TraceEndWithScatter(const FVector& TraceStart, const FVector& HitTarget)
{
	FVector ToTargetNormalized = (HitTarget - TraceStart).GetSafeNormal();
	FVector SphereCenter = TraceStart + ToTargetNormalized * DistanceToSphere;
	FVector RandVec = UKismetMathLibrary::RandomUnitVector() * FMath::FRandRange(0.f,SphereRadius);
	FVector EndLoc = SphereCenter + RandVec;
	FVector ToEndLoc = EndLoc - TraceStart;

	/*
	DrawDebugSphere(GetWorld(),SphereCenter,SphereRadius,12, FColor::Red , true);
	DrawDebugLine(GetWorld(),TraceStart, FVector(TraceStart + ToEndLoc * TRACE_LENGTH / ToEndLoc.Size()),FColor::Cyan,true);
	*/
	
	return FVector(TraceStart + ToEndLoc * TRACE_LENGTH / ToEndLoc.Size());
	
}


