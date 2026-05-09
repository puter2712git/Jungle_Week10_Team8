#include "FbxImporter.h"
#include "Platform/Paths.h"
#include "Core/Log.h"
#include "Math/Matrix.h"
#include "Mesh/SkeletalMesh.h"
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

	void NormalizeAndLimitTo4(const TArray<BoneInfluence>& influences, uint32* BoneIndices, float* BoneWeights, int32 BoneCount)
	{
		int cnt = 0;
		float WeightSum = 0.f;

		for (int i = 0; i < 4; ++i)
		{
			BoneIndices[i] = 0;
			BoneWeights[i] = 0.0f;
		}

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
		else if (BoneCount > 0)
		{
			BoneIndices[0] = 0;
			BoneWeights[0] = 1.0f;
		}
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

	void PrintMatrixT(const char* Label, const FbxAMatrix& M)
	{
		FbxVector4 T = M.GetT();
		FbxVector4 R = M.GetR();
		FbxVector4 S = M.GetS();

		UE_LOG("%s T=(%.3f %.3f %.3f) R=(%.3f %.3f %.3f) S=(%.3f %.3f %.3f)",
			Label,
			T[0], T[1], T[2],
			R[0], R[1], R[2],
			S[0], S[1], S[2]);
	}

	FbxNode* FindNodeByName(FbxNode* Node, const FString& NodeName)
	{
		if (!Node) return nullptr;
		if (Node->GetName() && NodeName == Node->GetName())
		{
			return Node;
		}

		for (int32 i = 0; i < Node->GetChildCount(); ++i)
		{
			if (FbxNode* Found = FindNodeByName(Node->GetChild(i), NodeName))
			{
				return Found;
			}
		}

		return nullptr;
	}

	bool IsSkeletonNode(FbxNode* Node)
	{
		if (!Node) return false;
		FbxNodeAttribute* Attr = Node->GetNodeAttribute();
		return Attr && Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton;
	}

	FbxNode* FindParentSkeletonNode(FbxNode* Node)
	{
		for (FbxNode* Parent = Node ? Node->GetParent() : nullptr; Parent; Parent = Parent->GetParent())
		{
			if (IsSkeletonNode(Parent))
			{
				return Parent;
			}
		}
		return nullptr;
	}

	FMatrix ConvertFbxMatrix(const FbxAMatrix& M)
	{
		const FbxVector4 Origin = M.MultT(FbxVector4(0.0, 0.0, 0.0));
		const FbxVector4 AxisX = M.MultT(FbxVector4(1.0, 0.0, 0.0)) - Origin;
		const FbxVector4 AxisY = M.MultT(FbxVector4(0.0, 1.0, 0.0)) - Origin;
		const FbxVector4 AxisZ = M.MultT(FbxVector4(0.0, 0.0, 1.0)) - Origin;

		FMatrix Out = FMatrix::Identity;
		Out.M[0][0] = static_cast<float>(AxisX[0]);
		Out.M[0][1] = static_cast<float>(AxisX[1]);
		Out.M[0][2] = static_cast<float>(AxisX[2]);
		Out.M[1][0] = static_cast<float>(AxisY[0]);
		Out.M[1][1] = static_cast<float>(AxisY[1]);
		Out.M[1][2] = static_cast<float>(AxisY[2]);
		Out.M[2][0] = static_cast<float>(AxisZ[0]);
		Out.M[2][1] = static_cast<float>(AxisZ[1]);
		Out.M[2][2] = static_cast<float>(AxisZ[2]);
		Out.M[3][0] = static_cast<float>(Origin[0]);
		Out.M[3][1] = static_cast<float>(Origin[1]);
		Out.M[3][2] = static_cast<float>(Origin[2]);
		return Out;
	}

	TArray<FStaticMaterial> BuildDefaultStaticMaterials()
	{
		TArray<FStaticMaterial> Materials;
		FStaticMaterial DefaultMaterial{};
		DefaultMaterial.MaterialSlotName = "None";
		DefaultMaterial.MaterialInterface = FMaterialManager::Get().GetOrCreateMaterial("None");
		Materials.push_back(DefaultMaterial);
		return Materials;
	}

	TArray<FStaticMaterial> BuildDefaultSkeletalMaterials()
	{
		TArray<FStaticMaterial> Materials;
		FStaticMaterial DefaultMaterial{};
		DefaultMaterial.MaterialSlotName = "None";
		DefaultMaterial.MaterialInterface = FMaterialManager::Get().GetOrCreateMaterial("None");
		Materials.push_back(DefaultMaterial);
		return Materials;
	}

	FStaticMeshSection BuildDefaultStaticSection(uint32 IndexCount)
	{
		FStaticMeshSection Section{};
		Section.MaterialSlotName = "None";
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.NumTriangles = IndexCount / 3;
		return Section;
	}

	FSkeletalMeshSection BuildDefaultSkeletalSection(uint32 IndexCount)
	{
		FSkeletalMeshSection Section{};
		Section.MaterialSlotName = "None";
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.NumTriangles = IndexCount / 3;
		return Section;
	}
}

