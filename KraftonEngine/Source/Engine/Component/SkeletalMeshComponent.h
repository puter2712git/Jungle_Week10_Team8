#pragma once

#include "MeshComponent.h"
#include "Core/PropertyTypes.h"

class USkeletalMesh;
class FArchive;

class USkeletalMeshComponent : public UMeshComponent
{
public:
	DECLARE_CLASS(USkeletalMeshComponent, UMeshComponent);

	USkeletalMeshComponent() = default;
	~USkeletalMeshComponent() override = default;

	FMeshBuffer* GetMeshBuffer() const override;
	void UpdateWorldMatrix() const override;
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

private:
	void CacheLocalBounds();

	USkeletalMesh* SkeletalMesh = nullptr;
	FString SkeletalMeshPath = "None";

	FVector CachedLocalCenter = { 0, 0, 0 };
	FVector CachedLocalExtent = { 0.5f, 0.5f, 0.5f };
	bool bHasValidBounds = false;
	bool bEnableSkinning = false;
};

