#include "Mesh/SkeletalMeshBuilder.h"

#include "Mesh/ImportedVertexTypes.h"
#include "Mesh/SkeletalMeshAsset.h"

static bool ValidateImportedMesh(const FImportedSkeletalMesh& Imported)
{
	if (Imported.SkeletalVertices.empty() || Imported.Indices.empty())
	{
		return false;
	}

	if (Imported.Indices.size() % 3 != 0)
	{
		return false;
	}

	if (Imported.Bones.empty())
	{
		return false;
	}

	const uint32 VertexCount = static_cast<uint32>(Imported.SkeletalVertices.size());
	for (uint32 Index : Imported.Indices)
	{
		if (Index >= VertexCount)
		{
			return false;
		}
	}

	return true;
}

static FSkeletalVertex BuildSkeletalVertex(const FImportedSkeletalVertex& ImportedVertex, uint32 BoneCount)
{
	FSkeletalVertex Vertex;
	Vertex.Position = ImportedVertex.Position;
	Vertex.Normal = ImportedVertex.Normal;
	Vertex.UV = ImportedVertex.UV;
	Vertex.Tangent = ImportedVertex.Tangent;

	float TotalWeight = 0.0f;
	for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
	{
		const uint32 BoneIndex = ImportedVertex.BoneIndices[InfluenceIndex];
		const float Weight = ImportedVertex.BoneWeights[InfluenceIndex];

		if (BoneIndex < BoneCount && Weight > 0.0f)
		{
			Vertex.BoneIndices[InfluenceIndex] = BoneIndex;
			Vertex.BoneWeights[InfluenceIndex] = Weight;
			TotalWeight += Weight;
		}
		else
		{
			Vertex.BoneIndices[InfluenceIndex] = 0;
			Vertex.BoneWeights[InfluenceIndex] = 0.0f;
		}
	}

	if (TotalWeight > 0.0f)
	{
		const float InvTotalWeight = 1.0f / TotalWeight;
		for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
		{
			Vertex.BoneWeights[InfluenceIndex] *= InvTotalWeight;
		}
	}
	else
	{
		Vertex.BoneIndices[0] = 0;
		Vertex.BoneWeights[0] = 1.0f;
	}

	return Vertex;
}

bool FSkeletalMeshBuilder::BuildFromImported(const FImportedSkeletalMesh& Imported, FSkeletalMesh& OutMesh)
{
	if (!ValidateImportedMesh(Imported))
	{
		OutMesh = FSkeletalMesh();
		return false;
	}

	OutMesh = FSkeletalMesh();
	OutMesh.Vertices.reserve(Imported.SkeletalVertices.size());

	const uint32 BoneCount = static_cast<uint32>(Imported.Bones.size());
	for (const FImportedSkeletalVertex& ImportedVertex : Imported.SkeletalVertices)
	{
		OutMesh.Vertices.push_back(BuildSkeletalVertex(ImportedVertex, BoneCount));
	}

	OutMesh.Indices = Imported.Indices;
	OutMesh.Bones = Imported.Bones;
	OutMesh.MeshRanges.clear();
	OutMesh.MeshRanges.reserve(Imported.MeshRanges.size());

	for (const FImportedSkeletalMeshRange& ImportedRange : Imported.MeshRanges)
	{
		FSkeletalMeshRange Range;
		Range.VertexStart = ImportedRange.VertexStart;
		Range.VertexEnd = ImportedRange.VertexEnd;
		Range.FirstIndex = ImportedRange.FirstIndex;
		Range.IndexCount = ImportedRange.IndexCount;
		Range.MeshSceneGlobal = ImportedRange.MeshSceneGlobal;
		Range.bHasMeshScene = ImportedRange.bHasMeshScene;
		Range.MeshBindGlobal = ImportedRange.MeshBindGlobal;
		Range.InverseMeshBindGlobal = ImportedRange.InverseMeshBindGlobal;
		Range.bHasMeshBind = ImportedRange.bHasMeshBind;
		Range.BindingType = ImportedRange.BindingType;
		Range.RigidBoneIndex = ImportedRange.RigidBoneIndex;
		OutMesh.MeshRanges.push_back(Range);
	}

	for (FImportedBone& Bone : OutMesh.Bones)
	{
		Bone.SourceNode = nullptr;
	}

	OutMesh.CacheBounds();
	return true;
}
