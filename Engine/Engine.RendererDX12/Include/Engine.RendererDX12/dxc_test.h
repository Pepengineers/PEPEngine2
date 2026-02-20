// dxc_test.h

#pragma once

#include <wrl/client.h>
#include <string>
#include <d3d12.h>

struct IDxcCompiler3;
struct IDxcUtils;
struct IDxcIncludeHandler;

namespace Engine::RendererDX12
{
    extern Microsoft::WRL::ComPtr<IDxcCompiler3> mDxcCompiler;
    extern Microsoft::WRL::ComPtr<IDxcUtils> mDxcUtils;
    extern Microsoft::WRL::ComPtr<IDxcIncludeHandler> mDxcIncludeHandler;
    extern D3D_SHADER_MODEL MaxSupportedShaderModel;
    extern std::wstring mAdapterName;

    void InitializeDXC();
    void ShutdownDXC();
}