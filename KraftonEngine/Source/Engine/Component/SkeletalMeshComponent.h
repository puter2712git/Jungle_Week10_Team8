#pragma once

#include "Component/SkinnedMeshComponent.h"
#include "Math/Matrix.h"
#include "Render/Resource/Buffer.h"

#include <memory>

class FPrimitiveSceneProxy;
class FArchive;
class UMaterial;
class USkeletalMesh;

class USkeletalMeshComponent : public USkinnedMeshComponent
{
public:
	DECLARE_CLASS(USkeletalMeshComponent, USkinnedMeshComponent)

	USkeletalMeshComponent() = default;
	~USkeletalMeshComponent() override = default;

	void Serialize(FArchive& Ar) override;
	void PostDuplicate() override;

	FPrimitiveSceneProxy* CreateSceneProxy() override;

	void SetSkeletalMesh(USkeletalMesh* InMesh);
	USkeletalMesh* GetSkeletalMesh() const;

	FMeshBuffer* GetMeshBuffer() const override;
	FMeshDataView GetMeshDataView() const override;
	void UpdateWorldAABB() const override;

	void SetMaterial(int32 ElementIndex, UMaterial* InMaterial);
	UMaterial* GetMaterial(int32 ElementIndex) const;
	const TArray<UMaterial*>& GetOverrideMaterials() const { return OverrideMaterials; }

	const FString& GetSkeletalMeshPath() const { return SkeletalMeshPath; }
	const TArray<FVertexPNCTT>& GetSkinnedVertices() const { return SkinnedVertices; }
	const TArray<FMatrix>& GetCurrentBoneLocalTransforms() const { return CurrentBoneLocalTransforms; }
	const TArray<FMatrix>& GetCurrentBoneGlobalTransforms() const { return CurrentBoneGlobalTransforms; }
	const TArray<FMatrix>& GetSkinningMatrices() const { return SkinningMatrices; }

	void DebugValidateBindPose() const;
	void UpdatePose();
	void UpdateSkinningMatrices();
	void UpdateCPUSkinning();
	bool UploadSkinnedVerticesToGPU();
	void MarkPoseDirty();

protected:
	void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction& ThisTickFunction) override;

private:
	void CacheLocalBounds();
	void InitializeSkinningState();
	void ReinitializeSkeletalMeshRuntimeState();
	void RebuildDynamicRenderBuffer();
	USkeletalMesh* LoadSkeletalMeshFromPath() const;
	void LogSkeletalMeshDebugInfo(bool bDynamicBufferCreated) const;

	USkeletalMesh* SkeletalMesh = nullptr;
	FString SkeletalMeshPath = "None";
	TArray<UMaterial*> OverrideMaterials;
	TArray<FVertexPNCTT> SkinnedVertices;
	TArray<FMatrix> CurrentBoneLocalTransforms;
	TArray<FMatrix> CurrentBoneGlobalTransforms;
	TArray<FMatrix> SkinningMatrices;
	std::unique_ptr<FMeshBuffer> DynamicRenderBuffer;

	FVector CachedLocalCenter = { 0, 0, 0 };
	FVector CachedLocalExtent = { 0.5f, 0.5f, 0.5f };
	bool bHasValidBounds = false;
	bool bPoseDirty = false;
	bool bSkinningDirty = false;
	bool bRenderDataDirty = false;

	// Test 용도
private:
	float DebugSkinningTime = 0.0f;
	int32 DebugAnimatedBoneIndex = -1;

	void ApplyDebugBoneAnimation(float DeltaTime);
	int32 FindBoneIndexByNameContains(const FString& Keyword) const;
};
