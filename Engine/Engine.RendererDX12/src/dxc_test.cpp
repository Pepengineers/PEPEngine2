// dxc_test.cpp

#include <Engine.RendererDX12/dxc_test.h>
#include <Engine.RendererDX12/D3DHelpers.h>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

namespace Engine::RendererDX12
{
	Microsoft::WRL::ComPtr<IDxcCompiler3> mDxcCompiler;
	Microsoft::WRL::ComPtr<IDxcUtils> mDxcUtils;
	Microsoft::WRL::ComPtr<IDxcIncludeHandler> mDxcIncludeHandler;
	D3D_SHADER_MODEL MaxSupportedShaderModel;
	std::wstring mAdapterName;

	void InitializeDXC()
	{
		ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&mDxcUtils)));
		ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&mDxcCompiler)));
		ThrowIfFailed(mDxcUtils->CreateDefaultIncludeHandler(&mDxcIncludeHandler));
	}

	void ShutdownDXC()
	{

	}
}