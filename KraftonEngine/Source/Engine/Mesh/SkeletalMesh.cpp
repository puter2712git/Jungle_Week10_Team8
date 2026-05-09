#include "Mesh/SkeletalMesh.h"

#include "Object/ObjectFactory.h"
#include "Render/Resource/Buffer.h"
#include "Render/Types/VertexTypes.h"

IMPLEMENT_CLASS(USkeletalMesh, UObject)

static const FString EmptyPath;

USkeletalMesh::~USkeletalMesh()
{
}

const FString& USkeletalMesh::GetAssetPathFileName() const
{
	if (SkeletalMeshAsset)
	{
		return SkeletalMeshAsset->PathFileName;
	}
	return EmptyPath;
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

	TMeshData<FVertexPNCTT> SkeletalMeshData;
	SkeletalMeshData.Vertices.reserve(SkeletalMeshAsset->Vertices.size());

	for (const FSkeletalVertex& RawVertex : SkeletalMeshAsset->Vertices)
	{
		FVertexPNCTT RenderVertex;
		RenderVertex.Position = RawVertex.Position;
		RenderVertex.Normal = RawVertex.Normal;
		RenderVertex.Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
		RenderVertex.UV = RawVertex.UV;
		RenderVertex.Tangent = RawVertex.Tangent;
		SkeletalMeshData.Vertices.push_back(RenderVertex);
	}
	SkeletalMeshData.Indices = SkeletalMeshAsset->Indices;

	SkeletalMeshAsset->RenderBuffer = std::make_unique<FMeshBuffer>();
	SkeletalMeshAsset->RenderBuffer->Create(InDevice, SkeletalMeshData);
}
