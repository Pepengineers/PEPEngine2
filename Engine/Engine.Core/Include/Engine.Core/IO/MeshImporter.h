// MeshImporter.h

#pragma once

#include <filesystem>

#include <Engine.Core/Types/MeshTypes.h>
#include <Engine.Core/IO/ImportPolicies.h>

namespace Engine::Core
{
	class MeshImporter
	{
	public:
		/// Imports one logical mesh asset from sourcePath.
		/// This path is intended for files that represent a single model asset,
		/// not a whole scene with multiple independent objects.
		[[nodiscard]] static std::unique_ptr<Mesh> ImportSingleMeshAsset(const std::filesystem::path& sourcePath, const MeshImportOptions& options = {});

		/// Imports one mesh sub-asset from sourcePath in local mesh space.
		/// Node hierarchy transforms are intentionally not baked in this mode,
		/// because the caller is expected to preserve them in a scene asset.
		[[nodiscard]] static std::unique_ptr<Mesh> ImportMeshSubAsset(const std::filesystem::path& sourcePath, std::uint32_t subAssetIndex, const MeshImportOptions& options = {});
	};
}