#include "FbxImporter.h"
#include "Platform/Paths.h"
#include "Core/Log.h"
#include "Math/Matrix.h"
#include "Mesh/StaticMesh.h"
#include "Mesh/StaticMeshAsset.h"
#include "Object/ObjectFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialManager.h"

#include <algorithm>
namespace
{
	// 내부에서만 사용하는 함수 모음 (Codex 아님 내가 다 씀)

	// FbxVector4 -> FVector3 Convert 함수
	// Fbx 단위 -> Engine 내부 단위로 변환
	FVector Convert(FbxVector4 Pos)
	{
		return FVector(
			static_cast<float>(Pos[0]),
			static_cast<float>(Pos[1]),
			static_cast<float>(Pos[2])
		);
	}

	FVector GetNormal(FbxMesh* Mesh, int32 pIdx, int32 corner)
	{
		FbxVector4 fbxNormal;
		Mesh->GetPolygonVertexNormal(pIdx, corner, fbxNormal);
		return FVector(
			static_cast<float>(fbxNormal[0]),
			static_cast<float>(fbxNormal[1]),
			static_cast<float>(fbxNormal[2])
		);
	}

	FVector2 GetUV(FbxMesh* Mesh, int pIndex, int corner, const char* uvSetName)
	{
		FbxVector2 fbxUV;
		bool unmapped = false;
		if (uvSetName && Mesh->GetPolygonVertexUV(pIndex, corner, uvSetName, fbxUV, unmapped))
		{
			return FVector2(
				static_cast<float>(fbxUV[0]),
				1.0f - static_cast<float>(fbxUV[1])	// Directx11에 맞게 변환
			);
		}
		else
		{
			return FVector2(0.0f, 0.0f);
		}
	}

	FVector4 GetTangent()
	{
		return FVector4(
			0.f, 0.f, 0.f, 0.f
		);
	}

	void NormalizeAndLimitTo4(TArray<BoneInfluence> influences, uint32* BoneIndices, float* BoneWeights)
	{
		int cnt = 0;
		float WeightSum = 0.f;

		// TODO: influence Sorting, 큰 weight 우선으로 정렬하는 게 좋음
		/*influences.sort([](const BoneInfluence& A, const BoneInfluence& B)
			{
				return A.boneWeight > B.boneWeight;
			});*/

		for (auto influence : influences)
		{
			if (cnt >= 4) break;

			BoneIndices[cnt] = influence.boneIndex;
			BoneWeights[cnt] = influence.boneWeight;

			WeightSum += BoneWeights[cnt];
			cnt++;
		}
		if (WeightSum > 1e-6f)
		{
			for (int i = 0; i < cnt; ++i)
			{
				BoneWeights[i] /= WeightSum;
			}
		}
		// TODO 합이 1이 되게 다시 조정
	}

	FVector BuildFallbackTangent(const FVector& N)
	{
		FVector Axis = std::abs(N.Y) < 0.9f
			? FVector(0.f, 1.f, 0.f)
			: FVector(1.f, 0.f, 0.f);

		FVector T = Axis - N * N.Dot(Axis);
		T.Normalize();

		return T;
	}
}

bool FFbxImporter::Import(const FString& FilePath)
{
	// import 전 clear
	vertices.clear();
	indices.clear();
	InfluencesPerControlPoint.clear();
	BoneNodeToIndex.clear();
	Bones.clear();

	FbxManager* SdkManager = FbxManager::Create();

	FbxIOSettings* ios = FbxIOSettings::Create(SdkManager, IOSROOT);
	SdkManager->SetIOSettings(ios);

	FbxScene* Scene = FbxScene::Create(SdkManager, "My Scene");

	FbxImporter* Importer = FbxImporter::Create(SdkManager, "");

	FString FullPath = FPaths::ToUtf8(FPaths::Combine(FPaths::AssetDir(), FPaths::ToWide(FilePath)));

	bool bInitialized = Importer->Initialize(FullPath.c_str(), -1, SdkManager->GetIOSettings());
	if (!bInitialized)
	{
		UE_LOG("Failed To Initialize Import");
		Importer->Destroy();
		SdkManager->Destroy();
		return false;
	}

	Importer->Import(Scene);

	// FBX 내부 Mesh, NURBS, Patch 등 삼각화 가능한 친구들 geometry를 삼각형으로 변환
	FbxGeometryConverter Converter(SdkManager);
	const bool bTriangulated = Converter.Triangulate(Scene, true);
	if (!bTriangulated)
	{
		UE_LOG("[FBXImporter] Failed To Trianglulate FBX Mesh");
	}
	Importer->Destroy();

	NormalizeCoordinateUnit(Scene);

	ProcessNode(Scene->GetRootNode());

	SdkManager->Destroy();
	return true;
}

