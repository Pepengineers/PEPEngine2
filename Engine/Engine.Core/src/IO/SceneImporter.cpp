// SceneImporter.cpp

#include <Engine.Core/IO/SceneImporter.h>

#include <Engine.Core/AssetLocators.h>
#include <Engine.Core/Registries/MeshRegistry.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace SimpleMath = DirectX::SimpleMath;

namespace
{
	void LogSceneImporterMessage(const std::wstring& message)
	{
		OutputDebugStringW(message.c_str());
	}

	Engine::Core::SceneNode CreateSceneNode(const aiNode& assimpNode, const std::int32_t parentIndex)
	{
		Engine::Core::SceneNode importedNode;
		importedNode.Name = assimpNode.mName.C_Str();
		importedNode.ParentIndex = parentIndex;

		aiVector3D scaling;
		aiQuaternion rotation;
		aiVector3D translation;
		assimpNode.mTransformation.Decompose(scaling, rotation, translation);

		importedNode.LocalTranslation = SimpleMath::Vector3(translation.x, translation.y, translation.z);
		importedNode.LocalRotation = SimpleMath::Quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
		importedNode.LocalScale = SimpleMath::Vector3(scaling.x, scaling.y, scaling.z);

		return importedNode;
	}

	std::uint32_t ImportSceneNode(
		const aiScene& assimpScene,
		const aiNode& assimpNode,
		const std::filesystem::path& sourcePath,
		Engine::Core::MeshRegistry& meshRegistry,
		const std::int32_t parentIndex,
		std::vector<Engine::Core::SceneNode>& importedNodes)
	{
		const std::uint32_t nodeIndex = static_cast<std::uint32_t>(importedNodes.size());
		importedNodes.push_back(CreateSceneNode(assimpNode, parentIndex));

		for (unsigned int nodeMeshIndex = 0; nodeMeshIndex < assimpNode.mNumMeshes; ++nodeMeshIndex)
		{
			const unsigned int sceneMeshIndex = assimpNode.mMeshes[nodeMeshIndex];
			if (sceneMeshIndex >= assimpScene.mNumMeshes)
			{
				continue;
			}

			Engine::Core::MeshAssetLocator locator;
			locator.SourcePath = sourcePath;
			locator.SubAssetIndex = sceneMeshIndex;

			const std::shared_ptr<const Engine::Core::Mesh> loadedMesh = meshRegistry.Load(locator);
			if (loadedMesh == nullptr)
			{
				LogSceneImporterMessage(L"[SceneImporter] Failed to load mesh sub-asset " + std::to_wstring(locator.SubAssetIndex) +
					L" from path: " + sourcePath.generic_wstring() + L"\n");
				continue;
			}

			const Engine::Core::MeshHandle meshHandle = meshRegistry.FindHandle(locator);
			if (!meshHandle.IsValid())
			{
				LogSceneImporterMessage(L"[SceneImporter] Failed to resolve mesh handle for sub-asset " +
					std::to_wstring(locator.SubAssetIndex) + L" from path: " + sourcePath.generic_wstring() + L"\n");
				continue;
			}

			importedNodes[nodeIndex].Meshes.push_back(meshHandle);
		}

		for (unsigned int childIndex = 0; childIndex < assimpNode.mNumChildren; ++childIndex)
		{
			const aiNode* childNode = assimpNode.mChildren[childIndex];
			if (childNode == nullptr)
			{
				continue;
			}

			const std::uint32_t importedChildIndex = ImportSceneNode(
				assimpScene,
				*childNode,
				sourcePath,
				meshRegistry,
				static_cast<std::int32_t>(nodeIndex),
				importedNodes);

			importedNodes[nodeIndex].ChildrenIndices.push_back(importedChildIndex);
		}

		return nodeIndex;
	}
}

namespace Engine::Core
{
	std::shared_ptr<SceneAsset> SceneImporter::ImportSceneAsset(const std::filesystem::path& sourcePath, MeshRegistry& meshRegistry)
	{
		if (sourcePath.empty())
		{
			LogSceneImporterMessage(L"[SceneImporter] Failed to import scene: empty source path.\n");
			return nullptr;
		}

		LogSceneImporterMessage(L"[SceneImporter] Importing scene from path: " + sourcePath.generic_wstring() + L"\n");

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const aiScene* assimpScene = importer.ReadFile(
			sourcePathUtf8.c_str(),
			aiProcess_Triangulate |
			aiProcess_FlipUVs |
			aiProcess_GenNormals |
			aiProcess_CalcTangentSpace);

		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			LogSceneImporterMessage(L"[SceneImporter] Failed to parse scene from path: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		std::vector<SceneNode> importedNodes;
		std::vector<std::uint32_t> rootNodeIndices;

		const std::uint32_t rootNodeIndex = ImportSceneNode(
			*assimpScene,
			*assimpScene->mRootNode,
			sourcePath,
			meshRegistry,
			-1,
			importedNodes);

		rootNodeIndices.push_back(rootNodeIndex);

		if (importedNodes.empty())
		{
			LogSceneImporterMessage(L"[SceneImporter] Scene import produced no nodes for path: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		LogSceneImporterMessage(L"[SceneImporter] Imported scene with " + std::to_wstring(importedNodes.size()) +
			L" nodes from path: " + sourcePath.generic_wstring() + L"\n");

		return std::make_shared<SceneAsset>(std::move(importedNodes), std::move(rootNodeIndices));
	}
}