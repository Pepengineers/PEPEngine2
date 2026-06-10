// TextureLoader.cpp

#include <Engine.Core/IO/TextureLoader.h>

#include <Common/AssetDebugLog.h>

#include <cwctype>
#include <fstream>
#include <string>

#include <directxtex/DirectXTex.h>
#include <wrl/client.h>
#include <windows.h>
#include <wincodec.h>

namespace
{
	class ScopedComInitialization
	{
	public:
		ScopedComInitialization()
		{
			// CoInitializeEx must be called before any COM objects (e.g. WIC) can be created.
			// COINIT_MULTITHREADED - this thread uses the free-threaded COM apartment model.
			_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		}

		~ScopedComInitialization()
		{
			// CoUninitialize must be called once for each successful CoInitializeEx call.
			// If CoInitializeEx returned RPC_E_CHANGED_MODE, COM was already initialized
			// by someone else - we must not call CoUninitialize in that case.
			if (SUCCEEDED(_result))
			{
				CoUninitialize();
			}
		}

		[[nodiscard]] bool IsUsable() const
		{
			// S_OK - COM was initialized successfully by this call.
			// S_FALSE - COM was already initialized on this thread with the same apartment model.
			// RPC_E_CHANGED_MODE - COM was already initialized with a different apartment model.
			// This is not an error - COM is still usable, we just don't own it.
			return SUCCEEDED(_result) || _result == RPC_E_CHANGED_MODE;
		}

	private:
		// Stores the result of CoInitializeEx to determine ownership and usability.
		HRESULT _result = E_FAIL;
	};

	void LogTextureLoaderMessage(const std::wstring& message)
	{
		LOG(message);
	}

	std::wstring ToLowerExtension(const std::filesystem::path& sourcePath)
	{
		std::wstring extension = sourcePath.extension().wstring();

		for (wchar_t& character : extension)
		{
			character = static_cast<wchar_t>(std::towlower(character));
		}

		return extension;
	}

	/// Returns true if text begins with the given prefix.
	bool StartsWith(const std::string& text, const std::string_view prefix)
	{
		return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
	}

	/// Reads up to maxBytes bytes from the beginning of the file at sourcePath.
	/// Returns fewer bytes if the file is smaller than maxBytes.
	/// The returned string contains raw binary data, not a null-terminated string.
	std::string ReadFilePrefix(const std::filesystem::path& sourcePath, const size_t maxBytes)
	{
		std::ifstream inputFile(sourcePath, std::ios::binary);
		if (!inputFile.is_open() || maxBytes == 0)
		{
			return {};
		}

		std::string prefix;
		prefix.resize(maxBytes);

		inputFile.read(prefix.data(), static_cast<std::streamsize>(maxBytes));
		prefix.resize(static_cast<size_t>(inputFile.gcount()));

		return prefix;
	}

	/// Returns true if the already-read file prefix is a Git LFS pointer rather than real asset data.
	bool IsGitLfsPointerFile(const std::string& filePrefix)
	{
		static constexpr std::string_view GitLfsPointerPrefix = "version https://git-lfs.github.com/spec/v1";
		return StartsWith(filePrefix, GitLfsPointerPrefix);
	}

	/// Returns true if the file at sourcePath is a Git LFS pointer rather than real asset data.
	bool IsGitLfsPointerFile(const std::filesystem::path& sourcePath)
	{
		return IsGitLfsPointerFile(ReadFilePrefix(sourcePath, 128));
	}

	/// Returns true if the already-read file prefix begins with the DDS magic bytes ("DDS ").
	bool HasDdsMagic(const std::string& filePrefix)
	{
		static constexpr std::string_view DdsMagic = "DDS ";
		return StartsWith(filePrefix, DdsMagic);
	}

	/// Returns true if the file at sourcePath begins with the DDS magic bytes ("DDS ").
	/// Used to guard against files that have a .dds extension but invalid or missing content.
	bool HasDdsMagic(const std::filesystem::path& sourcePath)
	{
		return HasDdsMagic(ReadFilePrefix(sourcePath, 4));
	}

	/// Returns true if DirectXTex can build a mip chain for the given texture metadata.
	bool CanGenerateMipMaps(const DirectX::TexMetadata& metadata)
	{
		if (metadata.mipLevels != 1 || (metadata.width <= 1 && metadata.height <= 1))
		{
			return false;
		}

		if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D)
		{
			return false;
		}

