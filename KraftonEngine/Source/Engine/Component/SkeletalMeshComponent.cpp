#include "Component/SkeletalMeshComponent.h"

#include "Core/Log.h"
#include "Engine/Platform/Paths.h"
#include "Mesh/FbxImporter.h"
#include "Mesh/SkeletalMesh.h"
#include "Object/ObjectFactory.h"
#include "Render/Proxy/SkeletalMeshSceneProxy.h"
#include "Runtime/Engine.h"
#include "Serialization/Archive.h"

#include <algorithm>
#include <cmath>

IMPLEMENT_CLASS(USkeletalMeshComponent, USkinnedMeshComponent)

namespace
{
	FVertexPNCTT MakeSkinnedRenderVertex(const FSkeletalMeshVertex& SourceVertex)
	{
		FVertexPNCTT RenderVertex;
		RenderVertex.Position = SourceVertex.Position;
		RenderVertex.Normal = SourceVertex.Normal;
		RenderVertex.Color = FVector4(1.f, 1.f, 1.f, 1.f);
		RenderVertex.UV = SourceVertex.UV;
		RenderVertex.Tangent = SourceVertex.Tangent;
		return RenderVertex;
	}
}

FPrimitiveSceneProxy* USkeletalMeshComponent::CreateSceneProxy()
{
	return new FSkeletalMeshSceneProxy(this);
}

void USkeletalMeshComponent::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar << SkeletalMeshPath;
}

void USkeletalMeshComponent::PostDuplicate()
{
	Super::PostDuplicate();

	const USkeletalMesh* SerializedMeshPointer = SkeletalMesh;
	SkeletalMesh = nullptr;
	DynamicRenderBuffer.reset();

	if (!SkeletalMeshPath.empty() && SkeletalMeshPath != "None")
	{
		USkeletalMesh* LoadedMesh = LoadSkeletalMeshFromPath();
		UE_LOG("[PIE Duplicate] SourceSkeletalMesh=%p Duplicated/AssignedSkeletalMesh=%p Path=%s",
			SerializedMeshPointer,
			LoadedMesh,
			SkeletalMeshPath.c_str());
		if (LoadedMesh)
		{
			SetSkeletalMesh(LoadedMesh);
			return;
		}

		ReinitializeSkeletalMeshRuntimeState();
		return;
	}

	ReinitializeSkeletalMeshRuntimeState();
}

void USkeletalMeshComponent::SetSkeletalMesh(USkeletalMesh* InMesh)
{
	SkeletalMesh = InMesh;
	if (InMesh)
	{
		SkeletalMeshPath = FPaths::MakeProjectRelative(InMesh->GetAssetPathFileName());
		const TArray<FStaticMaterial>& DefaultMaterials = SkeletalMesh->GetSkeletalMaterials();

		OverrideMaterials.resize(DefaultMaterials.size());
		for (int32 i = 0; i < static_cast<int32>(DefaultMaterials.size()); ++i)
		{
			OverrideMaterials[i] = DefaultMaterials[i].MaterialInterface;
		}
	}
	else
	{
		SkeletalMeshPath = "None";
		OverrideMaterials.clear();
	}

	ReinitializeSkeletalMeshRuntimeState();
}

USkeletalMesh* USkeletalMeshComponent::LoadSkeletalMeshFromPath() const
{
	if (SkeletalMeshPath.empty() || SkeletalMeshPath == "None")
	{
		return nullptr;
	}

	if (!GEngine)
	{
		UE_LOG("[PIE SKM] Cannot reload skeletal mesh, GEngine is null. Path=%s", SkeletalMeshPath.c_str());
		return nullptr;
	}

	ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
	if (!Device)
	{
		UE_LOG("[PIE SKM] Cannot reload skeletal mesh, D3D device is null. Path=%s", SkeletalMeshPath.c_str());
		return nullptr;
	}

	FFbxImporter Importer;
	return Importer.ImportAsSkeletalMesh(SkeletalMeshPath, Device);
}

void USkeletalMeshComponent::ReinitializeSkeletalMeshRuntimeState()
{
	DynamicRenderBuffer.reset();
	InitializeSkinningState();
	UpdatePose();
	UpdateSkinningMatrices();
	UpdateCPUSkinning();
	RebuildDynamicRenderBuffer();
	const bool bDynamicBufferCreated = UploadSkinnedVerticesToGPU();
	LogSkeletalMeshDebugInfo(bDynamicBufferCreated);

	CacheLocalBounds();
	MarkRenderStateDirty();
	MarkWorldBoundsDirty();

	// PIE duplicate 이후 첫 Tick에서 pose/skinning/render upload 경로가 한 번 더 안전하게 돈다.
	bPoseDirty = true;
	bSkinningDirty = true;
	bRenderDataDirty = true;
}

