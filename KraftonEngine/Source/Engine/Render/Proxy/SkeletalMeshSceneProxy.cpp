#include "SkeletalMeshSceneProxy.h"

#include "Component/SkeletalMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialManager.h"
#include "Mesh/SkeletalMesh.h"
#include "Mesh/SkeletalMeshAsset.h"

#include <algorithm>

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

void FSkeletalMeshSceneProxy::UpdateTransform()
{
	FPrimitiveSceneProxy::UpdateTransform();
	UpdateSectionObjectConstants();
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

	UpdateSectionObjectConstants();
}

void FSkeletalMeshSceneProxy::UpdateSectionObjectConstants()
{
	USkeletalMeshComponent* SMC = GetSkeletalMeshComponent();
	USkeletalMesh* Mesh = SMC ? SMC->GetSkeletalMesh() : nullptr;
	FSkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!SMC || !Asset || SMC->IsSkinningEnabled() || Asset->MeshRanges.empty())
	{
		for (FMeshSectionDraw& Draw : SectionDraws)
		{
			Draw.bOverridePerObjectConstants = false;
		}
		return;
	}

	const FMatrix& ComponentWorld = SMC->GetWorldMatrix();
	const uint32 Count = (std::min)(static_cast<uint32>(SectionDraws.size()), static_cast<uint32>(Asset->MeshRanges.size()));
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		FMeshSectionDraw& Draw = SectionDraws[Index];
		const FSkeletalMeshRange& Range = Asset->MeshRanges[Index];
		if (!Range.bHasMeshScene)
		{
			Draw.bOverridePerObjectConstants = false;
			continue;
		}

		Draw.bOverridePerObjectConstants = true;
		Draw.PerObjectConstants = FPerObjectConstants::FromWorldMatrix(Range.MeshSceneGlobal * ComponentWorld);
	}

	for (uint32 Index = Count; Index < static_cast<uint32>(SectionDraws.size()); ++Index)
	{
		SectionDraws[Index].bOverridePerObjectConstants = false;
	}
}