bool FFbxImporter::Import(const FString& FilePath)
{
	FFbxImportOptions Options;
	Options.MeshType = EFbxImportMeshType::StaticMesh;
	return Import(FilePath, Options);
}

bool FFbxImporter::Import(const FString& FilePath, const FFbxImportOptions& Options)
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

	if (!Importer->Import(Scene))
	{
		UE_LOG("[FBXImporter] Failed To Import Scene: %s", FilePath.c_str());
		Importer->Destroy();
		SdkManager->Destroy();
		return false;
	}

	if (Options.bTriangulate)
	{
		// FBX 내부 Mesh, NURBS, Patch 등 삼각화 가능한 친구들 geometry를 삼각형으로 변환
		FbxGeometryConverter Converter(SdkManager);
		const bool bTriangulated = Converter.Triangulate(Scene, true);
		if (!bTriangulated)
		{
			UE_LOG("[FBXImporter] Failed To Trianglulate FBX Mesh");
		}
	}
	Importer->Destroy();

	ApplySceneUnitConversion(Scene, Options);
	ApplySceneAxisConversion(Scene, Options);

	FbxNode* ImportRootNode = SelectImportRootNode(Scene, Options);
	if (!ImportRootNode)
	{
		UE_LOG("[FBXImporter] Import root selection failed: %s", FilePath.c_str());
		SdkManager->Destroy();
		return false;
	}

	FbxAMatrix ImportRootGlobal = ImportRootNode->EvaluateGlobalTransform();
	ImportRootGlobalInverse = ImportRootGlobal.Inverse();

	ProcessNode(Scene->GetRootNode(), 0, Options);

	// StaticMesh/SkeletalMesh 양쪽에서 tangent가 필요하므로 전체 노드 처리 후 한 번만 계산합니다.
	ComputeTangents();

	SdkManager->Destroy();
	return true;
}

// 재귀적 node 순회
void FFbxImporter::ProcessNode(FbxNode* Node, int32 cnt, const FFbxImportOptions& Options)
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
				if (Options.bLogMeshSummary)
				{
					UE_LOG("cnt: %d, Node: %s, Mesh: %s", cnt, NodeName, Mesh->GetName());
				}

				if (Options.bImportBones)
				{
					PreLoadCluster(Mesh);
					if (Options.MeshType == EFbxImportMeshType::SkeletalMesh && Bones.empty())
					{
						UE_LOG("[FBXImporter] Skeletal mesh has no bone influences on mesh: %s", Mesh->GetName());
					}
				}
				else
				{
					InfluencesPerControlPoint.clear();
					InfluencesPerControlPoint.resize(Mesh->GetControlPointsCount());
				}

				ProcessPolygon(Node, Options);
			}
		}
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		ProcessNode(Node->GetChild(i), cnt+1, Options);
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

