// AssetLocators.h

#pragma once

#include <cstdint>
#include <filesystem>

namespace Engine::Core
{
	/// Identifies a mesh asset inside a source file.
	/// For standalone mesh files SubAssetIndex stays invalid.
	struct MeshAssetLocator
	{
		static constexpr std::uint32_t InvalidSubAssetIndex = (std::numeric_limits<std::uint32_t>::max)();

		std::filesystem::path SourcePath;
		std::uint32_t SubAssetIndex = InvalidSubAssetIndex;

		/// Returns true if this locator points to a specific asset within a multi-asset source file.
		/// Returns false if the source file contains a single asset and no index is needed.
		[[nodiscard]] bool HasSubAssetIndex() const
		{
			return SubAssetIndex != InvalidSubAssetIndex;
		}
	};
}