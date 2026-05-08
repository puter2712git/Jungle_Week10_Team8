#include "FbxImporter.h"
#include "Platform/Paths.h"
#include "Core/Log.h"
#include "ImportedVertexTypes.h"

#include <fbxsdk.h>
#include <functional>

struct FImportedMeshRange
{
	FbxMesh* Mesh = nullptr;
	uint32 VertexStart = 0;
	uint32 VertexEnd = 0;
};

struct FFbxSkeletalVertexKey
{
	int ControlPointIndex = -1;
	float NormalX = 0.0f;
	float NormalY = 0.0f;
	float NormalZ = 0.0f;
	float UVX = 0.0f;
	float UVY = 0.0f;

	bool operator==(const FFbxSkeletalVertexKey& Other) const
	{
		return ControlPointIndex == Other.ControlPointIndex
			&& NormalX == Other.NormalX
			&& NormalY == Other.NormalY
			&& NormalZ == Other.NormalZ
			&& UVX == Other.UVX
			&& UVY == Other.UVY;
	}
};

namespace std
{
template<>
struct hash<FFbxSkeletalVertexKey>
{
	size_t operator()(const FFbxSkeletalVertexKey& Key) const noexcept
	{
		size_t Result = std::hash<int>()(Key.ControlPointIndex);
		auto Combine = [&Result](size_t Value)
			{
				Result ^= Value + 0x9e3779b9 + (Result << 6) + (Result >> 2);
			};

		Combine(std::hash<float>()(Key.NormalX));
		Combine(std::hash<float>()(Key.NormalY));
		Combine(std::hash<float>()(Key.NormalZ));
		Combine(std::hash<float>()(Key.UVX));
		Combine(std::hash<float>()(Key.UVY));
		return Result;
	}
};
}

static FMatrix ConvertFbxMatrix(const FbxAMatrix& M);
static void ProcessSkeleton(FbxNode* Node, int32 ParentBoneIndex, FImportedSkeletalMesh& OutMesh);
static void ProcessNode(FbxNode* Node, FImportedSkeletalMesh& OutMesh, TArray<FImportedMeshRange>& OutMeshRanges);
static void ProcessMesh(FbxNode* Node, FImportedSkeletalMesh& OutMesh, TArray<FImportedMeshRange>& OutMeshRanges);
static void ProcessSkinWeights(FbxNode* Node, FImportedSkeletalMesh& OutMesh, const TArray<FImportedMeshRange>& MeshRanges);
static void ProcessSkinWeightsForMesh(FbxMesh* Mesh, FImportedSkeletalMesh& OutMesh, uint32 VertexStart, uint32 VertexEnd);
static void NormalizeVertexWeights(FImportedSkeletalVertex& Vertex);
static int32 FindBoneIndexByNode(FbxNode* BoneNode, const FImportedSkeletalMesh& Mesh);
static void AddInfluenceToVertex(FImportedSkeletalVertex& Vertex, uint32 BoneIndex, float Weight);
static void GenerateTangents(FImportedSkeletalMesh& OutMesh);

