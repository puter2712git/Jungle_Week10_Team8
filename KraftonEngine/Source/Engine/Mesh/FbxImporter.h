#pragma once

#include "Core/CoreTypes.h"
#include "Math/Vector.h"
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

// Bone 정보가 있는 FBX Skeletal Mesh
struct FSkeletalMeshVertex
{
	FVector		Position;				// Control Point
	FVector		Normal;					// LayerElementNormal
	FVector2	UV;						// LayerElementUV
	FVector4	Tangent;				// LayerElementTangent
	uint32		MaterialIndex;			// LayetElementMaterial, (보통 ePolygon)

	uint32		BoneIndices[4];
	float		BoneWeights[4];			// FbxSkin / FbxCluster
};

struct EngineBone
{
	FString Name;
	int ParentIndex = 0;
	FbxNode* FbxNode = nullptr;

	// TODO: 나중에 Skinning할 때 필요
	/*FMatrix LocalBindTransform;
	FMatrix GlobalBindTransform;
	FMatrix InverseBindTransform;*/
};

struct BoneInfluence
{
	int32 boneIndex;
	float boneWeight;
};

class UStaticMesh;
struct ID3D11Device;

class FFbxImporter
{
public:
	bool Import(const FString& FilePath);

	UStaticMesh* ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device);

	void ProcessNode(FbxNode* Node, int32 cnt);
	void ProcessMesh(FbxNode* Node);
	void ProcessPolygon(FbxNode* Node);


	// TODO 여기있으면 안되는 정보 나중에 싹다 옮겨야함. 임시로 load 잘 되는지 테스트 목적
private:
	void			PreLoadCluster(FbxMesh* mesh);
	int				FindOrAddBone(FbxNode* BoneNode);
	int				FindOrAddVertex(FSkeletalMeshVertex Vertex);

	void			ComputeTangents();
	void			NormalizeCoordinateUnit(FbxScene* Scene);
	FbxNode*		FindFirstMesh(FbxNode* Node);

	FbxAMatrix					ImportRootGlobalInverse; // 기준으로 잡을 root
	UStaticMesh* BuildStaticMeshFromImportedData(const FString& FilePath, ID3D11Device* Device);

	TArray<TArray<BoneInfluence>> InfluencesPerControlPoint;	// ControlPoint에 영향을 주는 Cluster 모음
	TMap<FbxNode*, int> BoneNodeToIndex;						// key: FbxNode*, value: BoneIndex
	TArray<EngineBone> Bones;									// Bone 계층 정보

	// TMap<FSkeletalMeshVertex, int32> VertexToIndex;				// Key: Vertex, value: vertex Index

	// 최종값
	TArray<FSkeletalMeshVertex> vertices;						// Vertex 모음
	TArray<int32> indices;									// SkeletalMesh indices
	bool bIsRootFilled = false;
};
