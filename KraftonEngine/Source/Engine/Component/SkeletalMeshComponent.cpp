#include "SkeletalMeshComponent.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Core/PropertyTypes.h"
#include "Object/ObjectFactory.h"
#include "Engine/Runtime/Engine.h"
#include "Engine/Platform/Paths.h"
#include "Mesh/ObjManager.h"
#include "Mesh/SkeletalMesh.h"
#include "Render/Proxy/SkeletalMeshSceneProxy.h"
#include "Serialization/Archive.h"

IMPLEMENT_CLASS(USkeletalMeshComponent, UMeshComponent)

USkeletalMeshComponent::USkeletalMeshComponent()
{
	SetTickInEditor(true);
}

FMeshBuffer* USkeletalMeshComponent::GetMeshBuffer() const
{
	if (bEnableSkinning && SkinnedRenderBuffer)
	{
		return SkinnedRenderBuffer.get();
	}

	if (!SkeletalMesh)
	{
		return nullptr;
	}

	FSkeletalMesh* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || !Asset->RenderBuffer)
	{
		return nullptr;
	}
	return Asset->RenderBuffer.get();
}

void USkeletalMeshComponent::SetSkeletalMesh(USkeletalMesh* InMesh)
{
	SkeletalMesh = InMesh;
	if (InMesh)
	{
		SkeletalMeshPath = InMesh->GetAssetPathFileName();
	}
	else
	{
		SkeletalMeshPath = "None";
	}

	InitSkinningResources();
	CacheLocalBounds();
	MarkRenderStateDirty();
	MarkWorldBoundsDirty();
}

void USkeletalMeshComponent::SetSkinningEnabled(bool bInEnableSkinning)
{
	if (bEnableSkinning == bInEnableSkinning)
	{
		return;
	}

	bEnableSkinning = bInEnableSkinning;
	MarkRenderStateDirty();
	MarkTransformDirty();
	MarkWorldBoundsDirty();
}

FPrimitiveSceneProxy* USkeletalMeshComponent::CreateSceneProxy()
{
	return new FSkeletalMeshSceneProxy(this);
}

void USkeletalMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction& ThisTickFunction)
{
	UActorComponent::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bEnableSkinning)
	{
		return;
	}

	DebugSkinningTime += DeltaTime;
	UpdateCPUSkinning();
	UpdateSkinnedVertexBuffer();
}

void USkeletalMeshComponent::CacheLocalBounds()
{
	bHasValidBounds = false;
	if (!SkeletalMesh) return;

	FSkeletalMesh* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || Asset->Vertices.empty()) return;

	if (!Asset->bBoundsValid)
	{
		Asset->CacheBounds();
	}

	CachedLocalCenter = Asset->BoundsCenter;
	CachedLocalExtent = Asset->BoundsExtent;
	bHasValidBounds = Asset->bBoundsValid;
}

void USkeletalMeshComponent::InitSkinningResources()
{
	SkinnedVertices.clear();
	SkinnedRenderBuffer.reset();

	if (!SkeletalMesh)
		return;

	FSkeletalMesh* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || Asset->Vertices.empty() || Asset->Indices.empty())
		return;

	ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
	if (!Device)
		return;

	SkinnedVertices.resize(Asset->Vertices.size());

	TMeshData<FVertexPNCTT> MeshData;
	MeshData.Vertices.resize(Asset->Vertices.size());
	MeshData.Indices = Asset->Indices;

	for (size_t i = 0; i < Asset->Vertices.size(); ++i)
	{
		const FSkeletalVertex& Src = Asset->Vertices[i];

		FVertexPNCTT Dst;
		Dst.Position = Src.Position;
		Dst.Normal = Src.Normal;
		Dst.UV = Src.UV;
		Dst.Tangent = Src.Tangent;
		Dst.Color = FVector4(1, 1, 1, 1);

		MeshData.Vertices[i] = Dst;
		SkinnedVertices[i] = Dst;
	}

	SkinnedRenderBuffer = std::make_unique<FMeshBuffer>();
	SkinnedRenderBuffer->CreateDynamicVertexBuffer(Device, MeshData);
}