		if (metadata.format == DXGI_FORMAT_UNKNOWN || DirectX::IsCompressed(metadata.format) || DirectX::IsPacked(metadata.format) ||
			DirectX::IsTypeless(metadata.format) || DirectX::IsVideo(metadata.format))
		{
			return false;
		}

		return true;
	}

	/// Generates a full mip chain from the provided DirectXTex image set when possible.
	bool TryGenerateMipMaps(
		const DirectX::TexMetadata& metadata,
		const DirectX::Image* images,
		const size_t imageCount,
		DirectX::ScratchImage& mipChain,
		const std::filesystem::path& sourcePath)
	{
		if (images == nullptr || imageCount == 0 || !CanGenerateMipMaps(metadata))
		{
			return false;
		}

		const HRESULT generateResult = DirectX::GenerateMipMaps(
			images,
			imageCount,
			metadata,
			DirectX::TEX_FILTER_DEFAULT,
			0,
			mipChain);

		if (FAILED(generateResult))
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to generate mip maps for path: " + sourcePath.generic_wstring() + L"\n");
			return false;
		}

		const DirectX::TexMetadata& mipMetadata = mipChain.GetMetadata();
		if (mipMetadata.mipLevels <= 1)
		{
			return false;
		}

		LogTextureLoaderMessage(L"[TextureLoader] Generated " + std::to_wstring(mipMetadata.mipLevels) + L" mip levels for path: " + sourcePath.generic_wstring() + L"\n");

		return true;
	}

	std::unique_ptr<Engine::Core::Texture> CreateTextureFromScratchImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& scratchImage);

	/// Builds a generated mip chain from a single-mip CPU texture.
	std::unique_ptr<Engine::Core::Texture> GenerateMipMapsForTexture(const Engine::Core::Texture& texture, const std::filesystem::path& sourcePath)
	{
		if (texture.IsEmpty() || texture.GetArraySize() != 1 || texture.GetMipLevels() != 1)
		{
			return nullptr;
		}

		const Engine::Core::SubTexture& baseSubresource = texture.GetSubresource(0, 0);
		if (baseSubresource.Data.empty())
		{
			return nullptr;
		}

		DirectX::TexMetadata metadata = {};
		metadata.width = texture.GetWidth();
		metadata.height = texture.GetHeight();
		metadata.depth = texture.GetDepth();
		metadata.arraySize = texture.GetArraySize();
		metadata.mipLevels = texture.GetMipLevels();
		metadata.format = texture.GetFormat();
		metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

		DirectX::Image baseImage = {};
		baseImage.width = texture.GetWidth();
		baseImage.height = texture.GetHeight();
		baseImage.format = texture.GetFormat();
		baseImage.rowPitch = baseSubresource.RowPitch;
		baseImage.slicePitch = baseSubresource.SlicePitch;
		baseImage.pixels = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(baseSubresource.Data.data()));

		DirectX::ScratchImage mipChain;
		if (!TryGenerateMipMaps(metadata, &baseImage, 1, mipChain, sourcePath))
		{
			return nullptr;
		}

		std::unique_ptr<Engine::Core::Texture> generatedTexture = CreateTextureFromScratchImage(mipChain.GetMetadata(), mipChain);
		if (generatedTexture == nullptr)
		{
			return nullptr;
		}

		return std::make_unique<Engine::Core::Texture>(generatedTexture->WithType(texture.GetType()));
	}

	/// Loads a texture from a WIC-compatible format (PNG, JPG, BMP, etc.) into a CPU Texture.
	/// The decoded pixels are always converted to DXGI_FORMAT_R8G8B8A8_UNORM regardless
	/// of the source pixel format. A full mip chain is generated when supported.
	/// Returns nullptr on any failure.
	std::unique_ptr<Engine::Core::Texture> LoadTextureFromWicFile(const std::filesystem::path& sourcePath)
    {
    	using Microsoft::WRL::ComPtr;

		// Creating a WIC factory.
    	ComPtr<IWICImagingFactory> imagingFactory;
		// CoCreateInstance is the standard way to create a COM object.
		// CLSID_WICImagingFactory tells it to create specifically a WIC factory.
		// CLSCTX_INPROC_SERVER means the object lives in the same process.
		// IID_PPV_ARGS is a macro that automatically supplies the correct interface and the pointer where the result should be stored.
    	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(imagingFactory.GetAddressOf()));

    	if (FAILED(hr))
    	{
    		return nullptr;
    	}

		// Creating a decoder.
    	ComPtr<IWICBitmapDecoder> decoder;
		// WIC automatically detects the file format (PNG, JPG, BMP, etc.) and creates the appropriate decoder.
		// WICDecodeMetadataCacheOnDemand means metadata is loaded lazily, only when needed.
    	hr = imagingFactory->CreateDecoderFromFilename(sourcePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());

    	if (FAILED(hr))
    	{
    		return nullptr;
    	}

		// Getting a frame.
    	ComPtr<IWICBitmapFrameDecode> frame;
		// Some formats (e.g. GIF) can contain multiple frames.
		// Here, the first one is always taken - index 0.
    	hr = decoder->GetFrame(0, frame.GetAddressOf());
		
    	if (FAILED(hr))
    	{
    		return nullptr;
    	}

    	UINT width = 0;
    	UINT height = 0;
    	hr = frame->GetSize(&width, &height);
    	if (FAILED(hr) || width == 0 || height == 0)
    	{
    		return nullptr;
    	}

		// Converting the pixel format.
    	ComPtr<IWICBitmapSource> bitmapSource;

		// Different files store pixels differently - RGB without alpha, 16 bits per channel, palettes, etc.
		// The GPU expects R8G8B8A8_UNORM - 4 bytes per pixel.
    	WICPixelFormatGUID pixelFormat = {};
    	hr = frame->GetPixelFormat(&pixelFormat);
		
    	if (FAILED(hr))
    	{
    		return nullptr;
    	}

		// If the format is already correct, we use the frame directly via As
		// (this is a COM QueryInterface - getting another interface of the same object).
    	if (pixelFormat == GUID_WICPixelFormat32bppRGBA)
    	{
    		hr = frame.As(&bitmapSource);
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}
    	}
		// Otherwise, we create a converter that transforms the pixels into the required format on the fly.
    	else
    	{
    		ComPtr<IWICFormatConverter> formatConverter;
    		hr = imagingFactory->CreateFormatConverter(formatConverter.GetAddressOf());
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}

    		hr = formatConverter->Initialize(
    			frame.Get(),
    			GUID_WICPixelFormat32bppRGBA,
    			WICBitmapDitherTypeNone,
    			nullptr,
    			0.0,
    			WICBitmapPaletteTypeCustom);

    		if (FAILED(hr))
    		{
    			return nullptr;
    		}

    		hr = formatConverter.As(&bitmapSource);
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}
    	}

    	Engine::Core::TextureDesc textureDesc = {};
    	textureDesc.Dimension = Engine::Core::ETextureDimension::Texture2D;
		textureDesc.Type = Engine::Core::ETextureType::Unknown;
    	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    	textureDesc.Width = static_cast<std::uint32_t>(width);
    	textureDesc.Height = static_cast<std::uint32_t>(height);
    	textureDesc.Depth = 1;
    	textureDesc.ArraySize = 1;
    	textureDesc.MipLevels = 1;

    	Engine::Core::SubTexture subresource = {};
    	subresource.Width = textureDesc.Width;
    	subresource.Height = textureDesc.Height;
    	subresource.Depth = 1;
    	subresource.RowPitch = static_cast<size_t>(width) * 4u;
    	subresource.SlicePitch = subresource.RowPitch * static_cast<size_t>(height);
    	subresource.Data.resize(subresource.SlicePitch);

    	hr = bitmapSource->CopyPixels(
    		nullptr,
    		static_cast<UINT>(subresource.RowPitch),
    		static_cast<UINT>(subresource.SlicePitch),
    		reinterpret_cast<BYTE*>(subresource.Data.data()));

    	if (FAILED(hr))
    	{
    		return nullptr;
    	}

    	std::vector<Engine::Core::SubTexture> subresources;
    	subresources.push_back(std::move(subresource));

		auto baseTexture = std::make_unique<Engine::Core::Texture>(textureDesc, std::move(subresources));
		std::unique_ptr<Engine::Core::Texture> textureWithMipMaps = GenerateMipMapsForTexture(*baseTexture, sourcePath);
		if (textureWithMipMaps != nullptr)
		{
			return textureWithMipMaps;
		}

		return baseTexture;
    }

	/// Loads a DDS, TGA or HDR file into a DirectXTex ScratchImage.
	/// Returns a failing HRESULT if the extension is not recognized or loading fails.
	HRESULT LoadScratchImageFromFile(const std::filesystem::path& sourcePath, DirectX::TexMetadata& metadata, DirectX::ScratchImage& scratchImage)
	{
		const std::wstring extension = ToLowerExtension(sourcePath);

		if (extension == L".dds")
		{
			return DirectX::LoadFromDDSFile(sourcePath.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratchImage);
		}

		if (extension == L".tga")
		{
			return DirectX::LoadFromTGAFile(sourcePath.c_str(), DirectX::TGA_FLAGS_NONE, &metadata, scratchImage);
		}

		if (extension == L".hdr")
		{
			return DirectX::LoadFromHDRFile(sourcePath.c_str(), &metadata, scratchImage);
		}

		return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
	}

	/// Maps a DirectXTex texture dimension to the engine's ETextureDimension enum.
	/// Cubemaps are detected via metadata.IsCubemap() and returned as TextureCube.
	Engine::Core::ETextureDimension ConvertTextureDimension(const DirectX::TexMetadata& metadata)
	{
		switch (metadata.dimension)
		{
		case DirectX::TEX_DIMENSION_TEXTURE1D:
			return Engine::Core::ETextureDimension::Texture1D;

		case DirectX::TEX_DIMENSION_TEXTURE2D:
			return metadata.IsCubemap() ? Engine::Core::ETextureDimension::TextureCube : Engine::Core::ETextureDimension::Texture2D;

		case DirectX::TEX_DIMENSION_TEXTURE3D:
			return Engine::Core::ETextureDimension::Texture3D;

		default:
			return Engine::Core::ETextureDimension::Unknown;
		}
	}

	/// Converts a loaded DirectXTex ScratchImage into a CPU Texture by copying
	/// each subresource (mip level / array slice) into a SubTexture.
	/// 3D textures are not supported and return nullptr.
	/// Returns nullptr if the subresource set is incomplete
	/// or if any subresource has missing pixel data.
	std::unique_ptr<Engine::Core::Texture> CreateTextureFromScratchImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& scratchImage)
	{
		if (metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D)
		{
			return nullptr;
		}

		const DirectX::Image* images = scratchImage.GetImages();
		const size_t imageCount = scratchImage.GetImageCount();

		if (images == nullptr || imageCount == 0)
		{
			return nullptr;
		}

		Engine::Core::TextureDesc textureDesc = {};
		textureDesc.Dimension = ConvertTextureDimension(metadata);
		textureDesc.Type = Engine::Core::ETextureType::Unknown;
		textureDesc.Format = metadata.format;
		textureDesc.Width = static_cast<std::uint32_t>(metadata.width);
		textureDesc.Height = static_cast<std::uint32_t>(metadata.height);
		textureDesc.Depth = static_cast<std::uint32_t>(metadata.depth);
		textureDesc.ArraySize = static_cast<std::uint32_t>(metadata.arraySize);
		textureDesc.MipLevels = static_cast<std::uint32_t>(metadata.mipLevels);
		
		// ScratchImage must contain the full dense subresource set described by TextureDesc
		// (all mip levels for all array slices). Reject incomplete images here so we do not
		// build a partial Texture or rely on constructor asserts to catch the mismatch.
		const size_t expectedSubresourceCount = static_cast<size_t>(textureDesc.ArraySize) * textureDesc.MipLevels;
        if (imageCount != expectedSubresourceCount)
        {
        	LogTextureLoaderMessage(L"[TextureLoader] Failed to create texture from scratch image: expected " +
        		std::to_wstring(expectedSubresourceCount) + L" subresources, got " + std::to_wstring(imageCount) + L".\n");
        	return nullptr;
        }

		// Validate the full subresource set up front.
		// A Texture is expected to contain every mip level / array slice described by TextureDesc,
		// so partial textures are rejected before any data is copied.
		for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
		{
			const DirectX::Image& image = images[imageIndex];
			if (image.pixels == nullptr || image.slicePitch == 0)
			{
				LogTextureLoaderMessage(L"[TextureLoader] Failed to create texture from scratch image: subresource " +
					std::to_wstring(imageIndex) + L" has missing pixel data.\n");
				return nullptr;
			}
		}

		std::vector<Engine::Core::SubTexture> subresources;
		subresources.reserve(imageCount);

		for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
		{
			const DirectX::Image& image = images[imageIndex];

			Engine::Core::SubTexture subresource = {};
			subresource.Width = static_cast<std::uint32_t>(image.width);
			subresource.Height = static_cast<std::uint32_t>(image.height);
			subresource.Depth = 1;
			subresource.RowPitch = image.rowPitch;
			subresource.SlicePitch = image.slicePitch;
			subresource.Data.resize(image.slicePitch);

			std::memcpy(subresource.Data.data(), image.pixels, image.slicePitch);
			subresources.push_back(std::move(subresource));
		}

		return std::make_unique<Engine::Core::Texture>(textureDesc, std::move(subresources));
	}
}

