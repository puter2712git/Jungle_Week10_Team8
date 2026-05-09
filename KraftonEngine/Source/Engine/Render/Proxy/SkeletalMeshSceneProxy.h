#pragma once

#include "Render/Proxy/PrimitiveSceneProxy.h"

class USkeletalMeshComponent;

class FSkeletalMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	FSkeletalMeshSceneProxy(USkeletalMeshComponent* InComponent);

	void UpdateTransform() override;
	void UpdateMaterial() override;
	void UpdateMesh() override;

private:
	USkeletalMeshComponent* GetSkeletalMeshComponent() const;
	void RebuildSectionDraws();
	void UpdateSectionObjectConstants();

};

