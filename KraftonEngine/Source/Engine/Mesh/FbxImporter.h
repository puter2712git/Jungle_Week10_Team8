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

struct EngineBone
{
	FString Name;
	int ParentIndex = 0;
	FbxNode* FbxNode = nullptr;

	FMatrix MeshBindGlobalTransform = FMatrix::Identity;
	FMatrix LocalBindTransform = FMatrix::Identity;
	FMatrix GlobalBindTransform = FMatrix::Identity;
	FMatrix InverseBindTransform = FMatrix::Identity;
	bool bHasBindTransform = false;
};

struct BoneInfluence
{
	int32 boneIndex;
	float boneWeight;
};

enum class EFbxImportMeshType
{
	StaticMesh,
	SkeletalMesh
};

enum class EFbxAxisConversionMode
{
	None,
	FbxSdkConvertScene,
	ManualAxisFix
};

enum class EFbxImportPositionMode
{
	RawControlPoint,
	GeometryOnly,
	SceneBake,
	UnifiedRootLocal
};

enum class EFbxImportRootMode
{
	SceneRoot,
	FirstChild,
	FirstSkeleton,
	FirstMesh,
	FirstMeshParent,
	ExplicitNodeName
};

struct FFbxImportOptions
{
	bool bTriangulate = true;
	bool bConvertUnit = false;
	bool bConvertAxis = false;
	bool bBakeGeometryTransform = true;
	bool bBuildMaterials = false;
	bool bImportBones = true;
	bool bLogNodeTransforms = false;
	bool bLogMeshSummary = false;
	bool bLogImportRoot = true;

	EFbxImportMeshType MeshType = EFbxImportMeshType::StaticMesh;
	EFbxAxisConversionMode AxisMode = EFbxAxisConversionMode::None;
	EFbxImportPositionMode PositionMode = EFbxImportPositionMode::UnifiedRootLocal;
	// Preserve the importer behavior that fixed multi-node FBX meshes by using the first mesh as import root.
	EFbxImportRootMode RootMode = EFbxImportRootMode::FirstMesh;
	FString ExplicitRootNodeName;
};

class UStaticMesh;
class USkeletalMesh;
struct ID3D11Device;

class FFbxImporter
{
public:
	bool Import(const FString& FilePath);
	bool Import(const FString& FilePath, const FFbxImportOptions& Options);

	UStaticMesh* ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device);
	USkeletalMesh* ImportAsSkeletalMesh(const FString& FilePath, ID3D11Device* Device);

	void ProcessNode(FbxNode* Node, int32 cnt, const FFbxImportOptions& Options);
	void ProcessMesh(FbxNode* Node);
	void ProcessPolygon(FbxNode* Node, const FFbxImportOptions& Options);


	// TODO 여기있으면 안되는 정보 나중에 싹다 옮겨야함. 임시로 load 잘 되는지 테스트 목적
private:
	void			PreLoadCluster(FbxMesh* mesh);
	int				FindOrAddBone(FbxNode* BoneNode);
	void			FillBoneBindData(int32 BoneIndex, FbxCluster* Cluster);
	void			DeriveLocalBindTransformsFromGlobalBindTransforms();
	int				FindOrAddVertex(FSkeletalMeshVertex Vertex);

	void			ComputeTangents();
	void			NormalizeCoordinateUnit(FbxScene* Scene);
	void			ApplySceneUnitConversion(FbxScene* Scene, const FFbxImportOptions& Options);
	void			ApplySceneAxisConversion(FbxScene* Scene, const FFbxImportOptions& Options);
	FbxAMatrix		BuildManualAxisFixMatrix(const FFbxImportOptions& Options);
	FbxNode*		SelectImportRootNode(FbxScene* Scene, const FFbxImportOptions& Options);
	FbxNode*		FindFirstSkeletonRoot(FbxNode* Node);
	FbxNode*		FindFirstMeshRoot(FbxNode* Node);
	FbxNode*		FindFirstMesh(FbxNode* Node);
	FbxVector4		TransformControlPointForImport(
						const FbxVector4& ControlPoint,
						const FbxAMatrix& Geometry,
						const FbxAMatrix& NodeGlobal,
						const FbxAMatrix& ImportRootGlobalInverse,
						const FFbxImportOptions& Options);
	FVector			TransformNormalForImport(
						const FVector& Normal,
						const FbxAMatrix& Geometry,
						const FbxAMatrix& NodeGlobal,
						const FFbxImportOptions& Options);
	FVector4		TransformTangentForImport(
						const FVector4& Tangent,
						const FbxAMatrix& Geometry,
						const FbxAMatrix& NodeGlobal,
						const FFbxImportOptions& Options);

	FbxAMatrix					ImportRootGlobalInverse; // 기준으로 잡을 root
	UStaticMesh* BuildStaticMeshFromImportedData(const FString& FilePath, ID3D11Device* Device);
	USkeletalMesh* BuildSkeletalMeshFromImportedData(const FString& FilePath, ID3D11Device* Device);
	FSkeletalMeshRawData BuildSkeletalRawDataFromImportedData();

	TArray<TArray<BoneInfluence>> InfluencesPerControlPoint;	// ControlPoint에 영향을 주는 Cluster 모음
	TMap<FbxNode*, int> BoneNodeToIndex;						// key: FbxNode*, value: BoneIndex
	TArray<EngineBone> Bones;									// Bone 계층 정보

	// TMap<FSkeletalMeshVertex, int32> VertexToIndex;				// Key: Vertex, value: vertex Index

	// 최종값
	TArray<FSkeletalMeshVertex> vertices;						// Vertex 모음
	TArray<int32> indices;									// SkeletalMesh indices
	bool bIsRootFilled = false;
};
