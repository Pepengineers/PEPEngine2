// SceneTraversal.h

#pragma once

#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Types/SceneTypes.h>

#include <directxtk/SimpleMath.h>

namespace Engine::Core
{
	/// Result of traversing a single scene node.
	/// Contains the node's index, parent relationship, and both its local
	/// and fully accumulated world-space transforms.
	struct TraversedSceneNode
	{
		std::uint32_t NodeIndex = 0;
		std::int32_t ParentIndex = -1;
		DirectX::SimpleMath::Matrix LocalTransform = DirectX::SimpleMath::Matrix::Identity;
		DirectX::SimpleMath::Matrix WorldTransform = DirectX::SimpleMath::Matrix::Identity;
	};

	/// A single renderable unit extracted from a scene node.
	/// Pairs a mesh asset handle with the world transform of the node it belongs to,
	/// providing everything the renderer needs to draw one mesh instance.
	struct SceneRenderable
	{
		std::uint32_t NodeIndex = 0;
		MeshHandle Mesh;
		DirectX::SimpleMath::Matrix WorldTransform = DirectX::SimpleMath::Matrix::Identity;
	};
	
	/// Utility class for traversing a SceneAsset and computing per-node transforms.
	/// All methods perform a recursive depth-first walk of the scene hierarchy,
	/// accumulating parent-to-child transforms as they go. The results are
	/// returned as flat arrays indexed by node index, which can be passed
	/// directly to rendering or simulation systems.
	class SceneTraversal
	{
	public:
		/// Builds a local transform matrix from a node's translation, rotation and scale.
		/// The resulting matrix applies scale first, then rotation, then translation (TRS order).
		[[nodiscard]] static DirectX::SimpleMath::Matrix BuildLocalTransform(const SceneNode& node);

		/// Computes local and world transforms for every node in the scene.
		/// Returns a flat array indexed by node index, where each entry contains
		/// the node's local and accumulated world transform.
		/// Root nodes are transformed relative to the identity matrix.
		[[nodiscard]] static std::vector<TraversedSceneNode> BuildNodeTransforms(const SceneAsset& scene);

		/// Builds a flat list of renderables from all mesh-bearing nodes in the scene.
		/// Each renderable pairs a MeshHandle with the fully accumulated world transform
		/// of the node it belongs to, ready to be submitted to the renderer.
		[[nodiscard]] static std::vector<SceneRenderable> BuildRenderables(const SceneAsset& scene);
	};
}