USkeletalMesh* USkeletalMeshComponent::GetSkeletalMesh() const
{
	return SkeletalMesh;
}

FMeshBuffer* USkeletalMeshComponent::GetMeshBuffer() const
{
	return DynamicRenderBuffer ? DynamicRenderBuffer.get() : nullptr;
}

FMeshDataView USkeletalMeshComponent::GetMeshDataView() const
{
	if (!SkeletalMesh) return {};
	FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || SkinnedVertices.empty()) return {};

	FMeshDataView View;
	View.VertexData = SkinnedVertices.data();
	View.VertexCount = static_cast<uint32>(SkinnedVertices.size());
	View.Stride = sizeof(FVertexPNCTT);
	View.IndexData = Asset->Indices.data();
	View.IndexCount = static_cast<uint32>(Asset->Indices.size());
	return View;
}

void USkeletalMeshComponent::SetMaterial(int32 ElementIndex, UMaterial* InMaterial)
{
	if (ElementIndex >= 0 && ElementIndex < static_cast<int32>(OverrideMaterials.size()))
	{
		OverrideMaterials[ElementIndex] = InMaterial;
		MarkProxyDirty(EDirtyFlag::Material);
	}
}

UMaterial* USkeletalMeshComponent::GetMaterial(int32 ElementIndex) const
{
	if (ElementIndex >= 0 && ElementIndex < static_cast<int32>(OverrideMaterials.size()))
	{
		return OverrideMaterials[ElementIndex];
	}
	return nullptr;
}

void USkeletalMeshComponent::CacheLocalBounds()
{
	bHasValidBounds = false;
	if (!SkeletalMesh) return;

	FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || Asset->SourceVertices.empty()) return;

	if (!Asset->bBoundsValid)
	{
		Asset->CacheBounds();
	}

	CachedLocalCenter = Asset->BoundsCenter;
	CachedLocalExtent = Asset->BoundsExtent;
	bHasValidBounds = Asset->bBoundsValid;
}

void USkeletalMeshComponent::UpdateWorldAABB() const
{
	if (!bHasValidBounds)
	{
		UPrimitiveComponent::UpdateWorldAABB();
		return;
	}

	FVector WorldCenter = CachedWorldMatrix.TransformPositionWithW(CachedLocalCenter);

	float Ex = std::abs(CachedWorldMatrix.M[0][0]) * CachedLocalExtent.X
		+ std::abs(CachedWorldMatrix.M[1][0]) * CachedLocalExtent.Y
		+ std::abs(CachedWorldMatrix.M[2][0]) * CachedLocalExtent.Z;
	float Ey = std::abs(CachedWorldMatrix.M[0][1]) * CachedLocalExtent.X
		+ std::abs(CachedWorldMatrix.M[1][1]) * CachedLocalExtent.Y
		+ std::abs(CachedWorldMatrix.M[2][1]) * CachedLocalExtent.Z;
	float Ez = std::abs(CachedWorldMatrix.M[0][2]) * CachedLocalExtent.X
		+ std::abs(CachedWorldMatrix.M[1][2]) * CachedLocalExtent.Y
		+ std::abs(CachedWorldMatrix.M[2][2]) * CachedLocalExtent.Z;

	WorldAABBMinLocation = WorldCenter - FVector(Ex, Ey, Ez);
	WorldAABBMaxLocation = WorldCenter + FVector(Ex, Ey, Ez);
	bWorldAABBDirty = false;
	bHasValidWorldAABB = true;

	// TODO: Animation update
	// TODO: Bone palette update
	// TODO: CPU/GPU Skinning update
}