namespace Engine::Core
{
	std::unique_ptr<Texture> TextureLoader::LoadTextureAsset(const std::filesystem::path& sourcePath)
	{
		if (sourcePath.empty())
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to load texture: empty source path.\n");
			return nullptr;
		}

		if (!std::filesystem::exists(sourcePath))
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to load texture: file does not exist: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		const std::string filePrefix = ReadFilePrefix(sourcePath, 128);

		if (IsGitLfsPointerFile(filePrefix))
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to load texture: file is a Git LFS pointer, not real texture data: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		const std::wstring extension = ToLowerExtension(sourcePath);
		if (extension == L".dds" && !HasDdsMagic(filePrefix))
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to load texture: invalid DDS header in file: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		ScopedComInitialization comInitialization;
		if (!comInitialization.IsUsable())
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to initialize COM for path: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		std::unique_ptr<Texture> loadedTexture;

		if (extension == L".dds" || extension == L".tga" || extension == L".hdr")
		{
			DirectX::TexMetadata metadata = {};
			DirectX::ScratchImage scratchImage;

			const HRESULT loadResult = LoadScratchImageFromFile(sourcePath, metadata, scratchImage);
			if (FAILED(loadResult))
			{
				LogTextureLoaderMessage(L"[TextureLoader] Failed to read texture file: " + sourcePath.generic_wstring() + L"\n");
				return nullptr;
			}

			DirectX::ScratchImage mipChain;
			const DirectX::ScratchImage* textureImage = &scratchImage;
			DirectX::TexMetadata textureMetadata = metadata;

			if (TryGenerateMipMaps(metadata, scratchImage.GetImages(), scratchImage.GetImageCount(), mipChain, sourcePath))
			{
				textureImage = &mipChain;
				textureMetadata = mipChain.GetMetadata();
			}

			loadedTexture = CreateTextureFromScratchImage(textureMetadata, *textureImage);
		}
		else
		{
			loadedTexture = LoadTextureFromWicFile(sourcePath);
		}

