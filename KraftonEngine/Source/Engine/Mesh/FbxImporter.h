#pragma once

#include "Core/CoreTypes.h"

struct FSkeletalMesh;

class FFbxImporter
{
public:
	static bool Import(const FString& FilePath, FSkeletalMesh& OutMesh);
};