void USkeletalMeshComponent::InitializeSkinningState()
{
	SkinnedVertices.clear();
	CurrentBoneLocalTransforms.clear();
	CurrentBoneGlobalTransforms.clear();
	SkinningMatrices.clear();

	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		DynamicRenderBuffer.reset();
		bPoseDirty = false;
		bSkinningDirty = false;
		bRenderDataDirty = false;
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	SkinnedVertices.reserve(Asset->SourceVertices.size());
	for (const FSkeletalMeshVertex& SourceVertex : Asset->SourceVertices)
	{
		SkinnedVertices.push_back(MakeSkinnedRenderVertex(SourceVertex));
	}

	if (SkinnedVertices.size() != Asset->SourceVertices.size())
	{
		UE_LOG("[PIE SKM ERROR] Vertex count mismatch after InitializeSkinningState. Source=%d Skinned=%d",
			static_cast<int32>(Asset->SourceVertices.size()),
			static_cast<int32>(SkinnedVertices.size()));
	}

	const uint32 BoneCount = static_cast<uint32>(Asset->Bones.size());
	CurrentBoneLocalTransforms.resize(BoneCount);
	CurrentBoneGlobalTransforms.resize(BoneCount);
	SkinningMatrices.resize(BoneCount);

	for (uint32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		CurrentBoneLocalTransforms[BoneIndex] = Asset->Bones[BoneIndex].LocalBindTransform;
		CurrentBoneGlobalTransforms[BoneIndex] = Asset->Bones[BoneIndex].GlobalBindTransform;
		// TODO(PRID): Skinning matrix 계산 예정. 지금은 배열 크기와 안전한 기본값만 준비한다.
		SkinningMatrices[BoneIndex] = FMatrix::Identity;
	}

	bPoseDirty = true;
	bSkinningDirty = true;
	bRenderDataDirty = true;
}

void USkeletalMeshComponent::UpdatePose()
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		bPoseDirty = false;
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	const uint32 BoneCount = static_cast<uint32>(Asset->Bones.size());

	if (CurrentBoneLocalTransforms.size() != BoneCount)
	{
		CurrentBoneLocalTransforms.resize(BoneCount);

		for (uint32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			CurrentBoneLocalTransforms[BoneIndex] = Asset->Bones[BoneIndex].LocalBindTransform;
		}
	}

	CurrentBoneGlobalTransforms.resize(BoneCount);

	for (uint32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		const int32 ParentIndex = Asset->Bones[BoneIndex].ParentIndex;

		if (ParentIndex >= 0 && ParentIndex < static_cast<int32>(BoneIndex))
		{
			CurrentBoneGlobalTransforms[BoneIndex] =
				CurrentBoneLocalTransforms[BoneIndex] * CurrentBoneGlobalTransforms[ParentIndex];
		}
		else
		{
			CurrentBoneGlobalTransforms[BoneIndex] = CurrentBoneLocalTransforms[BoneIndex];
		}
	}

	bPoseDirty = false;
	bSkinningDirty = true;
}

void USkeletalMeshComponent::UpdateSkinningMatrices()
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		SkinningMatrices.clear();
		bSkinningDirty = false;
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	const uint32 BoneCount = static_cast<uint32>(Asset->Bones.size());

	SkinningMatrices.resize(BoneCount);

	// TODO(PRID):
	// 여기서 InverseBindTransform과 CurrentBoneGlobalTransform을 조합해서
	// SkinningMatrix를 직접 계산할 예정.
	// 이번 작업에서는 알고리즘을 구현하지 않고 안전한 기본값만 둔다.

	for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		const FSkeletalBoneInfo& Bone = Asset->Bones[BoneIndex];

		SkinningMatrices[BoneIndex] = Bone.InverseBindTransform * CurrentBoneGlobalTransforms[BoneIndex];
	}
}

void USkeletalMeshComponent::UpdateCPUSkinning()
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		SkinnedVertices.clear();
		bSkinningDirty = false;
		bRenderDataDirty = true;
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	SkinnedVertices.clear();
	SkinnedVertices.reserve(Asset->SourceVertices.size());

	// TODO(PRID):
	// 여기서 SourceVertices를 읽고,
	// BoneIndices / BoneWeights / SkinningMatrices를 이용해
	// SkinnedVertices를 갱신할 예정.
	//
	// 이번 작업에서는 스키닝하지 않고 SourceVertices를 렌더 정점으로 단순 복사만 한다.
	for (const FSkeletalMeshVertex& SourceVertex : Asset->SourceVertices)
	{
		FVector SkinnedPosition = FVector::ZeroVector;
		FVector SkinnedNormal = FVector::ZeroVector;

		// SkinnedVertices.push_back(MakeSkinnedRenderVertex(SourceVertex));

		float TotalWeight = 0.0f;

		for (int32 idx = 0; idx < 4; ++idx)
		{
			const uint32 BoneIndex = SourceVertex.BoneIndices[idx];
			const float Weight = SourceVertex.BoneWeights[idx];

			if (Weight <= 0.0f)
			{
				continue;
			}
			if (BoneIndex >= SkinningMatrices.size())
			{
				continue;
			}

			const FMatrix& SkinningMatrix = SkinningMatrices[BoneIndex];

			// Position은 이동성분 필요하므로 동차좌표계 사용
			const FVector TransformPosition = SkinningMatrix.TransformPositionWithW(SourceVertex.Position);

			// Normal은 벡터라서 wㅇ필요없음.
			const FVector TransformNormal = SkinningMatrix.TransformVector(SourceVertex.Normal);
			SkinnedPosition += TransformPosition * Weight;
			SkinnedNormal += TransformNormal * Weight;

			TotalWeight += Weight;
		}

		if (TotalWeight <= 1e-4)
		{
			SkinnedPosition = SourceVertex.Position;
			SkinnedNormal = SourceVertex.Normal;
		}
		else
		{
			const float InvTotalWeight = 1.f / TotalWeight;
			SkinnedPosition *= InvTotalWeight;
			SkinnedNormal *= InvTotalWeight;
		}

		SkinnedNormal.Normalize();

		FVertexPNCTT RenderVertex;
		RenderVertex.Position = SkinnedPosition;
		RenderVertex.Normal = SkinnedNormal;
		RenderVertex.Color = FVector4(1.f, 1.f, 1.f, 1.f);
		RenderVertex.UV = SourceVertex.UV;
		RenderVertex.Tangent = SourceVertex.Tangent;

		SkinnedVertices.push_back(RenderVertex);
	}
	

	bSkinningDirty = false;
	bRenderDataDirty = true;
}

