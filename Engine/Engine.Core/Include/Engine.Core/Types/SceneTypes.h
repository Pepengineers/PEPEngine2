// SceneTypes.h

#pragma once

#include <string>

#include <Engine.Core/AssetHandles.h>

#include <directxtk/SimpleMath.h>

namespace SimpleMath = DirectX::SimpleMath;

namespace Engine::Core
{
	/// A single node in a scene hierarchy tree.
	/// Nodes form a parent-child hierarchy and may reference one or more mesh assets.
	struct SceneNode
	{
		std::string Name;
		std::int32_t ParentIndex = -1;
		std::vector<std::uint32_t> ChildrenIndices;
		std::vector<MeshHandle> Meshes;
		SimpleMath::Vector3 LocalTranslation = {0.0f, 0.0f, 0.0f};
		SimpleMath::Quaternion LocalRotation = {0.0f, 0.0f, 0.0f, 1.0f};
		SimpleMath::Vector3 LocalScale = {1.0f, 1.0f, 1.0f};

		/// Returns true if this node has a parent node.
		[[nodiscard]] bool HasParent() const;

		/// Returns true if this node has at least one child node.
		[[nodiscard]] bool HasChildren() const;

		/// Returns true if this node has at least one mesh attached.
		[[nodiscard]] bool HasMeshes() const;
	};

	/// CPU-side scene representation.
	class SceneAsset
	{
	private:
		/// Flat array of all nodes in the scene.
		/// Parent-child relationships are encoded via index references within the nodes themselves.
		std::vector<SceneNode> _nodes;

		/// Indices of the top-level nodes that have no parent.
		std::vector<std::uint32_t> _rootNodeIndices;

	public:
		SceneAsset() = default;
		SceneAsset(std::vector<SceneNode> nodes, std::vector<std::uint32_t> rootNodeIndices);

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
		/// Returns a default-constructed node if the index is out of range.
		[[nodiscard]] const SceneNode& GetNode(const size_t nodeIndex) const;
	};
}