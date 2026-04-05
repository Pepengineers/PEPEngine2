// AssetHandles.h

#pragma once

#include <cstdint>
#include <limits>

namespace Engine::Core
{
	/// Stable identifier of a asset entry inside AssetManager storage.
	template<typename Tag>
	struct AssetHandle
	{
		static constexpr std::uint32_t InvalidValue = (std::numeric_limits<std::uint32_t>::max)();
		std::uint32_t Value = InvalidValue;

		[[nodiscard]] bool IsValid() const
		{
			return Value != InvalidValue;
		}
	};

	struct MeshTag{};
	struct TextureTag{};

	/// Stable identifier of a mesh entry inside AssetManager storage.
	using MeshHandle = AssetHandle<MeshTag>;

	/// Stable identifier of a texture entry inside AssetManager storage.
	using TextureHandle = AssetHandle<TextureTag>;
}