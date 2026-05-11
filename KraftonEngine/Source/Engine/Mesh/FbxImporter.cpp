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
#include <cfloat>

#pragma region 내부사용함수모음
namespace
{
	// ============================================================
	// FBX 기본 값 변환
	// ============================================================
	// FBX SDK의 벡터 값을 엔진 FVector 형식으로 옮긴다.
	// 이 함수는 좌표계 보정이나 Node Transform 적용을 하지 않고,
	// 저장된 숫자를 단순히 엔진 자료형으로 바꾸는 역할만 한다.
	FVector Convert(FbxVector4 Pos)
	{
		return FVector(
			static_cast<float>(Pos[0]),
			static_cast<float>(Pos[1]),
			static_cast<float>(Pos[2])
		);
	}

	// ============================================================
	// Geometry Transform 행렬 생성
	// ============================================================
	// FBX의 Geometry Transform은 일반적인 Node Local/Global Transform과 다르다.
	// 이 값은 자식 Node나 Bone에 상속되는 Transform이 아니라,
	// 현재 Mesh Attribute에만 적용되는 추가 보정값이다.
	// 그래서 SkeletalMesh 정점에는 미리 굽지 않고 Range에 따로 저장한다.
	FbxAMatrix BuildFbxGeometryMatrix(FbxNode* Node)
	{
		FbxAMatrix Geometry;
		Geometry.SetIdentity();

		if (!Node)
		{
			return Geometry;
		}

		Geometry.SetT(Node->GetGeometricTranslation(FbxNode::eSourcePivot));
		Geometry.SetR(Node->GetGeometricRotation(FbxNode::eSourcePivot));
		Geometry.SetS(Node->GetGeometricScaling(FbxNode::eSourcePivot));
		return Geometry;
	}

	// ============================================================
	// FBX 행렬 변환
	// ============================================================
	// FBX SDK의 행렬을 엔진 FMatrix 형식으로 변환한다.
	// 이 함수는 좌표계 변환 정책을 숨기지 않도록 단순 변환만 담당한다.
	// 행벡터 기준 엔진 행렬에 맞춰 translation은 4번째 행에 저장한다.
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

