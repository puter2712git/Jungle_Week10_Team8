#pragma once

#include "Core/CoreTypes.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "Mesh/SkeletalMeshAsset.h"
#include <fbxsdk.h>


// Bone 관련 정보 없는 FBX Type Static Mesh
struct FStaticMeshVertex
{
	FVector		Position;
	FVector		Normal;
	FVector2	UV;
	FVector4	Tangent;
	uint32		MaterialIndex;
};

// Engine에서 사용하는 Bone 계층 정보 구조체
struct EngineBone
{
	FString Name;				// Bone 이름
	int32 ParentIndex = -1;		// Bone의 부모 idx
	FbxNode* FbxNode = nullptr;	// 해당 Bone이 어떤 FbxNode에 속해있는가
	int32 ChildCount = -1;		// 해당 Bone의 Child의 개수

	FMatrix MeshBindGlobalTransform = FMatrix::Identity;
	FMatrix LocalBindTransform = FMatrix::Identity;
	FMatrix GlobalBindTransform = FMatrix::Identity;
	FMatrix InverseBindTransform = FMatrix::Identity;
	bool bHasBindTransform = false;
};

// Mesh에 영향을 미치는 Bone 정보를 보관하는 구조체 (Sort 때문에 일부 함수 설정)
struct BoneInfluence
{
	int32 boneIndex;
	float boneWeight;

	BoneInfluence() = default;

	BoneInfluence(int32 InBoneIndex, float InBoneWeight) : boneIndex(InBoneIndex), boneWeight(InBoneWeight) {}
	BoneInfluence(const BoneInfluence& other) = default;
	BoneInfluence(BoneInfluence&& other) noexcept = default;

	BoneInfluence& operator=(const BoneInfluence& other)
	{
		if (this == &other)
		{
			return *this;
		}

		boneIndex = other.boneIndex;
		boneWeight = other.boneWeight;

		return *this;
	}

	BoneInfluence& operator=(BoneInfluence&& other) noexcept = default;

	friend void swap(BoneInfluence& lhs, BoneInfluence& rhs) noexcept
	{
		using std::swap;
		swap(lhs.boneIndex, rhs.boneIndex);
		swap(lhs.boneWeight, rhs.boneWeight);
	}
};

// deformer 여부로 구분 가능
enum class EFbxImportMeshType
{
	StaticMesh,
	SkeletalMesh
};

// Fbx 축 변환 모드 지정
enum class EFbxAxisConversionMode
{
	None,
	FbxSdkConvertScene,
	ManualAxisFix
};

// =================================
// FBX Import 설정 옵션
// - Triangulate, Unit, Axis Convert, Material Build 여부, Bone 여부(static, skeletal 구분), Log 출력 flag 등 관리.
// =================================
struct FFbxImportOptions
{
	bool bTriangulate = true;
	bool bConvertUnit = true;
	bool bConvertAxis = false;
	bool bBuildMaterials = false;
	bool bImportBones = true;
	bool bLogNodeTransforms = true;
	bool bLogMeshSummary = true;

	EFbxImportMeshType MeshType = EFbxImportMeshType::StaticMesh;
	EFbxAxisConversionMode AxisMode = EFbxAxisConversionMode::None;
};

class UStaticMesh;
class USkeletalMesh;
struct ID3D11Device;

class FFbxImporter
{
public:
	bool Import(const FString& FilePath);
	bool Import(const FString& FilePath, const FFbxImportOptions& Options);

	// FBX 파일을 Static Mesh로 Import하는 함수
	UStaticMesh* ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device);
	// FBX 파일을 Skeletal Mesh로 Import하는 함수
	USkeletalMesh* ImportAsSkeletalMesh(const FString& FilePath, ID3D11Device* Device);

	void ProcessNode(FbxNode* Node, int32 cnt, const FFbxImportOptions& Options);
	void ProcessPolygon(FbxNode* Node, const FFbxImportOptions& Options);


private:
	void					PreLoadCluster(FbxMesh* mesh);
	int						FindOrAddBone(FbxNode* BoneNode);
	void					FillBoneBindData(int32 BoneIndex, FbxCluster* Cluster);
	void					DeriveLocalBindTransformsFromGlobalBindTransforms();
	int						FindOrAddVertex(FSkeletalMeshVertex Vertex);

	void					ComputeTangents();
	void					NormalizeCoordinateUnit(FbxScene* Scene);
	void					ApplySceneUnitConversion(FbxScene* Scene, const FFbxImportOptions& Options);
	void					ApplySceneAxisConversion(FbxScene* Scene, const FFbxImportOptions& Options);
	FbxAMatrix				BuildManualAxisFixMatrix(const FFbxImportOptions& Options);
	FMatrix					GetGeometryTransformFromNode(FbxNode* Node) const;
	UStaticMesh*			BuildStaticMeshFromImportedData(const FString& FilePath, ID3D11Device* Device);
	USkeletalMesh*			BuildSkeletalMeshFromImportedData(const FString& FilePath, ID3D11Device* Device);
	FSkeletalMeshRawData	BuildSkeletalRawDataFromImportedData();

	TArray<TArray<BoneInfluence>> InfluencesPerControlPoint;	// ControlPoint에 영향을 주는 Cluster 모음
	TMap<FbxNode*, int> BoneNodeToIndex;						// key: FbxNode*, value: BoneIndex
	TArray<EngineBone> Bones;									// Bone 계층 정보

	// TMap<FSkeletalMeshVertex, int32> VertexToIndex;				// Key: Vertex, value: vertex Index

	int32 CurrentRigidFallbackBoneIndex = -1;
	// Bone이 없는 Static Mesh의 경우 Mesh기준 가장 가까운 Bone에 Bind한다.
	int32 FindRigidFallbackBoneForMesh(FbxNode* MeshNode);

	// 최종값
	TArray<FSkeletalMeshVertex> vertices;						// Vertex 모음
	TArray<int32> indices;									// SkeletalMesh indices
	TArray<FSkeletalMeshVertexRange> vertexRanges;				// Mesh Node별 Vertex/Index 범위
};
