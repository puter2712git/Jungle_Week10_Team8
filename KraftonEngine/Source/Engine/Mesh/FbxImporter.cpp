#include "FbxImporter.h"
#include "Platform/Paths.h"
#include "Core/Log.h"
#include "Engine/Mesh/SkeletalMesh.h"

#include <fbxsdk.h>


namespace FBX {
	// 참고용
	struct FSkeletalVertex
	{
		FVector pos;
		FVector normal;
		FVector4 color;
		FVector2 tex;
		FVector4 tangent;
		uint32 BoneIDs[4];
		float BoneWeights[4];
	};

	struct FImportedBone
	{
		FString Name;
		int32 ParentIndex = -1;
		fbxsdk::FbxNode* Node = nullptr;
	};

	struct FImportContext
	{
		fbxsdk::FbxManager* Manager = nullptr;
		FSkeletalMesh Mesh;
		TArray<FImportedBone> Bones;
		std::unordered_map<fbxsdk::FbxNode*, int32> NodeToBoneIndex;
	};

	struct FVertexKey
	{
		uint32 PosIndex;
		uint32 NormalIndex;
		uint32 TangentIndex;
		uint32 UVIndex;
		bool operator==(const FVertexKey& Other) const
		{
			return PosIndex == Other.PosIndex && NormalIndex == Other.NormalIndex && TangentIndex == Other.TangentIndex && UVIndex == Other.UVIndex;
		}
	};

	struct FVertexKeyHash
	{
		size_t operator()(const FVertexKey& Key) const
		{
			return ((size_t)Key.PosIndex) ^ (((size_t)Key.NormalIndex) << 8) ^ (((size_t)Key.TangentIndex) << 16) ^ (((size_t)Key.UVIndex) << 24);
		}
	};

	static const char* ToString(fbxsdk::FbxLayerElement::EMappingMode MappingMode)
	{
		switch (MappingMode)
		{
		case fbxsdk::FbxLayerElement::eNone: return "eNone";
		case fbxsdk::FbxLayerElement::eByControlPoint: return "eByControlPoint";
		case fbxsdk::FbxLayerElement::eByPolygonVertex: return "eByPolygonVertex";
		case fbxsdk::FbxLayerElement::eByPolygon: return "eByPolygon";
		case fbxsdk::FbxLayerElement::eByEdge: return "eByEdge";
		case fbxsdk::FbxLayerElement::eAllSame: return "eAllSame";
		default: return "UnknownMappingMode";
		}
	}

	static const char* ToString(fbxsdk::FbxLayerElement::EReferenceMode ReferenceMode)
	{
		switch (ReferenceMode)
		{
		case fbxsdk::FbxLayerElement::eDirect: return "eDirect";
		case fbxsdk::FbxLayerElement::eIndex: return "eIndex";
		case fbxsdk::FbxLayerElement::eIndexToDirect: return "eIndexToDirect";
		default: return "UnknownReferenceMode";
		}
	}

	template <typename LayerElementType>
	static void LogUnsupportedLayer(const char* LayerName, const LayerElementType* Layer)
	{
		if (Layer == nullptr)
		{
			UE_LOG("Unsupported %s layer. layer=null", LayerName);
			return;
		}

		UE_LOG(
			"Unsupported %s layer. map=%s, ref=%s",
			LayerName,
			ToString(Layer->GetMappingMode()),
			ToString(Layer->GetReferenceMode()));
	}

}

