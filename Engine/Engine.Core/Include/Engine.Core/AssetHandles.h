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

		constexpr AssetHandle() noexcept = default;
		explicit constexpr AssetHandle(const std::uint32_t value, const std::uint32_t generation = 0) noexcept
			: _value(value)
			, _generation(generation)
		{
		}

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return _value != InvalidValue;
		}

		[[nodiscard]] constexpr std::uint32_t GetValue() const noexcept
		{
			return _value;
		}

		[[nodiscard]] constexpr std::uint32_t GetGeneration() const noexcept
		{
			return _generation;
		}

		[[nodiscard]] constexpr bool operator==(const AssetHandle& other) const noexcept
		{
			return _value == other._value && _generation == other._generation;
		}

		[[nodiscard]] constexpr bool operator!=(const AssetHandle& other) const noexcept
		{
			return !(*this == other);
		}

	private:
		std::uint32_t _value = InvalidValue;
		std::uint32_t _generation = 0;
	};

	struct MeshTag{};
	struct TextureTag{};
	struct SceneTag{};

	/// Stable identifier of a mesh entry inside AssetManager storage.
	using MeshHandle = AssetHandle<MeshTag>;

	/// Stable identifier of a texture entry inside AssetManager storage.
	using TextureHandle = AssetHandle<TextureTag>;

	/// Stable identifier of a scene entry inside AssetManager storage.
	using SceneHandle = AssetHandle<SceneTag>;
}