void FFbxImporter::ProcessPolygon(FbxNode* Node, const FFbxImportOptions& Options)
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

	// Geometry Transform
	FbxAMatrix Geometry;
	Geometry.SetIdentity();
	Geometry.SetT(Node->GetGeometricTranslation(FbxNode::eSourcePivot));
	Geometry.SetR(Node->GetGeometricRotation(FbxNode::eSourcePivot));
	Geometry.SetS(Node->GetGeometricScaling(FbxNode::eSourcePivot));

	if (!Options.bBakeGeometryTransform)
	{
		Geometry.SetIdentity();
	}

	if (Options.bLogNodeTransforms)
	{
		UE_LOG("==== Mesh Node: %s / Mesh: %s ====", Node->GetName(), Mesh->GetName());
		PrintMatrixT("Global", GlobalTransform);
		PrintMatrixT("Geometry", Geometry);
	}

	// mesh polygon 순회
	for (int32 pIdx = 0; pIdx < Mesh->GetPolygonCount(); ++pIdx)
	{
		// int materialIdx = Mesh->GetMaterialIndices();

		for (int32 corner = 0; corner < Mesh->GetPolygonSize(pIdx); ++corner)
		{
			int cpIndex = Mesh->GetPolygonVertex(pIdx, corner); //Polygon이 사용하는 Control Point Idx
			FSkeletalMeshVertex v = {};

			// FbxVector4 -> FVector3로 변환
			// ControlPoint
			FbxVector4 ControlPoint = Mesh->GetControlPoints()[cpIndex];

			FbxVector4 FinalPos = TransformControlPointForImport(
				ControlPoint,
				Geometry,
				GlobalTransform,
				ImportRootGlobalInverse,
				Options);

			v.Position = Convert(FinalPos);

			// FBX -> Normal 변환
			// TODO: Position 변환 정책에 맞춰 normal/tangent 변환 필요. 현재는 임시로 원본 normal 사용.
			v.Normal = TransformNormalForImport(GetNormal(Mesh, pIdx, corner), Geometry, GlobalTransform, Options);
			// FBX UV -> 프로젝트 기준 UV로 변환해주는 
			v.UV = GetUV(Mesh, pIdx, corner, uvSetName);
			v.Tangent = TransformTangentForImport(FVector4(0.f, 0.f, 0.f, 0.f), Geometry, GlobalTransform, Options);
			// v.MaterialIndex 나중에 연결

			// Bone Indices, Bone Weight 채우기
			TArray<BoneInfluence> EmptyInfluences;
			const TArray<BoneInfluence>* Influences = &EmptyInfluences;
			if (cpIndex >= 0 && cpIndex < static_cast<int32>(InfluencesPerControlPoint.size()))
			{
				Influences = &InfluencesPerControlPoint[cpIndex]; // cpIndex에 해당하는 Cluster 가져오기
			}
			NormalizeAndLimitTo4(*Influences, v.BoneIndices, v.BoneWeights, static_cast<int32>(Bones.size()));

			uint32 VertexIndex = FindOrAddVertex(v);
			indices.push_back(VertexIndex);
		}
	}
}

