#pragma once
#include "MeshComponent.h"

class UStaticMesh;

class USkinnedMeshComponent : public UMeshComponent
{
public:
	DECLARE_CLASS(USkinnedMeshComponent, UMeshComponent)

	UStaticMesh* StaticMesh;
};

