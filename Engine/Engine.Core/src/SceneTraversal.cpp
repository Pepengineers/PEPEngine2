// SceneTraversal.cpp

#include <Engine.Core/SceneTraversal.h>

namespace
{
	/// Recursively visits nodeIndex and all its descendants.
	/// Writes per-node local/world transforms into traversedNodes if it is not nullptr
	/// and appends one SceneRenderable per valid mesh handle into renderables if it is not nullptr.
	void TraverseSceneNode(
		const Engine::Core::SceneAsset& scene,
		const std::uint32_t nodeIndex,
		const DirectX::SimpleMath::Matrix& parentWorldTransform,
		std::vector<Engine::Core::TraversedSceneNode>* traversedNodes,
		std::vector<Engine::Core::SceneRenderable>* renderables)
	{
		const Engine::Core::SceneNode& node = scene.GetNode(nodeIndex);

		const DirectX::SimpleMath::Matrix localTransform = Engine::Core::SceneTraversal::BuildLocalTransform(node);
		const DirectX::SimpleMath::Matrix worldTransform = localTransform * parentWorldTransform;

		if (traversedNodes != nullptr)
		{
			Engine::Core::TraversedSceneNode traversedNode = {};
			traversedNode.NodeIndex = nodeIndex;
			traversedNode.ParentIndex = node.ParentIndex;
			traversedNode.LocalTransform = localTransform;
			traversedNode.WorldTransform = worldTransform;

			(*traversedNodes)[nodeIndex] = traversedNode;
		}

		if (renderables != nullptr)
		{
			for (Engine::Core::MeshHandle meshHandle : node.Meshes)
			{
				if (!meshHandle.IsValid())
				{
					continue;
				}

				Engine::Core::SceneRenderable renderable = {};
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

	/// Traverses the full scene hierarchy starting from all root nodes.
	/// Depending on which output pointers are provided, collects node transforms,
	/// renderables, or both in a single shared traversal path.
	void TraverseScene(
		const Engine::Core::SceneAsset& scene,
		std::vector<Engine::Core::TraversedSceneNode>* traversedNodes,
		std::vector<Engine::Core::SceneRenderable>* renderables)
	{
		for (std::uint32_t rootNodeIndex : scene.GetRootNodeIndices())
		{
			TraverseSceneNode(
				scene,
				rootNodeIndex,
				DirectX::SimpleMath::Matrix::Identity,
				traversedNodes,
				renderables);
		}
	}
}

namespace Engine::Core
{
	DirectX::SimpleMath::Matrix SceneTraversal::BuildLocalTransform(const SceneNode& node)
	{
		return
			DirectX::SimpleMath::Matrix::CreateScale(node.LocalScale) *
			DirectX::SimpleMath::Matrix::CreateFromQuaternion(node.LocalRotation) *
			DirectX::SimpleMath::Matrix::CreateTranslation(node.LocalTranslation);
	}

	std::vector<TraversedSceneNode> SceneTraversal::BuildNodeTransforms(const SceneAsset& scene)
	{
		std::vector<TraversedSceneNode> traversedNodes(scene.GetNodeCount());
		TraverseScene(scene, &traversedNodes, nullptr);
		return traversedNodes;
	}

	std::vector<SceneRenderable> SceneTraversal::BuildRenderables(const SceneAsset& scene)
	{
		std::vector<SceneRenderable> renderables;
		TraverseScene(scene, nullptr, &renderables);
		return renderables;
	}
}