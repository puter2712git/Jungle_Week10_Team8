#pragma once

#include "Core/CoreTypes.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Mesh/StaticMeshAsset.h"
#include "Render/Resource/Buffer.h"
#include "Serialization/Archive.h"

#include <algorithm>
#include <memory>

// Bone 정보가 있는 FBX Skeletal Mesh 정점입니다.
// BoneIndices/BoneWeights는 아직 셰이더 입력에 연결하지 않고 CPU RawData/Asset에 보존합니다.
struct FSkeletalMeshVertex
{
	FVector		Position;
	FVector		Normal;
	FVector2	UV;
	FVector4	Tangent;
	uint32		MaterialIndex = 0;

	uint32		BoneIndices[4] = {};
	float		BoneWeights[4] = {};
};

inline FArchive& SerializeSkeletalMatrix(FArchive& Ar, FMatrix& Matrix)
{
	for (int32 i = 0; i < 16; ++i)
	{
		Ar << Matrix.Data[i];
	}
	return Ar;
}

struct FSkeletalBoneInfo
{
	FString Name;
	int32 ParentIndex = -1;

	// Bind matrices are stored in the same space as SourceVertices, currently ImportRootLocal.
	// TODO: Animation pose sampling 이후 CurrentBoneTransform 갱신 예정
	FMatrix MeshBindGlobalTransform = FMatrix::Identity;
	FMatrix LocalBindTransform = FMatrix::Identity;
	FMatrix GlobalBindTransform = FMatrix::Identity;
	FMatrix InverseBindTransform = FMatrix::Identity;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalBoneInfo& Bone)
	{
		Ar << Bone.Name;
		Ar << Bone.ParentIndex;
		SerializeSkeletalMatrix(Ar, Bone.MeshBindGlobalTransform);
		SerializeSkeletalMatrix(Ar, Bone.LocalBindTransform);
		SerializeSkeletalMatrix(Ar, Bone.GlobalBindTransform);
		SerializeSkeletalMatrix(Ar, Bone.InverseBindTransform);
		return Ar;
	}
};

struct FSkeletalMeshSection
{
	int32 MaterialIndex = -1;
	FString MaterialSlotName = "None";
	uint32 FirstIndex = 0;
	uint32 NumTriangles = 0;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalMeshSection& Section)
	{
		Ar << Section.MaterialIndex;
		Ar << Section.MaterialSlotName;
		Ar << Section.FirstIndex;
		Ar << Section.NumTriangles;
		return Ar;
	}
};

struct FSkeletalMeshLODData
{
	TArray<FSkeletalMeshVertex> SourceVertices;
	TArray<int32> Indices;
	TArray<FSkeletalMeshSection> Sections;
};

struct FSkeletalMeshRawData
{
	TArray<FSkeletalMeshVertex> SourceVertices;
	TArray<int32> Indices;
	TArray<FSkeletalBoneInfo> Bones;
	TArray<FSkeletalMeshSection> Sections;

	FVector BoundsCenter = FVector(0, 0, 0);
	FVector BoundsExtent = FVector(0, 0, 0);
	bool bBoundsValid = false;

	void CacheBounds()
	{
		bBoundsValid = false;
		if (SourceVertices.empty()) return;

		FVector LocalMin = SourceVertices[0].Position;
		FVector LocalMax = SourceVertices[0].Position;
		for (const FSkeletalMeshVertex& V : SourceVertices)
		{
			LocalMin.X = (std::min)(LocalMin.X, V.Position.X);
			LocalMin.Y = (std::min)(LocalMin.Y, V.Position.Y);
			LocalMin.Z = (std::min)(LocalMin.Z, V.Position.Z);
			LocalMax.X = (std::max)(LocalMax.X, V.Position.X);
			LocalMax.Y = (std::max)(LocalMax.Y, V.Position.Y);
			LocalMax.Z = (std::max)(LocalMax.Z, V.Position.Z);
		}

		BoundsCenter = (LocalMin + LocalMax) * 0.5f;
		BoundsExtent = (LocalMax - LocalMin) * 0.5f;
		bBoundsValid = true;
	}
};

struct FSkeletalMeshAsset
{
	FString PathFileName;
	// Bind pose 기준 원본 정점입니다. CPU Skinning에서는 읽기 전용으로 사용해야 합니다.
	TArray<FSkeletalMeshVertex> SourceVertices;
	TArray<uint32> Indices;
	TArray<FSkeletalBoneInfo> Bones;
	TArray<FSkeletalMeshSection> Sections;

	FVector BoundsCenter = FVector(0, 0, 0);
	FVector BoundsExtent = FVector(0, 0, 0);
	bool bBoundsValid = false;

	void CacheBounds()
	{
		bBoundsValid = false;
		if (SourceVertices.empty()) return;

		FVector LocalMin = SourceVertices[0].Position;
		FVector LocalMax = SourceVertices[0].Position;
		for (const FSkeletalMeshVertex& V : SourceVertices)
		{
			LocalMin.X = (std::min)(LocalMin.X, V.Position.X);
			LocalMin.Y = (std::min)(LocalMin.Y, V.Position.Y);
			LocalMin.Z = (std::min)(LocalMin.Z, V.Position.Z);
			LocalMax.X = (std::max)(LocalMax.X, V.Position.X);
			LocalMax.Y = (std::max)(LocalMax.Y, V.Position.Y);
			LocalMax.Z = (std::max)(LocalMax.Z, V.Position.Z);
		}

		BoundsCenter = (LocalMin + LocalMax) * 0.5f;
		BoundsExtent = (LocalMax - LocalMin) * 0.5f;
		bBoundsValid = true;
	}

	void Serialize(FArchive& Ar)
	{
		Ar << PathFileName;
		Ar << SourceVertices;
		Ar << Indices;
		Ar << Bones;
		Ar << Sections;
		Ar << BoundsCenter;
		Ar << BoundsExtent;
		Ar << bBoundsValid;
	}
};
