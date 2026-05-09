#include "SkeletalMeshSceneProxy.h"

#include "Component/SkeletalMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialManager.h"
#include "Mesh/SkeletalMesh.h"
#include "Mesh/SkeletalMeshAsset.h"

FSkeletalMeshSceneProxy::FSkeletalMeshSceneProxy(USkeletalMeshComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
}

USkeletalMeshComponent* FSkeletalMeshSceneProxy::GetSkeletalMeshComponent() const
{
	return static_cast<USkeletalMeshComponent*>(GetOwner());
}

void FSkeletalMeshSceneProxy::UpdateMesh()
{
	MeshBuffer = GetOwner()->GetMeshBuffer();
	RebuildSectionDraws();
}

void FSkeletalMeshSceneProxy::UpdateMaterial()
{
	RebuildSectionDraws();
}

void FSkeletalMeshSceneProxy::RebuildSectionDraws()
{
	SectionDraws.clear();

	USkeletalMeshComponent* SMC = GetSkeletalMeshComponent();
	USkeletalMesh* Mesh = SMC ? SMC->GetSkeletalMesh() : nullptr;
	FSkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!MeshBuffer || !Asset || Asset->Indices.empty())
	{
		return;
	}

	UMaterial* Material = FMaterialManager::Get().GetOrCreateMaterial("Asset/Materials/None.mat");
	if (!Material)
	{
		return;
	}

	if (!Asset->MeshRanges.empty())
	{
		for (const FSkeletalMeshRange& Range : Asset->MeshRanges)
		{
			FMeshSectionDraw Draw;
			Draw.Material = Material;
			Draw.FirstIndex = Range.FirstIndex;
			Draw.IndexCount = Range.IndexCount;
			SectionDraws.push_back(Draw);
		}
	}

	if (SectionDraws.empty())
	{
		const uint32 IndexCount = MeshBuffer->GetIndexBuffer().GetIndexCount();
		if (IndexCount > 0)
		{
			SectionDraws.push_back({ Material, 0, IndexCount });
		}
	}
}
