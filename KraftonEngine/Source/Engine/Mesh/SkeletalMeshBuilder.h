#pragma once

#include "Core/CoreTypes.h"

struct FImportedSkeletalMesh;
struct FSkeletalMesh;

struct FSkeletalMeshBuilder
{
	static bool BuildFromImported(
		const FImportedSkeletalMesh& Imported,
		FSkeletalMesh& OutMesh
	);
};
