#pragma once

#include "Object/Object.h"
#include "Mesh/SkeletalMeshAsset.h"

class FArchive;
struct ID3D11Device;

class USkeletalMesh : public UObject
{
public:
	DECLARE_CLASS(USkeletalMesh, UObject)

	USkeletalMesh() = default;
	~USkeletalMesh() override = default;

	void Serialize(FArchive& Ar) override;
	void PostDuplicate() override;

	const FString& GetAssetPathFileName() const;
	void SetSkeletalMeshAsset(FSkeletalMeshAsset* InMesh);
	FSkeletalMeshAsset* GetSkeletalMeshAsset() const;

	void SetSkeletalMaterials(TArray<FStaticMaterial>&& InMaterials);
	const TArray<FStaticMaterial>& GetSkeletalMaterials() const;

	void InitResources(ID3D11Device* InDevice);
	void ReleaseResources();

private:
	FSkeletalMeshAsset* SkeletalMeshAsset = nullptr;
	TArray<FStaticMaterial> SkeletalMaterials;
};
