// TextureLoader.h

#pragma once

#include <filesystem>

#include <Engine.Core/Types/TextureTypes.h>

namespace Engine::Core
{
	class TextureLoader
	{
	public:
		[[nodiscard]] static std::shared_ptr<Texture> LoadTextureAsset(const std::filesystem::path& sourcePath);
		[[nodiscard]] static std::shared_ptr<Texture> CreateDefaultTexture();
	};
}