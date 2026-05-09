#include "SkeletalMeshActor.h"
#include "Component/SkeletalMeshComponent.h"
#include "Runtime/Engine.h"
#include "Mesh/FbxImporter.h"
#include "Mesh/SkeletalMesh.h"
#include <Object/ObjectFactory.h>

IMPLEMENT_CLASS(ASkeletalMeshActor, AActor);

void ASkeletalMeshActor::BeginPlay()
{
	Super::BeginPlay();
}

void ASkeletalMeshActor::InitDefaultComponents(const FString& UMeshFileName /*= "Meshes/lowpolyboy/SimpleMan.fbx"*/)
{
	SkeletalMeshComponent = AddComponent<USkeletalMeshComponent>();
	SetRootComponent(SkeletalMeshComponent);

	FFbxImporter Importer;

	ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
	USkeletalMesh* Asset = Importer.ImportAsSkeletalMesh(UMeshFileName, Device);

	SkeletalMeshComponent->SetSkeletalMesh(Asset);
}
