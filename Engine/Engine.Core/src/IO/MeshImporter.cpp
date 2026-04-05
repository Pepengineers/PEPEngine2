// MeshImporter.cpp

#include <Engine.Core/IO/MeshImporter.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace SimpleMath = DirectX::SimpleMath;

namespace 
{
	/// Converts engine-side MeshImportOptions into Assimp post-process flags bitmask.
	std::uint32_t BuildAssimpPostProcessFlags(const Engine::Core::MeshImportOptions& options)
	{
		std::uint32_t assimpFlags = 0;

		if (options.bTriangulate)
		{
			assimpFlags |= aiProcess_Triangulate;
		}

		if (options.bFlipUVs)
		{
			assimpFlags |= aiProcess_FlipUVs;
		}

		if (options.bGenerateNormals)
		{
			assimpFlags |= aiProcess_GenNormals;
		}

		if (options.bGenerateTangents)
		{
			assimpFlags |= aiProcess_CalcTangentSpace;
		}

		return assimpFlags;
	}

	/// Constructs a BoundingBox from explicit min/max corner points.
	/// Center and Extents are computed as the midpoint and half-size of the AABB.
	DirectX::BoundingBox CreateBoundsFromMinMax(const SimpleMath::Vector3& minPoint, const SimpleMath::Vector3& maxPoint)
	{
		DirectX::BoundingBox bounds;
		bounds.Center =
		{
			(minPoint.x + maxPoint.x) * 0.5f,
			(minPoint.y + maxPoint.y) * 0.5f,
			(minPoint.z + maxPoint.z) * 0.5f
		};
		bounds.Extents =
		{
			(maxPoint.x - minPoint.x) * 0.5f,
			(maxPoint.y - minPoint.y) * 0.5f,
			(maxPoint.z - minPoint.z) * 0.5f
		};

		return bounds;
	}

	/// Computes a tight axis-aligned bounding box enclosing all given vertices.
	/// Returns a default-constructed BoundingBox if the vertex list is empty.
	DirectX::BoundingBox CalculateVertexBounds(const std::vector<Engine::Core::Vertex>& vertices)
	{
		if (vertices.empty())
		{
			return {};
		}

		SimpleMath::Vector3 minPoint = vertices.front().Position;
		SimpleMath::Vector3 maxPoint = vertices.front().Position;

		for (const Engine::Core::Vertex& vertex : vertices)
		{
			minPoint.x = (std::min)(minPoint.x, vertex.Position.x);
			minPoint.y = (std::min)(minPoint.y, vertex.Position.y);
			minPoint.z = (std::min)(minPoint.z, vertex.Position.z);

			maxPoint.x = (std::max)(maxPoint.x, vertex.Position.x);
			maxPoint.y = (std::max)(maxPoint.y, vertex.Position.y);
			maxPoint.z = (std::max)(maxPoint.z, vertex.Position.z);
		}

		return CreateBoundsFromMinMax(minPoint, maxPoint);
	}

	/// Computes a tight axis-aligned bounding box enclosing all given submeshes.
	/// Each submesh's Bounds is unpacked into min/max corners and merged into a single AABB.
	/// Returns a default-constructed BoundingBox if the submesh list is empty.
	DirectX::BoundingBox CalculateMeshBounds(const std::vector<Engine::Core::SubMesh>& subMeshes)
	{
		if (subMeshes.empty())
		{
			return {};
		}

		const DirectX::BoundingBox& firstBounds = subMeshes.front().Bounds;

		SimpleMath::Vector3 minPoint =
		{
			firstBounds.Center.x - firstBounds.Extents.x,
			firstBounds.Center.y - firstBounds.Extents.y,
			firstBounds.Center.z - firstBounds.Extents.z
		};

		SimpleMath::Vector3 maxPoint =
		{
			firstBounds.Center.x + firstBounds.Extents.x,
			firstBounds.Center.y + firstBounds.Extents.y,
			firstBounds.Center.z + firstBounds.Extents.z
		};

		for (const Engine::Core::SubMesh& subMesh : subMeshes)
		{
			const DirectX::BoundingBox& subMeshBounds = subMesh.Bounds;

			const SimpleMath::Vector3 subMeshMinPoint =
			{
				subMeshBounds.Center.x - subMeshBounds.Extents.x,
				subMeshBounds.Center.y - subMeshBounds.Extents.y,
				subMeshBounds.Center.z - subMeshBounds.Extents.z
			};

			const SimpleMath::Vector3 subMeshMaxPoint =
			{
				subMeshBounds.Center.x + subMeshBounds.Extents.x,
				subMeshBounds.Center.y + subMeshBounds.Extents.y,
				subMeshBounds.Center.z + subMeshBounds.Extents.z
			};

			minPoint.x = (std::min)(minPoint.x, subMeshMinPoint.x);
			minPoint.y = (std::min)(minPoint.y, subMeshMinPoint.y);
			minPoint.z = (std::min)(minPoint.z, subMeshMinPoint.z);

			maxPoint.x = (std::max)(maxPoint.x, subMeshMaxPoint.x);
			maxPoint.y = (std::max)(maxPoint.y, subMeshMaxPoint.y);
			maxPoint.z = (std::max)(maxPoint.z, subMeshMaxPoint.z);
		}

		return CreateBoundsFromMinMax(minPoint, maxPoint);
	}

