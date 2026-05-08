#include "Mesh/SkeletalMesh.h"

#include "Object/ObjectFactory.h"
#include "Render/Resource/Buffer.h"

IMPLEMENT_CLASS(USkeletalMesh, UObject)

USkeletalMesh::~USkeletalMesh()
{
}

void USkeletalMesh::SetSkeletalMeshAsset(FSkeletalMesh* InMesh)
{
	SkeletalMeshAsset = InMesh;
}

FSkeletalMesh* USkeletalMesh::GetSkeletalMeshAsset() const
{
	return SkeletalMeshAsset;
}

void USkeletalMesh::InitResources(ID3D11Device* InDevice)
{
	if (!InDevice || !SkeletalMeshAsset)
	{
		return;
	}

	TMeshData<FSkeletalVertex> SkeletalMeshData;
	SkeletalMeshData.Vertices = SkeletalMeshAsset->Vertices;
	SkeletalMeshData.Indices = SkeletalMeshAsset->Indices;

	SkeletalMeshAsset->RenderBuffer = std::make_unique<FMeshBuffer>();
	SkeletalMeshAsset->RenderBuffer->Create(InDevice, SkeletalMeshData);
}