void USkeletalMeshComponent::UpdateCPUSkinning()
{
	if (!bEnableSkinning || !SkeletalMesh || SkinnedVertices.empty())
	{
		return;
	}

	const FSkeletalMesh* Asset = SkeletalMesh->GetSkeletalMeshAsset();
	if (!Asset || Asset->Vertices.empty())
	{
		return;
	}

	const TArray<FImportedBone>& Bones = Asset->Bones;
	const int BoneCount = static_cast<int>(Bones.size());

	if (BoneCount == 0)
	{
		return;
	}

	BoneCurrentGlobalMatrices.resize(BoneCount);
	SkinMatrices.resize(BoneCount);
	TArray<FMatrix> CurrentLocalTransforms;
	CurrentLocalTransforms.resize(BoneCount);

	for (int i = 0; i < BoneCount; ++i)
	{
		const FImportedBone& Bone = Bones[i];
		if (Bone.ParentIndex < 0)
		{
			CurrentLocalTransforms[i] = Bone.BindGlobal;
		}
		else
		{
			const FMatrix& ParentBindGlobal = Bones[Bone.ParentIndex].BindGlobal;
			CurrentLocalTransforms[i] = Bone.BindGlobal * ParentBindGlobal.GetInverse();
		}
		//이후 애니메이션 추가
	}

	if (bEnableDebugBoneAnimation && BoneCount > 1)
	{
		const int32 BoneIndex = (std::clamp)(DebugAnimatedBoneIndex, 1, BoneCount - 1);
		const float Angle = std::sin(DebugSkinningTime * 2.0f) * 0.7f;
		const FMatrix DebugRotation = FMatrix::MakeRotationAxis(FVector::UpVector, Angle);
		CurrentLocalTransforms[BoneIndex] = DebugRotation * CurrentLocalTransforms[BoneIndex];
	}

	for (int i = 0; i < BoneCount; ++i)
	{
		const FImportedBone& Bone = Bones[i];
		if (Bone.ParentIndex < 0)
		{
			BoneCurrentGlobalMatrices[i] = CurrentLocalTransforms[i];
		}
		else
		{
			const FMatrix& ParentGlobal = BoneCurrentGlobalMatrices[Bone.ParentIndex];
			BoneCurrentGlobalMatrices[i] = CurrentLocalTransforms[i] * ParentGlobal;
		}
	}

	for (int i = 0; i < BoneCount; ++i)
	{
		SkinMatrices[i] = Bones[i].InverseBindGlobal * BoneCurrentGlobalMatrices[i];
	}

	auto SkinVertexRange = [this, Asset, &Bones](uint32 VertexStart, uint32 VertexEnd, const FMatrix& MeshBindGlobal, const FMatrix& MeshCurrentGlobal)
		{
			const FMatrix InverseMeshBindGlobal = MeshBindGlobal.GetInverse();
			const uint32 SafeVertexEnd = (std::min)(VertexEnd, static_cast<uint32>(Asset->Vertices.size()));
			const uint32 SafeVertexStart = (std::min)(VertexStart, SafeVertexEnd);

			for (uint32 VertexIndex = SafeVertexStart; VertexIndex < SafeVertexEnd; ++VertexIndex)
			{
				const FSkeletalVertex& Src = Asset->Vertices[VertexIndex];
				FVector Position(0.0f, 0.0f, 0.0f);
				FVector Normal(0.0f, 0.0f, 0.0f);
				FVector Tangent(0.0f, 0.0f, 0.0f);
				float TotalWeight = 0.0f;

				for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
				{
					const float Weight = Src.BoneWeights[InfluenceIndex];
					const uint32 BoneIndex = Src.BoneIndices[InfluenceIndex];
					if (Weight <= 0.0f || BoneIndex >= Bones.size())
					{
						continue;
					}

					const FMatrix SkinMatrix =
						MeshBindGlobal *
						Bones[BoneIndex].InverseBindGlobal *
						BoneCurrentGlobalMatrices[BoneIndex] *
						InverseMeshBindGlobal *
						MeshCurrentGlobal;

					Position += SkinMatrix.TransformPositionWithW(Src.Position) * Weight;
					Normal += SkinMatrix.TransformVector(Src.Normal) * Weight;
					Tangent += SkinMatrix.TransformVector(FVector(Src.Tangent.X, Src.Tangent.Y, Src.Tangent.Z)) * Weight;
					TotalWeight += Weight;
				}

				if (TotalWeight <= 0.0f)
				{
					Position = Src.Position;
					Normal = Src.Normal;
					Tangent = FVector(Src.Tangent.X, Src.Tangent.Y, Src.Tangent.Z);
				}
				else if (std::abs(TotalWeight - 1.0f) > 1e-4f)
				{
					const float InvTotalWeight = 1.0f / TotalWeight;
					Position *= InvTotalWeight;
					Normal *= InvTotalWeight;
					Tangent *= InvTotalWeight;
				}

				if (Normal.Length() > 1e-6f)
				{
					Normal.Normalize();
				}
				else
				{
					Normal = Src.Normal;
				}

				if (Tangent.Length() > 1e-6f)
				{
					Tangent.Normalize();
				}
				else
				{
					Tangent = FVector(Src.Tangent.X, Src.Tangent.Y, Src.Tangent.Z);
				}

				FVertexPNCTT& Dst = SkinnedVertices[VertexIndex];
				Dst.Position = Position;
				Dst.Normal = Normal;
				Dst.Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
				Dst.UV = Src.UV;
				Dst.Tangent = FVector4(Tangent.X, Tangent.Y, Tangent.Z, Src.Tangent.W);
			}
		};

	auto TransformVertexRange = [this, Asset](uint32 VertexStart, uint32 VertexEnd, const FMatrix& TransformMatrix)
		{
			const uint32 SafeVertexEnd = (std::min)(VertexEnd, static_cast<uint32>(Asset->Vertices.size()));
			const uint32 SafeVertexStart = (std::min)(VertexStart, SafeVertexEnd);

			for (uint32 VertexIndex = SafeVertexStart; VertexIndex < SafeVertexEnd; ++VertexIndex)
			{
				const FSkeletalVertex& Src = Asset->Vertices[VertexIndex];
				FVector Normal = TransformMatrix.TransformVector(Src.Normal);
				FVector Tangent = TransformMatrix.TransformVector(FVector(Src.Tangent.X, Src.Tangent.Y, Src.Tangent.Z));

				if (Normal.Length() > 1e-6f)
				{
					Normal.Normalize();
				}
				else
				{
					Normal = Src.Normal;
				}

				if (Tangent.Length() > 1e-6f)
				{
					Tangent.Normalize();
				}
				else
				{
					Tangent = FVector(Src.Tangent.X, Src.Tangent.Y, Src.Tangent.Z);
				}

				FVertexPNCTT& Dst = SkinnedVertices[VertexIndex];
				Dst.Position = TransformMatrix.TransformPositionWithW(Src.Position);
				Dst.Normal = Normal;
				Dst.Color = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
				Dst.UV = Src.UV;
				Dst.Tangent = FVector4(Tangent.X, Tangent.Y, Tangent.Z, Src.Tangent.W);
			}
		};

	if (!Asset->MeshRanges.empty())
	{
		for (const FSkeletalMeshRange& Range : Asset->MeshRanges)
		{
			const FMatrix& MeshBindGlobal = Range.bHasMeshBind ? Range.MeshBindGlobal : FMatrix::Identity;
			const FMatrix& MeshCurrentGlobal = Range.bHasMeshScene ? Range.MeshSceneGlobal : MeshBindGlobal;

			if (Range.BindingType == ESkeletalMeshRangeBinding::Skinned)
			{
				SkinVertexRange(Range.VertexStart, Range.VertexEnd, MeshBindGlobal, MeshCurrentGlobal);
			}
			else if (Range.BindingType == ESkeletalMeshRangeBinding::RigidBone
				&& Range.RigidBoneIndex >= 0
				&& Range.RigidBoneIndex < static_cast<int32>(Bones.size()))
			{
				const FMatrix RigidTransform =
					MeshCurrentGlobal *
					Bones[Range.RigidBoneIndex].InverseBindGlobal *
					BoneCurrentGlobalMatrices[Range.RigidBoneIndex];
				TransformVertexRange(Range.VertexStart, Range.VertexEnd, RigidTransform);
			}
			else
			{
				TransformVertexRange(Range.VertexStart, Range.VertexEnd, MeshCurrentGlobal);
			}
		}
	}
	else
	{
		SkinVertexRange(0, static_cast<uint32>(Asset->Vertices.size()), FMatrix::Identity, FMatrix::Identity);
	}
}

