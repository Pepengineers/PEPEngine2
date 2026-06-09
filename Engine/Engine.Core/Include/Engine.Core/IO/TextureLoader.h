// TextureLoader.h

#pragma once

#include <filesystem>

#include <Engine.Core/Types/TextureTypes.h>

namespace Engine::Core
{
	class TextureLoader
	{
	public:
		/// Loads a texture asset from the given file path into a CPU-side Texture object.
		///
		/// Supports two loading paths depending on the file format:
		///   - DDS, TGA, HDR: loaded via DirectXTex, preserving GPU-native formats,
		///     array slices and cubemap faces as stored in the file.
		///   - All other formats (PNG, JPG, BMP, etc.): loaded via WIC and converted
		///     to DXGI_FORMAT_R8G8B8A8_UNORM.
		///
		/// Generates a full mip chain for supported 2D textures that only contain
		/// the base mip level. Existing mip chains are preserved.
		///
		/// Performs the following validation before loading:
		///   - Rejects empty paths.
		///   - Rejects files that do not exist on disk.
		///   - Rejects Git LFS pointer files (present when the repo is cloned without LFS).
		///   - Rejects .dds files that do not begin with the DDS magic bytes.
		///
		/// Returns nullptr on any validation or loading failure.
		[[nodiscard]] static std::unique_ptr<Texture> LoadTextureAsset(const std::filesystem::path& sourcePath);

		/// Creates a minimal 2x2 fallback texture used when a real texture fails to load.
		///
		/// The texture is a magenta/black checkerboard pattern in DXGI_FORMAT_R8G8B8A8_UNORM:
		///   [magenta] [black ]
		///   [black  ] [magenta]
		///
		/// This makes missing textures immediately visible in the viewport
		/// without crashing or leaving GPU slots unbound.
		[[nodiscard]] static std::unique_ptr<Texture> CreateDefaultTexture();
	};
}