		if (loadedTexture == nullptr)
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to convert texture data for path: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		LogTextureLoaderMessage(L"[TextureLoader] Loaded texture with " + std::to_wstring(loadedTexture->GetSubresourceCount()) +
			L" subresources and " + std::to_wstring(loadedTexture->GetMipLevels()) + L" mip levels from path: " + sourcePath.generic_wstring() + L"\n");

		if (loadedTexture->GetMipLevels() > 1)
		{
			LogTextureLoaderMessage(L"[TextureLoader] Mip chain check passed: " + sourcePath.generic_wstring() + L" has " +
				std::to_wstring(loadedTexture->GetMipLevels()) + L" mip levels.\n");
		}

		return loadedTexture;
	}

	std::unique_ptr<Texture> TextureLoader::CreateDefaultTexture()
	{
		TextureDesc textureDesc = {};
		textureDesc.Dimension = ETextureDimension::Texture2D;
		textureDesc.Type = ETextureType::Color;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = 2;
		textureDesc.Height = 2;
		textureDesc.Depth = 1;
		textureDesc.ArraySize = 1;
		textureDesc.MipLevels = 1;

		SubTexture subresource = {};
		subresource.Width = 2;
		subresource.Height = 2;
		subresource.Depth = 1;
		subresource.RowPitch = 2u * 4u;
		subresource.SlicePitch = 2u * subresource.RowPitch;
		subresource.Data =
		{
			std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
			std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
			std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
		};

		std::vector<SubTexture> subresources;
		subresources.push_back(std::move(subresource));

		LogTextureLoaderMessage(L"[TextureLoader] Created default error texture.\n");
		return std::make_unique<Texture>(textureDesc, std::move(subresources));
	}
}