// TextureLoader.cpp

#include <Engine.Core/IO/TextureLoader.h>

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
			_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		}

		~ScopedComInitialization()
		{
			if (SUCCEEDED(_result))
			{
				CoUninitialize();
			}
		}

		[[nodiscard]] bool IsUsable() const
		{
			return SUCCEEDED(_result) || _result == RPC_E_CHANGED_MODE;
		}

	private:
		HRESULT _result = E_FAIL;
	};

	void LogTextureLoaderMessage(const std::wstring& message)
	{
		OutputDebugStringW(message.c_str());
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

	bool StartsWith(const std::string& text, const std::string_view prefix)
	{
		return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
	}

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

	bool IsGitLfsPointerFile(const std::filesystem::path& sourcePath)
	{
		static constexpr std::string_view GitLfsPointerPrefix = "version https://git-lfs.github.com/spec/v1";

		const std::string filePrefix = ReadFilePrefix(sourcePath, 128);
		return StartsWith(filePrefix, GitLfsPointerPrefix);
	}

	bool HasDdsMagic(const std::filesystem::path& sourcePath)
	{
		static constexpr std::string_view DdsMagic = "DDS ";

		const std::string filePrefix = ReadFilePrefix(sourcePath, DdsMagic.size());
		return StartsWith(filePrefix, DdsMagic);
	}

	std::shared_ptr<Engine::Core::Texture> LoadTextureFromWicFile(const std::filesystem::path& sourcePath)
    	{
    		using Microsoft::WRL::ComPtr;
    
    		ComPtr<IWICImagingFactory> imagingFactory;
    		HRESULT hr = CoCreateInstance(
    			CLSID_WICImagingFactory,
    			nullptr,
    			CLSCTX_INPROC_SERVER,
    			IID_PPV_ARGS(imagingFactory.GetAddressOf()));
    
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}
    
    		ComPtr<IWICBitmapDecoder> decoder;
    		hr = imagingFactory->CreateDecoderFromFilename(
    			sourcePath.c_str(),
    			nullptr,
    			GENERIC_READ,
    			WICDecodeMetadataCacheOnDemand,
    			decoder.GetAddressOf());
    
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}
    
    		ComPtr<IWICBitmapFrameDecode> frame;
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
    
    		ComPtr<IWICBitmapSource> bitmapSource;
    
    		WICPixelFormatGUID pixelFormat = {};
    		hr = frame->GetPixelFormat(&pixelFormat);
    		if (FAILED(hr))
    		{
    			return nullptr;
    		}
    
    		if (pixelFormat == GUID_WICPixelFormat32bppRGBA)
    		{
    			hr = frame.As(&bitmapSource);
    			if (FAILED(hr))
    			{
    				return nullptr;
    			}
    		}
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
    
    		Engine::Core::TextureDesc textureDesc;
    		textureDesc.Dimension = Engine::Core::ETextureDimension::Texture2D;
    		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    		textureDesc.Width = static_cast<std::uint32_t>(width);
    		textureDesc.Height = static_cast<std::uint32_t>(height);
    		textureDesc.Depth = 1;
    		textureDesc.ArraySize = 1;
    		textureDesc.MipLevels = 1;
    		textureDesc.bIsCubeMap = false;
    
    		Engine::Core::SubTexture subresource;
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
    
    		return std::make_shared<Engine::Core::Texture>(textureDesc, std::move(subresources));
    	}

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

	std::shared_ptr<Engine::Core::Texture> CreateTextureFromScratchImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& scratchImage)
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

		Engine::Core::TextureDesc textureDesc;
		textureDesc.Dimension = ConvertTextureDimension(metadata);
		textureDesc.Format = metadata.format;
		textureDesc.Width = static_cast<std::uint32_t>(metadata.width);
		textureDesc.Height = static_cast<std::uint32_t>(metadata.height);
		textureDesc.Depth = static_cast<std::uint32_t>(metadata.depth);
		textureDesc.ArraySize = static_cast<std::uint32_t>(metadata.arraySize);
		textureDesc.MipLevels = static_cast<std::uint32_t>(metadata.mipLevels);
		textureDesc.bIsCubeMap = metadata.IsCubemap();

		std::vector<Engine::Core::SubTexture> subresources;
		subresources.reserve(imageCount);

		for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
		{
			const DirectX::Image& image = images[imageIndex];
			if (image.pixels == nullptr || image.slicePitch == 0)
			{
				return nullptr;
			}

			Engine::Core::SubTexture subresource;
			subresource.Width = static_cast<std::uint32_t>(image.width);
			subresource.Height = static_cast<std::uint32_t>(image.height);
			subresource.Depth = 1;
			subresource.RowPitch = image.rowPitch;
			subresource.SlicePitch = image.slicePitch;
			subresource.Data.resize(image.slicePitch);

			std::memcpy(subresource.Data.data(), image.pixels, image.slicePitch);
			subresources.push_back(std::move(subresource));
		}

		return std::make_shared<Engine::Core::Texture>(textureDesc, std::move(subresources));
	}
}

namespace Engine::Core
{
	std::shared_ptr<Texture> TextureLoader::LoadTextureAsset(const std::filesystem::path& sourcePath)
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

		if (IsGitLfsPointerFile(sourcePath))
		{
			LogTextureLoaderMessage(L"[TextureLoader] Failed to load texture: file is a Git LFS pointer, not real texture data: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}


		const std::wstring extension = ToLowerExtension(sourcePath);
		if (extension == L".dds" && !HasDdsMagic(sourcePath))
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

		std::shared_ptr<Texture> loadedTexture;

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

			loadedTexture = CreateTextureFromScratchImage(metadata, scratchImage);
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
			L" subresources from path: " + sourcePath.generic_wstring() + L"\n");

		return loadedTexture;
	}

	std::shared_ptr<Texture> TextureLoader::CreateDefaultTexture()
	{
		TextureDesc textureDesc;
		textureDesc.Dimension = ETextureDimension::Texture2D;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = 2;
		textureDesc.Height = 2;
		textureDesc.Depth = 1;
		textureDesc.ArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.bIsCubeMap = false;

		SubTexture subresource;
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
		return std::make_shared<Texture>(textureDesc, std::move(subresources));
	}
}