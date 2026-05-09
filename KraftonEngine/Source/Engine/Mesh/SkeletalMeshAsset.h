#pragma once

#include "Core/EngineTypes.h"
#include "Mesh/ImportedVertexTypes.h"
#include "Render/Resource/Buffer.h"

#include <algorithm>
#include <memory>

struct FSkeletalVertex
{
	FVector Position;
	FVector Normal;
	FVector2 UV;
	FVector4 Tangent;
	uint32 BoneIndices[4] = { 0, 0, 0, 0 };
	float BoneWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct FSkeletalMeshRange
{
	uint32 VertexStart = 0;
	uint32 VertexEnd = 0;
	uint32 FirstIndex = 0;
	uint32 IndexCount = 0;

	FMatrix MeshBindGlobal = FMatrix::Identity;
	FMatrix InverseMeshBindGlobal = FMatrix::Identity;
	bool bHasMeshBind = false;
};

struct FSkeletalMesh
{
	FString PathFileName;
	TArray<FSkeletalVertex> Vertices;
	TArray<uint32> Indices;

	TArray<FImportedBone> Bones;
	TArray<FSkeletalMeshRange> MeshRanges;
	std::unique_ptr<FMeshBuffer> RenderBuffer;

	FVector BoundsCenter = FVector(0.0f, 0.0f, 0.0f);
	FVector BoundsExtent = FVector(0.0f, 0.0f, 0.0f);
	bool bBoundsValid = false;

	void CacheBounds()
	{
		bBoundsValid = false;
		if (Vertices.empty())
		{
			return;
		}

		FVector LocalMin = Vertices[0].Position;
		FVector LocalMax = Vertices[0].Position;

		for (const FSkeletalVertex& Vertex : Vertices)
		{
			LocalMin.X = (std::min)(LocalMin.X, Vertex.Position.X);
			LocalMin.Y = (std::min)(LocalMin.Y, Vertex.Position.Y);
			LocalMin.Z = (std::min)(LocalMin.Z, Vertex.Position.Z);

			LocalMax.X = (std::max)(LocalMax.X, Vertex.Position.X);
			LocalMax.Y = (std::max)(LocalMax.Y, Vertex.Position.Y);
			LocalMax.Z = (std::max)(LocalMax.Z, Vertex.Position.Z);
		}

		BoundsCenter = (LocalMin + LocalMax) * 0.5f;
		BoundsExtent = (LocalMax - LocalMin) * 0.5f;
		bBoundsValid = true;
	}
};
