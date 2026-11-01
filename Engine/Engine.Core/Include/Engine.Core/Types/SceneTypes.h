// SceneTypes.h

#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <Engine.Core/AssetHandles.h>

#include <directxtk/SimpleMath.h>

namespace Engine::Core
{
	/// A single node in a scene hierarchy tree.
	/// Nodes form a parent-child hierarchy and may reference one or more mesh assets.
	struct SceneNode
	{
		std::string Name;
		DirectX::SimpleMath::Matrix LocalBindMatrix;
		std::int32_t ParentIndex = -1;
		std::vector<std::uint32_t> ChildrenIndices;
		std::vector<MeshHandle> Meshes;
		DirectX::SimpleMath::Vector3 LocalTranslation = {0.0f, 0.0f, 0.0f};
		DirectX::SimpleMath::Quaternion LocalRotation = {0.0f, 0.0f, 0.0f, 1.0f};
		DirectX::SimpleMath::Vector3 LocalScale = {1.0f, 1.0f, 1.0f};

		/// Returns true if this node has a parent node.
		[[nodiscard]] bool HasParent() const;

		/// Returns true if this node has at least one child node.
		[[nodiscard]] bool HasChildren() const;

		/// Returns true if this node has at least one mesh attached.
		[[nodiscard]] bool HasMeshes() const;
	};

	template<typename T> struct AnimationKey
	{
		double TimeSeconds = 0;
		T Value;
	};

	struct NodeAnimation
	{
		std::uint32_t NodeIndex = 0;
		std::vector<AnimationKey<DirectX::SimpleMath::Vector3>> Translation;
		std::vector<AnimationKey<DirectX::SimpleMath::Quaternion>> Rotation;
		std::vector<AnimationKey<DirectX::SimpleMath::Vector3>> Scale;
	};

	struct AnimationClip
	{
		std::string Name;
		double DurationSeconds = 0;
		std::vector<NodeAnimation> Tracks;
	};

	/// CPU-side scene representation.
	class SceneAsset
	{
	public:
		SceneAsset() = default;
		SceneAsset(std::vector<SceneNode> nodes, std::vector<std::uint32_t> rootNodeIndices,
			std::vector<AnimationClip> clips = {});

		/// Returns true if the scene has no nodes.
		[[nodiscard]] bool IsEmpty() const;

		/// Returns the total number of nodes in the scene.
		[[nodiscard]] size_t GetNodeCount() const;

		/// Returns the number of root nodes (nodes with no parent).
		[[nodiscard]] size_t GetRootNodeCount() const;

		/// Returns all nodes as a flat array.
		[[nodiscard]] const std::vector<SceneNode>& GetNodes() const;

		/// Returns the indices of all root nodes.
		[[nodiscard]] const std::vector<std::uint32_t>& GetRootNodeIndices() const;

		/// Returns the node at the given index.
		[[nodiscard]] const SceneNode& GetNode(const size_t nodeIndex) const;

		[[nodiscard]] const std::vector<AnimationClip>& GetClips() const { return _clips; }
		// clipIndex=-1 selects exact bind matrices; localPose replaces the entire local pose.
		[[nodiscard]] std::vector<DirectX::SimpleMath::Matrix> EvaluatePose(
			int clipIndex, double timeSeconds, bool loop,
			const std::vector<DirectX::SimpleMath::Matrix>& localPose = {}) const;

	private:
		std::vector<AnimationClip> _clips;
		/// Flat array of all nodes in the scene.
		/// Parent-child relationships are encoded via index references within the nodes themselves.
		std::vector<SceneNode> _nodes;

		/// Indices of the top-level nodes that have no parent.
		std::vector<std::uint32_t> _rootNodeIndices;
	};
}