// 재귀적 node 순회
void FFbxImporter::ProcessNode(FbxNode* Node)
{
	if (!Node) return;
	const char* NodeName = Node->GetName();
	FbxNodeAttribute* Attribute = Node->GetNodeAttribute();

	if (Attribute)
	{
		if (Attribute->GetAttributeType() == FbxNodeAttribute::eMesh)
		{
			FbxMesh* Mesh = static_cast<FbxMesh*>(Attribute);

			if (Mesh)
			{
				PreLoadCluster(Mesh);
				ProcessPolygon(Node);
			}
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		ProcessNode(Node->GetChild(i));
	}
}

void FFbxImporter::ProcessMesh(FbxNode* Node)
{
	FbxMesh* Mesh = Node->GetMesh();
	FbxVector4* Vertices = Mesh->GetControlPoints();


	for (int i = 0; i < Mesh->GetControlPointsCount(); ++i)
	{
		float x = (float)Vertices[i][0];
		float y = (float)Vertices[i][1];
		float z = (float)Vertices[i][2];

		UE_LOG("Mesh Vertex: (%f, %f, %f)", x, y, z);
	}
}

void FFbxImporter::ProcessPolygon(FbxNode* Node)
{
	FbxMesh* Mesh = Node->GetMesh();

	// Temp UV Name Code
	const char* uvSetName = nullptr;
	FbxStringList uvSetNames;
	Mesh->GetUVSetNames(uvSetNames);
	if (uvSetNames.GetCount() > 0)
	{
		uvSetName = uvSetNames.GetStringAt(0);
	}

	// Node의 GlobalTransform
	FbxAMatrix GlobalTransform = Node->EvaluateGlobalTransform();

	// mesh polygon 순회
	for (int32 pIdx = 0; pIdx < Mesh->GetPolygonCount(); ++pIdx)
	{
		// int materialIdx = Mesh->GetMaterialIndices();

		for (int32 corner = 0; corner < Mesh->GetPolygonSize(pIdx); ++corner)
		{
			int cpIndex = Mesh->GetPolygonVertex(pIdx, corner); //Polygon이 사용하는 Control Point Idx
			FSkeletalMeshVertex v = {};

			// FbxVector4 -> FVector3로 변환
			FbxVector4 LocalPos = Mesh->GetControlPoints()[cpIndex]; 
			FbxVector4 WorldPos = GlobalTransform.MultT(LocalPos);
			v.Position = Convert(WorldPos);
			// v.Position = Convert(Mesh->GetControlPoints()[cpIndex]);
			// FBX -> Normal 변환
			v.Normal = GetNormal(Mesh, pIdx, corner);
			// FBX UV -> 프로젝트 기준 UV로 변환해주는 
			v.UV = GetUV(Mesh, pIdx, corner, uvSetName);
			v.Tangent = FVector4(0.f, 0.f, 0.f, 0.f);
			// v.MaterialIndex 나중에 연결

			// Bone Indices, Bone Weight 채우기
			auto influences = InfluencesPerControlPoint[cpIndex]; // cpIndex에 해당하는 Cluster 가져오기
			NormalizeAndLimitTo4(influences, v.BoneIndices, v.BoneWeights);

			uint32 VertexIndex = FindOrAddVertex(v);
			indices.push_back(VertexIndex);
		}
	}

	// 마지막으로 Tangent값 계산해서 넣어주기
	ComputeTangents();
}

UStaticMesh* FFbxImporter::ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device)
{
	if (!Import(FilePath))
	{
		UE_LOG("[FBXImport] Import failed: %s", FilePath.c_str());
		return nullptr;
	}

	if (vertices.empty() || indices.empty())
	{
		UE_LOG("[FBXImport] Import mesh is empty : %s", FilePath.c_str());
		return nullptr;
	}
	return BuildStaticMeshFromImportedData(FilePath, Device);
}