	// 최대 4개의 영향을 주는 Bone을 찾아서 넣어주고, Weight를 정규화하는 함수
	void NormalizeAndLimitTo4(const TArray<BoneInfluence>& influences, uint32* BoneIndices, float* BoneWeights, int32 BoneCount, int32 FallbackBoneIndex)
	{
		// 초기화
		int cnt = 0;
		float WeightSum = 0.f;

		for (int i = 0; i < 4; ++i)
		{
			BoneIndices[i] = 0;
			BoneWeights[i] = 0.0f;
		}

		TArray<BoneInfluence> SortedInfluences = influences;
		// influence Sorting, 큰 weight 우선으로 정렬 (영향을 많이 주는 Bone만 남긴다)
		(std::sort)(SortedInfluences.begin(), SortedInfluences.end(),
			[](BoneInfluence& A, BoneInfluence& B)
			{
				return A.boneWeight > B.boneWeight;
			});

		// mesh에 영향을 주는 bone, weight 정보를 채운다
		for (auto influence : SortedInfluences)
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
			// Skin Cluster가 없는 경우 Mesh위치에서 가장 가까운 Bone에 rigid bind 시킨다.
			const int32 SafeFallbackBoneIndex =
				(FallbackBoneIndex >= 0 && FallbackBoneIndex < BoneCount)
				? FallbackBoneIndex : 0;
			BoneIndices[0] = static_cast<uint32>(SafeFallbackBoneIndex);
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
#pragma endregion

bool FFbxImporter::Import(const FString& FilePath)
{
	FFbxImportOptions Options;
	Options.MeshType = EFbxImportMeshType::SkeletalMesh;
	return Import(FilePath, Options);
}

// 재귀적 node 순회
bool FFbxImporter::Import(const FString& FilePath, const FFbxImportOptions& Options)
{
	// import 전 clear
	vertices.clear();
	indices.clear();
	vertexRanges.clear();
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

	// ============================================================
	// Scene 순회 시작
	// ============================================================
	// SkeletalMesh는 ControlPoint를 Mesh Node local 원본 좌표로 보존한다.
	// 이전처럼 임의 root inverse를 곱해 하나의 root local space로 강제 보정하지 않는다.
	ProcessNode(Scene->GetRootNode(), 0, Options);

	// StaticMesh/SkeletalMesh 양쪽에서 tangent가 필요하므로 전체 노드 처리 후 한 번만 계산합니다.
	ComputeTangents();

	SdkManager->Destroy();
	return true;
}

UStaticMesh* FFbxImporter::ImportAsStaticMesh(const FString& FilePath, ID3D11Device* Device)
{
	FFbxImportOptions Options;
	Options.MeshType = EFbxImportMeshType::StaticMesh;

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
	Options.bImportBones = true;
	Options.AxisMode = EFbxAxisConversionMode::FbxSdkConvertScene;

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

				CurrentRigidFallbackBoneIndex = -1;

				if (Options.bImportBones)
				{
					PreLoadCluster(Mesh);
					if (Options.MeshType == EFbxImportMeshType::SkeletalMesh)
					{
						CurrentRigidFallbackBoneIndex = FindRigidFallbackBoneForMesh(Node);
					}

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

void FFbxImporter::ProcessPolygon(FbxNode* Node, const FFbxImportOptions& Options)
{
	FbxMesh* Mesh = Node->GetMesh();
	if (!Mesh)
	{
		return;
	}

	// Mesh가 가진 UV Set 중 첫 번째 채널을 읽어 렌더 정점에 저장한다.
	const char* uvSetName = nullptr;
	FbxStringList uvSetNames;
	Mesh->GetUVSetNames(uvSetNames);
	if (uvSetNames.GetCount() > 0)
	{
		uvSetName = uvSetNames.GetStringAt(0);
	}

	// ============================================================
	// Mesh Range 생성
	// ============================================================
	// 하나의 FBX Mesh Node에서 나온 정점/인덱스 범위를 기록한다.
	// 여러 Mesh Node를 하나의 SkeletalMesh로 합치더라도,
	// 각 범위가 어떤 Node Transform과 Geometry Transform을 써야 하는지
	// 잃어버리지 않기 위해 필요하다.
	const FbxAMatrix NodeGlobal = Node->EvaluateGlobalTransform();
	const FbxAMatrix FbxGeometry = BuildFbxGeometryMatrix(Node);
	const FMatrix GeometryTransform = GetGeometryTransformFromNode(Node);
	const FMatrix MeshNodeGlobalBindTransform = ConvertFbxMatrix(NodeGlobal);

	FSkeletalMeshVertexRange VertexRange;
	if (Options.MeshType == EFbxImportMeshType::SkeletalMesh)
	{
		VertexRange.BaseVertex = static_cast<uint32>(vertices.size());
		VertexRange.BaseIndex = static_cast<uint32>(indices.size());
		VertexRange.MeshNodeName = Node->GetName();
		VertexRange.GeometryTransform = GeometryTransform;
		VertexRange.MeshNodeGlobalBindTransform = MeshNodeGlobalBindTransform;
		VertexRange.MeshNodeGlobalBindInverseTransform = MeshNodeGlobalBindTransform.GetInverse();
	}

	if (Options.bLogNodeTransforms)
	{
		UE_LOG("==== Mesh Node: %s / Mesh: %s ====", Node->GetName(), Mesh->GetName());
		PrintMatrixT("Global", NodeGlobal);
		PrintMatrixT("Geometry", FbxGeometry);
	}

	// mesh polygon 순회
	for (int32 pIdx = 0; pIdx < Mesh->GetPolygonCount(); ++pIdx)
	{
		// FbxGeometryElementMaterial* Material = Mesh->GetElementMaterial(pIdx); -> 나중에 사용

		for (int32 corner = 0; corner < Mesh->GetPolygonSize(pIdx); ++corner)
		{
			// Polygon corner가 참조하는 FBX ControlPoint 인덱스다.
			const int cpIndex = Mesh->GetPolygonVertex(pIdx, corner);
			FSkeletalMeshVertex v = {};

			// 1. 원본 ControlPoint 좌표를 가져온다.
			FbxVector4 ControlPoint = Mesh->GetControlPoints()[cpIndex];

			if (Options.MeshType == EFbxImportMeshType::SkeletalMesh)
			{
				// SkeletalMesh 정점은 FBX Mesh의 ControlPoint 원본 좌표를 그대로 보존한다.
				// Geometry, Mesh Node Global, Common Root 보정은 여기서 절대 굽지 않는다.
				v.Position = Convert(ControlPoint);
			}
			else
			{
				// StaticMesh는 현재 렌더링 구조에 맞춰 Mesh Node의 Geometry와 Global Transform을 정점에 굽는다.
				// 이 경로는 SkeletalMesh의 원본 ControlPoint 보존 정책과 분리되어 있다.
				v.Position = Convert(NodeGlobal.MultT(FbxGeometry.MultT(ControlPoint)));
			}

			// FBX -> Normal 변환
			// TODO: Non-uniform scale 대응 시 inverse-transpose가 필요하다.
			v.Normal = GetNormal(Mesh, pIdx, corner);
			// FBX UV는 DirectX 텍스처 좌표 기준에 맞춰 V를 뒤집어 저장한다.
			v.UV = GetUV(Mesh, pIdx, corner, uvSetName);
			v.Tangent = FVector4(0.f, 0.f, 0.f, 0.f);
			// v.MaterialIndex 나중에 연결
			// v.MaterialIndex = 

			// 2. 이 정점이 어떤 Bone의 영향을 받는지 저장한다.
			TArray<BoneInfluence> EmptyInfluences;
			const TArray<BoneInfluence>* Influences = &EmptyInfluences;
			if (cpIndex >= 0 && cpIndex < static_cast<int32>(InfluencesPerControlPoint.size()))
			{
				Influences = &InfluencesPerControlPoint[cpIndex]; // cpIndex에 해당하는 Cluster 가져오기
			}
			NormalizeAndLimitTo4(
				*Influences, 
				v.BoneIndices, 
				v.BoneWeights, 
				static_cast<int32>(Bones.size()), 
				CurrentRigidFallbackBoneIndex);

			uint32 VertexIndex = FindOrAddVertex(v);
			// 3. IndexBuffer는 전체 VertexBuffer 기준 absolute index를 유지한다.
			indices.push_back(VertexIndex);

			if (Influences->empty() && CurrentRigidFallbackBoneIndex >= 0)
			{
				UE_LOG("[FBXImporter] Rigid fallback mesh=%s cp=%d -> Bone[%d]=%s",
					Node->GetName(),
					cpIndex,
					CurrentRigidFallbackBoneIndex,
					Bones[CurrentRigidFallbackBoneIndex].Name.c_str());
			}
		}
	}

	if (Options.MeshType == EFbxImportMeshType::SkeletalMesh)
	{
		VertexRange.VertexCount = static_cast<uint32>(vertices.size()) - VertexRange.BaseVertex;
		VertexRange.IndexCount = static_cast<uint32>(indices.size()) - VertexRange.BaseIndex;

		if (VertexRange.VertexCount > 0 && VertexRange.IndexCount > 0)
		{
			vertexRanges.push_back(VertexRange);

			const FVector GeometryT = VertexRange.GeometryTransform.GetLocation();
			const FVector MeshNodeT = VertexRange.MeshNodeGlobalBindTransform.GetLocation();
			UE_LOG("[FBX MeshRange] MeshNode=%s BaseVertex=%u VertexCount=%u BaseIndex=%u IndexCount=%u GeometryT=(%.3f %.3f %.3f) MeshNodeBindT=(%.3f %.3f %.3f)",
				VertexRange.MeshNodeName.c_str(),
				VertexRange.BaseVertex,
				VertexRange.VertexCount,
				VertexRange.BaseIndex,
				VertexRange.IndexCount,
				GeometryT.X, GeometryT.Y, GeometryT.Z,
				MeshNodeT.X, MeshNodeT.Y, MeshNodeT.Z);
		}
	}
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
	bone.ChildCount = BoneNode->GetChildCount();
	bone.FbxNode = BoneNode;
	FbxAMatrix BoneGlobal = BoneNode->EvaluateGlobalTransform();
	bone.MeshBindGlobalTransform = FMatrix::Identity;
	bone.GlobalBindTransform = ConvertFbxMatrix(BoneGlobal);
	bone.InverseBindTransform = bone.GlobalBindTransform.GetInverse();
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

	// ============================================================
	// Bone Bind Transform 저장
	// ============================================================
	// Cluster가 제공하는 Link Matrix는 Bone의 bind pose Global Transform이다.
	// 이 값은 FBX Scene Global 기준 그대로 저장한다.
	// CommonRootInverse나 MeshNode 보정을 Bone 행렬에 섞지 않는다.
	Bone.MeshBindGlobalTransform = ConvertFbxMatrix(MeshBindGlobal);
	Bone.GlobalBindTransform = ConvertFbxMatrix(BoneBindGlobal);
	Bone.InverseBindTransform = Bone.GlobalBindTransform.GetInverse();
	Bone.bHasBindTransform = true;

	const FVector GlobalBindT = Bone.GlobalBindTransform.GetLocation();
	const FVector InverseBindT = Bone.InverseBindTransform.GetLocation();
	UE_LOG("[FBX BoneBind] Bone=%s ParentIndex=%d GlobalBindT=(%.3f %.3f %.3f) InverseBindT=(%.3f %.3f %.3f)",
		Bone.Name.c_str(),
		Bone.ParentIndex,
		GlobalBindT.X, GlobalBindT.Y, GlobalBindT.Z,
		InverseBindT.X, InverseBindT.Y, InverseBindT.Z);
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
	if (SceneUnit != FbxSystemUnit::mm)
	{
		FbxSystemUnit::mm.ConvertScene(Scene);
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

FMatrix FFbxImporter::GetGeometryTransformFromNode(FbxNode* Node) const
{
	// ============================================================
	// Geometry Transform 추출
	// ============================================================
	// FBX의 Geometry Transform은 일반적인 Node Local/Global Transform과 다르다.
	// 이 값은 자식 Node나 Bone에 상속되는 Transform이 아니라,
	// 현재 Mesh Attribute에만 적용되는 추가 보정값이다.
	// 그래서 정점에 미리 굽지 않고 Range에 따로 저장해둔다.
	// Skinning 또는 렌더링 단계에서 해당 Range의 정점에만 적용한다.
	return ConvertFbxMatrix(BuildFbxGeometryMatrix(Node));
}

// FBX에서 읽은 정점/인덱스를 엔진 StaticMesh 리소스로 변환한다.
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
	MeshAsset->VertexRanges = std::move(RawData.VertexRanges);
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
FSkeletalMeshRawData FFbxImporter::BuildSkeletalRawDataFromImportedData()
{
	DeriveLocalBindTransformsFromGlobalBindTransforms();

	FSkeletalMeshRawData RawData;
	RawData.SourceVertices = vertices;
	RawData.Indices = indices;
	RawData.VertexRanges = vertexRanges;
	RawData.Sections.push_back(BuildDefaultSkeletalSection(static_cast<uint32>(indices.size())));

	RawData.Bones.reserve(Bones.size());
	for (const EngineBone& Bone : Bones)
	{
		FSkeletalBoneInfo BoneInfo;
		BoneInfo.Name = Bone.Name;
		BoneInfo.ParentIndex = Bone.ParentIndex;
		BoneInfo.ChildCount = Bone.ChildCount;
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


int32 FFbxImporter::FindRigidFallbackBoneForMesh(FbxNode* MeshNode)
{
	if (!MeshNode || Bones.empty()) return -1;

	// 1. Mesh Node의 Parent Chain 중에서 SkeletalMesh가 있으면 그 bone에 붙인다.
	if (FbxNode* ParentSkeleton = FindParentSkeletonNode(MeshNode))
	{
		const int32 BoneIndex = FindOrAddBone(ParentSkeleton);
		if (BoneIndex >= 0)
		{
			return BoneIndex;
		}
	}

	// 2. 부모 Skeleton이 없으면 FBX Scene Global 위치끼리 비교해서 가장 가까운 bone을 찾는다.
	FbxAMatrix MeshGlobal = MeshNode->EvaluateGlobalTransform();
	FbxVector4 MeshPos = MeshGlobal.GetT();

	int32 BestBoneIndex = -1;
	double BestDistSq = DBL_MAX; // max_double

	for (int32 BoneIndex = 0; BoneIndex < static_cast<int32>(Bones.size()); ++BoneIndex)
	{
		const EngineBone& Bone = Bones[BoneIndex];
		const FVector BonePos = Bone.GlobalBindTransform.GetLocation();

		const double Dx = static_cast<double>(MeshPos[0] - BonePos.X);
		const double Dy = static_cast<double>(MeshPos[1] - BonePos.Y);
		const double Dz = static_cast<double>(MeshPos[2] - BonePos.Z);

		const double Distsq = Dx * Dx + Dy * Dy + Dz * Dz;
		if (Distsq < BestDistSq)
		{
			BestDistSq = Distsq;
			BestBoneIndex = BoneIndex;
		}
	}
	return BestBoneIndex;

}