// TextureTypes.h

#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>

namespace Engine::Core
{
	/// Describes the logical texture shape independently of the renderer.
	enum class ETextureDimension : std::uint8_t
	{
		Unknown = 0,
		Texture1D = 1,
		Texture2D = 2,
		Texture3D = 3,
		TextureCube = 4,
	};

	/// Describes how texture pixels should be interpreted by the renderer.
	enum class ETextureType : std::uint8_t
	{
		Unknown = 0,
		Color = 1,
		Data = 2,
	};

	/// Raw data for one texture subresource.
	/// One subresource corresponds to exactly one mip level of one array slice.
	struct SubTexture
	{
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint32_t Depth = 1;
		
		/// Number of bytes between the start of one row of texels and the start of the next.
		/// May be larger than Width * BytesPerTexel due to GPU alignment padding.
		/// For compressed formats, this is the number of bytes per row of compressed blocks.
		size_t RowPitch = 0;
		/// Number of bytes between the start of one depth slice and the start of the next.
		/// May be larger than RowPitch * Height due to alignment padding.
		/// For 1D and 2D textures this equals the total byte size of the subresource data.
		size_t SlicePitch = 0;
		
		std::vector<std::byte> Data;
	};

	/// Describes the properties of a texture asset before it is created.
	/// Pass this to the Texture constructor together with subresource data.
	struct TextureDesc
	{
		ETextureDimension Dimension = ETextureDimension::Unknown;
		ETextureType Type = ETextureType::Unknown;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint32_t Depth = 1;
		std::uint32_t ArraySize = 1;
		std::uint32_t MipLevels = 1;
	};

	/// CPU-side texture representation.
	class Texture
	{
	public:
		Texture() = default;
		Texture(const TextureDesc& desc, std::vector<SubTexture> subresources);

		/// Returns the logical shape of the texture.
		[[nodiscard]] ETextureDimension GetDimension() const;

		/// Returns how texture pixels should be interpreted by the renderer.
		[[nodiscard]] ETextureType GetType() const;

		/// Returns a copy of this texture with the given semantic type.
		[[nodiscard]] Texture WithType(ETextureType type) const;

		/// Returns the DXGI pixel format of the texture.
		[[nodiscard]] DXGI_FORMAT GetFormat() const;

		/// Returns the width of the base mip level in texels.
		[[nodiscard]] std::uint32_t GetWidth() const;

		/// Returns the height of the base mip level in texels. 1 for 1D textures.
		[[nodiscard]] std::uint32_t GetHeight() const;

		/// Returns the depth of the base mip level in slices. 1 for non-volumetric textures.
		[[nodiscard]] std::uint32_t GetDepth() const;

		/// Returns the number of textures in the array. 1 for non-array textures, 6 for cube maps.
		[[nodiscard]] std::uint32_t GetArraySize() const;

		/// Returns the number of mip levels, including the base level.
		[[nodiscard]] std::uint32_t GetMipLevels() const;

		/// Returns true if this texture is a cube map.
		[[nodiscard]] bool IsCubeMap() const;
		
		/// Returns true if the texture has no subresources.
		[[nodiscard]] bool IsEmpty() const;

		/// Returns the total number of subresources (ArraySize * MipLevels).
		[[nodiscard]] size_t GetSubresourceCount() const;

		/// Returns all subresources as a flat array.
		[[nodiscard]] const std::vector<SubTexture>& GetSubresources() const;

		/// Returns the subresource for the given mip level and array slice.
		/// Parameter order matches D3D12CalcSubresource: mip is the inner index, array slice is the outer.
		/// Subresource index is computed as: arraySlice * MipLevels + mipLevel.
		[[nodiscard]] const SubTexture& GetSubresource(const std::uint32_t mipLevel, const std::uint32_t arraySlice) const;

	private:
		TextureDesc _desc;
		std::vector<SubTexture> _subresources;
    };
}