// Control Point기준 influence Map으로 변환
void FFbxImporter::PreLoadCluster(FbxMesh* mesh)
{
	InfluencesPerControlPoint.clear();
	InfluencesPerControlPoint.resize(mesh->GetControlPointsCount());

	// skin deformer 개수만큼 순회
	for (int32 skinidx = 0; skinidx < mesh->GetDeformerCount(FbxDeformer::eSkin); ++skinidx)
	{
		FbxSkin* skin = static_cast<FbxSkin*>(mesh->GetDeformer(skinidx, FbxDeformer::eSkin));

		// Cluster 개수만큼 순회
		for (int clusteridx = 0; clusteridx < skin->GetClusterCount(); ++clusteridx)
		{
			// cluster 정보 수집
			FbxCluster* cluster = skin->GetCluster(clusteridx);

			// cluster가 영향 미치는 bone 정보 수집
			FbxNode* boneNode = cluster->GetLink();

			// bone Node값 기반으로 bone의 index 정보 가져오기
			int boneindex = FindOrAddBone(boneNode);

			int count = cluster->GetControlPointIndicesCount(); // Control Point의 indices 배열 길이
			int* cpIndices = cluster->GetControlPointIndices(); // 실제 배열 값
			double* weights = cluster->GetControlPointWeights(); // Cluster의 Weight 값

			for (int i = 0; i < count; i++)
			{
				int cpIndex = cpIndices[i]; //control point index;
				float weight = static_cast<float>(weights[i]);
				InfluencesPerControlPoint[cpIndex].push_back({ boneindex, weight });
			}
		}
	}
}

int FFbxImporter::FindOrAddBone(FbxNode* BoneNode)
{
	if (!BoneNode) return -1;

	auto it = BoneNodeToIndex.find(BoneNode);
	if (it != BoneNodeToIndex.end())
	{
		return it->second;
	}

	// 새로할당된 BoneNode의 index
	const int32 newIndex = static_cast<int>(Bones.size());

	EngineBone bone;
	bone.Name = BoneNode->GetName();
	bone.ParentIndex = -1; //todo: 채우기
	bone.FbxNode = BoneNode;

	Bones.push_back(bone);
	BoneNodeToIndex[BoneNode] = newIndex;

	return newIndex;
}

int FFbxImporter::FindOrAddVertex(FSkeletalMeshVertex Vertex)
{
	//auto it = VertexToIndex.find(Vertex);

	//// 있으면 값 반환
	//if (it != VertexToIndex.end())
	//{
	//	return it->second;
	//}

	//// 업는 경우 새로 추가 
	const int32 newIndex = static_cast<int32>(vertices.size());

	vertices.push_back(Vertex);
	// VertexToIndex[Vertex] = newIndex;

	return newIndex;
}

void FFbxImporter::ComputeTangents()
{
	// Resize & Initialize
	TArray<FVector> TangentAccum;
	TArray<FVector> BitangentAccum;

	TangentAccum.resize(vertices.size());
	BitangentAccum.resize(vertices.size());

	for (int32 i = 0; i < static_cast<int32>(TangentAccum.size()); ++i)
	{
		TangentAccum[i] = FVector(0.f, 0.f, 0.f);
		BitangentAccum[i] = FVector(0.f, 0.f, 0.f);
	}

	// Tangent, Bitangent 구하기
	for (int32 i = 0; i + 2 < static_cast<int32>(indices.size()); i += 3)
	{
		const int32 i0 = indices[i + 0];
		const int32 i1 = indices[i + 1];
		const int32 i2 = indices[i + 2];

		if (i0 < 0 || i1 < 0 || i2 < 0) continue;

		int32 VerticesSize = static_cast<int32>(vertices.size());

		if (i0 >= VerticesSize || i1 >= VerticesSize || i2 >= VerticesSize) continue;

		const FVector& P0 = vertices[i0].Position;
		const FVector& P1 = vertices[i1].Position;
		const FVector& P2 = vertices[i2].Position;

		const FVector2 UV0 = vertices[i0].UV;
		const FVector2 UV1 = vertices[i1].UV;
		const FVector2 UV2 = vertices[i2].UV;

		const FVector Edge1 = P1 - P0;
		const FVector Edge2 = P2 - P0;

		const FVector2 DeltaUV1 = UV1 - UV0;
		const FVector2 DeltaUV2 = UV2 - UV0;

		const float Det =
			DeltaUV1.X * DeltaUV2.Y -
			DeltaUV2.X * DeltaUV1.Y;

		if (std::abs(Det) <= 1e-8f)
		{
			continue;
		}

		const float InvDet = 1.0f / Det;

		const FVector Tangent =
			(Edge1 * DeltaUV2.Y - Edge2 * DeltaUV1.Y) * InvDet;

		const FVector Bitangent =
			(Edge2 * DeltaUV1.X - Edge1 * DeltaUV2.X) * InvDet;

		TangentAccum[i0] += Tangent;
		TangentAccum[i1] += Tangent;
		TangentAccum[i2] += Tangent;

		BitangentAccum[i0] += Bitangent;
		BitangentAccum[i1] += Bitangent;
		BitangentAccum[i2] += Bitangent;
	}

	for (int32 i = 0; i < static_cast<int32>(vertices.size()); ++i)
	{
		FVector N = vertices[i].Normal.Normalized();
		FVector T = TangentAccum[i];

		// Gram-Schmidt orthogonalization
		T = T - N * N.Dot(T);
		if (T.Length() < 1e-4f)
		{
			T = BuildFallbackTangent(N);
		}

		const FVector B = BitangentAccum[i];

		//Tangent w는 bitangent 방향 부호
		const float Handedness = N.Cross(T).Dot(B) < 0.0f ? -1.0f : 1.0f;
		vertices[i].Tangent = FVector4(T.X, T.Y, T.Z, Handedness);
	}
}

