// AssetManager.cpp

#include <Engine.Core/AssetManager.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cassert>
#include <cwctype>

namespace SimpleMath = DirectX::SimpleMath;

namespace
{
#pragma region Bounds Helpers
	/// Creates a BoundingBox from explicit min and max corner points.
	DirectX::BoundingBox CreateBoundsFromMinMax(const SimpleMath::Vector3& minPoint, const SimpleMath::Vector3& maxPoint)
	{
		DirectX::BoundingBox bounds;
		bounds.Center =
		{
			(minPoint.x + maxPoint.x) * 0.5f,
			(minPoint.y + maxPoint.y) * 0.5f,
			(minPoint.z + maxPoint.z) * 0.5f,
		};
		bounds.Extents =
		{
			(maxPoint.x - minPoint.x) * 0.5f,
			(maxPoint.y - minPoint.y) * 0.5f,
			(maxPoint.z - minPoint.z) * 0.5f,
		};

		return bounds;
	}

	/// Computes a tight AABB enclosing all vertex positions in the given list.
	/// Returns a default-constructed BoundingBox if the list is empty.
	DirectX::BoundingBox CalculateVertexBounds(const std::vector<Engine::Core::Vertex>& vertices)
	{
		if (vertices.empty())
		{
			return {};
		}

		// front() - returns the first element of the vector
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

	/// Computes a tight AABB enclosing all submesh bounds in the given list.
	/// Returns a default-constructed BoundingBox if the list is empty.
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
			firstBounds.Center.z - firstBounds.Extents.z,
		};
		SimpleMath::Vector3 maxPoint =
		{
			firstBounds.Center.x + firstBounds.Extents.x,
			firstBounds.Center.y + firstBounds.Extents.y,
			firstBounds.Center.z + firstBounds.Extents.z,
		};
		
