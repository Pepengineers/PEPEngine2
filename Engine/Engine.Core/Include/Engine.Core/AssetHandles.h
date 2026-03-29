// AssetHandles.h

#pragma once

#include <cstdint>
#include <limits>

namespace Engine::Core
{
#pragma region Mesh Handle
	/// Stable identifier of a mesh entry inside AssetManager storage.
	struct MeshHandle
	{
		static constexpr std::uint32_t InvalidValue = (std::numeric_limits<std::uint32_t>::max)();

		std::uint32_t Value = InvalidValue;

		[[nodiscard]] bool IsValid() const
		{
			return Value != InvalidValue;
		}
	};
#pragma endregion Mesh Handle

#pragma region Texture Handle

	/// Stable identifier of a texture entry inside AssetManager storage.
	struct TextureHandle
	{
		static constexpr std::uint32_t InvalidValue = (std::numeric_limits<std::uint32_t>::max)();

		std::uint32_t Value = InvalidValue;

		[[nodiscard]] bool IsValid() const
		{
			return Value != InvalidValue;
		}
	};

#pragma endregion Texture Handle
}