static void ProcessMesh(FbxNode* Node, FBX::FImportContext& Context)
{
	fbxsdk::FbxMesh* Mesh = Node->GetMesh();

	TArray<FSkeletalVertex> Vertices;
	TArray<uint32> Indices;
	std::unordered_map<FBX::FVertexKey, uint32, FBX::FVertexKeyHash> VertexCache;

	const uint32 ControlPointCount = Mesh->GetControlPointsCount();
	TArray<TStaticArray<uint32, 4>> ControlPointBoneIDs(ControlPointCount, { 0, 0, 0, 0});
	TArray<TStaticArray<float, 4>> ControlPointBoneWeights(ControlPointCount, { 0.0f, 0.0f, 0.0f, 0.0f });

	for (int32 DeformerIndex = 0; DeformerIndex < Mesh->GetDeformerCount(); ++DeformerIndex)
	{
		fbxsdk::FbxDeformer* Deformer = Mesh->GetDeformer(DeformerIndex);
		if (Deformer->GetDeformerType() != fbxsdk::FbxDeformer::eSkin)
		{
			continue;
		}
		fbxsdk::FbxSkin* Skin = static_cast<fbxsdk::FbxSkin*>(Deformer);
		for (int32 ClusterIndex = 0; ClusterIndex < Skin->GetClusterCount(); ++ClusterIndex)
		{
			fbxsdk::FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
			fbxsdk::FbxNode* BoneNode = Cluster->GetLink();
			if (!BoneNode)
			{
				continue;
			}
			auto BoneIt = Context.NodeToBoneIndex.find(BoneNode);
			if (BoneIt == Context.NodeToBoneIndex.end())
			{
				continue;
			}
			const int32 BoneIndex = BoneIt->second;
			int32 ControlPointIndexCount = Cluster->GetControlPointIndicesCount();
			const int* ControlPointIndices = Cluster->GetControlPointIndices();
			const double* ControlPointWeights = Cluster->GetControlPointWeights();
			for (int32 i = 0; i < ControlPointIndexCount; ++i)
			{
				int32 ControlPointIndex = ControlPointIndices[i];
				float Weight = static_cast<float>(ControlPointWeights[i]);
				TStaticArray<uint32, 4>& BoneIDs = ControlPointBoneIDs[ControlPointIndex];
				TStaticArray<float, 4>& BoneWeights = ControlPointBoneWeights[ControlPointIndex];
				for (int j = 0; j < 4; ++j)
				{
					if (Weight > BoneWeights[j])
					{
						for (int k = 3; k > j; --k)
						{
							BoneIDs[k] = BoneIDs[k - 1];
							BoneWeights[k] = BoneWeights[k - 1];
						}
						BoneIDs[j] = static_cast<uint32>(BoneIndex);
						BoneWeights[j] = Weight;
						break;
					}
				}
			}
		}
	}

	UE_LOG("Polygon Count : %d", Mesh->GetPolygonCount());

	if (Mesh->GetElementNormalCount() == 0)
	{
		UE_LOG("No normal data found. Generating normals.");
		if (!Mesh->GenerateNormals())
		{
			UE_LOG("Failed to generate normals.");
		}
	}

	fbxsdk::FbxLayerElementNormal* pNormalLayer = Mesh->GetElementNormal(0);
	fbxsdk::FbxLayerElementUV* pUVLayer = Mesh->GetElementUV(0);

	const bool bHasNormalLayer = pNormalLayer != nullptr
		&& (pNormalLayer->GetMappingMode() == fbxsdk::FbxLayerElement::eByPolygonVertex
			|| pNormalLayer->GetMappingMode() == fbxsdk::FbxLayerElement::eByControlPoint)
		&& pNormalLayer->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;

	const bool bHasUVLayer = pUVLayer != nullptr
		&& pUVLayer->GetMappingMode() == fbxsdk::FbxLayerElement::eByPolygonVertex
		&& (pUVLayer->GetReferenceMode() == fbxsdk::FbxLayerElement::eDirect
			|| pUVLayer->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect);

	fbxsdk::FbxLayerElementTangent* pTangentLayer = Mesh->GetElementTangent(0);
	const bool bHasTangentLayer = pTangentLayer != nullptr
		&& pTangentLayer->GetMappingMode() == fbxsdk::FbxLayerElement::eByPolygonVertex
		&& pTangentLayer->GetReferenceMode() == fbxsdk::FbxLayerElement::eDirect;

	if (!bHasNormalLayer)
	{
		FBX::LogUnsupportedLayer("normal", pNormalLayer);
		return;
	}

	if (!bHasTangentLayer)
	{
		UE_LOG("Unsupported tangent layer. Filling zero tangents.");
	}

	if (!bHasUVLayer)
	{
		UE_LOG("Unsupported UV layer. Filling zero UVs.");
	}

	int VertexCounter = 0;

	for (int PolygonIndex = 0; PolygonIndex < Mesh->GetPolygonCount(); ++PolygonIndex)
	{
		int PolygonSize = Mesh->GetPolygonSize(PolygonIndex);
		// 앵간하면 3이지 안을가
		if (PolygonSize > 3)
		{
			UE_LOG("비상");
		}
		for (int i = 0; i < PolygonSize; ++i)
		{
			uint32 ControlPointIndex = Mesh->GetPolygonVertex(PolygonIndex, i);
			fbxsdk::FbxVector4 Position = Mesh->GetControlPointAt(ControlPointIndex);

			const int NormalIndex = pNormalLayer->GetMappingMode() == fbxsdk::FbxLayerElement::eByControlPoint
				? pNormalLayer->GetIndexArray().GetAt(ControlPointIndex)
				: pNormalLayer->GetIndexArray().GetAt(VertexCounter);
			const fbxsdk::FbxVector4 Normal = pNormalLayer->GetDirectArray().GetAt(NormalIndex);

			const fbxsdk::FbxVector4 Tangent = bHasTangentLayer
				? pTangentLayer->GetDirectArray().GetAt(VertexCounter)
				: fbxsdk::FbxVector4(0.0, 0.0, 0.0, 0.0);

			const int UVIndex = bHasUVLayer && pUVLayer->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect
				? pUVLayer->GetIndexArray().GetAt(VertexCounter)
				: VertexCounter;
			const fbxsdk::FbxVector2 UV = bHasUVLayer
				? pUVLayer->GetDirectArray().GetAt(UVIndex)
				: fbxsdk::FbxVector2(0.0, 0.0);

			FBX::FVertexKey Key{ ControlPointIndex, (uint32)NormalIndex, (uint32)VertexCounter, (uint32)UVIndex };
			auto CacheIt = VertexCache.find(Key);

			if (CacheIt != VertexCache.end())
			{
				Indices.push_back(CacheIt->second);
			}
			else
			{
				FSkeletalVertex Vertex{};
				Vertex.pos = FVector(Position[0], Position[1], Position[2]);
				Vertex.normal = FVector(Normal[0], Normal[1], Normal[2]);
				Vertex.tex = FVector2(UV[0], UV[1]);
				Vertex.tangent = FVector4(Tangent[0], Tangent[1], Tangent[2], 1.0f);
				float TotalBoneWeight = 0.0f;
				for (int j = 0; j < 4; ++j)
				{
					Vertex.BoneIDs[j] = ControlPointBoneIDs[ControlPointIndex][j];
					Vertex.BoneWeights[j] = ControlPointBoneWeights[ControlPointIndex][j];
					TotalBoneWeight += Vertex.BoneWeights[j];
				}
				if (TotalBoneWeight > 0.0f)
				{
					for (int j = 0; j < 4; ++j)
					{
						Vertex.BoneWeights[j] /= TotalBoneWeight;
					}
				}
				uint32 NewIndex = (uint32)Vertices.size();
				Vertices.push_back(Vertex);
				Indices.push_back(NewIndex);
				VertexCache[Key] = NewIndex;
			}
			VertexCounter += 1;
		}
	}

	const uint32 BaseVertexIndex = static_cast<uint32>(Context.Mesh.Vertices.size());
	Context.Mesh.Vertices.insert(Context.Mesh.Vertices.end(), Vertices.begin(), Vertices.end());
	const uint32 FirstIndex = static_cast<uint32>(Context.Mesh.Indices.size());
	for (uint32 Index : Indices)
	{
		Context.Mesh.Indices.push_back(BaseVertexIndex + Index);
	}

	if (!Indices.empty())
	{
		FSkeletalMeshSection Section;
		Section.MaterialSlotName = "None";
		if (Node->GetMaterialCount())
		{
			Section.MaterialSlotName = Node->GetMaterial(0)->GetName();
		}
		Section.FirstIndex = FirstIndex;
		Section.NumTriangles = static_cast<uint32>(Indices.size() / 3);
		Context.Mesh.Sections.push_back(Section);
	}
}