		for (const Engine::Core::SubMesh& subMesh : subMeshes)
		{
			const DirectX::BoundingBox& subMeshBounds = subMesh.Bounds;

			const SimpleMath::Vector3 subMeshMinPoint =
			{
				subMeshBounds.Center.x - subMeshBounds.Extents.x,
				subMeshBounds.Center.y - subMeshBounds.Extents.y,
				subMeshBounds.Center.z - subMeshBounds.Extents.z,
			};
			const SimpleMath::Vector3 subMeshMaxPoint =
			{
				subMeshBounds.Center.x + subMeshBounds.Extents.x,
				subMeshBounds.Center.y + subMeshBounds.Extents.y,
				subMeshBounds.Center.z + subMeshBounds.Extents.z,
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
#pragma endregion Bounds Helpers

#pragma region Transform Helpers
	/// Applies a 4x4 affine transform to a position (point).
	/// Translation is included (homogeneous w=1).
	aiVector3D TransformPosition(const aiMatrix4x4& transform, const aiVector3D& position)
	{
		return
	    {
			transform.a1 * position.x + transform.a2 * position.y + transform.a3 * position.z + transform.a4,
	    	transform.b1 * position.x + transform.b2 * position.y + transform.b3 * position.z + transform.b4,
	    	transform.c1 * position.x + transform.c2 * position.y + transform.c3 * position.z + transform.c4,
		};
	}

	/// Applies a 4x4 transform to a direction vector (normal or tangent).
	/// Uses the inverse-transpose to preserve perpendicularity under non-uniform scale.
	/// The result is normalized before returning.
	aiVector3D TransformDirection(const aiMatrix4x4& transform, const aiVector3D& direction)
	{
		// Normals and tangents must use inverse-transpose of the linear transform.
		// Copy the transform to avoid modifying the original matrix.
		aiMatrix4x4 normalTransform = transform;
		normalTransform.Inverse().Transpose();

		aiVector3D transformedDirection =
		{
			normalTransform.a1 * direction.x + normalTransform.a2 * direction.y + normalTransform.a3 * direction.z,
			normalTransform.b1 * direction.x + normalTransform.b2 * direction.y + normalTransform.b3 * direction.z,
			normalTransform.c1 * direction.x + normalTransform.c2 * direction.y + normalTransform.c3 * direction.z,
		};

		if (transformedDirection.SquareLength() > 0.0f)
		{
			transformedDirection.Normalize();
		}

		return transformedDirection;
	}
#pragma endregion Transform Helpers

#pragma region Mesh Import
	/// Imports a single submesh from an Assimp mesh, applying the given node transform.
	/// Vertex positions are transformed as points; normals and tangents use inverse-transpose.
	/// startVertexLocation and startIndexLocation define the offset of this submesh within the shared vertex and index buffers.
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
			const aiVector3D& assimpPosition = assimpMesh.mVertices[vertexIndex];
			const aiVector3D transformedPosition = TransformPosition(nodeTransform, assimpPosition);
			
			Engine::Core::Vertex importedVertex;
			importedVertex.Position = SimpleMath::Vector3(transformedPosition.x, transformedPosition.y, transformedPosition.z);

			if (assimpMesh.HasNormals())
			{
				const aiVector3D& assimpNormal = assimpMesh.mNormals[vertexIndex];
				const aiVector3D transformedNormal = TransformDirection(nodeTransform, assimpNormal);
				importedVertex.Normal = SimpleMath::Vector3(transformedNormal.x, transformedNormal.y, transformedNormal.z);
			}

			if (assimpMesh.HasTextureCoords(0))
			{
				const aiVector3D& assimpTexCoord = assimpMesh.mTextureCoords[0][vertexIndex];
				importedVertex.TexCoord = SimpleMath::Vector2(assimpTexCoord.x, assimpTexCoord.y);
			}

			if (assimpMesh.HasTangentsAndBitangents())
			{
				const aiVector3D& assimpTangent = assimpMesh.mTangents[vertexIndex];
				const aiVector3D transformedTangent = TransformDirection(nodeTransform, assimpTangent);
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

	/// Recursively imports all meshes from a node and its children into importedSubMeshes.
	/// Accumulates the node's local transform with parentTransform before processing.
	/// startVertexLocation and startIndexLocation are updated in-place as submeshes are added.
	void ImportNodeMeshes(
		const aiScene& assimpScene,
		const aiNode& assimpNode,
		const aiMatrix4x4& parentTransform,
		std::vector<Engine::Core::SubMesh>& importedSubMeshes,
		std::uint32_t& startVertexLocation,
		std::uint32_t& startIndexLocation)
	{
		// Concatenate the parent transform with the node's local transform to get the final world-space transform for this node's meshes.
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

			startVertexLocation += static_cast<uint32_t>(importedSubMesh.GetVertexCount());
			startIndexLocation += static_cast<uint32_t>(importedSubMesh.GetIndexCount());

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
#pragma endregion Mesh Import
}

namespace Engine::Core
{
#pragma region Internal Helpers
	std::filesystem::path AssetManager::ResolveMeshSourcePath(const std::filesystem::path& meshPath)
	{
		if (meshPath.is_absolute())
		{
			// lexically_normal - normalizes the path without accessing the file system (removes '.', '..' and extra slashes)
			// (e.g. "models/../models/./cube.fbx" -> "models/cube.fbx")
			return meshPath.lexically_normal();
		}

		std::filesystem::path resolvedMeshPath = MODELS_FOLDER;
		// resolvedMeshPath = "C:/Assets/Models", meshPath = "cube.fbx" -> resolvedMeshPath = "C:/Assets/Models/cube.fbx"
		resolvedMeshPath /= meshPath;
		return resolvedMeshPath.lexically_normal();
	}
	
	std::wstring AssetManager::BuildMeshCacheKey(const std::filesystem::path& meshPath)
	{
		const std::filesystem::path resolvedMeshPath = ResolveMeshSourcePath(meshPath);
		// generic_wstring - converts the path to a string with a unified slash format (with '/' instead of '\')
		// (e.g. "models\\cube.fbx" -> "models/cube.fbx")
		std::wstring cacheKey = resolvedMeshPath.generic_wstring();

		std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
		return cacheKey;
	}

	std::shared_ptr<Mesh> AssetManager::ImportMeshFromFile(const std::filesystem::path& meshPath)
	{
		if (meshPath.empty())
		{
			return nullptr;
		}

		Assimp::Importer importer;
		const aiScene* assimpScene = importer.ReadFile(meshPath.string(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			return nullptr;
		}

		std::vector<SubMesh> importedSubMeshes;
		std::uint32_t startVertexLocation = 0;
		std::uint32_t startIndexLocation = 0;

		// Start traversal with an identity matrix; each node accumulates its own transform on top.
		const aiMatrix4x4 identityTransform;
		ImportNodeMeshes(*assimpScene, *assimpScene->mRootNode, identityTransform, importedSubMeshes, startVertexLocation, startIndexLocation);

		if (importedSubMeshes.empty())
		{
			return nullptr;
		}

		const DirectX::BoundingBox meshBounds = CalculateMeshBounds(importedSubMeshes);
		return std::make_shared<Mesh>(std::move(importedSubMeshes), meshBounds);
	}
#pragma endregion Internal Helpers

#pragma region Singleton
	// Meyers Singleton
	AssetManager& AssetManager::GetInstance()
	{
		static AssetManager instance;
		return instance;
	}
#pragma endregion Singleton

#pragma region Mesh Cache
	std::shared_ptr<const Mesh> AssetManager::LoadMesh(const std::filesystem::path& meshPath)
	{
		if (meshPath.empty())
		{
			return nullptr;
		}

		const std::shared_ptr<const Mesh> cachedMesh = FindMesh(meshPath);
		if (cachedMesh != nullptr)
		{
			return cachedMesh;
		}

		const std::filesystem::path resolvedMeshPath = ResolveMeshSourcePath(meshPath);
		std::shared_ptr<Mesh> importedMesh = ImportMeshFromFile(resolvedMeshPath);
		if (importedMesh == nullptr)
		{
			return nullptr;
		}

		return CacheMesh(resolvedMeshPath, std::move(importedMesh));
	}
	
	bool AssetManager::IsMeshLoaded(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			return false;
		}

		return _meshAssets.find(BuildMeshCacheKey(meshPath)) != _meshAssets.end();
	}

	std::shared_ptr<const Mesh> AssetManager::FindMesh(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			return nullptr;
		}

		const auto meshAssetIterator = _meshAssets.find(BuildMeshCacheKey(meshPath));
		if (meshAssetIterator == _meshAssets.end())
		{
			return nullptr;
		}

		return meshAssetIterator->second.MeshData;
	}

	std::shared_ptr<const Mesh> AssetManager::CacheMesh(const std::filesystem::path& meshPath, std::shared_ptr<Mesh> meshData)
	{
		assert(!meshPath.empty());
		assert(meshData != nullptr);

		if (meshPath.empty() || meshData == nullptr)
		{
			return nullptr;
		}

		const std::wstring cacheKey = BuildMeshCacheKey(meshPath);
		const auto meshAssetIterator = _meshAssets.find(cacheKey);
		if (meshAssetIterator != _meshAssets.end())
		{
			return meshAssetIterator->second.MeshData;
		}

		MeshAssetRecord meshAssetRecord;
		meshAssetRecord.SourcePath = ResolveMeshSourcePath(meshPath);
		meshAssetRecord.MeshData = std::move(meshData);

		const auto insertedIterator = _meshAssets.emplace(std::move(cacheKey), std::move(meshAssetRecord)).first;
		return insertedIterator->second.MeshData;
	}

	size_t AssetManager::GetLoadedMeshCount() const
	{
		return _meshAssets.size();
	}
#pragma endregion Mesh Cache
}