	/// Transforms a position vector by a 4x4 matrix, including the translation component (w=1).
	aiVector3D TransformPosition(const aiMatrix4x4& transform, const aiVector3D& position)
	{
		return
		{
			transform.a1 * position.x + transform.a2 * position.y + transform.a3 * position.z + transform.a4,
			transform.b1 * position.x + transform.b2 * position.y + transform.b3 * position.z + transform.b4,
			transform.c1 * position.x + transform.c2 * position.y + transform.c3 * position.z + transform.c4
		};
	}

	/// Transforms a direction vector (normal, tangent) by the inverse-transpose of the given matrix.
	/// The result is re-normalized to unit length.
	aiVector3D TransformDirection(const aiMatrix4x4& transform, const aiVector3D& direction)
	{
		aiMatrix4x4 normalTransform = transform;
		normalTransform.Inverse().Transpose();

		aiVector3D transformedDirection =
		{
			normalTransform.a1 * direction.x + normalTransform.a2 * direction.y + normalTransform.a3 * direction.z,
			normalTransform.b1 * direction.x + normalTransform.b2 * direction.y + normalTransform.b3 * direction.z,
			normalTransform.c1 * direction.x + normalTransform.c2 * direction.y + normalTransform.c3 * direction.z
		};

		if (transformedDirection.SquareLength() > 0.0f)
		{
			transformedDirection.Normalize();
		}

		return transformedDirection;
	}
	
	/// Converts a single Assimp mesh into an engine SubMesh.
	/// Vertex positions, normals, UVs and tangents are read from the Assimp mesh and
	/// baked into world space using nodeTransform.
	/// startVertexLocation and startIndexLocation are recorded for use in merged GPU buffers.
	Engine::Core::SubMesh ImportSubMesh(const aiMesh& assimpMesh, const aiMatrix4x4& nodeTransform, const std::uint32_t startVertexLocation, const std::uint32_t startIndexLocation)
	{
		Engine::Core::SubMesh importedSubMesh;
		importedSubMesh.Vertices.reserve(assimpMesh.mNumVertices);
		importedSubMesh.Indices.reserve(assimpMesh.mNumFaces * 3u);
		importedSubMesh.MaterialIndex = assimpMesh.mMaterialIndex;
		importedSubMesh.StartVertexLocation = startVertexLocation;
		importedSubMesh.StartIndexLocation = startIndexLocation;

		for (unsigned int vertexIndex = 0; vertexIndex < assimpMesh.mNumVertices; ++vertexIndex)
		{
			const aiVector3D transformedPosition = TransformPosition(nodeTransform, assimpMesh.mVertices[vertexIndex]);

			Engine::Core::Vertex importedVertex;
			importedVertex.Position = SimpleMath::Vector3(transformedPosition.x, transformedPosition.y, transformedPosition.z);

			if (assimpMesh.HasNormals())
			{
				const aiVector3D transformedNormal = TransformDirection(nodeTransform, assimpMesh.mNormals[vertexIndex]);
				importedVertex.Normal = SimpleMath::Vector3(transformedNormal.x, transformedNormal.y, transformedNormal.z);
			}

			if (assimpMesh.HasTextureCoords(0))
			{
				const aiVector3D& assimpTexCoord = assimpMesh.mTextureCoords[0][vertexIndex];
				importedVertex.TexCoord = SimpleMath::Vector2(assimpTexCoord.x, assimpTexCoord.y);
			}

			if (assimpMesh.HasTangentsAndBitangents())
			{
				const aiVector3D transformedTangent = TransformDirection(nodeTransform, assimpMesh.mTangents[vertexIndex]);
				importedVertex.Tangent = SimpleMath::Vector3(transformedTangent.x, transformedTangent.y, transformedTangent.z);
			}

			importedSubMesh.Vertices.push_back(importedVertex);
		}

		for (unsigned int faceIndex = 0; faceIndex < assimpMesh.mNumFaces; ++faceIndex)
		{
			const aiFace& assimpFace = assimpMesh.mFaces[faceIndex];

			assert(assimpFace.mNumIndices == 3);
			if (assimpFace.mNumIndices != 3)
			{
				continue;
			}

			for (unsigned int indexIndex = 0; indexIndex < assimpFace.mNumIndices; ++indexIndex)
			{
				importedSubMesh.Indices.push_back(assimpFace.mIndices[indexIndex]);
			}
		}

		importedSubMesh.Bounds = CalculateVertexBounds(importedSubMesh.Vertices);
		return importedSubMesh;
	}

