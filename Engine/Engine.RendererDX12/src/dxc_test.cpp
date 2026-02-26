// dxc_test.cpp

#include <Engine.RendererDX12/dxc_test.h>
#include <Engine.RendererDX12/D3DHelpers.h>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

#include <directx-headers/directx/d3d12.h>
#include <directx-headers/directx/d3dx12.h>

#include <pix/pix3.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <directxtk/SimpleMath.h>

#include <directxtex/DirectXTex.h>

//#include <debugdrawer/DebugRenderSysImpl.h>

#include <ffx-api/ffx_api.h>
#include <ffx-api/ffx_upscale.h>
#include <ffx-api/dx12/ffx_api_dx12.h>

#include <filesystem>

static_assert(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS21) > 0, "No D3D12_OPTIONS21 (Work Graphs) in headers");

namespace Engine::RendererDX12
{
	Microsoft::WRL::ComPtr<IDxcCompiler3> mDxcCompiler;
	Microsoft::WRL::ComPtr<IDxcUtils> mDxcUtils;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> mDxcIncludeHandler;
	D3D_SHADER_MODEL MaxSupportedShaderModel;
	std::wstring mAdapterName;

	void InitializeDXC(ID3D12GraphicsCommandList* cmdList)
	{
		if (cmdList)
		{
			PIXBeginEvent(cmdList, 0x00FF00, "Initialize DXC");
		}

		ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&mDxcUtils)));
		ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&mDxcCompiler)));
		ThrowIfFailed(mDxcUtils->CreateDefaultIncludeHandler(&mDxcIncludeHandler));

		if (cmdList)
		{
			PIXEndEvent(cmdList);
		}

		DirectX::SimpleMath::Vector3 a(1, 2, 3);
		auto b = a.Length();

		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;

		DirectX::ScratchImage img;
		auto hr = img.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
		ThrowIfFailed(hr);

		const auto& texMeta = img.GetMetadata();
		if (texMeta.width == 1 && texMeta.height == 1 && texMeta.format == DXGI_FORMAT_R8G8B8A8_UNORM)
		{
			OutputDebugStringA("DirectXTex ScratchImage::Initialize2D OK");
		}

		// The debugdrawer cannot be tested yet because the engine lacks the necessary functionality.
		// As development progresses, the debugdrawer will be rewritten to match the new functionality

		// FSR
		ffxContext mFFXContext;

		ffxCreateBackendDX12Desc backendDesc = {};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = nullptr;

		ffxCreateContextDescUpscale upscaleDesc = {};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.header.pNext = &backendDesc.header;
		upscaleDesc.maxRenderSize = { static_cast<uint32_t>(1920), static_cast<uint32_t>(1080) };
		upscaleDesc.maxUpscaleSize = { static_cast<uint32_t>(1920), static_cast<uint32_t>(1080) };
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
		upscaleDesc.fpMessage = [](uint32_t type, const wchar_t* message)
			{
				std::wstring wideMessage(message);
				std::string narrowMessage(wideMessage.begin(), wideMessage.end());

				std::string prefix;
				switch (type) {
				case FFX_API_MESSAGE_TYPE_ERROR:
					prefix = "[FFX ERROR] ";
					break;
				case FFX_API_MESSAGE_TYPE_WARNING:
					prefix = "[FFX WARNING] ";
					break;
				default:
					prefix = "[FFX DEBUG] ";
					break;
				}

				std::string fullMessage = prefix + narrowMessage + "\n";
				OutputDebugStringA(fullMessage.c_str());
			};

		// It crashes now because there is no device
		/*ffxReturnCode_t errorCode = ffxCreateContext(&mFFXContext, &upscaleDesc.header, nullptr);
		if (errorCode != FFX_API_RETURN_OK)
		{
			std::string errorMsg = "ERROR: FSR3 CONTEXT NOT CREATED\n";
			OutputDebugStringA(errorMsg.c_str());
		}*/
	}

	void ShutdownDXC()
	{

	}

	void CheckAssimp()
	{
		Assimp::Importer Importer;

		std::filesystem::path ModelPath = MODELS_FOLDER;
		ModelPath /= "african_head.obj";
		const aiScene* Scene = Importer.ReadFile(ModelPath.string(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

		if (!Scene)
		{
			OutputDebugStringA(Importer.GetErrorString());
		}
		else
		{
			OutputDebugStringA("Scene was loaded!");
		}
	}
}
