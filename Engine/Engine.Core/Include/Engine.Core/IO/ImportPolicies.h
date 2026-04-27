// ImportPolicies.h

#pragma once

#include <cstdint>

namespace Engine::Core
{
	/// Describes what kind of asset the callers expects from a source file.
	enum class EAssetImportMode : std::uint8_t
	{
		SingleMeshAsset = 0,
		SceneAsset = 1,
	};

	/// Options for importing one logical mesh asset.
	struct MeshImportOptions
	{
		bool bTriangulate = true;
		bool bGenerateNormals = true;
		bool bGenerateTangents = true;
		bool bFlipUVs = true;
	};

	/// Options for importing a scene asset while preserving logical structure.
	struct SceneImportOptions
	{
		bool bPreserveHierarchy = true;
		bool bReuseMeshAssets = true;
	};
}