#include "SkeletalMesh.h"
#include "Render/Resource/Buffer.h"


void USkeletalMesh::InitResources(ID3D11Device* InDevice)
{
	TMeshData<FSkeletalVertex> SkeletalMeshData;
	SkeletalMeshData.Vertices.reserve(SkeletalMeshAsset->Vertices.size());

	SkeletalMeshData.Vertices = SkeletalMeshAsset->Vertices;
	SkeletalMeshAsset->Indices = SkeletalMeshAsset->Indices;

	SkeletalMeshAsset->RenderBuffer = std::make_unique<FMeshBuffer>();
	SkeletalMeshAsset->RenderBuffer->Create(InDevice, SkeletalMeshData);
}