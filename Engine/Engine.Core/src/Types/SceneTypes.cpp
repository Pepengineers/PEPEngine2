// SceneTypes.cpp

#include <Engine.Core/Types/SceneTypes.h>

namespace Engine::Core
{
#pragma region SceneNode
	bool SceneNode::HasParent() const
	{
		return ParentIndex >= 0;
	}

	bool SceneNode::HasChildren() const
	{
		return !ChildrenIndices.empty();
	}

	bool SceneNode::HasMeshes() const
	{
		return !Meshes.empty();
	}
#pragma endregion SceneNode

#pragma region SceneAsset
	SceneAsset::SceneAsset(std::vector<SceneNode> nodes, std::vector<std::uint32_t> rootNodeIndices) : _nodes(std::move(nodes)), _rootNodeIndices(std::move(rootNodeIndices)) {}

	bool SceneAsset::IsEmpty() const
	{
		return _nodes.empty();
	}

	size_t SceneAsset::GetNodeCount() const
	{
		return _nodes.size();
	}

	size_t SceneAsset::GetRootNodeCount() const
	{
		return _rootNodeIndices.size();
	}

	const std::vector<SceneNode>& SceneAsset::GetNodes() const
	{
		return _nodes;
	}

	const std::vector<std::uint32_t>& SceneAsset::GetRootNodeIndices() const
	{
		return _rootNodeIndices;
	}

	const SceneNode& SceneAsset::GetNode(const size_t nodeIndex) const
	{
		assert(nodeIndex < _nodes.size());
		return _nodes.at(nodeIndex);
	}
#pragma endregion SceneAsset
}