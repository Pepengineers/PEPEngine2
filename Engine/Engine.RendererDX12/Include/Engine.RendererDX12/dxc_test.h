// dxc_test.h

#pragma once

#include <wrl/client.h>
#include <string>
#include <directx-headers/directx/d3d12.h>

struct IDxcCompiler3;
struct IDxcUtils;
struct IDxcIncludeHandler;

namespace Engine::RendererDX12
{
	extern Microsoft::WRL::ComPtr<IDxcCompiler3> gDxcCompiler;
	extern Microsoft::WRL::ComPtr<IDxcUtils> gDxcUtils;
	extern Microsoft::WRL::ComPtr<IDxcIncludeHandler> gDxcIncludeHandler;
	extern D3D_SHADER_MODEL gMaxSupportedShaderModel;
	extern std::wstring gAdapterName;

	void InitializeLibraries (ID3D12GraphicsCommandList* cmdList);
	void ShutdownLibraries ();

	void CheckAssimp ();
}
