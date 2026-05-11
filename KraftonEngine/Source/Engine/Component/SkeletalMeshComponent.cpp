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
#include "Math/Matrix.h"

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

	for (uint32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		const FSkeletalBoneInfo& Bone = Asset->Bones[BoneIndex];

		// 논리 공식은 행벡터 기준이다.
		// SceneBindPosition * Bone.InverseBindTransform * CurrentBoneGlobalTransform
		SkinningMatrices[BoneIndex] = Bone.InverseBindTransform * CurrentBoneGlobalTransforms[BoneIndex];

		if (BoneIndex < 4) //Unreal은 8임.
		{
			const FMatrix BindCheckMatrix = Bone.InverseBindTransform * Bone.GlobalBindTransform;
			const FVector BindCheckT = BindCheckMatrix.GetLocation();
			const bool bAlmostIdentity = BindCheckMatrix.IsIdentity();
			UE_LOG("[Skin BindCheck] Bone[%d] %s BindCheckT=(%.6f %.6f %.6f) Identity=%s",
				BoneIndex,
				Bone.Name.c_str(),
				BindCheckT.X, BindCheckT.Y, BindCheckT.Z,
				bAlmostIdentity ? "true" : "false");
		}
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
	SkinnedVertices.resize(Asset->SourceVertices.size());

	// ============================================================
	// CPU Skinning: Mesh Range 단위 처리
	// ============================================================
	// SourceVertex.Position은 FBX ControlPoint 원본 좌표다.
	// 따라서 VertexBuffer 전체에 같은 보정 행렬을 한 번에 적용하지 않는다.
	// 각 VertexRange가 가진 GeometryTransform과 MeshNodeGlobalBindTransform을
	// 먼저 적용한 뒤 Bone의 Global Bind 기준 SkinningMatrix를 곱한다.
	TArray<FSkeletalMeshVertexRange> RuntimeRanges = Asset->VertexRanges;
	if (RuntimeRanges.empty() && !Asset->SourceVertices.empty())
	{
		FSkeletalMeshVertexRange FullRange;
		FullRange.BaseVertex = 0;
		FullRange.VertexCount = static_cast<uint32>(Asset->SourceVertices.size());
		FullRange.BaseIndex = 0;
		FullRange.IndexCount = static_cast<uint32>(Asset->Indices.size());
		FullRange.MeshNodeName = "Unknown";
		RuntimeRanges.push_back(FullRange);
	}

	for (const FSkeletalMeshVertexRange& Range : RuntimeRanges)
	{
		const uint32 RangeEnd = (std::min)(
			Range.BaseVertex + Range.VertexCount,
			static_cast<uint32>(Asset->SourceVertices.size()));

		for (uint32 VertexIndex = Range.BaseVertex; VertexIndex < RangeEnd; ++VertexIndex)
		{
			const FSkeletalMeshVertex& SourceVertex = Asset->SourceVertices[VertexIndex];

			// 논리 공식은 행벡터 기준이다.
			// ControlPoint -> GeometryTransform -> MeshNodeGlobalBindTransform -> BoneInverseBindTransform -> BoneCurrentGlobalTransform
			const FVector MeshLocalPosition = SourceVertex.Position;
			const FVector GeometryLocalPosition = Range.GeometryTransform.TransformPositionWithW(MeshLocalPosition);
			const FVector SceneBindPosition = Range.MeshNodeGlobalBindTransform.TransformPositionWithW(GeometryLocalPosition);

			const FVector GeometryLocalNormal = Range.GeometryTransform.TransformVector(SourceVertex.Normal);
			const FVector SceneBindNormal = Range.MeshNodeGlobalBindTransform.TransformVector(GeometryLocalNormal);
			const FVector SourceTangent(SourceVertex.Tangent.X, SourceVertex.Tangent.Y, SourceVertex.Tangent.Z);
			const FVector GeometryLocalTangent = Range.GeometryTransform.TransformVector(SourceTangent);
			const FVector SceneBindTangent = Range.MeshNodeGlobalBindTransform.TransformVector(GeometryLocalTangent);

			FVector SkinnedPosition = FVector::ZeroVector;
			FVector SkinnedNormal = FVector::ZeroVector;
			FVector SkinnedTangent = FVector::ZeroVector;

			float TotalWeight = 0.0f;

			for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
			{
				const uint32 BoneIndex = SourceVertex.BoneIndices[InfluenceIndex];
				const float Weight = SourceVertex.BoneWeights[InfluenceIndex];

				if (Weight <= 0.0f)
				{
					continue;
				}
				if (BoneIndex >= SkinningMatrices.size())
				{
					continue;
				}

				const FMatrix& SkinningMatrix = SkinningMatrices[BoneIndex];

				// Position은 이동 성분이 필요하므로 동차좌표계로 변환한다.
				const FVector WeightedPosition = SkinningMatrix.TransformPositionWithW(SceneBindPosition);

				// Normal은 방향 벡터라서 translation을 제외한다.
				const FVector WeightedNormal = SkinningMatrix.TransformVector(SceneBindNormal);
				const FVector WeightedTangent = SkinningMatrix.TransformVector(SceneBindTangent);

				SkinnedPosition += WeightedPosition * Weight;
				SkinnedNormal += WeightedNormal * Weight;
				SkinnedTangent += WeightedTangent * Weight;
				TotalWeight += Weight;
			}

			if (TotalWeight <= 1e-4f)
			{
				SkinnedPosition = SceneBindPosition;
				SkinnedNormal = SceneBindNormal;
				SkinnedTangent = SceneBindTangent;
			}
			else
			{
				const float InvTotalWeight = 1.0f / TotalWeight;
				SkinnedPosition *= InvTotalWeight;
				SkinnedNormal *= InvTotalWeight;
				SkinnedTangent *= InvTotalWeight;
			}

			SkinnedNormal.Normalize();
			SkinnedTangent.Normalize();

			FVertexPNCTT RenderVertex;
			RenderVertex.Position = SkinnedPosition;
			RenderVertex.Normal = SkinnedNormal;
			RenderVertex.Color = FVector4(1.f, 1.f, 1.f, 1.f);
			RenderVertex.UV = SourceVertex.UV;
			RenderVertex.Tangent = FVector4(SkinnedTangent.X, SkinnedTangent.Y, SkinnedTangent.Z, SourceVertex.Tangent.W);

			SkinnedVertices[VertexIndex] = RenderVertex;
		}
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

void USkeletalMeshComponent::DebugValidateBindPose() const
{
	if (!SkeletalMesh || !SkeletalMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	const FSkeletalMeshAsset* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	const int32 BoneLogCount = (std::min)(static_cast<int32>(Asset->Bones.size()), 16);

	for (int32 BoneIndex = 0; BoneIndex < BoneLogCount; ++BoneIndex)
	{
		const FSkeletalBoneInfo& Bone = Asset->Bones[BoneIndex];
		const FMatrix BindCheckMatrix = Bone.InverseBindTransform * Bone.GlobalBindTransform;
		const FVector BindCheckT = BindCheckMatrix.GetLocation();
		UE_LOG("[Skin BindCheck] Bone[%d] %s BindCheckT=(%.6f %.6f %.6f) Identity=%s",
			BoneIndex,
			Bone.Name.c_str(),
			BindCheckT.X, BindCheckT.Y, BindCheckT.Z,
			BindCheckMatrix.IsIdentity() ? "true" : "false");
	}
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

	const int32 RangeLogCount = (std::min)(static_cast<int32>(Asset->VertexRanges.size()), 16);
	for (int32 RangeIndex = 0; RangeIndex < RangeLogCount; ++RangeIndex)
	{
		const FSkeletalMeshVertexRange& Range = Asset->VertexRanges[RangeIndex];
		const FVector GeometryT = Range.GeometryTransform.GetLocation();
		const FVector MeshNodeT = Range.MeshNodeGlobalBindTransform.GetLocation();
		UE_LOG("[FBX MeshRange] Range[%d] MeshNode=%s BaseVertex=%u VertexCount=%u BaseIndex=%u IndexCount=%u GeometryT=(%.3f %.3f %.3f) MeshNodeBindT=(%.3f %.3f %.3f)",
			RangeIndex,
			Range.MeshNodeName.c_str(),
			Range.BaseVertex,
			Range.VertexCount,
			Range.BaseIndex,
			Range.IndexCount,
			GeometryT.X, GeometryT.Y, GeometryT.Z,
			MeshNodeT.X, MeshNodeT.Y, MeshNodeT.Z);
	}

	const int32 BoneLogCount = (std::min)(static_cast<int32>(Asset->Bones.size()), 16);
	for (int32 BoneIndex = 0; BoneIndex < BoneLogCount; ++BoneIndex)
	{
		const FSkeletalBoneInfo& Bone = Asset->Bones[BoneIndex];
		const FVector GlobalBindT = Bone.GlobalBindTransform.GetLocation();
		const FVector InverseBindT = Bone.InverseBindTransform.GetLocation();
		UE_LOG("[FBX BoneBind] Bone[%d] Name=%s ParentIndex=%d ChildCount=%d GlobalBindT=(%.3f %.3f %.3f) InverseBindT=(%.3f %.3f %.3f)",
			BoneIndex,
			Bone.Name.c_str(),
			Bone.ParentIndex,
			Bone.ChildCount,
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
		DebugAnimatedBoneIndex = FindBoneIndexByNameContains("spine");
		// DebugAnimatedBoneIndex = FindBoneIndexByNameContains("Base");
	}

	if (DebugAnimatedBoneIndex < 0 ||
		DebugAnimatedBoneIndex >= static_cast<int32>(Asset->Bones.size()))
	{
		return;
	}

	const FMatrix BindLocal = Asset->Bones[DebugAnimatedBoneIndex].LocalBindTransform;

	// 행벡터 기준: translation은 4번째 행에 있음.
	const FVector BindTranslation = BindLocal.GetLocation();

	// Local bind에서 translation만 제거한다.
	// 즉, bind pose의 scale/rotation만 남긴다.
	FMatrix BindLocalNoTranslation = BindLocal;
	BindLocalNoTranslation.SetLocation(FVector::ZeroVector);

	// -45도 ~ +45도
	constexpr float MaxAngleRad = 45.f * FMath::DegToRad;
	const float AngleRad = sinf(DebugSkinningTime) * MaxAngleRad;

	// 테스트 축. spine이면 Z보다 X/Y가 더 자연스러울 수 있음.
	const FMatrix DeltaRotation = FMatrix::MakeRotationZ(AngleRad);

	// 행벡터 기준:
	// v * DebugRotation * BindSR * BindTranslation
	const FMatrix AnimatedLocal =
		BindLocalNoTranslation *
		DeltaRotation *
		FMatrix::MakeTranslationMatrix(BindTranslation);

	CurrentBoneLocalTransforms[DebugAnimatedBoneIndex] = AnimatedLocal;

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
