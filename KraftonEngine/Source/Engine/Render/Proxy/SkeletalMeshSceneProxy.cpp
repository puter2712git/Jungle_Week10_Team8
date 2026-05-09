#include "Render/Proxy/SkeletalMeshSceneProxy.h"

#include "Component/SkeletalMeshComponent.h"
#include "Materials/Material.h"
#include "Mesh/SkeletalMesh.h"

#include <algorithm>

namespace
{
	bool SectionMaterialLess(const FMeshSectionDraw& A, const FMeshSectionDraw& B)
	{
		const uintptr_t AMat = reinterpret_cast<uintptr_t>(A.Material);
		const uintptr_t BMat = reinterpret_cast<uintptr_t>(B.Material);
		if (AMat != BMat)
			return AMat < BMat;

		return A.FirstIndex < B.FirstIndex;
	}

	void SortSectionDrawsByMaterial(TArray<FMeshSectionDraw>& Draws)
	{
		if (Draws.size() > 1)
		{
			std::sort(Draws.begin(), Draws.end(), SectionMaterialLess);
		}
	}
}

FSkeletalMeshSceneProxy::FSkeletalMeshSceneProxy(USkeletalMeshComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
}

USkeletalMeshComponent* FSkeletalMeshSceneProxy::GetSkeletalMeshComponent() const
{
	return static_cast<USkeletalMeshComponent*>(GetOwner());
}

void FSkeletalMeshSceneProxy::UpdateMaterial()
{
	RebuildSectionDraws();
}

void FSkeletalMeshSceneProxy::UpdateMesh()
{
	MeshBuffer = GetOwner()->GetMeshBuffer();
	RebuildSectionDraws();
}

const TArray<FMatrix>& FSkeletalMeshSceneProxy::GetSkinningMatrices() const
{
	return SkinningMatrices;
}

void FSkeletalMeshSceneProxy::UpdateSkinningPalette()
{
	// TODO: CPU/GPU Skinning 구현 예정
	// TODO: Bone palette 계산 예정
	// TODO: Animation pose sampling 이후 CurrentBoneTransform 갱신 예정
}

uint32 FSkeletalMeshSceneProxy::GetBoneCount() const
{
	USkeletalMeshComponent* SMC = GetSkeletalMeshComponent();
	if (!SMC || !SMC->GetSkeletalMesh() || !SMC->GetSkeletalMesh()->GetSkeletalMeshAsset())
	{
		return 0;
	}

	return static_cast<uint32>(SMC->GetSkeletalMesh()->GetSkeletalMeshAsset()->Bones.size());
}

void FSkeletalMeshSceneProxy::RebuildSectionDraws()
{
	USkeletalMeshComponent* SMC = GetSkeletalMeshComponent();
	USkeletalMesh* Mesh = SMC ? SMC->GetSkeletalMesh() : nullptr;
	FSkeletalMeshAsset* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;

	if (!Mesh || !Asset)
	{
		MeshBuffer = nullptr;
		SectionDraws.clear();
		SkinningMatrices.clear();
		return;
	}

	MeshBuffer = SMC->GetMeshBuffer();
	SkinningMatrices = SMC->GetSkinningMatrices();
	SectionDraws.clear();
	SectionDraws.reserve(Asset->Sections.size());

	const TArray<FStaticMaterial>& Slots = Mesh->GetSkeletalMaterials();
	const TArray<UMaterial*>& Overrides = SMC->GetOverrideMaterials();

	for (const FSkeletalMeshSection& Section : Asset->Sections)
	{
		FMeshSectionDraw Draw;
		Draw.FirstIndex = Section.FirstIndex;
		Draw.IndexCount = Section.NumTriangles * 3;

		int32 MaterialSlotIndex = Section.MaterialIndex;
		if (MaterialSlotIndex >= 0 && MaterialSlotIndex < static_cast<int32>(Slots.size()))
		{
			if (MaterialSlotIndex < static_cast<int32>(Overrides.size()) && Overrides[MaterialSlotIndex])
				Draw.Material = Overrides[MaterialSlotIndex];
			else if (Slots[MaterialSlotIndex].MaterialInterface)
				Draw.Material = Slots[MaterialSlotIndex].MaterialInterface;
		}

		SectionDraws.push_back(Draw);
	}

	SortSectionDrawsByMaterial(SectionDraws);
}
