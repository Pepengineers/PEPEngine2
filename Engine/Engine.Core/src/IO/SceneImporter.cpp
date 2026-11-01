// SceneImporter.cpp

#include <Engine.Core/IO/SceneImporter.h>
#include "Engine.Core/Logging/Logger.h"
#include <Engine.Core/IO/AssimpImportHelpers.h>

#include <Engine.Core/AssetLocators.h>
#include <Engine.Core/Registries/MeshRegistry.h>

#include <Common/AssetDebugLog.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <stdexcept>

namespace
{
	void LogSceneImporterMessage(const std::wstring& message)
	{
		LOG(message);
	}

	/// Creates a SceneNode from an Assimp node, decomposing its local transform
	/// into separate translation, rotation and scale components.
	/// parentIndex is -1 for root nodes.
	Engine::Core::SceneNode CreateSceneNode(const aiNode& assimpNode, const std::int32_t parentIndex)
	{
		Engine::Core::SceneNode importedNode = {};
		importedNode.Name = assimpNode.mName.C_Str();
		importedNode.ParentIndex = parentIndex;
		importedNode.LocalBindMatrix = Engine::Core::ImportAssimpMatrix(assimpNode.mTransformation);

		aiVector3D scaling;
		aiQuaternion rotation;
		aiVector3D translation;
		assimpNode.mTransformation.Decompose(scaling, rotation, translation);

		importedNode.LocalTranslation = DirectX::SimpleMath::Vector3(translation.x, translation.y, translation.z);
		importedNode.LocalRotation = DirectX::SimpleMath::Quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
		importedNode.LocalScale = DirectX::SimpleMath::Vector3(scaling.x, scaling.y, scaling.z);

		return importedNode;
	}

	/// Recursively imports an Assimp node and all its children into importedNodes.
	/// For each mesh referenced by the node, registers and loads the corresponding
	/// sub-asset into meshRegistry and records the resulting MeshHandle on the node.
	/// Returns the index of the newly created node within importedNodes.
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

			Engine::Core::MeshAssetLocator locator = {};
			locator.SourcePath = sourcePath;
			locator.SubAssetIndex = sceneMeshIndex;

			Engine::Core::MeshHandle meshHandle;
			const Engine::Core::Mesh* loadedMesh = meshRegistry.Load(locator, meshHandle);
			if (loadedMesh == nullptr)
			{
				LogSceneImporterMessage(L"[SceneImporter] Failed to load mesh sub-asset " + std::to_wstring(locator.SubAssetIndex) +
					L" from path: " + sourcePath.generic_wstring() + L"\n");
				continue;
			}

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
	std::unique_ptr<SceneAsset> SceneImporter::ImportSceneAsset(const std::filesystem::path& sourcePath, MeshRegistry& meshRegistry)
	{
		Log::Info("Scene", "Importing scene hierarchy: ", sourcePath);
		if (sourcePath.empty())
		{
			LogSceneImporterMessage(L"[SceneImporter] Failed to import scene: empty source path.\n");
			return nullptr;
		}

		LogSceneImporterMessage(L"[SceneImporter] Importing scene from path: " + sourcePath.generic_wstring() + L"\n");

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const std::uint32_t assimpFlags = BuildDefaultSceneAssimpPostProcessFlags();
		const aiScene* assimpScene = importer.ReadFile(sourcePathUtf8.c_str(), assimpFlags);

		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			Log::Error("Scene", "Assimp scene import failed for ", sourcePath, ": ", importer.GetErrorString());
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

		std::unordered_map<std::string, std::uint32_t> nodeIndices;
		std::unordered_set<std::string> ambiguousNames;
		for (std::uint32_t i = 0; i < importedNodes.size(); ++i)
			if (!nodeIndices.emplace(importedNodes[i].Name, i).second) ambiguousNames.insert(importedNodes[i].Name);
		const auto findNode = [&](const std::string& name)
		{
			const auto it = nodeIndices.find(name);
			if (it == nodeIndices.end() || ambiguousNames.count(name) != 0)
				throw std::runtime_error("Skin/animation node name is missing or ambiguous: " + name);
			return it->second;
		};
		for (const auto& node : importedNodes)
			for (const auto handle : node.Meshes)
				for (const auto& part : meshRegistry.GetMesh(handle)->GetSubMeshes())
					for (const auto& bone : part.Bones) (void)findNode(bone.NodeName);
		std::vector<AnimationClip> clips;
		for (unsigned int i = 0; i < assimpScene->mNumAnimations; ++i)
		{
			const auto& animation = *assimpScene->mAnimations[i];
			if (animation.mNumMeshChannels || animation.mNumMorphMeshChannels)
				throw std::runtime_error("Mesh/morph animation is not skeletal animation");
			const double ticksPerSecond = animation.mTicksPerSecond > 0 ? animation.mTicksPerSecond : 25.0;
			AnimationClip clip;
			clip.Name = animation.mName.C_Str();
			clip.DurationSeconds = animation.mDuration / ticksPerSecond;
			if (!std::isfinite(clip.DurationSeconds) || clip.DurationSeconds < 0 || !std::isfinite(ticksPerSecond))
				throw std::runtime_error("Invalid animation duration");
			std::unordered_set<std::uint32_t> trackedNodes;
			for (unsigned int channelIndex = 0; channelIndex < animation.mNumChannels; ++channelIndex)
			{
				const auto& channel = *animation.mChannels[channelIndex];
				NodeAnimation track;
				track.NodeIndex = findNode(channel.mNodeName.C_Str());
				if (!trackedNodes.insert(track.NodeIndex).second) throw std::runtime_error("Duplicate animation track");
				const auto importVectors = [&](const aiVectorKey* keys, unsigned int count, auto& destination)
				{
					for (unsigned int key = 0; key < count; ++key)
					{
						const auto& v = keys[key].mValue;
						const double time = keys[key].mTime / ticksPerSecond;
						if (!std::isfinite(time) || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
							(!destination.empty() && time <= destination.back().TimeSeconds))
							throw std::runtime_error("Invalid animation vector key");
						destination.push_back({time, {v.x, v.y, v.z}});
					}
				};
				importVectors(channel.mPositionKeys, channel.mNumPositionKeys, track.Translation);
				importVectors(channel.mScalingKeys, channel.mNumScalingKeys, track.Scale);
				for (unsigned int key = 0; key < channel.mNumRotationKeys; ++key)
				{
					const auto& q = channel.mRotationKeys[key].mValue;
					const double time = channel.mRotationKeys[key].mTime / ticksPerSecond;
					DirectX::SimpleMath::Quaternion rotation(q.x, q.y, q.z, q.w);
					if (!std::isfinite(time) || !std::isfinite(rotation.LengthSquared()) ||
						rotation.LengthSquared() < 1e-12f ||
						(!track.Rotation.empty() && time <= track.Rotation.back().TimeSeconds))
						throw std::runtime_error("Invalid animation quaternion key");
					rotation.Normalize();
					track.Rotation.push_back({time, rotation});
				}
				clip.Tracks.push_back(std::move(track));
			}
			clips.push_back(std::move(clip));
		}
		Log::Info("Scene", "Imported ", importedNodes.size(), " scene nodes: ", sourcePath);
		return std::make_unique<SceneAsset>(std::move(importedNodes), std::move(rootNodeIndices), std::move(clips));
	}
}
