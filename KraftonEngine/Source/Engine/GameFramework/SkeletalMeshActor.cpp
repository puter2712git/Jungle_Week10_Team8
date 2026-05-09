#include "SkeletalMeshActor.h"
#include "Component/SkeletalMeshComponent.h"
#include "Component/StaticMeshComponent.h"
#include "Runtime/Engine.h"
#include "Mesh/FbxImporter.h"
#include <Object/ObjectFactory.h>

IMPLEMENT_CLASS(ASkeletalMeshActor, AActor);

void ASkeletalMeshActor::BeginPlay()
{
	Super::BeginPlay();
}

void ASkeletalMeshActor::InitDefaultComponents(const FString& UMeshFileName /*= "Meshes/lowpolyboy/SimpleMan.fbx"*/)
{
	SkeletalMeshComponent = AddComponent<UStaticMeshComponent>();
	SetRootComponent(SkeletalMeshComponent);

	FFbxImporter Importer;

	ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
	UStaticMesh* Asset = Importer.ImportAsStaticMesh(UMeshFileName, Device);

	SkeletalMeshComponent->SetStaticMesh(Asset);
}
