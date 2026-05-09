#include "SkeletalMeshComponent.h"

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

FMeshBuffer* USkeletalMeshComponent::GetMeshBuffer() const
{
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
	MarkTransformDirty();
	MarkWorldBoundsDirty();
}

FPrimitiveSceneProxy* USkeletalMeshComponent::CreateSceneProxy()
{
	return new FSkeletalMeshSceneProxy(this);
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

void USkeletalMeshComponent::UpdateWorldAABB() const
{
	if (!bEnableSkinning && SkeletalMesh)
	{
		const FSkeletalMesh* Asset = SkeletalMesh->GetSkeletalMeshAsset();
		if (Asset && !Asset->Vertices.empty() && !Asset->MeshRanges.empty())
		{
			bool bHasPoint = false;
			FVector WorldMin(0.0f, 0.0f, 0.0f);
			FVector WorldMax(0.0f, 0.0f, 0.0f);

			for (const FSkeletalMeshRange& Range : Asset->MeshRanges)
			{
				if (!Range.bHasMeshScene)
				{
					continue;
				}

				const uint32 VertexStart = (std::min)(Range.VertexStart, static_cast<uint32>(Asset->Vertices.size()));
				const uint32 VertexEnd = (std::min)(Range.VertexEnd, static_cast<uint32>(Asset->Vertices.size()));
				const FMatrix RangeWorld = Range.MeshSceneGlobal * CachedWorldMatrix;

				for (uint32 VertexIndex = VertexStart; VertexIndex < VertexEnd; ++VertexIndex)
				{
					const FVector WorldPosition = RangeWorld.TransformPositionWithW(Asset->Vertices[VertexIndex].Position);
					if (!bHasPoint)
					{
						WorldMin = WorldPosition;
						WorldMax = WorldPosition;
						bHasPoint = true;
					}
					else
					{
						WorldMin.X = (std::min)(WorldMin.X, WorldPosition.X);
						WorldMin.Y = (std::min)(WorldMin.Y, WorldPosition.Y);
						WorldMin.Z = (std::min)(WorldMin.Z, WorldPosition.Z);
						WorldMax.X = (std::max)(WorldMax.X, WorldPosition.X);
						WorldMax.Y = (std::max)(WorldMax.Y, WorldPosition.Y);
						WorldMax.Z = (std::max)(WorldMax.Z, WorldPosition.Z);
					}
				}
			}

			if (bHasPoint)
			{
				WorldAABBMinLocation = WorldMin;
				WorldAABBMaxLocation = WorldMax;
				bWorldAABBDirty = false;
				bHasValidWorldAABB = true;
				return;
			}
		}
	}

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
