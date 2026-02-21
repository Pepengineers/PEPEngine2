// dxc_test.cpp

#include <Engine.RendererDX12/dxc_test.h>
#include <Engine.RendererDX12/D3DHelpers.h>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

#include <pix/pix3.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <filesystem>


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