UStaticMesh* FFbxImporter::ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device)
{
	FFbxImportOptions Options;
	Options.MeshType = EFbxImportMeshType::StaticMesh;
	Options.PositionMode = EFbxImportPositionMode::UnifiedRootLocal;

	if (!Import(FilePath, Options))
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

USkeletalMesh* FFbxImporter::ImportAsSkeletalMesh(const FString& FilePath, ID3D11Device* Device)
{
	FFbxImportOptions Options;
	Options.MeshType = EFbxImportMeshType::SkeletalMesh;
	Options.PositionMode = EFbxImportPositionMode::UnifiedRootLocal;
	Options.bImportBones = true;

	if (!Import(FilePath, Options))
	{
		UE_LOG("[FBXImport] Skeletal import failed: %s", FilePath.c_str());
		return nullptr;
	}

	if (vertices.empty() || indices.empty())
	{
		UE_LOG("[FBXImport] Skeletal mesh is empty: %s", FilePath.c_str());
		return nullptr;
	}

	return BuildSkeletalMeshFromImportedData(FilePath, Device);
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
		if (!skin) continue;

		// Cluster 개수만큼 순회
		for (int clusteridx = 0; clusteridx < skin->GetClusterCount(); ++clusteridx)
		{
			// cluster 정보 수집
			FbxCluster* cluster = skin->GetCluster(clusteridx);
			if (!cluster) continue;

			// cluster가 영향 미치는 bone 정보 수집
			FbxNode* boneNode = cluster->GetLink();

			// bone Node값 기반으로 bone의 index 정보 가져오기
			int boneindex = FindOrAddBone(boneNode);
			if (boneindex < 0) continue;
			FillBoneBindData(boneindex, cluster);

			int count = cluster->GetControlPointIndicesCount(); // Control Point의 indices 배열 길이
			int* cpIndices = cluster->GetControlPointIndices(); // 실제 배열 값
			double* weights = cluster->GetControlPointWeights(); // Cluster의 Weight 값

			for (int i = 0; i < count; i++)
			{
				int cpIndex = cpIndices[i]; //control point index;
				if (cpIndex < 0 || cpIndex >= static_cast<int32>(InfluencesPerControlPoint.size())) continue;
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

	EngineBone bone;
	bone.Name = BoneNode->GetName();
	if (FbxNode* ParentBoneNode = FindParentSkeletonNode(BoneNode))
	{
		bone.ParentIndex = FindOrAddBone(ParentBoneNode);
	}
	else
	{
		bone.ParentIndex = -1;
	}
	bone.FbxNode = BoneNode;
	FbxAMatrix BoneGlobalImportRoot = BoneNode->EvaluateGlobalTransform() * ImportRootGlobalInverse;
	bone.MeshBindGlobalTransform = FMatrix::Identity;
	bone.GlobalBindTransform = ConvertFbxMatrix(BoneGlobalImportRoot);
	bone.InverseBindTransform = ConvertFbxMatrix(BoneGlobalImportRoot.Inverse());
	bone.LocalBindTransform = bone.GlobalBindTransform;

	// 새로할당된 BoneNode의 index
	const int32 newIndex = static_cast<int>(Bones.size());
	Bones.push_back(bone);
	BoneNodeToIndex[BoneNode] = newIndex;

	return newIndex;
}

void FFbxImporter::FillBoneBindData(int32 BoneIndex, FbxCluster* Cluster)
{
	if (BoneIndex < 0 || BoneIndex >= static_cast<int32>(Bones.size()) || !Cluster)
	{
		return;
	}

	EngineBone& Bone = Bones[BoneIndex];

	FbxAMatrix MeshBindGlobal;
	Cluster->GetTransformMatrix(MeshBindGlobal);

	FbxAMatrix BoneBindGlobal;
	Cluster->GetTransformLinkMatrix(BoneBindGlobal);

	// SourceVertices는 기본 UnifiedRootLocal 정책에서 ImportRootLocal 공간에 저장된다.
	// 따라서 FBX scene global bind matrix도 같은 ImportRootLocal 공간으로 변환해 저장한다.
	const FbxAMatrix MeshBindImportRoot = MeshBindGlobal * ImportRootGlobalInverse;
	const FbxAMatrix BoneBindImportRoot = BoneBindGlobal * ImportRootGlobalInverse;

	Bone.MeshBindGlobalTransform = ConvertFbxMatrix(MeshBindImportRoot);
	Bone.GlobalBindTransform = ConvertFbxMatrix(BoneBindImportRoot);
	Bone.InverseBindTransform = ConvertFbxMatrix(BoneBindImportRoot.Inverse());
	Bone.bHasBindTransform = true;
}

void FFbxImporter::DeriveLocalBindTransformsFromGlobalBindTransforms()
{
	for (int32 BoneIndex = 0; BoneIndex < static_cast<int32>(Bones.size()); ++BoneIndex)
	{
		EngineBone& Bone = Bones[BoneIndex];
		if (Bone.ParentIndex >= 0 && Bone.ParentIndex < static_cast<int32>(Bones.size()))
		{
			const FMatrix ParentGlobalInverse = Bones[Bone.ParentIndex].GlobalBindTransform.GetInverse();
			Bone.LocalBindTransform = Bone.GlobalBindTransform * ParentGlobalInverse;
		}
		else
		{
			Bone.LocalBindTransform = Bone.GlobalBindTransform;
		}
	}
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

	FFbxImportOptions Options;
	Options.bConvertUnit = true;
	Options.bConvertAxis = true;
	Options.AxisMode = EFbxAxisConversionMode::FbxSdkConvertScene;
	ApplySceneUnitConversion(Scene, Options);
	ApplySceneAxisConversion(Scene, Options);
}

void FFbxImporter::ApplySceneUnitConversion(FbxScene* Scene, const FFbxImportOptions& Options)
{
	if (!Options.bConvertUnit)
	{
		return;
	}

	if (!Scene)
	{
		UE_LOG("[FBXImporter] Failed to convert unit, Scene is nullptr");
		return;
	}

	FbxSystemUnit SceneUnit = Scene->GetGlobalSettings().GetSystemUnit();
	if (SceneUnit != FbxSystemUnit::cm)
	{
		FbxSystemUnit::cm.ConvertScene(Scene);
	}
}

void FFbxImporter::ApplySceneAxisConversion(FbxScene* Scene, const FFbxImportOptions& Options)
{
	if (!Options.bConvertAxis || Options.AxisMode == EFbxAxisConversionMode::None)
	{
		return;
	}

	if (!Scene)
	{
		UE_LOG("[FBXImporter] Failed to convert axis, Scene is nullptr");
		return;
	}

	if (Options.AxisMode == EFbxAxisConversionMode::ManualAxisFix)
	{
		UE_LOG("[FBXImporter] ManualAxisFix selected; FbxSdk ConvertScene is skipped to avoid double axis conversion.");
		BuildManualAxisFixMatrix(Options);
		return;
	}

	FbxAxisSystem TargetAxisSystem(FbxAxisSystem::eZAxis, FbxAxisSystem::eParityOdd, FbxAxisSystem::eLeftHanded);
	FbxAxisSystem SceneAxisSystem = Scene->GetGlobalSettings().GetAxisSystem();
	if (SceneAxisSystem != TargetAxisSystem)
	{
		TargetAxisSystem.ConvertScene(Scene);
	}
}

FbxAMatrix FFbxImporter::BuildManualAxisFixMatrix(const FFbxImportOptions& Options)
{
	FbxAMatrix ManualAxisFix;
	ManualAxisFix.SetIdentity();

	if (Options.AxisMode == EFbxAxisConversionMode::ManualAxisFix)
	{
		// TODO: 프로젝트 수동 축 보정 정책이 확정되면 position/normal/tangent에 동일하게 적용.
	}

	return ManualAxisFix;
}

FbxNode* FFbxImporter::SelectImportRootNode(FbxScene* Scene, const FFbxImportOptions& Options)
{
	FbxNode* SceneRoot = Scene ? Scene->GetRootNode() : nullptr;
	FbxNode* SelectedRoot = nullptr;

	switch (Options.RootMode)
	{
	case EFbxImportRootMode::SceneRoot:
		SelectedRoot = SceneRoot;
		break;
	case EFbxImportRootMode::FirstChild:
		SelectedRoot = (SceneRoot && SceneRoot->GetChildCount() > 0) ? SceneRoot->GetChild(0) : nullptr;
		break;
	case EFbxImportRootMode::FirstSkeleton:
		SelectedRoot = FindFirstSkeletonRoot(SceneRoot);
		break;
	case EFbxImportRootMode::FirstMesh:
		SelectedRoot = FindFirstMeshRoot(SceneRoot);
		break;
	case EFbxImportRootMode::FirstMeshParent:
		if (FbxNode* MeshRoot = FindFirstMeshRoot(SceneRoot))
		{
			SelectedRoot = MeshRoot->GetParent();
		}
		break;
	case EFbxImportRootMode::ExplicitNodeName:
		SelectedRoot = FindNodeByName(SceneRoot, Options.ExplicitRootNodeName);
		if (!SelectedRoot)
		{
			UE_LOG("[FBXImporter] Explicit import root not found: %s", Options.ExplicitRootNodeName.c_str());
		}
		break;
	default:
		break;
	}

	if (!SelectedRoot)
	{
		SelectedRoot = SceneRoot;
	}

	if (Options.bLogImportRoot)
	{
		UE_LOG("[FBXImporter] ImportRootNode: %s", SelectedRoot ? SelectedRoot->GetName() : "(null)");
	}

	return SelectedRoot;
}

FbxNode* FFbxImporter::FindFirstSkeletonRoot(FbxNode* Node)
{
	if (!Node) return nullptr;

	FbxNodeAttribute* Attr = Node->GetNodeAttribute();
	if (Attr && Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
	{
		return Node;
	}

	for (int32 i = 0; i < Node->GetChildCount(); ++i)
	{
		if (FbxNode* Found = FindFirstSkeletonRoot(Node->GetChild(i)))
		{
			return Found;
		}
	}

	return nullptr;
}

FbxNode* FFbxImporter::FindFirstMeshRoot(FbxNode* Node)
{
	return FindFirstMesh(Node);
}

FbxNode* FFbxImporter::FindFirstMesh(FbxNode* Node)
{
	if (!Node) return nullptr;

	FbxNodeAttribute* Attr = Node->GetNodeAttribute();
	if (Attr && Attr->GetAttributeType() == FbxNodeAttribute::eMesh)
	{
		return Node;
	}

	for (int i = 0; i < Node->GetChildCount(); ++i)
	{
		if (FbxNode* Found = FindFirstMesh(Node->GetChild(i)))
		{
			return Found;
		}
	}

	return nullptr;
}

FbxVector4 FFbxImporter::TransformControlPointForImport(
	const FbxVector4& ControlPoint,
	const FbxAMatrix& Geometry,
	const FbxAMatrix& NodeGlobal,
	const FbxAMatrix& ImportRootGlobalInverse,
	const FFbxImportOptions& Options)
{
	/*
	행벡터 개념:
	RawControlPoint = ControlPoint
	GeometryOnly = ControlPoint * Geometry
	SceneBake = ControlPoint * Geometry * NodeGlobal
	UnifiedRootLocal = ControlPoint * Geometry * NodeGlobal * Inverse(ImportRootGlobal)

	FBX SDK MultT 호출 순서는 현재 정상 동작한 방식과 맞춘다:
	ScenePos = NodeGlobal.MultT(Geometry.MultT(ControlPoint));
	UnifiedLocalPos = ImportRootGlobalInverse.MultT(ScenePos);
	*/
	switch (Options.PositionMode)
	{
	case EFbxImportPositionMode::RawControlPoint:
		return ControlPoint;
	case EFbxImportPositionMode::GeometryOnly:
		return Geometry.MultT(ControlPoint);
	case EFbxImportPositionMode::SceneBake:
		return NodeGlobal.MultT(Geometry.MultT(ControlPoint));
	case EFbxImportPositionMode::UnifiedRootLocal:
	default:
	{
		FbxVector4 ScenePos = NodeGlobal.MultT(Geometry.MultT(ControlPoint));
		return ImportRootGlobalInverse.MultT(ScenePos);
	}
	}
}

FVector FFbxImporter::TransformNormalForImport(
	const FVector& Normal,
	const FbxAMatrix& Geometry,
	const FbxAMatrix& NodeGlobal,
	const FFbxImportOptions& Options)
{
	(void)Geometry;
	(void)NodeGlobal;
	(void)Options;
	// TODO: Translation 제외.
	// TODO: Non-uniform scale 대응 시 inverse-transpose 필요.
	// TODO: ManualAxisFix 사용 시 normal/tangent에도 회전 보정 필요.
	return Normal;
}

FVector4 FFbxImporter::TransformTangentForImport(
	const FVector4& Tangent,
	const FbxAMatrix& Geometry,
	const FbxAMatrix& NodeGlobal,
	const FFbxImportOptions& Options)
{
	(void)Geometry;
	(void)NodeGlobal;
	(void)Options;
	// TODO: Translation 제외.
	// TODO: Non-uniform scale 대응 시 inverse-transpose 필요.
	// TODO: ManualAxisFix 사용 시 normal/tangent에도 회전 보정 필요.
	return Tangent;
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
	MeshAsset->Sections.push_back(BuildDefaultStaticSection(static_cast<uint32>(MeshAsset->Indices.size())));

	MeshAsset->CacheBounds();

	TArray<FStaticMaterial> Materials = BuildDefaultStaticMaterials();

	UStaticMesh* StaticMesh = UObjectManager::Get().CreateObject<UStaticMesh>();
	StaticMesh->SetStaticMaterials(std::move(Materials));
	StaticMesh->SetStaticMeshAsset(MeshAsset);
	StaticMesh->InitResources(Device);

	return StaticMesh;
}

FSkeletalMeshRawData FFbxImporter::BuildSkeletalRawDataFromImportedData()
{
	DeriveLocalBindTransformsFromGlobalBindTransforms();

	FSkeletalMeshRawData RawData;
	RawData.SourceVertices = vertices;
	RawData.Indices = indices;
	RawData.Sections.push_back(BuildDefaultSkeletalSection(static_cast<uint32>(indices.size())));

	RawData.Bones.reserve(Bones.size());
	for (const EngineBone& Bone : Bones)
	{
		FSkeletalBoneInfo BoneInfo;
		BoneInfo.Name = Bone.Name;
		BoneInfo.ParentIndex = Bone.ParentIndex;
		BoneInfo.MeshBindGlobalTransform = Bone.MeshBindGlobalTransform;
		BoneInfo.LocalBindTransform = Bone.LocalBindTransform;
		BoneInfo.GlobalBindTransform = Bone.GlobalBindTransform;
		BoneInfo.InverseBindTransform = Bone.InverseBindTransform;
		// TODO: Animation pose sampling 이후 CurrentBoneTransform 갱신 예정
		RawData.Bones.push_back(BoneInfo);
	}

	RawData.CacheBounds();
	return RawData;
}

USkeletalMesh* FFbxImporter::BuildSkeletalMeshFromImportedData(const FString& FilePath, ID3D11Device* Device)
{
	if (!Device)
	{
		UE_LOG("[FBXImporter] Device is null");
		return nullptr;
	}

	FSkeletalMeshRawData RawData = BuildSkeletalRawDataFromImportedData();
	if (RawData.SourceVertices.empty() || RawData.Indices.empty())
	{
		return nullptr;
	}

	FSkeletalMeshAsset* MeshAsset = new FSkeletalMeshAsset;
	MeshAsset->PathFileName = FilePath;
	MeshAsset->SourceVertices = std::move(RawData.SourceVertices);
	MeshAsset->Bones = std::move(RawData.Bones);
	MeshAsset->Sections = std::move(RawData.Sections);
	MeshAsset->BoundsCenter = RawData.BoundsCenter;
	MeshAsset->BoundsExtent = RawData.BoundsExtent;
	MeshAsset->bBoundsValid = RawData.bBoundsValid;

	MeshAsset->Indices.reserve(RawData.Indices.size());
	for (int32 Index : RawData.Indices)
	{
		if (Index >= 0)
		{
			MeshAsset->Indices.push_back(static_cast<uint32>(Index));
		}
	}

	USkeletalMesh* SkeletalMesh = UObjectManager::Get().CreateObject<USkeletalMesh>();
	SkeletalMesh->SetSkeletalMaterials(BuildDefaultSkeletalMaterials());
	SkeletalMesh->SetSkeletalMeshAsset(MeshAsset);
	SkeletalMesh->InitResources(Device);

	return SkeletalMesh;
}
