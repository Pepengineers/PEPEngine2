// ImportPolicies.h

#pragma once

#include <cstdint>
#include <functional>

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
        bool bCache = false; // Native Mesh cache for OBJ; source/MTL changes invalidate it.
		// Return false to cancel; may run on the import worker and must not throw.
		std::function<bool()> Continue;
	};

	/// Options for importing a scene asset while preserving logical structure.
	struct SceneImportOptions
	{
		bool bPreserveHierarchy = true;
		bool bReuseMeshAssets = true;
	};
}
