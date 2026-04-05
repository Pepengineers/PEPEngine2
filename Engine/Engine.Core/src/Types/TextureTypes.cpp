// TextureTypes.cpp

#include <Engine.Core/Types/TextureTypes.h>

#include <cassert>

namespace Engine::Core
{
	Texture::Texture(const TextureDesc& desc, std::vector<SubTexture> subresources)
	{
		_dimension = desc.Dimension;
		_format = desc.Format;
		_width = desc.Width;
		_height = desc.Height;
		_depth = desc.Depth;
		_arraySize = desc.ArraySize;
		_mipLevels = desc.MipLevels;
		_bIsCubeMap = desc.bIsCubeMap;

		_subresources = std::move(subresources);
	}

	ETextureDimension Texture::GetDimension() const
	{
		return _dimension;
	}

	DXGI_FORMAT Texture::GetFormat() const
	{
		return _format;
	}

	std::uint32_t Texture::GetWidth() const
	{
		return _width;
	}

	std::uint32_t Texture::GetHeight() const
	{
		return _height;
	}

	std::uint32_t Texture::GetDepth() const
	{
		return _depth;
	}

	std::uint32_t Texture::GetArraySize() const
	{
		return _arraySize;
	}

	std::uint32_t Texture::GetMipLevels() const
	{
		return _mipLevels;
	}

	bool Texture::IsCubeMap() const
	{
		return _bIsCubeMap;
	}

	bool Texture::IsEmpty() const
	{
		return _subresources.empty();
	}

	size_t Texture::GetSubresourceCount() const
	{
		return _subresources.size();
	}

	const std::vector<SubTexture>& Texture::GetSubresources() const
	{
		return _subresources;
	}

	const SubTexture& Texture::GetSubresource(const std::uint32_t mipLevel, const std::uint32_t arraySlice) const
	{
		const size_t index = static_cast<size_t>(arraySlice) * _mipLevels + mipLevel;

		assert(index < _subresources.size());

		return _subresources[index];
	}
}