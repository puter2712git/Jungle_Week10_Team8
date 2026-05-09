#pragma once

#include "Render/Proxy/PrimitiveSceneProxy.h"
#include "Math/Matrix.h"

class USkeletalMeshComponent;

class FSkeletalMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	FSkeletalMeshSceneProxy(USkeletalMeshComponent* InComponent);

	void UpdateMaterial() override;
	void UpdateMesh() override;

	const TArray<FMatrix>& GetSkinningMatrices() const;
	void UpdateSkinningPalette();
	uint32 GetBoneCount() const;

private:
	USkeletalMeshComponent* GetSkeletalMeshComponent() const;
	void RebuildSectionDraws();

	TArray<FMatrix> SkinningMatrices;
};