void USkeletalMeshComponent::RebuildDynamicRenderBuffer()
{
	DynamicRenderBuffer.reset();

	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset() || SkinnedVertices.empty())
	{
		return;
	}

	if (!GEngine)
	{
		return;
	}

	ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
	if (!Device)
	{
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	TMeshData<FVertexPNCTT> RenderMeshData;
	RenderMeshData.Vertices = SkinnedVertices;
	RenderMeshData.Indices = Asset->Indices;

	DynamicRenderBuffer = std::make_unique<FMeshBuffer>();
	DynamicRenderBuffer->CreateDynamicVertexBuffer(Device, RenderMeshData);
	bRenderDataDirty = true;
}

bool USkeletalMeshComponent::UploadSkinnedVerticesToGPU()
{
	if (!bRenderDataDirty)
	{
		return DynamicRenderBuffer && DynamicRenderBuffer->IsValid();
	}

	if (!DynamicRenderBuffer || !DynamicRenderBuffer->IsValid() || !GEngine)
	{
		return false;
	}

	ID3D11DeviceContext* Context = GEngine->GetRenderer().GetFD3DDevice().GetDeviceContext();
	if (!Context)
	{
		return false;
	}

	const bool bUpdated = DynamicRenderBuffer->UpdateDynamicVertices(Context, SkinnedVertices);
	if (bUpdated)
	{
		bRenderDataDirty = false;
	}
	return bUpdated;
}

void USkeletalMeshComponent::MarkPoseDirty()
{
	bPoseDirty = true;
}

void USkeletalMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction& ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ApplyDebugBoneAnimation(DeltaTime);

	if (bPoseDirty)
	{
		UpdatePose();
	}

	if (bSkinningDirty)
	{
		UpdateSkinningMatrices();
		UpdateCPUSkinning();
	}

	if (bRenderDataDirty)
	{
		UploadSkinnedVerticesToGPU();
	}
}

