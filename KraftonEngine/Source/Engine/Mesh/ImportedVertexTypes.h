#pragma once

#include "Core/EngineTypes.h"
#include "Math/Matrix.h"

struct FImportedBoneInfluence
{
	uint32 BoneIndex = 0;
	float Weight = 0.0f;
};

struct FImportedSkeletalVertex
{
	FVector Position;
	FVector Normal;
	FVector2 UV;
	FVector4 Tangent;

	uint32 BoneIndices[4] = { 0, 0, 0, 0 };
	float BoneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// FBX Skin Weight는 ControlPoint 기준이라,
	// Polygon Vertex 생성 후에도 원본 ControlPoint 추적용으로 필요함
	int ControlPointIndex = -1;
};

struct FImportedBone
{
	FString Name;
	int ParentIndex = -1;

	// 이후 FBX Bone Node와 매핑할 때 사용
	void* SourceNode = nullptr;

	FMatrix BindGlobal;
	FMatrix InverseBindGlobal;
};

enum class ESkeletalMeshRangeBinding
{
	Skinned,
	RigidBone,
	Static
};

struct FImportedSkeletalMeshRange
{
	uint32 VertexStart = 0;
	uint32 VertexEnd = 0;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;

	FMatrix MeshSceneGlobal = FMatrix::Identity;
	bool bHasMeshScene = false;

	FMatrix MeshBindGlobal = FMatrix::Identity;
	FMatrix InverseMeshBindGlobal = FMatrix::Identity;
	bool bHasMeshBind = false;

	ESkeletalMeshRangeBinding BindingType = ESkeletalMeshRangeBinding::Static;
	int32 RigidBoneIndex = -1;
};

struct FImportedSkeletalMesh
{
	TArray<FImportedSkeletalVertex> SkeletalVertices;
	TArray<uint32> Indices;
	TArray<FImportedBone> Bones;
	TArray<FImportedSkeletalMeshRange> MeshRanges;

	bool HasNormals = false;
	bool HasUVs = false;
	bool HasSkinWeights = false;
};
