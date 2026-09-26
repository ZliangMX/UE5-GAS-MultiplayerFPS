#include "ZoneCornerMarker.h"
#include "ZoneCornerMarkerComponent.h"
#include "UObject/ConstructorHelpers.h"

AZoneCornerMarker::AZoneCornerMarker()
{
	PrimaryActorTick.bCanEverTick = false;

	MarkerMesh = CreateDefaultSubobject<UZoneCornerMarkerComponent>(TEXT("MarkerMesh"));
	SetRootComponent(MarkerMesh);

	// 小号球体：编辑器里可见（用于摆放/选中），运行时隐藏，无碰撞
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		MarkerMesh->SetStaticMesh(SphereMesh.Object);
	}
	MarkerMesh->SetWorldScale3D(FVector(0.5f));
	MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MarkerMesh->SetHiddenInGame(true);
}
