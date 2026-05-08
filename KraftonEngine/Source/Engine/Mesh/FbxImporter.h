#pragma once

#include "Core/CoreTypes.h"
struct FImportedSkeletalMesh;

class FFbxImporter
{
public:
	static bool Import(const FString& FilePath, FImportedSkeletalMesh& OutMesh);
};
