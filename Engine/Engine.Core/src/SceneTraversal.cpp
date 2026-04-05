// SceneTraversal.cpp

#include <Engine.Core/SceneTraversal.h>

namespace
{
	void TraverseSceneNode(
		const Engine::Core::SceneAsset& scene,
		const std::uint32_t nodeIndex,
		const SimpleMath::Matrix& parentWorldTransform,
		std::vector<Engine::Core::TraversedSceneNode>& traversedNodes,
		std::vector<Engine::Core::SceneRenderable>* renderables)
	{
		const Engine::Core::SceneNode& node = scene.GetNode(nodeIndex);

		const SimpleMath::Matrix localTransform = Engine::Core::SceneTraversal::BuildLocalTransform(node);
		const SimpleMath::Matrix worldTransform = localTransform * parentWorldTransform;

		Engine::Core::TraversedSceneNode traversedNode;
		traversedNode.NodeIndex = nodeIndex;
		traversedNode.ParentIndex = node.ParentIndex;
		traversedNode.LocalTransform = localTransform;
		traversedNode.WorldTransform = worldTransform;

		traversedNodes[nodeIndex] = traversedNode;

		if (renderables != nullptr)
		{
			for (Engine::Core::MeshHandle meshHandle : node.Meshes)
			{
				if (!meshHandle.IsValid())
				{
					continue;
				}

				Engine::Core::SceneRenderable renderable;
				renderable.NodeIndex = nodeIndex;
				renderable.Mesh = meshHandle;
				renderable.WorldTransform = worldTransform;

				renderables->push_back(renderable);
			}
		}

		for (std::uint32_t childIndex : node.ChildrenIndices)
		{
			TraverseSceneNode(scene, childIndex, worldTransform, traversedNodes, renderables);
		}
	}
}

namespace Engine::Core
{
	SimpleMath::Matrix SceneTraversal::BuildLocalTransform(const SceneNode& node)
	{
		return
			SimpleMath::Matrix::CreateScale(node.LocalScale) *
			SimpleMath::Matrix::CreateFromQuaternion(node.LocalRotation) *
			SimpleMath::Matrix::CreateTranslation(node.LocalTranslation);
	}

	std::vector<TraversedSceneNode> SceneTraversal::BuildNodeTransforms(const SceneAsset& scene)
	{
		std::vector<TraversedSceneNode> traversedNodes(scene.GetNodeCount());

		for (std::uint32_t rootNodeIndex : scene.GetRootNodeIndices())
		{
			TraverseSceneNode(
				scene,
				rootNodeIndex,
				SimpleMath::Matrix::Identity,
				traversedNodes,
				nullptr);
		}

		return traversedNodes;
	}

	std::vector<SceneRenderable> SceneTraversal::BuildRenderables(const SceneAsset& scene)
	{
		std::vector<TraversedSceneNode> traversedNodes(scene.GetNodeCount());
		std::vector<SceneRenderable> renderables;

		for (std::uint32_t rootNodeIndex : scene.GetRootNodeIndices())
		{
			TraverseSceneNode(
				scene,
				rootNodeIndex,
				SimpleMath::Matrix::Identity,
				traversedNodes,
				&renderables);
		}

		return renderables;
	}
}