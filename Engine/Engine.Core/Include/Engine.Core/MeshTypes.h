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
		std::uint32_t StartVertexLocation = 0;
		std::uint32_t StartIndexLocation = 0;

		[[nodiscard]] size_t GetVertexCount() const;
		[[nodiscard]] size_t GetIndexCount() const;
		[[nodiscard]] bool HasIndices() const;
	};

	/// CPU-side mesh representation.
	class Mesh
	{
	private:
		std::vector<SubMesh> _subMeshes;
		DirectX::BoundingBox _bounds = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};

	public:
		Mesh() = default;
		Mesh(std::vector<SubMesh> subMeshes, const DirectX::BoundingBox& bounds);

		/// Returns true if the mesh has no subresources.
		[[nodiscard]] bool IsEmpty() const;

		/// Returns the total number of subresources.
		[[nodiscard]] size_t GetSubMeshCount() const;

		[[nodiscard]] const DirectX::BoundingBox& GetBounds() const;
		[[nodiscard]] const std::vector<SubMesh>& GetSubMeshes() const;
		[[nodiscard]] const SubMesh& GetSubMesh(const size_t subMeshIndex) const;
	};
}