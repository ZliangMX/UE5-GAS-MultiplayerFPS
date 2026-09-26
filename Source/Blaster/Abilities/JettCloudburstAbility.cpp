#include "JettCloudburstAbility.h"

#include "AbilitySystemComponent.h"
#include "GameFramework/Controller.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Weapon/JettCloudburst.h"

UJettCloudburstAbility::UJettCloudburstAbility()
{
	bUseArmedActivation = false;

	// Valorant 逐风云是两发、每发 40 秒回。配 GE_Jett_CloudburstCooldown（Duration 40s）。
	MaxCharges = 2;
}

void UJettCloudburstAbility::ExecuteSkillAction()
{
	// LocalPredicted：客户端预测实例也会执行这里，Spawn 只能服务器做（否则双份云 + 双份烟）
	if (!CurrentActorInfo || !CurrentActorInfo->IsNetAuthority())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo());
	if (!Char || !CloudClass)
	{
		if (Char && !CloudClass)
		{
			// 没配 CloudClass —— 按 C 会静默无效。这条日志是唯一的线索。
			UE_LOG(LogTemp, Warning,
				TEXT("[逐风云] %s 没有填 CloudClass，技能不会生效（请在 GA_Jett_Cloudburst 的蓝图子类里填 BP_JettCloudburst）"),
				*GetName());
		}
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	// 飞行方向 = 角色视角（**含俯仰**，这是和曲线球最大的区别：往哪看就往哪飞）
	FRotator ViewRot = Char->GetActorRotation();
	if (const AController* C = Char->GetController())
	{
		ViewRot = C->GetControlRotation();
	}
	FVector Dir = ViewRot.Vector();
	if (!Dir.Normalize())
	{
		Dir = Char->GetActorForwardVector();
		ViewRot = Dir.Rotation();
	}

	// —— 出生点：优先骨骼 socket（真的贴着指尖），没有就按视角偏移估一个 ——
	// ⚠️ 名字里带 Socket 但**也可以是骨骼名**：本工程的第三人称网格
	//   (TP_Wushu_S0_Mesh) 上一个手部 socket 都没有，只有 WeaponSocket_Rifle/Pistol/Boltsniper
	//   三个挂在 R_WeaponPoint 上的武器 socket。想真的"从指尖出"就得填骨骼名（hand_r 之类），
	//   所以这里不能只判 DoesSocketExist —— 它只认 socket，骨骼名会一律返回 false，
	//   白白掉进下面的视角偏移分支，表现为"填了 hand_r 却完全没效果"。
	//   GetSocketLocation 本身是可以给骨骼名的（SkinnedMeshComponent.cpp:2889 那段
	//   `else { GetBoneIndex(...) }` 就是骨骼兜底），所以补一个 GetBoneIndex 判断即可。
	FVector SpawnLoc = FVector::ZeroVector;
	bool bUsedSocket = false;
	if (!SpawnSocketName.IsNone())
	{
		if (const USkeletalMeshComponent* Mesh = Char->GetMesh())
		{
			if (Mesh->DoesSocketExist(SpawnSocketName) || Mesh->GetBoneIndex(SpawnSocketName) != INDEX_NONE)
			{
				SpawnLoc = Mesh->GetSocketLocation(SpawnSocketName);
				bUsedSocket = true;
			}
			else
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[逐风云] 骨骼网格上既没有名为 %s 的 socket 也没有同名骨骼，改用视角偏移生成"
					     "（核对 TP_Wushu_S0_Mesh 的 socket/骨骼名）"),
					*SpawnSocketName.ToString());
			}
		}
	}
	if (!bUsedSocket)
	{
		// 眼睛位置 + 视角坐标系里的前/右/上偏移。
		// 用 GetPawnViewLocation 而不是 ActorLocation：瞄准时要贴着镜头，不是贴着脚底。
		const FRotationMatrix ViewMatrix(ViewRot);
		SpawnLoc = Char->GetPawnViewLocation()
			+ ViewMatrix.GetUnitAxis(EAxis::X) * SpawnForwardOffset
			+ ViewMatrix.GetUnitAxis(EAxis::Y) * SpawnRightOffset
			+ ViewMatrix.GetUnitAxis(EAxis::Z) * SpawnUpOffset;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Char;
	SpawnParams.Instigator = Char;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AJettCloudburst* Cloud = GetWorld()->SpawnActor<AJettCloudburst>(
		CloudClass, SpawnLoc, Dir.Rotation(), SpawnParams);
	if (Cloud)
	{
		// 方向在生成之后再给：出生时要先有一个合法的朝向，不然第一帧的碰撞扫描方向是乱的
		Cloud->InitCloud(Dir);

		// 记下来给保险丝用（这一趟最多按多久 = 这朵云活多久）。见 OnEmptyHandStarted。
		ActiveCloud = Cloud;
	}

	/*
	 * —— 进入"按住控云"状态 ——
	 *
	 * ★ 必须和放云**同一拍**（而不是等松手/客户端再来一条 RPC）：云那一帧起就开始
	 *   每帧问角色"还按着吗"（AJettCloudburst::SteerTowardsCrosshair），标志晚一帧置
	 *   就少一帧的控制权；而"服务器上到底按没按着"只有服务器说了算。
	 *
	 * 和服务端放冷却/生成云同理：客户端预测实例在函数开头就 EndAbility 走人了，到不了这里。
	 */
	Char->BeginCloudburstHold();

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UJettCloudburstAbility::OnEmptyHandStarted()
{
	/*
	 * 空手那段的保险丝：按"这朵云最多活多久"重设，而不是按动画长度（见头文件里为什么）。
	 *
	 * 云没了（放出去失败 / 已经被销毁 / 还没赋值）→ 传 0：
	 * ABlasterCharacter::SetEmptyHandFuseDuration 会把它夹成"下一拍就收尾"，
	 * 降级成"没有 Cloudburst 这段空手"，不会把人卡在不可打断的状态里。
	 */
	ABlasterCharacter* Char = Cast<ABlasterCharacter>(GetAvatarActorFromActorInfo());
	if (Char == nullptr) return;

	const float FuseDuration = ActiveCloud.IsValid() ? ActiveCloud->GetMaxHoldDuration() : 0.f;
	Char->SetEmptyHandFuseDuration(FuseDuration);
}
