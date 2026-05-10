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
	int32 ChildCount = -1;

	// Bone Bind 행렬은 FBX Scene Global 기준으로 저장한다.
	// 정점은 Mesh Node local ControlPoint로 남겨두고,
	// Skinning 단계에서 Mesh Range의 행렬과 Bone 행렬을 같은 Scene Global 기준에서 만난다.
	FMatrix MeshBindGlobalTransform = FMatrix::Identity;
	FMatrix LocalBindTransform = FMatrix::Identity;
	FMatrix GlobalBindTransform = FMatrix::Identity;
	FMatrix InverseBindTransform = FMatrix::Identity;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalBoneInfo& Bone)
	{
		Ar << Bone.Name;
		Ar << Bone.ParentIndex;
		Ar << Bone.ChildCount;
		SerializeSkeletalMatrix(Ar, Bone.MeshBindGlobalTransform);
		SerializeSkeletalMatrix(Ar, Bone.LocalBindTransform);
		SerializeSkeletalMatrix(Ar, Bone.GlobalBindTransform);
		SerializeSkeletalMatrix(Ar, Bone.InverseBindTransform);
		return Ar;
	}
};

// ============================================================
// SkeletalMesh Vertex Range
// ============================================================
// 하나의 FBX Mesh Node에서 나온 VertexBuffer/IndexBuffer 범위를 기록한다.
// ControlPoint 좌표는 정점에 원본 그대로 보존하고,
// Geometry Transform과 Mesh Node Global Bind Transform은 이 범위에 따로 저장한다.
// 여러 Mesh Node를 하나의 SkeletalMesh로 합쳐도 각 정점 범위가 사용할 행렬을 잃지 않는다.
struct FSkeletalMeshVertexRange
{
	uint32 BaseVertex = 0;
	uint32 VertexCount = 0;

	uint32 BaseIndex = 0;
	uint32 IndexCount = 0;

	FString MeshNodeName;

	FMatrix GeometryTransform = FMatrix::Identity;
	FMatrix MeshNodeGlobalBindTransform = FMatrix::Identity;
	FMatrix MeshNodeGlobalBindInverseTransform = FMatrix::Identity;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalMeshVertexRange& Range)
	{
		Ar << Range.BaseVertex;
		Ar << Range.VertexCount;
		Ar << Range.BaseIndex;
		Ar << Range.IndexCount;
		Ar << Range.MeshNodeName;
		SerializeSkeletalMatrix(Ar, Range.GeometryTransform);
		SerializeSkeletalMatrix(Ar, Range.MeshNodeGlobalBindTransform);
		SerializeSkeletalMatrix(Ar, Range.MeshNodeGlobalBindInverseTransform);
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
	TArray<FSkeletalMeshVertexRange> VertexRanges;
};

struct FSkeletalMeshRawData
{
	TArray<FSkeletalMeshVertex> SourceVertices;
	TArray<int32> Indices;
	TArray<FSkeletalBoneInfo> Bones;
	TArray<FSkeletalMeshSection> Sections;
	TArray<FSkeletalMeshVertexRange> VertexRanges;

	FVector BoundsCenter = FVector(0, 0, 0);
	FVector BoundsExtent = FVector(0, 0, 0);
	bool bBoundsValid = false;

	void CacheBounds()
	{
		bBoundsValid = false;
		if (SourceVertices.empty()) return;

		auto GetBindPosePosition = [this](uint32 VertexIndex) -> FVector
		{
			const FVector MeshLocalPosition = SourceVertices[VertexIndex].Position;

			for (const FSkeletalMeshVertexRange& Range : VertexRanges)
			{
				if (VertexIndex >= Range.BaseVertex && VertexIndex < Range.BaseVertex + Range.VertexCount)
				{
					// Range 행렬은 행벡터 기준 논리 순서로 적용한다.
					const FVector GeometryLocalPosition = Range.GeometryTransform.TransformPositionWithW(MeshLocalPosition);
					return Range.MeshNodeGlobalBindTransform.TransformPositionWithW(GeometryLocalPosition);
				}
			}

			return MeshLocalPosition;
		};

		FVector LocalMin = GetBindPosePosition(0);
		FVector LocalMax = LocalMin;
		for (uint32 VertexIndex = 0; VertexIndex < static_cast<uint32>(SourceVertices.size()); ++VertexIndex)
		{
			const FVector BindPosePosition = GetBindPosePosition(VertexIndex);
			LocalMin.X = (std::min)(LocalMin.X, BindPosePosition.X);
			LocalMin.Y = (std::min)(LocalMin.Y, BindPosePosition.Y);
			LocalMin.Z = (std::min)(LocalMin.Z, BindPosePosition.Z);
			LocalMax.X = (std::max)(LocalMax.X, BindPosePosition.X);
			LocalMax.Y = (std::max)(LocalMax.Y, BindPosePosition.Y);
			LocalMax.Z = (std::max)(LocalMax.Z, BindPosePosition.Z);
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
	TArray<FSkeletalMeshVertexRange> VertexRanges;

	FVector BoundsCenter = FVector(0, 0, 0);
	FVector BoundsExtent = FVector(0, 0, 0);
	bool bBoundsValid = false;

	void CacheBounds()
	{
		bBoundsValid = false;
		if (SourceVertices.empty()) return;

		auto GetBindPosePosition = [this](uint32 VertexIndex) -> FVector
		{
			const FVector MeshLocalPosition = SourceVertices[VertexIndex].Position;

			for (const FSkeletalMeshVertexRange& Range : VertexRanges)
			{
				if (VertexIndex >= Range.BaseVertex && VertexIndex < Range.BaseVertex + Range.VertexCount)
				{
					// Range 행렬은 행벡터 기준 논리 순서로 적용한다.
					const FVector GeometryLocalPosition = Range.GeometryTransform.TransformPositionWithW(MeshLocalPosition);
					return Range.MeshNodeGlobalBindTransform.TransformPositionWithW(GeometryLocalPosition);
				}
			}

			return MeshLocalPosition;
		};

		FVector LocalMin = GetBindPosePosition(0);
		FVector LocalMax = LocalMin;
		for (uint32 VertexIndex = 0; VertexIndex < static_cast<uint32>(SourceVertices.size()); ++VertexIndex)
		{
			const FVector BindPosePosition = GetBindPosePosition(VertexIndex);
			LocalMin.X = (std::min)(LocalMin.X, BindPosePosition.X);
			LocalMin.Y = (std::min)(LocalMin.Y, BindPosePosition.Y);
			LocalMin.Z = (std::min)(LocalMin.Z, BindPosePosition.Z);
			LocalMax.X = (std::max)(LocalMax.X, BindPosePosition.X);
			LocalMax.Y = (std::max)(LocalMax.Y, BindPosePosition.Y);
			LocalMax.Z = (std::max)(LocalMax.Z, BindPosePosition.Z);
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
		Ar << VertexRanges;
		Ar << BoundsCenter;
		Ar << BoundsExtent;
		Ar << bBoundsValid;
	}
};
