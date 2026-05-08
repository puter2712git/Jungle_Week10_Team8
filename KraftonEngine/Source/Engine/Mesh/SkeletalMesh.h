#pragma once

#include "Object/Object.h"
#include "Object/ObjectFactory.h"
#include "Mesh/SkeletalMeshAsset.h"

class USkeletalMesh : public UObject
{
public:
	DECLARE_CLASS(USkeletalMesh, UObject)

	USkeletalMesh() = default;
	~USkeletalMesh() override;

	void SetSkeletalMeshAsset(FSkeletalMesh& InMesh);
	FSkeletalMesh GetSkeletalMeshAsset() const;

	void InitResources(ID3D11Device* InDevice);

private:
	FSkeletalMesh* SkeletalMeshAsset = nullptr;
};

