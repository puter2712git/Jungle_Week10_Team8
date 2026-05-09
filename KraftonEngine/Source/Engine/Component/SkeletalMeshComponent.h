#pragma once

#include "MeshComponent.h"
#include "Core/PropertyTypes.h"
#include <memory>

class USkeletalMesh;
class FArchive;

class USkeletalMeshComponent : public UMeshComponent
{
public:
	DECLARE_CLASS(USkeletalMeshComponent, UMeshComponent);

	USkeletalMeshComponent() = default;
	~USkeletalMeshComponent() override = default;

	FMeshBuffer* GetMeshBuffer() const override;
	void UpdateWorldAABB() const override;

	USkeletalMesh* GetSkeletalMesh() const { return SkeletalMesh; }
	void SetSkeletalMesh(USkeletalMesh* InMesh);
	bool IsSkinningEnabled() const { return bEnableSkinning; }
	void SetSkinningEnabled(bool bInEnableSkinning);

	FPrimitiveSceneProxy* CreateSceneProxy() override;

	void Serialize(FArchive& Ar) override;
	void PostDuplicate() override;

	void GetEditableProperties(TArray<FPropertyDescriptor>& OutProps) override;
	void PostEditProperty(const char* PropertyName) override;

	const FString& GetSkeletalMeshPath() const { return SkeletalMeshPath; }

protected:
	void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction& ThisTickFunction) override;

private:
	void CacheLocalBounds();
	void InitSkinningResources();

	void UpdateCPUSkinning();
	void UpdateSkinnedVertexBuffer();

	USkeletalMesh* SkeletalMesh = nullptr;
	FString SkeletalMeshPath = "None";

	TArray<FVertexPNCTT> SkinnedVertices;
	TArray<FMatrix> BoneCurrentGlobalMatrices;
	TArray<FMatrix> SkinMatrices;

	std::unique_ptr<FMeshBuffer> SkinnedRenderBuffer;

	FVector CachedLocalCenter = { 0, 0, 0 };
	FVector CachedLocalExtent = { 0.5f, 0.5f, 0.5f };
	bool bHasValidBounds = false;
	bool bEnableSkinning = false;
};