static void ProcessMesh(FbxNode* Node, FImportedSkeletalMesh& OutMesh, TArray<FImportedMeshRange>& OutMeshRanges)
{
	if (!Node)
	{
		return;
	}

	FbxMesh* Mesh = Node->GetMesh();
	if (!Mesh)
	{
		return;
	}

	if (Mesh->GetElementNormalCount() == 0)
	{
		Mesh->GenerateNormals();
	}

	OutMesh.HasNormals = Mesh->GetElementNormalCount() > 0;

	const bool bHasUV = Mesh->GetElementUVCount() > 0;
	OutMesh.HasUVs = OutMesh.HasUVs || bHasUV;

	FbxString UVSetName;

	if (bHasUV)
	{
		UVSetName = Mesh->GetElementUV(0)->GetName();
	}

	const int PolygonCount = Mesh->GetPolygonCount();
	const uint32 VertexStart = static_cast<uint32>(OutMesh.SkeletalVertices.size());
	TMap<FFbxSkeletalVertexKey, uint32> VertexMap;

	for (int PolygonIndex = 0; PolygonIndex < PolygonCount; ++PolygonIndex)
	{
		const int PolygonSize = Mesh->GetPolygonSize(PolygonIndex);

		if (PolygonSize != 3)
		{
			UE_LOG("Invalid Polygon Size: %d", PolygonSize);
			continue;
		}

		for (int VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
		{
			const int ControlPointIndex =
				Mesh->GetPolygonVertex(PolygonIndex, VertexIndex);

			if (ControlPointIndex < 0)
			{
				continue;
			}

			FImportedSkeletalVertex ImportedVertex = {};
			ImportedVertex.ControlPointIndex = ControlPointIndex;

			// Position
			{
				const FbxVector4 FbxPosition =
					Mesh->GetControlPointAt(ControlPointIndex);

				ImportedVertex.Position = FVector(
					static_cast<float>(FbxPosition[0]),
					static_cast<float>(FbxPosition[1]),
					static_cast<float>(FbxPosition[2])
				);
			}

			// Normal
			{
				FbxVector4 FbxNormal(0.0, 0.0, 1.0, 0.0);

				if (OutMesh.HasNormals)
				{
					Mesh->GetPolygonVertexNormal(
						PolygonIndex,
						VertexIndex,
						FbxNormal
					);

					FbxNormal.Normalize();
				}

				ImportedVertex.Normal = FVector(
					static_cast<float>(FbxNormal[0]),
					static_cast<float>(FbxNormal[1]),
					static_cast<float>(FbxNormal[2])
				);
			}

			// UV
			{
				ImportedVertex.UV = FVector2(0.0f, 0.0f);

				if (bHasUV)
				{
					FbxVector2 FbxUV;
					bool bUnmappedUV = false;

					const bool bSuccess =
						Mesh->GetPolygonVertexUV(
							PolygonIndex,
							VertexIndex,
							UVSetName.Buffer(),
							FbxUV,
							bUnmappedUV
						);

					if (bSuccess && !bUnmappedUV)
					{
						ImportedVertex.UV = FVector2(
							static_cast<float>(FbxUV[0]),
							1.0f - static_cast<float>(FbxUV[1])
						);
					}
				}
			}

			// BoneIndices / BoneWeights는 이후 SkinWeight 단계에서 채움
			for (int32 Slot = 0; Slot < 4; ++Slot)
			{
				ImportedVertex.BoneIndices[Slot] = 0;
				ImportedVertex.BoneWeights[Slot] = 0.0f;
			}

			FFbxSkeletalVertexKey Key;
			Key.ControlPointIndex = ImportedVertex.ControlPointIndex;
			Key.NormalX = ImportedVertex.Normal.X;
			Key.NormalY = ImportedVertex.Normal.Y;
			Key.NormalZ = ImportedVertex.Normal.Z;
			Key.UVX = ImportedVertex.UV.X;
			Key.UVY = ImportedVertex.UV.Y;

			if (auto It = VertexMap.find(Key); It != VertexMap.end())
			{
				OutMesh.Indices.push_back(It->second);
			}
			else
			{
				const uint32 NewIndex =
					static_cast<uint32>(OutMesh.SkeletalVertices.size());

				OutMesh.SkeletalVertices.push_back(ImportedVertex);
				OutMesh.Indices.push_back(NewIndex);
				VertexMap[Key] = NewIndex;
			}
		}
	}

	FImportedMeshRange Range;
	Range.Mesh = Mesh;
	Range.VertexStart = VertexStart;
	Range.VertexEnd = static_cast<uint32>(OutMesh.SkeletalVertices.size());
	OutMeshRanges.push_back(Range);
}

static void ProcessNode(FbxNode* Node, FImportedSkeletalMesh& OutMesh, TArray<FImportedMeshRange>& OutMeshRanges)
{
	FbxNodeAttribute* Attribute = Node->GetNodeAttribute();
	if (Attribute)
	{
		if (Attribute->GetAttributeType() == FbxNodeAttribute::eMesh)
		{
			ProcessMesh(Node, OutMesh, OutMeshRanges);
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		ProcessNode(Node->GetChild(i), OutMesh, OutMeshRanges);
	}
}

bool FFbxImporter::Import(const FString& FilePath, FImportedSkeletalMesh& OutMesh)
{
	OutMesh = FImportedSkeletalMesh();

	FbxManager* SdkManager = FbxManager::Create();

	FbxIOSettings* ios = FbxIOSettings::Create(SdkManager, IOSROOT);
	SdkManager->SetIOSettings(ios);

	FbxScene* Scene = FbxScene::Create(SdkManager, "My Scene");

	FbxImporter* Importer = FbxImporter::Create(SdkManager, "");

	FString FullPath = FPaths::ToUtf8(FPaths::Combine(FPaths::AssetDir(), FPaths::ToWide(FilePath)));

	if (!Importer->Initialize(FullPath.c_str(), -1, SdkManager->GetIOSettings()))
	{
		return false;
	}

	Importer->Import(Scene);
	Importer->Destroy();

	FbxGeometryConverter GeometryConverter(SdkManager);
	if (!GeometryConverter.Triangulate(Scene, true))
	{
		UE_LOG("FBX Triangulate Failed");
		SdkManager->Destroy();
		return false;
	}

	auto* RootNode = Scene->GetRootNode();
	if (!RootNode)
	{
		SdkManager->Destroy();
		return false;
	}

	TArray<FImportedMeshRange> MeshRanges;
	ProcessSkeleton(RootNode, -1, OutMesh);
	ProcessNode(RootNode, OutMesh, MeshRanges);
	GenerateTangents(OutMesh);
	ProcessSkinWeights(RootNode, OutMesh, MeshRanges);

	SdkManager->Destroy();
	return true;
}


static FMatrix ConvertFbxMatrix(const FbxAMatrix& M)
{
	FMatrix Result;

	for (int Row = 0; Row < 4; ++Row)
	{
		for (int Col = 0; Col < 4; ++Col)
		{
			Result.M[Row][Col] = static_cast<float>(M.Get(Row, Col));
		}
	}

	return Result;
}

static void ProcessSkeleton(FbxNode* Node, int32 ParentBoneIndex, FImportedSkeletalMesh& OutMesh)
{
	if (!Node)
	{
		return;
	}

	FbxNodeAttribute* Attribute = Node->GetNodeAttribute();
	int32 CurrentBoneIndex = ParentBoneIndex;


	if (Attribute &&
		Attribute->GetAttributeType() == FbxNodeAttribute::eSkeleton)
	{
		FImportedBone Bone;

		Bone.Name = Node->GetName();
		Bone.ParentIndex = ParentBoneIndex;
		Bone.SourceNode = Node;

		const FbxAMatrix FbxGlobalTransform = Node->EvaluateGlobalTransform();
		Bone.BindGlobal = ConvertFbxMatrix(FbxGlobalTransform);
		Bone.InverseBindGlobal = Bone.BindGlobal.GetInverse();

		CurrentBoneIndex = static_cast<int32>(OutMesh.Bones.size());

		OutMesh.Bones.push_back(Bone);
	}

	const int ChildCount = Node->GetChildCount();

	for (int i = 0; i < ChildCount; ++i)
	{
		ProcessSkeleton(
			Node->GetChild(i),
			CurrentBoneIndex,
			OutMesh);
	}
}


static void NormalizeVertexWeights(FImportedSkeletalVertex& Vertex)
{
	float TotalWeight = 0.0f;

	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		TotalWeight += Vertex.BoneWeights[Slot];
	}

	if (TotalWeight <= 0.0f)
	{
		Vertex.BoneIndices[0] = 0;
		Vertex.BoneWeights[0] = 1.0f;

		for (int32 Slot = 1; Slot < 4; ++Slot)
		{
			Vertex.BoneIndices[Slot] = 0;
			Vertex.BoneWeights[Slot] = 0.0f;
		}

		return;
	}

	const float InvTotalWeight = 1.0f / TotalWeight;

	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		Vertex.BoneWeights[Slot] *= InvTotalWeight;
	}
}