void USkeletalMeshComponent::UpdateSkinnedVertexBuffer()
{
	if (!SkinnedRenderBuffer || SkinnedVertices.empty())
	{
		return;
	}

	ID3D11DeviceContext* DeviceContext = GEngine->GetRenderer().GetFD3DDevice().GetDeviceContext();
	if (!DeviceContext)
	{
		return;
	}

	SkinnedRenderBuffer->UpdateDynamicVertices(DeviceContext, SkinnedVertices);
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
}

void USkeletalMeshComponent::Serialize(FArchive& Ar)
{
	UMeshComponent::Serialize(Ar);
	Ar << SkeletalMeshPath;
}

void USkeletalMeshComponent::PostDuplicate()
{
	UMeshComponent::PostDuplicate();

	if (!SkeletalMeshPath.empty() && SkeletalMeshPath != "None")
	{
		ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
		SetSkeletalMesh(FObjManager::LoadFbxSkeletalMesh(SkeletalMeshPath, Device));
	}

	CacheLocalBounds();
	MarkRenderStateDirty();
	MarkWorldBoundsDirty();
}

void USkeletalMeshComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	UPrimitiveComponent::GetEditableProperties(OutProps);
	OutProps.push_back({ "Skeletal Mesh", EPropertyType::SkeletalMeshRef, "Mesh", &SkeletalMeshPath });
}

void USkeletalMeshComponent::PostEditProperty(const char* PropertyName)
{
	UPrimitiveComponent::PostEditProperty(PropertyName);

	if (std::strcmp(PropertyName, "Skeletal Mesh") == 0)
	{
		if (SkeletalMeshPath.empty() || SkeletalMeshPath == "None")
		{
			SetSkeletalMesh(nullptr);
		}
		else
		{
			ID3D11Device* Device = GEngine->GetRenderer().GetFD3DDevice().GetDevice();
			SetSkeletalMesh(FObjManager::LoadFbxSkeletalMesh(SkeletalMeshPath, Device));
		}
	}
}
