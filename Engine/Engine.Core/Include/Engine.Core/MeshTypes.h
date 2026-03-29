// MeshTypes.h

#pragma once

#include <vector>

#include <DirectXCollision.h>
#include <directxtk/SimpleMath.h>

namespace Engine::Core
{
	namespace SimpleMath = DirectX::SimpleMath;
	
#pragma region Vertex
	struct Vertex
	{
		SimpleMath::Vector3 Position = {0.0f, 0.0f, 0.0f};
		SimpleMath::Vector3 Normal = {0.0f, 0.0f, 0.0f};
		SimpleMath::Vector2 TexCoord = {0.0f, 0.0f};
		SimpleMath::Vector3 Tangent = {1.0f, 0.0f, 0.0f};
	};
#pragma endregion Vertex

#pragma region SubMesh
	// Materials are not owned by the mesh asset itself.
	// Each submesh stores only material slot index that can be resolved later.
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
#pragma endregion SubMesh

#pragma region Mesh
	// Mesh stores CPU-side geometry only.
	// GPU buffers and upload details remain renderer-side concerns.
	class Mesh
	{
	private:
		std::vector<SubMesh> _subMeshes;
		DirectX::BoundingBox _bounds = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};

	public:
		Mesh() = default;
		Mesh(std::vector<SubMesh> subMeshes, const DirectX::BoundingBox& bounds);

		[[nodiscard]] size_t GetSubMeshCount() const;
		[[nodiscard]] bool IsEmpty() const;

		[[nodiscard]] const DirectX::BoundingBox& GetBounds() const;
		[[nodiscard]] const std::vector<SubMesh>& GetSubMeshes() const;
		[[nodiscard]] const SubMesh& GetSubMesh(const size_t subMeshIndex) const;
	};
#pragma endregion Mesh
}