//최적화 필요해요
static int32 FindBoneIndexByNode(FbxNode* BoneNode, const FImportedSkeletalMesh& Mesh)
{
	if (!BoneNode)
	{
		return -1;
	}

	for (int32 BoneIndex = 0; BoneIndex < static_cast<int32>(Mesh.Bones.size()); ++BoneIndex)
	{
		if (Mesh.Bones[BoneIndex].SourceNode == BoneNode)
		{
			return BoneIndex;
		}
	}

	return -1;
}

static void AddInfluenceToVertex(FImportedSkeletalVertex& Vertex, uint32 BoneIndex, float Weight)
{
	if (Weight <= 0.0f)
	{
		return;
	}

	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		if (Vertex.BoneWeights[Slot] == 0.0f)
		{
			Vertex.BoneIndices[Slot] = BoneIndex;
			Vertex.BoneWeights[Slot] = Weight;
			return;
		}
	}

	int32 MinSlot = 0;
	for (int32 Slot = 1; Slot < 4; ++Slot)
	{
		if (Vertex.BoneWeights[Slot] < Vertex.BoneWeights[MinSlot])
		{
			MinSlot = Slot;
		}
	}

	if (Weight > Vertex.BoneWeights[MinSlot])
	{
		Vertex.BoneIndices[MinSlot] = BoneIndex;
		Vertex.BoneWeights[MinSlot] = Weight;
	}
}