static void ProcessSkeleton(fbxsdk::FbxNode* Node, FBX::FImportContext& Context)
{
	if (Context.NodeToBoneIndex.find(Node) != Context.NodeToBoneIndex.end())
	{
		UE_LOG("Node %s already processed as bone. Skipping.", Node->GetName());
		return;
	}

	fbxsdk::FbxNode* ParentNode = Node->GetParent();
	if (ParentNode && Context.NodeToBoneIndex.find(ParentNode) == Context.NodeToBoneIndex.end())
	{
		FbxNodeAttribute* ParentAttribute = ParentNode->GetNodeAttribute();
		if (ParentAttribute != nullptr)
		{
			switch (ParentAttribute->GetAttributeType())
			{
			case fbxsdk::FbxNodeAttribute::eNull:
			case fbxsdk::FbxNodeAttribute::eSkeleton:
				ProcessSkeleton(ParentNode, Context);
				break;
			}
		}
	}

	FBX::FImportedBone Bone;
	Bone.Node = Node;
	Bone.Name = Node->GetName();

	if (ParentNode)
	{
		auto It = Context.NodeToBoneIndex.find(ParentNode);
		if (It != Context.NodeToBoneIndex.end())
		{
			Bone.ParentIndex = It->second;
		}
		//else Bone.ParentIndex = -1;
	}

	const int32 NewBoneIndex = static_cast<int32>(Context.Bones.size());
	Context.Bones.push_back(Bone);
	Context.NodeToBoneIndex[Node] = NewBoneIndex;
}