void USkeletalMeshComponent::LogSkeletalMeshDebugInfo(bool bDynamicBufferCreated) const
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	UE_LOG("[SkeletalMesh] Loaded: %s", SkeletalMeshPath.c_str());
	UE_LOG("[PIE SKM] Component=%p SkeletalMesh=%p Asset=%p DynamicRenderBuffer=%p ID3D11VertexBuffer=%p SceneProxy=%p",
		this,
		SkeletalMesh,
		Asset,
		DynamicRenderBuffer.get(),
		DynamicRenderBuffer ? DynamicRenderBuffer->GetVertexBuffer().GetBuffer() : nullptr,
		GetSceneProxy());
	UE_LOG("[SkeletalMesh] Source Vertex Count: %d, Index Count: %d, Bone Count: %d",
		static_cast<int32>(Asset->SourceVertices.size()),
		static_cast<int32>(Asset->Indices.size()),
		static_cast<int32>(Asset->Bones.size()));

	const int32 BoneLogCount = (std::min)(static_cast<int32>(Asset->Bones.size()), 16);
	for (int32 BoneIndex = 0; BoneIndex < BoneLogCount; ++BoneIndex)
	{
		const FSkeletalBoneInfo& Bone = Asset->Bones[BoneIndex];
		const FVector GlobalBindT = Bone.GlobalBindTransform.GetLocation();
		const FVector InverseBindT = Bone.InverseBindTransform.GetLocation();
		UE_LOG("[SkeletalMesh] Bone[%d] Name=%s ParentIndex=%d GlobalBindT=(%.3f %.3f %.3f) InverseBindT=(%.3f %.3f %.3f)",
			BoneIndex,
			Bone.Name.c_str(),
			Bone.ParentIndex,
			GlobalBindT.X, GlobalBindT.Y, GlobalBindT.Z,
			InverseBindT.X, InverseBindT.Y, InverseBindT.Z);
	}

	const int32 VertexLogCount = (std::min)(static_cast<int32>(Asset->SourceVertices.size()), 8);
	if (!Asset->SourceVertices.empty())
	{
		const FSkeletalMeshVertex& Vertex = Asset->SourceVertices[0];
		UE_LOG("[SkeletalMesh] SourceVertex[0] Position=(%.3f %.3f %.3f)", Vertex.Position.X, Vertex.Position.Y, Vertex.Position.Z);
		UE_LOG("[SkeletalMesh] Vertex[0] BoneIndices=(%u %u %u %u) BoneWeights=(%.4f %.4f %.4f %.4f)",
			Vertex.BoneIndices[0], Vertex.BoneIndices[1], Vertex.BoneIndices[2], Vertex.BoneIndices[3],
			Vertex.BoneWeights[0], Vertex.BoneWeights[1], Vertex.BoneWeights[2], Vertex.BoneWeights[3]);
	}

	for (int32 VertexIndex = 0; VertexIndex < VertexLogCount; ++VertexIndex)
	{
		const FSkeletalMeshVertex& Vertex = Asset->SourceVertices[VertexIndex];
		const float WeightSum = Vertex.BoneWeights[0] + Vertex.BoneWeights[1] + Vertex.BoneWeights[2] + Vertex.BoneWeights[3];
		UE_LOG("[SkeletalMesh] Vertex[%d] BoneWeightSum=%.4f", VertexIndex, WeightSum);
	}

	UE_LOG("[SkeletalMesh] Dynamic VertexBuffer Created: %s", bDynamicBufferCreated ? "true" : "false");
}

void USkeletalMeshComponent::ApplyDebugBoneAnimation(float DeltaTime)
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();

	if (Asset->Bones.empty())
	{
		return;
	}

	DebugSkinningTime += DeltaTime;

	if (DebugAnimatedBoneIndex < 0)
	{
		// 우선 팔 쪽 이름을 대충 찾아본다.
		DebugAnimatedBoneIndex = FindBoneIndexByNameContains("spine");

		//if (DebugAnimatedBoneIndex < 0)
		//{
		//	DebugAnimatedBoneIndex = FindBoneIndexByNameContains("arm");
		//}
		//if (DebugAnimatedBoneIndex < 0)
		//{
		//	DebugAnimatedBoneIndex = FindBoneIndexByNameContains("ForeArm");
		//}
		//if (DebugAnimatedBoneIndex < 0)
		//{
		//	DebugAnimatedBoneIndex = FindBoneIndexByNameContains("forearm");
		//}

		//// 못 찾으면 root 말고 1번 bone으로 임시 테스트
		//if (DebugAnimatedBoneIndex < 0 && Asset->Bones.size() > 1)
		//{
		//	DebugAnimatedBoneIndex = 1;
		//}

		//if (DebugAnimatedBoneIndex >= 0)
		//{
		//	UE_LOG("[CPU Skinning Debug] AnimatedBoneIndex=%d Name=%s",
		//		DebugAnimatedBoneIndex,
		//		Asset->Bones[DebugAnimatedBoneIndex].Name.c_str());
		//}
	}

	if (DebugAnimatedBoneIndex < 0 ||
		DebugAnimatedBoneIndex >= static_cast<int32>(Asset->Bones.size()))
	{
		return;
	}

	const float AngleRad = sinf(DebugSkinningTime);

	// 처음에는 Z축 회전으로 테스트.
	// 이상하면 X/Y로 바꿔가며 확인.
	const FMatrix DeltaRotation = FMatrix::MakeRotationZ(AngleRad);

	CurrentBoneLocalTransforms[DebugAnimatedBoneIndex] =
		Asset->Bones[DebugAnimatedBoneIndex].LocalBindTransform * DeltaRotation;

	bPoseDirty = true;
}

int32 USkeletalMeshComponent::FindBoneIndexByNameContains(const FString& Keyword) const
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		return -1;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();

	for (int32 BoneIndex = 0; BoneIndex < static_cast<int32>(Asset->Bones.size()); ++BoneIndex)
	{
		const FString& BoneName = Asset->Bones[BoneIndex].Name;

		if (BoneName.find(Keyword) != FString::npos)
		{
			return BoneIndex;
		}
	}

	return -1;
}
