#include "Mesh/SkeletalMesh.h"

#include "Core/Log.h"
#include "Object/ObjectFactory.h"
#include "Serialization/Archive.h"

IMPLEMENT_CLASS(USkeletalMesh, UObject)

static const FString EmptySkeletalMeshPath;

void USkeletalMesh::Serialize(FArchive& Ar)
{
	UObject::Serialize(Ar);

	if (Ar.IsLoading() && !SkeletalMeshAsset)
	{
		SkeletalMeshAsset = new FSkeletalMeshAsset();
	}

	FSkeletalMeshAsset EmptyAsset;
	FSkeletalMeshAsset* SerializableAsset = SkeletalMeshAsset ? SkeletalMeshAsset : &EmptyAsset;
	SerializableAsset->Serialize(Ar);

	Ar << SkeletalMaterials;

	if (Ar.IsLoading() && SkeletalMeshAsset)
	{
		if (!SkeletalMeshAsset->bBoundsValid && !SkeletalMeshAsset->SourceVertices.empty())
		{
			SkeletalMeshAsset->CacheBounds();
		}

		SetSkeletalMeshAsset(SkeletalMeshAsset);
	}
}

void USkeletalMesh::PostDuplicate()
{
	UObject::PostDuplicate();

	ReleaseResources();

	const int32 SourceVertexCount = SkeletalMeshAsset
		? static_cast<int32>(SkeletalMeshAsset->SourceVertices.size())
		: 0;
	const int32 BoneCount = SkeletalMeshAsset
		? static_cast<int32>(SkeletalMeshAsset->Bones.size())
		: 0;

	UE_LOG("[PIE SKM] DuplicatedMesh=%p SourceVertices=%d Bones=%d",
		this,
		SourceVertexCount,
		BoneCount);
}

const FString& USkeletalMesh::GetAssetPathFileName() const
{
	if (SkeletalMeshAsset)
	{
		return SkeletalMeshAsset->PathFileName;
	}
	return EmptySkeletalMeshPath;
}

void USkeletalMesh::SetSkeletalMeshAsset(FSkeletalMeshAsset* InMesh)
{
	SkeletalMeshAsset = InMesh;

	if (SkeletalMeshAsset)
	{
		for (FSkeletalMeshSection& Section : SkeletalMeshAsset->Sections)
		{
			Section.MaterialIndex = -1;
			for (int32 i = 0; i < static_cast<int32>(SkeletalMaterials.size()); ++i)
			{
				if (SkeletalMaterials[i].MaterialSlotName == Section.MaterialSlotName)
				{
					Section.MaterialIndex = i;
					break;
				}
			}
		}
	}
}

FSkeletalMeshAsset* USkeletalMesh::GetSkeletalMeshAsset() const
{
	return SkeletalMeshAsset;
}

void USkeletalMesh::SetSkeletalMaterials(TArray<FStaticMaterial>&& InMaterials)
{
	SkeletalMaterials = std::move(InMaterials);
}

const TArray<FStaticMaterial>& USkeletalMesh::GetSkeletalMaterials() const
{
	return SkeletalMaterials;
}

void USkeletalMesh::InitResources(ID3D11Device* InDevice)
{
	(void)InDevice;
	// SkeletalMesh의 vertex buffer는 component별 pose 결과가 들어가므로
	// USkeletalMeshComponent가 SkinnedVertices용 dynamic vertex buffer를 생성한다.
}

void USkeletalMesh::ReleaseResources()
{
	// Component-owned dynamic render buffers are released by USkeletalMeshComponent.
}