void FFbxImporter::NormalizeCoordinateUnit(FbxScene* Scene)
{
	if (!Scene)
	{
		UE_LOG("[FBXImporter] Failed to Normalize, Scene is nullptr");
		return;
	}

	// Unit 적용
	FbxSystemUnit SceneUnit = Scene->GetGlobalSettings().GetSystemUnit();
	if (SceneUnit != FbxSystemUnit::cm)
	{
		FbxSystemUnit::cm.ConvertScene(Scene);
	}

	// Coordinate 적용
	FbxAxisSystem TargetAxisSystem(FbxAxisSystem::eXAxis, FbxAxisSystem::eParityOdd, FbxAxisSystem::eLeftHanded);
	FbxAxisSystem SceneAxisSystem = Scene->GetGlobalSettings().GetAxisSystem();
	if (SceneAxisSystem != TargetAxisSystem)
	{
		TargetAxisSystem.ConvertScene(Scene);
	}
}

// 임시로 화면에 띄우기 위해 FBX 파일을 SkeletalMesh로 변환
UStaticMesh* FFbxImporter::BuildStaticMeshFromImportedData(const FString& FilePath, ID3D11Device* Device)
{
	if (!Device)
	{
		UE_LOG("[FBXImporter] Device is null");
		return nullptr;
	}

	FStaticMesh* MeshAsset = new FStaticMesh;
	MeshAsset->PathFileName = FilePath;

	MeshAsset->Vertices.reserve(vertices.size());
	MeshAsset->Indices.reserve(indices.size());

	// Vertex 설정
	for (const FSkeletalMeshVertex& src : vertices)
	{
		FNormalVertex Dst{};
		Dst.pos = src.Position;
		Dst.normal = src.Normal;
		Dst.tex = src.UV;
		Dst.tangent = src.Tangent;

		Dst.color = FVector4(1.f, 1.f, 1.f, 1.f);
		MeshAsset->Vertices.push_back(Dst);
	}

	for (int32 Idx : indices)
	{
		MeshAsset->Indices.push_back(Idx);
	}

	// Static Mesh Material Section 설정
	FStaticMeshSection Section{};
	Section.MaterialSlotName = "None";
	Section.MaterialIndex = 0;
	Section.FirstIndex = 0;
	Section.NumTriangles = static_cast<uint32>(MeshAsset->Indices.size() / 3);
	MeshAsset->Sections.push_back(Section);

	MeshAsset->CacheBounds();

	TArray<FStaticMaterial> Materials;
	FStaticMaterial DefaultMaterial{};
	DefaultMaterial.MaterialSlotName = "None";

	// Use None
	DefaultMaterial.MaterialInterface = FMaterialManager::Get().GetOrCreateMaterial("None");

	Materials.push_back(DefaultMaterial);

	UStaticMesh* StaticMesh = UObjectManager::Get().CreateObject<UStaticMesh>();
	StaticMesh->SetStaticMaterials(std::move(Materials));
	StaticMesh->SetStaticMeshAsset(MeshAsset);
	StaticMesh->InitResources(Device);

	return StaticMesh;
}
