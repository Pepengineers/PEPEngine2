// TextureTypes.cpp

#include <Engine.Core/Types/TextureTypes.h>

#include <cassert>

namespace Engine::Core
{
	Texture::Texture(const TextureDesc& desc, std::vector<SubTexture> subresources)
		: _desc(desc),
		_subresources(std::move(subresources))
	{
		assert(_desc.ArraySize > 0);
		assert(_desc.MipLevels > 0);
		const size_t expectedSubresourceCount = static_cast<size_t>(_desc.ArraySize) * _desc.MipLevels;
		assert(_subresources.size() == expectedSubresourceCount);
	}

	ETextureDimension Texture::GetDimension() const
	{
		return _desc.Dimension;
	}

	DXGI_FORMAT Texture::GetFormat() const
	{
		return _desc.Format;
	}

	std::uint32_t Texture::GetWidth() const
	{
		return _desc.Width;
	}

	std::uint32_t Texture::GetHeight() const
	{
		return _desc.Height;
	}

	std::uint32_t Texture::GetDepth() const
	{
		return _desc.Depth;
	}

	std::uint32_t Texture::GetArraySize() const
	{
		return _desc.ArraySize;
	}

	std::uint32_t Texture::GetMipLevels() const
	{
		return _desc.MipLevels;
	}

	bool Texture::IsCubeMap() const
	{
		return _desc.Dimension == ETextureDimension::TextureCube;
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
		assert(mipLevel < _desc.MipLevels);
		assert(arraySlice < _desc.ArraySize);
		const size_t index = static_cast<size_t>(arraySlice) * _desc.MipLevels + mipLevel;
		assert(index < _subresources.size());
		return _subresources.at(index);
	}
}