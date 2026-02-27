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

#include <ffx-api/ffx_api.h>
#include <ffx-api/ffx_upscale.h>
#include <ffx-api/dx12/ffx_api_dx12.h>

#include <nvidia-sdk/sl.h>

#include <directstorage/dstorage.h>
#include <directxmesh/DirectXMesh.h>

#include <wwise/AK/SoundEngine/Common/AkSoundEngine.h>
#include <wwise/AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <wwise/AK/SoundEngine/Common/AkStreamMgrModule.h>

#include <cmath>
#include <filesystem>

static_assert(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS21) > 0, "No D3D12_OPTIONS21 (Work Graphs) in headers");

namespace Engine::RendererDX12
{
	Microsoft::WRL::ComPtr<IDxcCompiler3> gDxcCompiler;
	Microsoft::WRL::ComPtr<IDxcUtils> gDxcUtils;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> gDxcIncludeHandler;
	D3D_SHADER_MODEL gMaxSupportedShaderModel;
	std::wstring gAdapterName;
	static bool gBStreamlineInitialized = false;

	void InitializeLibraries (ID3D12GraphicsCommandList* cmdList)
	{
		if (cmdList)
		{
			PIXBeginEvent(cmdList, 0x00FF00, "Initialize DXC");
		}

		ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&gDxcUtils)));
		ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&gDxcCompiler)));
		ThrowIfFailed(gDxcUtils->CreateDefaultIncludeHandler(&gDxcIncludeHandler));

		if (cmdList)
		{
			PIXEndEvent(cmdList);
		}

		DirectX::SimpleMath::Vector3 VectorA(1, 2, 3);
		float VectorLength = VectorA.Length();
		UNREFERENCED_PARAMETER(VectorLength);

		D3D12_RESOURCE_DESC ResourceDesc = {};
		ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		UNREFERENCED_PARAMETER(ResourceDesc);

		// DirectXTex
		DirectX::ScratchImage TextureImage;
		HRESULT TextureInitResult = TextureImage.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
		ThrowIfFailed(TextureInitResult);

		const auto& TextureMetadata = TextureImage.GetMetadata();
		if (TextureMetadata.width == 1 && TextureMetadata.height == 1 && TextureMetadata.format == DXGI_FORMAT_R8G8B8A8_UNORM)
		{
			OutputDebugStringA("DirectXTex ScratchImage::Initialize2D OK\n");
		}

		// The debugdrawer cannot be tested yet because the engine lacks the necessary functionality.
		// As development progresses, the debugdrawer will be rewritten to match the new functionality

		// FSR
		ffxContext FfxContext = nullptr;
		ffxCreateBackendDX12Desc BackendDesc = {};
		BackendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		BackendDesc.device = nullptr;

		ffxCreateContextDescUpscale UpscaleDesc = {};
		UpscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		UpscaleDesc.header.pNext = &BackendDesc.header;
		UpscaleDesc.maxRenderSize = { static_cast<uint32_t>(1920), static_cast<uint32_t>(1080) };
		UpscaleDesc.maxUpscaleSize = { static_cast<uint32_t>(1920), static_cast<uint32_t>(1080) };
		UpscaleDesc.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
		UpscaleDesc.fpMessage = [](uint32_t type, const wchar_t* message)
		{
			std::wstring WideMessage(message);
			std::string NarrowMessage(WideMessage.begin(), WideMessage.end());

			std::string Prefix;
			switch (type)
			{
			case FFX_API_MESSAGE_TYPE_ERROR:
				Prefix = "[FFX ERROR] ";
				break;
			case FFX_API_MESSAGE_TYPE_WARNING:
				Prefix = "[FFX WARNING] ";
				break;
			default:
				Prefix = "[FFX DEBUG] ";
				break;
			}

			std::string FullMessage = Prefix + NarrowMessage + "\n";
			OutputDebugStringA(FullMessage.c_str());
		};

		// It crashes now because there is no device
		/*ffxReturnCode_t errorCode = ffxCreateContext(&mFFXContext, &upscaleDesc.header, nullptr);
		if (errorCode != FFX_API_RETURN_OK)
		{
			std::string errorMsg = "ERROR: FSR3 CONTEXT NOT CREATED\n";
			OutputDebugStringA(errorMsg.c_str());
		}*/

		// nvidiaSDK
		if (!gBStreamlineInitialized)
		{
			const sl::Feature FeaturesToLoad[] = { sl::kFeatureNIS };

			sl::Preferences Preferences = {};
			Preferences.showConsole = true;
			Preferences.logLevel = sl::LogLevel::eVerbose;
			Preferences.featuresToLoad = FeaturesToLoad;
			Preferences.numFeaturesToLoad = static_cast<uint32_t>(sizeof(FeaturesToLoad) / sizeof(FeaturesToLoad[0]));
			Preferences.renderAPI = sl::RenderAPI::eD3D12;
			Preferences.engine = sl::EngineType::eCustom;
			Preferences.engineVersion = "0.1.0";

			sl::Result InitResult = slInit(Preferences);
			if (InitResult != sl::Result::eOk)
			{
				std::string Message = "FAILED: slInit() = " + std::to_string(static_cast<int>(InitResult)) + "\n";
				OutputDebugStringA(Message.c_str());
			}
			else
			{
				gBStreamlineInitialized = true;
				OutputDebugStringA("OK: Streamline initialized successfully\n");

				bool bIsNisLoaded = false;
				sl::Result IsLoadedResult = slIsFeatureLoaded(sl::kFeatureNIS, bIsNisLoaded);
				if (IsLoadedResult == sl::Result::eOk && bIsNisLoaded)
				{
					OutputDebugStringA("OK: Streamline feature loaded: kFeatureNIS\n");

					sl::FeatureVersion NisVersion = {};
					sl::Result VersionResult = slGetFeatureVersion(sl::kFeatureNIS, NisVersion);
					if (VersionResult == sl::Result::eOk)
					{
						std::string VersionMessage = "OK: kFeatureNIS SL version = " + NisVersion.versionSL.toStr() + "\n";
						OutputDebugStringA(VersionMessage.c_str());
					}
				}
				else
				{
					std::string Message = "FAILED: slIsFeatureLoaded(kFeatureNIS) = " + std::to_string(static_cast<int>(IsLoadedResult)) + "\n";
					OutputDebugStringA(Message.c_str());
				}
			}
		}

		// DirectStorage
		IDStorageFactory* StorageFactory = nullptr;
		HRESULT StorageResult = DStorageGetFactory(IID_PPV_ARGS(&StorageFactory));
		if (SUCCEEDED(StorageResult) && StorageFactory)
		{
			OutputDebugStringA("DirectStorage success\n");
			StorageFactory->Release();
		}

		// DirectXMesh
		const uint32_t Indices[3] = { 0u, 1u, 2u };
		const DirectX::XMFLOAT3 Positions[3] =
		{
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }
		};
		DirectX::XMFLOAT3 Normals[3] = {};

		HRESULT MeshResult = DirectX::ComputeNormals(Indices, 1, Positions, 3, DirectX::CNORM_DEFAULT, Normals);

		auto IsAlmost = [](float leftValue, float rightValue)
		{
			return std::fabs(leftValue - rightValue) < 1e-3f;
		};

		if (SUCCEEDED(MeshResult)
			&& IsAlmost(Normals[0].x, 0.0f) && IsAlmost(Normals[0].y, 0.0f) && Normals[0].z > 0.99f
			&& IsAlmost(Normals[1].x, 0.0f) && IsAlmost(Normals[1].y, 0.0f) && Normals[1].z > 0.99f
			&& IsAlmost(Normals[2].x, 0.0f) && IsAlmost(Normals[2].y, 0.0f) && Normals[2].z > 0.99f)
		{
			OutputDebugStringA("DirectXMesh test OK: ComputeNormals\n");
		}
		else
		{
			std::string Message = "DirectXMesh test FAILED, hr=" + std::to_string(static_cast<long>(MeshResult)) + "\n";
			OutputDebugStringA(Message.c_str());
		}

		// Wwise smoke-test
		AkMemSettings MemorySettings = {};
		AK::MemoryMgr::GetDefaultSettings(MemorySettings);
		AKRESULT WwiseResult = AK::MemoryMgr::Init(&MemorySettings);
		if (WwiseResult != AK_Success)
		{
			std::string Message = "Wwise FAILED: MemoryMgr::Init, code=" + std::to_string(static_cast<int>(WwiseResult)) + "\n";
			OutputDebugStringA(Message.c_str());
		}
		else
		{
			AkStreamMgrSettings StreamSettings = {};
			AK::StreamMgr::GetDefaultSettings(StreamSettings);
			AK::IAkStreamMgr* StreamManager = AK::StreamMgr::Create(StreamSettings);
			if (!StreamManager)
			{
				OutputDebugStringA("Wwise FAILED: StreamMgr::Create returned nullptr\n");
				AK::MemoryMgr::Term();
			}
			else
			{
				AkInitSettings InitSettings = {};
				AkPlatformInitSettings PlatformInitSettings = {};
				AK::SoundEngine::GetDefaultInitSettings(InitSettings);
				AK::SoundEngine::GetDefaultPlatformInitSettings(PlatformInitSettings);

				WwiseResult = AK::SoundEngine::Init(&InitSettings, &PlatformInitSettings);
				if (WwiseResult == AK_Success)
				{
					OutputDebugStringA("Wwise smoke-test OK: MemoryMgr/StreamMgr/SoundEngine initialized\n");
					AK::SoundEngine::Term();
				}
				else
				{
					std::string Message = "Wwise FAILED: SoundEngine::Init, code=" + std::to_string(static_cast<int>(WwiseResult)) + "\n";
					OutputDebugStringA(Message.c_str());
				}

				StreamManager->Destroy();
				AK::MemoryMgr::Term();
			}
		}
	}

	void ShutdownLibraries ()
	{
		if (gBStreamlineInitialized)
		{
			sl::Result ShutdownResult = slShutdown();
			if (ShutdownResult != sl::Result::eOk)
			{
				std::string Message = "FAILED: slShutdown() = " + std::to_string(static_cast<int>(ShutdownResult)) + "\n";
				OutputDebugStringA(Message.c_str());
			}
			else
			{
				OutputDebugStringA("OK: Streamline shutdown\n");
			}

			gBStreamlineInitialized = false;
		}
	}

	void CheckAssimp ()
	{
		Assimp::Importer Importer;

		std::filesystem::path ModelPath = MODELS_FOLDER;
		ModelPath /= "african_head.obj";
		const aiScene* Scene = Importer.ReadFile(
			ModelPath.string(),
			aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace);

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