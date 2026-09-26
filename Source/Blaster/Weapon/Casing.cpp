
#include "Casing.h"

#include "Kismet/GameplayStatics.h"
#include "Sound/SoundCue.h"
// ECC_SkeletalMesh 是**工程自定义宏**（= ECC_GameTraceChannel1），定义在 Blaster.h 里。
// ⚠ 不能写成 ECollisionChannel::ECC_SkeletalMesh —— 宏展开会变成
//   ECollisionChannel::ECollisionChannel::ECC_GameTraceChannel1，直接编不过。
#include "Blaster/Blaster.h"

ACasing::ACasing()
{

	PrimaryActorTick.bCanEverTick = false;

	CasingMesh=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CasingMesh"));
	SetRootComponent(CasingMesh);

	/*
	 *弹壳是**纯表现**：它只负责"在地上弹一下、响一声"，任何一条视线/射线都不该被它挡住。
	 *
	 *⚠ 弹壳的默认碰撞档是 BlockAll（什么都没设就是这个）—— 所有通道全 Block，其中包括
	 *   ECC_Visibility，而**射线武器的命中判定、准星追踪、近战/回溯全都是 ECC_Visibility**。
	 *   弹壳又正好是从枪身（本人看到的是第一人称武器副本）上飞出来的，出生点就在眼前几十厘米，
	 *   卡在自己身体/墙角上不动的那一颗，接下来每一发都会被它吃掉：
	 *   表现就是「子弹打在离脸特别近的地方打不出去」——而且只有它正好在射线上时才出现，时有时无。
	 *   相机通道原来已经单独忽略了（同一个理由），这里把可见性一起关掉。
	 *
	 *Pawn / SkeletalMesh 也忽略：弹壳出生点在身体附近，撞上自己（或别人的）胶囊和骨骼网格时
	 *   会被挤住、被推着走 —— 那一颗就永远飘在脸前。让它穿过人体、落到真正的地面（WorldStatic）上。
	 *   落地和"叮"一声靠 WorldStatic/WorldDynamic，不受影响。
	 */
	CasingMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Ignore);
	CasingMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
	CasingMesh->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Ignore);
	CasingMesh->SetCollisionResponseToChannel(ECC_SkeletalMesh, ECollisionResponse::ECR_Ignore);

	CasingMesh->SetSimulatePhysics(true);
	CasingMesh->SetEnableGravity(true);
	CasingMesh->SetNotifyRigidBodyCollision(true);
	ShellEjectionImpulse = 10.f;

}


void ACasing::BeginPlay()
{
	Super::BeginPlay();

	CasingMesh->OnComponentHit.AddDynamic(this, &ACasing::OnHit);
	CasingMesh->AddImpulse(GetActorForwardVector()*ShellEjectionImpulse);

	/*
	 *兜底寿命。
	 *
	 *⚠ 原来的销毁路径只有 OnHit 一条，而它还挂在 ShellSound 上（没配音效的弹壳 = 永远不消失）；
	 *   一颗没碰到任何东西的弹壳（悬在墙角、卡在自己身上、掉出地图）就真的留在场上不走了。
	 *   连着打几百发就是几百个物理 actor 堆在玩家脚边/脚前 —— 它们全是 BlockAll，既费性能又挡枪。
	 *   给它一个上限：到点连自己一起收掉，漏网的也不会留。
	 */
	SetLifeSpan(15.f);
}

void ACasing::OnHit(UPrimitiveComponent* HitComp, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	if (ShellSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, ShellSound, GetActorLocation());
	}

	// ⚠ 销毁**不能**挂在 ShellSound 上：原来写的是 `if (ShellSound) { 播放; Destroy(); }`，
	// 于是"没配音效"的弹壳（BP 里留空是常态）落地后不销毁 —— 一场下来攒满地图。
	// 音效是可选项，寿命不是。
	Destroy();
}

