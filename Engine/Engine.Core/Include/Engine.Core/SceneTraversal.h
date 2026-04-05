// SceneTraversal.h

#pragma once

#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Types/SceneTypes.h>

#include <directxtk/SimpleMath.h>

namespace SimpleMath = DirectX::SimpleMath;

namespace Engine::Core
{
	struct TraversedSceneNode
	{
		std::uint32_t NodeIndex = 0;
		std::int32_t ParentIndex = -1;
		SimpleMath::Matrix LocalTransform = SimpleMath::Matrix::Identity;
		SimpleMath::Matrix WorldTransform = SimpleMath::Matrix::Identity;
	};

	struct SceneRenderable
	{
		std::uint32_t NodeIndex = 0;
		MeshHandle Mesh;
		SimpleMath::Matrix WorldTransform = SimpleMath::Matrix::Identity;
	};

	class SceneTraversal
	{
	public:
		[[nodiscard]] static SimpleMath::Matrix BuildLocalTransform(const SceneNode& node);
		[[nodiscard]] static std::vector<TraversedSceneNode> BuildNodeTransforms(const SceneAsset& scene);
		[[nodiscard]] static std::vector<SceneRenderable> BuildRenderables(const SceneAsset& scene);
	};
}