	/// Recursively walks the Assimp node tree and imports all meshes into importedSubMeshes.
	/// nodeTransform accumulates the full chain of transforms from the root to the current node,
	/// so that each mesh is baked into a common world space.
	/// startVertexLocation and startIndexLocation are updated after each imported submesh
	/// to maintain correct offsets for a merged GPU vertex/index buffer.
	void ImportNodeMeshes(
		const aiScene& assimpScene,
		const aiNode& assimpNode,
		const aiMatrix4x4& parentTransform,
		std::vector<Engine::Core::SubMesh>& importedSubMeshes,
		std::uint32_t& startVertexLocation,
		std::uint32_t& startIndexLocation)
	{
		const aiMatrix4x4 nodeTransform = parentTransform * assimpNode.mTransformation;

		for (unsigned int nodeMeshIndex = 0; nodeMeshIndex < assimpNode.mNumMeshes; ++nodeMeshIndex)
		{
			const unsigned int sceneMeshIndex = assimpNode.mMeshes[nodeMeshIndex];
			if (sceneMeshIndex >= assimpScene.mNumMeshes)
			{
				continue;
			}

			const aiMesh* assimpMesh = assimpScene.mMeshes[sceneMeshIndex];
			if (assimpMesh == nullptr)
			{
				continue;
			}

			Engine::Core::SubMesh importedSubMesh = ImportSubMesh(*assimpMesh, nodeTransform, startVertexLocation, startIndexLocation);
			if (importedSubMesh.GetVertexCount() == 0 || importedSubMesh.GetIndexCount() == 0)
			{
				continue;
			}

			startVertexLocation += static_cast<std::uint32_t>(importedSubMesh.GetVertexCount());
			startIndexLocation += static_cast<std::uint32_t>(importedSubMesh.GetIndexCount());

			importedSubMeshes.push_back(std::move(importedSubMesh));
		}

		for (unsigned int childIndex = 0; childIndex < assimpNode.mNumChildren; ++childIndex)
		{
			const aiNode* childNode = assimpNode.mChildren[childIndex];
			if (childNode == nullptr)
			{
				continue;
			}

			ImportNodeMeshes(assimpScene, *childNode, nodeTransform, importedSubMeshes, startVertexLocation, startIndexLocation);
		}
	}
}

namespace Engine::Core
{
	std::shared_ptr<Mesh> MeshImporter::ImportSingleMeshAsset(const std::filesystem::path& sourcePath, const MeshImportOptions& options)
	{
		if (sourcePath.empty())
		{
			return nullptr;
		}

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const std::uint32_t assimpFlags = BuildAssimpPostProcessFlags(options);
		const aiScene* assimpScene = importer.ReadFile(sourcePathUtf8.c_str(), assimpFlags);
		
		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			return nullptr;
		}

		std::vector<SubMesh> importedSubMeshes;
		std::uint32_t startVertexLocation = 0;
		std::uint32_t startIndexLocation = 0;

		const aiMatrix4x4 identityTransform;
		ImportNodeMeshes(*assimpScene, *assimpScene->mRootNode, identityTransform, importedSubMeshes, startVertexLocation, startIndexLocation);

		if (importedSubMeshes.empty())
		{
			return nullptr;
		}

		const DirectX::BoundingBox meshBounds = CalculateMeshBounds(importedSubMeshes);
		return std::make_shared<Mesh>(std::move(importedSubMeshes), meshBounds);
	}

	std::shared_ptr<Mesh> MeshImporter::ImportMeshSubAsset(const std::filesystem::path& sourcePath, const std::uint32_t subAssetIndex, const MeshImportOptions& options)
	{
		if (sourcePath.empty())
		{
			return nullptr;
		}

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const std::uint32_t assimpFlags = BuildAssimpPostProcessFlags(options);
		const aiScene* assimpScene = importer.ReadFile(sourcePathUtf8.c_str(), assimpFlags);

		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			return nullptr;
		}

		if (subAssetIndex >= assimpScene->mNumMeshes)
		{
			return nullptr;
		}

		const aiMesh* assimpMesh = assimpScene->mMeshes[subAssetIndex];
		if (assimpMesh == nullptr)
		{
			return nullptr;
		}

		const aiMatrix4x4 identityTransform;
		SubMesh importedSubMesh = ImportSubMesh(*assimpMesh, identityTransform, 0u, 0u);
		if (importedSubMesh.GetVertexCount() == 0 || importedSubMesh.GetIndexCount() == 0)
		{
			return nullptr;
		}

		std::vector<SubMesh> importedSubMeshes;
		importedSubMeshes.push_back(std::move(importedSubMesh));

		const DirectX::BoundingBox meshBounds = CalculateMeshBounds(importedSubMeshes);
		return std::make_shared<Mesh>(std::move(importedSubMeshes), meshBounds);
	}
}