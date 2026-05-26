// MeshTypes.h

#pragma once

#include <vector>

#include <DirectXCollision.h>
#include <directxtk/SimpleMath.h>

namespace Engine::Core
{
	struct Vertex
	{
		DirectX::SimpleMath::Vector3 Position = {0.0f, 0.0f, 0.0f};
		DirectX::SimpleMath::Vector3 Normal = {0.0f, 0.0f, 0.0f};
		DirectX::SimpleMath::Vector2 TexCoord = {0.0f, 0.0f};
		DirectX::SimpleMath::Vector3 Tangent = {1.0f, 0.0f, 0.0f};
	};

	struct SubMesh
	{
		std::vector<Vertex> Vertices;
		std::vector<std::uint32_t> Indices;
		std::uint32_t MaterialIndex = 0;
		DirectX::BoundingBox Bounds = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};

		/// Returns the number of vertices in this submesh.
		[[nodiscard]] size_t GetVertexCount() const;
		
		/// Returns the number of indices in this submesh.
		[[nodiscard]] size_t GetIndexCount() const;

		/// Returns true if this submesh has an index buffer.
		[[nodiscard]] bool HasIndices() const;
	};

	/// CPU-side mesh representation.
	class Mesh
	{
	public:
		Mesh() = default;
		Mesh(std::vector<SubMesh> subMeshes, const DirectX::BoundingBox& bounds);

		/// Returns true if the mesh has no subresources.
		[[nodiscard]] bool IsEmpty() const;

		/// Returns the number of submeshes.
		[[nodiscard]] size_t GetSubMeshCount() const;

		/// Returns the bounding box enclosing all submeshes.
		[[nodiscard]] const DirectX::BoundingBox& GetBounds() const;

		/// Returns all submeshes as a flat array.
		[[nodiscard]] const std::vector<SubMesh>& GetSubMeshes() const;

		/// Returns the submesh at the given index.
		[[nodiscard]] const SubMesh& GetSubMesh(const size_t subMeshIndex) const;

	private:
		std::vector<SubMesh> _subMeshes;
		DirectX::BoundingBox _bounds = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
	};
}