static void ProcessSkinWeights(FbxNode* Node, FImportedSkeletalMesh& OutMesh, const TArray<FImportedMeshRange>& MeshRanges)
{
	if (!Node)
	{
		return;
	}

	if (FbxMesh* Mesh = Node->GetMesh())
	{
		for (const FImportedMeshRange& Range : MeshRanges)
		{
			if (Range.Mesh == Mesh)
			{
				ProcessSkinWeightsForMesh(Mesh, OutMesh, Range.VertexStart, Range.VertexEnd);
				break;
			}
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		ProcessSkinWeights(Node->GetChild(i), OutMesh, MeshRanges);
	}
}

static void ProcessSkinWeightsForMesh(FbxMesh* Mesh, FImportedSkeletalMesh& OutMesh, uint32 VertexStart, uint32 VertexEnd)
{
	if (!Mesh || OutMesh.Bones.empty())
	{
		return;
	}

	const int DeformerCount = Mesh->GetDeformerCount(FbxDeformer::eSkin);
	if (DeformerCount <= 0)
	{
		return;
	}

	bool bAppliedAnyWeight = false;
	TMap<int, TArray<uint32>> ControlPointToVertices;

	for (uint32 VertexIndex = VertexStart; VertexIndex < VertexEnd; ++VertexIndex)
	{
		FImportedSkeletalVertex& Vertex = OutMesh.SkeletalVertices[VertexIndex];
		if (Vertex.ControlPointIndex >= 0)
		{
			ControlPointToVertices[Vertex.ControlPointIndex].push_back(VertexIndex);
		}
	}

	for (int DeformerIndex = 0; DeformerIndex < DeformerCount; ++DeformerIndex)
	{
		FbxSkin* Skin = static_cast<FbxSkin*>(
			Mesh->GetDeformer(DeformerIndex, FbxDeformer::eSkin)
			);

		if (!Skin)
		{
			continue;
		}

		const int ClusterCount = Skin->GetClusterCount();

		for (int ClusterIndex = 0; ClusterIndex < ClusterCount; ++ClusterIndex)
		{
			FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
			if (!Cluster)
			{
				continue;
			}

			FbxNode* BoneNode = Cluster->GetLink();
			const int32 BoneIndex = FindBoneIndexByNode(BoneNode, OutMesh);
			if (BoneIndex < 0)
			{
				continue;
			}

			// Bind Pose 보정
			FbxAMatrix TransformLinkMatrix;
			Cluster->GetTransformLinkMatrix(TransformLinkMatrix);

			OutMesh.Bones[BoneIndex].BindGlobal =
				ConvertFbxMatrix(TransformLinkMatrix);

			OutMesh.Bones[BoneIndex].InverseBindGlobal =
				OutMesh.Bones[BoneIndex].BindGlobal.GetInverse();

			const int IndexCount = Cluster->GetControlPointIndicesCount();
			int* ControlPointIndices = Cluster->GetControlPointIndices();
			double* ControlPointWeights = Cluster->GetControlPointWeights();

			for (int Index = 0; Index < IndexCount; ++Index)
			{
				const int ControlPointIndex = ControlPointIndices[Index];
				const float Weight = static_cast<float>(ControlPointWeights[Index]);

				if (Weight <= 0.0f)
				{
					continue;
				}

				auto It = ControlPointToVertices.find(ControlPointIndex);
				if (It == ControlPointToVertices.end())
				{
					continue;
				}

				for (uint32 VertexIndex : It->second)
				{
					FImportedSkeletalVertex& Vertex = OutMesh.SkeletalVertices[VertexIndex];
					AddInfluenceToVertex(Vertex, static_cast<uint32>(BoneIndex), Weight);
					bAppliedAnyWeight = true;
				}
			}
		}
	}

	if (!bAppliedAnyWeight)
	{
		return;
	}

	for (uint32 VertexIndex = VertexStart; VertexIndex < VertexEnd; ++VertexIndex)
	{
		NormalizeVertexWeights(OutMesh.SkeletalVertices[VertexIndex]);
	}

	OutMesh.HasSkinWeights = OutMesh.HasSkinWeights || bAppliedAnyWeight;
}

static void GenerateTangents(FImportedSkeletalMesh& OutMesh)
{
	TArray<FVector> TangentSums(
		OutMesh.SkeletalVertices.size(),
		FVector(0.0f, 0.0f, 0.0f)
	);

	TArray<FVector> BitangentSums(
		OutMesh.SkeletalVertices.size(),
		FVector(0.0f, 0.0f, 0.0f)
	);

	for (size_t i = 0; i + 2 < OutMesh.Indices.size(); i += 3)
	{
		const uint32 I0 = OutMesh.Indices[i + 0];
		const uint32 I1 = OutMesh.Indices[i + 1];
		const uint32 I2 = OutMesh.Indices[i + 2];

		const FImportedSkeletalVertex& V0 = OutMesh.SkeletalVertices[I0];
		const FImportedSkeletalVertex& V1 = OutMesh.SkeletalVertices[I1];
		const FImportedSkeletalVertex& V2 = OutMesh.SkeletalVertices[I2];

		const FVector Edge1 = V1.Position - V0.Position;
		const FVector Edge2 = V2.Position - V0.Position;

		const FVector2 DeltaUV1 = V1.UV - V0.UV;
		const FVector2 DeltaUV2 = V2.UV - V0.UV;

		const float Det = DeltaUV1.X * DeltaUV2.Y - DeltaUV2.X * DeltaUV1.Y;
		if (std::abs(Det) < 1e-8f)
		{
			continue;
		}

		const float InvDet = 1.0f / Det;

		const FVector Tangent = (Edge1 * DeltaUV2.Y - Edge2 * DeltaUV1.Y) * InvDet;
		const FVector Bitangent = (Edge2 * DeltaUV1.X - Edge1 * DeltaUV2.X) * InvDet;

		TangentSums[I0] += Tangent;
		TangentSums[I1] += Tangent;
		TangentSums[I2] += Tangent;

		BitangentSums[I0] += Bitangent;
		BitangentSums[I1] += Bitangent;
		BitangentSums[I2] += Bitangent;
	}

	for (size_t i = 0; i < OutMesh.SkeletalVertices.size(); ++i)
	{
		FImportedSkeletalVertex& V = OutMesh.SkeletalVertices[i];

		FVector N = V.Normal.Normalized();
		FVector T = TangentSums[i];

		T = T - N * N.Dot(T);

		if (T.Length() < 1e-8f)
		{
			FVector Axis = std::abs(N.Z) < 0.999f
				? FVector(0.0f, 0.0f, 1.0f)
				: FVector(0.0f, 1.0f, 0.0f);

			T = Axis.Cross(N).Normalized();
		}
		else
		{
			T.Normalize();
		}

		const FVector B = BitangentSums[i];
		const float Handedness = N.Cross(T).Dot(B) < 0.0f ? -1.0f : 1.0f;

		V.Tangent = FVector4(T, Handedness);
	}
}


