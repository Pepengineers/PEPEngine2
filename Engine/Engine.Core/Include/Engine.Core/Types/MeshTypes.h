// MeshTypes.h

#pragma once

#include <filesystem>
#include <string>
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

	/// Describes how an imported material should be routed by the renderer.
	enum class EMaterialType : std::uint8_t
	{
		Opaque = 0,
		Masked = 1,
		Transparent = 2,
	};

	struct MeshMaterial
	{
		std::string Name;
		std::filesystem::path DiffuseTexturePath;
		std::filesystem::path NormalTexturePath;
		std::filesystem::path SpecularTexturePath;
		std::filesystem::path RoughnessTexturePath;
		std::filesystem::path EmissiveTexturePath;
		std::filesystem::path OpacityTexturePath;
		DirectX::SimpleMath::Vector3 DiffuseColor = {1.0f, 1.0f, 1.0f};
		DirectX::SimpleMath::Vector3 SpecularColor = {0.0f, 0.0f, 0.0f};
		DirectX::SimpleMath::Vector3 EmissiveColor = {0.0f, 0.0f, 0.0f};
		float Roughness = 1.0f;
		float Opacity = 1.0f;
		EMaterialType Type = EMaterialType::Opaque;
		bool UseBakedLighting = false;
	};

	/// CPU-side mesh representation.
	class Mesh
	{
	public:
		Mesh() = default;
		Mesh(
			std::vector<SubMesh> subMeshes,
			const DirectX::BoundingBox& bounds,
			std::vector<MeshMaterial> materials = {});

		/// Returns true if the mesh has no subresources.
		[[nodiscard]] bool IsEmpty() const;

		/// Returns the number of submeshes.
		[[nodiscard]] size_t GetSubMeshCount() const;

		/// Returns the bounding box enclosing all submeshes.
		[[nodiscard]] const DirectX::BoundingBox& GetBounds() const;

		/// Returns all submeshes as a flat array.
		[[nodiscard]] const std::vector<SubMesh>& GetSubMeshes() const;

		/// Returns imported material metadata in the same order used by SubMesh::MaterialIndex.
		[[nodiscard]] const std::vector<MeshMaterial>& GetMaterials() const;

		/// Returns the submesh at the given index.
		[[nodiscard]] const SubMesh& GetSubMesh(const size_t subMeshIndex) const;

		/// Returns material metadata at the given index.
		[[nodiscard]] const MeshMaterial& GetMaterial(const size_t materialIndex) const;

	private:
		std::vector<SubMesh> _subMeshes;
		std::vector<MeshMaterial> _materials;
		DirectX::BoundingBox _bounds = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
	};
}