static void CollectSkeletonNodes(fbxsdk::FbxNode* Node, FBX::FImportContext& Context)
{
	FbxNodeAttribute* Attribute = Node->GetNodeAttribute();
	if (Attribute)
	{
		switch (Attribute->GetAttributeType())
		{
		case fbxsdk::FbxNodeAttribute::eNull: // 1, Armature
			break;
		case fbxsdk::FbxNodeAttribute::eSkeleton: // 3, Bone
			ProcessSkeleton(Node, Context);
			break;
		case fbxsdk::FbxNodeAttribute::eMesh: // 4, Mesh
			if (fbxsdk::FbxMesh* Mesh = Node->GetMesh())
			{
				for (int32 DeformerIndex = 0; DeformerIndex < Mesh->GetDeformerCount(); ++DeformerIndex)
				{
					fbxsdk::FbxDeformer* Deformer = Mesh->GetDeformer(DeformerIndex);
					if (Deformer->GetDeformerType() != fbxsdk::FbxDeformer::eSkin)
					{
						continue;
					}

					fbxsdk::FbxSkin* Skin = static_cast<fbxsdk::FbxSkin*>(Deformer);
					for (int32 ClusterIndex = 0; ClusterIndex < Skin->GetClusterCount(); ++ClusterIndex)
					{
						fbxsdk::FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
						fbxsdk::FbxNode* BoneNode = Cluster->GetLink();
						if (BoneNode)
						{
							ProcessSkeleton(BoneNode, Context);
						}
					}
				}
			}
			break;
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		UE_LOG("[Skeleton] Root Node: %s -> Child Node: %s", Node->GetName(), Node->GetChild(i)->GetName());
		CollectSkeletonNodes(Node->GetChild(i), Context);
	}
}

static void ProcessNode(fbxsdk::FbxNode* Node, FBX::FImportContext& Context)
{
	FbxNodeAttribute* Attribute = Node->GetNodeAttribute();
	if (Attribute)
	{
		switch (Attribute->GetAttributeType())
		{
		case fbxsdk::FbxNodeAttribute::eNull: // 1
			break;
		case fbxsdk::FbxNodeAttribute::eSkeleton: // 3
			// 이미 CollectSkeletonNodes에서 처리했음
			break;
		case fbxsdk::FbxNodeAttribute::eMesh: // 4
			ProcessMesh(Node, Context);
			break;
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		ProcessNode(Node->GetChild(i), Context);
	}
}

bool FFbxImporter::Import(const FString& FilePath, FSkeletalMesh& OutMesh)
{
	fbxsdk::FbxManager* SdkManager = fbxsdk::FbxManager::Create();

	fbxsdk::FbxIOSettings* ios = fbxsdk::FbxIOSettings::Create(SdkManager, IOSROOT);
	SdkManager->SetIOSettings(ios);

	fbxsdk::FbxScene* Scene = fbxsdk::FbxScene::Create(SdkManager, "My Scene");

	fbxsdk::FbxImporter* Importer = fbxsdk::FbxImporter::Create(SdkManager, "");

	FString FullPath = FPaths::ToUtf8(FPaths::Combine(FPaths::AssetDir(), FPaths::ToWide(FilePath)));

	if (!Importer->Initialize(FullPath.c_str(), -1, SdkManager->GetIOSettings()))
	{
		UE_LOG("Failed to initialize FBX importer for file: %s", FullPath.c_str());
		return false;
	}

	Importer->Import(Scene);
	Importer->Destroy();

	fbxsdk::FbxGeometryConverter GeometryConverter(SdkManager);
	if (!GeometryConverter.Triangulate(Scene, true))
	{
		UE_LOG("Scene triangulation failed.");
	}
	 
	FBX::FImportContext Context{};
	Context.Mesh.PathFileName = FilePath;
	Context.Mesh.Vertices.clear();
	Context.Mesh.Indices.clear();
	Context.Mesh.Sections.clear();
	Context.Mesh.bBoundsValid = false;

	Context.Manager = SdkManager;

	CollectSkeletonNodes(Scene->GetRootNode(), Context);
	ProcessNode(Scene->GetRootNode(), Context);
	Context.Mesh.CacheBounds();
	UE_LOG("FBX Import completed. Vertex Count: %d, Index Count: %d, Section Count: %d, Bone Count: %d",
		Context.Mesh.Vertices.size(),
		Context.Mesh.Indices.size(),
		Context.Mesh.Sections.size(),
		Context.Bones.size());
	OutMesh = std::move(Context.Mesh);

	Scene->Destroy();
	SdkManager->Destroy();
	return true;
}
