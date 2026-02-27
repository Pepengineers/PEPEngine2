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

#include <nvidia-sdk/sl.h>

#include <directstorage/dstorage.h>

#include <directxmesh/DirectXMesh.h>

#include <wwise/AK/SoundEngine/Common/AkSoundEngine.h>
#include <wwise/AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <wwise/AK/SoundEngine/Common/AkStreamMgrModule.h>

#include <filesystem>

static_assert(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS21) > 0, "No D3D12_OPTIONS21 (Work Graphs) in headers");

namespace Engine::RendererDX12
{
	Microsoft::WRL::ComPtr<IDxcCompiler3> mDxcCompiler;
	Microsoft::WRL::ComPtr<IDxcUtils> mDxcUtils;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> mDxcIncludeHandler;
	D3D_SHADER_MODEL MaxSupportedShaderModel;
	std::wstring mAdapterName;
	static bool gStreamlineInitialized = false;

	void InitializeLibraries(ID3D12GraphicsCommandList* cmdList)
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

		// DirectXTex
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

		// Streamline smoke-test: request one concrete feature and verify it was loaded.
		if (!gStreamlineInitialized)
		{
			const sl::Feature featuresToLoad[] = { sl::kFeatureNIS };

			sl::Preferences pref = {};
			pref.showConsole = true;
			pref.logLevel = sl::LogLevel::eVerbose;
			pref.featuresToLoad = featuresToLoad;
			pref.numFeaturesToLoad = static_cast<uint32_t>(sizeof(featuresToLoad) / sizeof(featuresToLoad[0]));
			pref.renderAPI = sl::RenderAPI::eD3D12;
			pref.engine = sl::EngineType::eCustom;
			pref.engineVersion = "0.1.0";

			sl::Result res = slInit(pref);
			if (res != sl::Result::eOk)
			{
				std::string message = "FAILED: slInit() = " + std::to_string((int)res) + "\n";
				OutputDebugStringA(message.c_str());
			}
			else
			{
				gStreamlineInitialized = true;
				OutputDebugStringA("OK: Streamline initialized successfully\n");

				bool isNisLoaded = false;
				sl::Result loadedResult = slIsFeatureLoaded(sl::kFeatureNIS, isNisLoaded);
				if (loadedResult == sl::Result::eOk && isNisLoaded)
				{
					OutputDebugStringA("OK: Streamline feature loaded: kFeatureNIS\n");

					sl::FeatureVersion nisVersion = {};
					sl::Result versionResult = slGetFeatureVersion(sl::kFeatureNIS, nisVersion);
					if (versionResult == sl::Result::eOk)
					{
						std::string versionMessage = "OK: kFeatureNIS SL version = " + nisVersion.versionSL.toStr() + "\n";
						OutputDebugStringA(versionMessage.c_str());
					}
				}
				else
				{
					std::string message = "FAILED: slIsFeatureLoaded(kFeatureNIS) = " + std::to_string((int)loadedResult) + "\n";
					OutputDebugStringA(message.c_str());
				}
			}
		}

		// DirectStorage
		IDStorageFactory* factory = nullptr;
		HRESULT hr2 = DStorageGetFactory(IID_PPV_ARGS(&factory));
		if (SUCCEEDED(hr2) && factory)
		{
			OutputDebugStringA("DiretStorage success");
			factory->Release();
		}

		// DirectXMesh
		const uint32_t indices[3] = { 0u, 1u, 2u };
		const DirectX::XMFLOAT3 positions[3] =
		{
			{ 0.0f, 0.0f, 0.0f },
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }
		};
		DirectX::XMFLOAT3 normals[3] = {};

		const HRESULT hrMesh = DirectX::ComputeNormals(
			indices,
			1, // nFaces
			positions,
			3, // nVerts
			DirectX::CNORM_DEFAULT,
			normals);

		const auto isAlmost = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

		if (SUCCEEDED(hrMesh)
			&& isAlmost(normals[0].x, 0.0f) && isAlmost(normals[0].y, 0.0f) && normals[0].z > 0.99f
			&& isAlmost(normals[1].x, 0.0f) && isAlmost(normals[1].y, 0.0f) && normals[1].z > 0.99f
			&& isAlmost(normals[2].x, 0.0f) && isAlmost(normals[2].y, 0.0f) && normals[2].z > 0.99f)
		{
			OutputDebugStringA("DirectXMesh test OK: ComputeNormals\n");
		}
		else
		{
			std::string msg = "DirectXMesh test FAILED, hr=" + std::to_string(static_cast<long>(hrMesh)) + "\n";
			OutputDebugStringA(msg.c_str());
		}

		// Wwise smoke-test
		AkMemSettings memSettings = {};
		AK::MemoryMgr::GetDefaultSettings(memSettings);
		AKRESULT akResult = AK::MemoryMgr::Init(&memSettings);
		if (akResult != AK_Success)
		{
			std::string msg = "Wwise FAILED: MemoryMgr::Init, code=" + std::to_string(static_cast<int>(akResult)) + "\n";
			OutputDebugStringA(msg.c_str());
		}
		else
		{
			AkStreamMgrSettings stmSettings = {};
			AK::StreamMgr::GetDefaultSettings(stmSettings);
			AK::IAkStreamMgr* pStreamMgr = AK::StreamMgr::Create(stmSettings);
			if (!pStreamMgr)
			{
				OutputDebugStringA("Wwise FAILED: StreamMgr::Create returned nullptr\n");
				AK::MemoryMgr::Term();
			}
			else
			{
				AkInitSettings initSettings = {};
				AkPlatformInitSettings platformInitSettings = {};
				AK::SoundEngine::GetDefaultInitSettings(initSettings);
				AK::SoundEngine::GetDefaultPlatformInitSettings(platformInitSettings);

				akResult = AK::SoundEngine::Init(&initSettings, &platformInitSettings);
				if (akResult == AK_Success)
				{
					OutputDebugStringA("Wwise smoke-test OK: MemoryMgr/StreamMgr/SoundEngine initialized\n");
					AK::SoundEngine::Term();
				}
				else
				{
					std::string msg = "Wwise FAILED: SoundEngine::Init, code=" + std::to_string(static_cast<int>(akResult)) + "\n";
					OutputDebugStringA(msg.c_str());
				}

				pStreamMgr->Destroy();
				AK::MemoryMgr::Term();
			}
		}
	}

	void ShutdownLibraries()
	{
		if (gStreamlineInitialized)
		{
			sl::Result res = slShutdown();
			if (res != sl::Result::eOk)
			{
				std::string message = "FAILED: slShutdown() = " + std::to_string((int)res) + "\n";
				OutputDebugStringA(message.c_str());
			}
			else
			{
				OutputDebugStringA("OK: Streamline shutdown\n");
			}

			gStreamlineInitialized = false;
		}
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
