// MeshTypes.cpp

#include <Engine.Core/MeshTypes.h>

#include <cassert>

namespace Engine::Core
{
#pragma region SubMesh
	size_t SubMesh::GetVertexCount() const
	{
		return Vertices.size();
	}

	size_t SubMesh::GetIndexCount() const
	{
		return Indices.size();
	}

	bool SubMesh::HasIndices() const
	{
		return !Indices.empty();
	}
#pragma endregion SubMesh

#pragma region Mesh
	Mesh::Mesh(std::vector<SubMesh> subMeshes, const DirectX::BoundingBox& bounds) : _subMeshes(std::move(subMeshes)), _bounds(bounds) {}

	size_t Mesh::GetSubMeshCount() const
	{
		return _subMeshes.size();
	}

	bool Mesh::IsEmpty() const
	{
		return _subMeshes.empty();
	}

	const DirectX::BoundingBox& Mesh::GetBounds() const
	{
		return _bounds;
	}

	const std::vector<SubMesh>& Mesh::GetSubMeshes() const
	{
		return _subMeshes;
	}
	
	const SubMesh& Mesh::GetSubMesh(const size_t subMeshIndex) const
	{
		assert(subMeshIndex < _subMeshes.size());
		return _subMeshes[subMeshIndex];
	}
#pragma endregion Mesh
}