#pragma once

#include <windows.h>
#include <DirectXMath.h>
#include <string>
#include <wrl/client.h>
#include <directx-headers/directx/d3d12.h>
#include <directx-headers/directx/d3dx12.h>
#include <directstorage/dstorage.h>
#include <directxmesh/DirectXMesh.h>
#include <dxgi1_6.h>
#include <D3Dcompiler.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <directxtex/DirectXTex.h>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <cassert>
#include "directxtk/SimpleMath.h"

using namespace DirectX;
using namespace DirectX::SimpleMath;
using Microsoft::WRL::ComPtr;

static XMFLOAT4X4 Identity4x4 ()
{
	static XMFLOAT4X4 IdentityMatrix(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);

	return IdentityMatrix;
}

inline std::wstring StringToWString (const std::string& sourceString)
{
	WCHAR Buffer[512];
	MultiByteToWideChar(CP_ACP, 0, sourceString.c_str(), -1, Buffer, 512);
	return std::wstring(Buffer);
}

inline std::string WStringToString(const std::wstring& sourceString)
{
    CHAR Buffer[512];
    WideCharToMultiByte(CP_ACP, 0, sourceString.c_str(), -1, Buffer, 512, NULL, NULL);
    return std::string(Buffer);
}

class DxException
{
public:
	DxException () = default;
	DxException (HRESULT hr, const std::wstring& functionName, const std::wstring& fileName, int lineNumber);

	std::wstring ToString () const;

	HRESULT ErrorCode = S_OK;
	std::wstring FunctionName;
	std::wstring FileName;
	int LineNumber = -1;
};

#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                              \
{                                                                     \
	HRESULT HrResult = (x);                                           \
	std::wstring FileName = StringToWString(__FILE__);                  \
	if (FAILED(HrResult))                                             \
	{                                                                 \
		throw DxException(HrResult, L#x, FileName, __LINE__);         \
	}                                                                 \
}
#endif

inline std::string FeatureLevelToString(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
    case D3D_FEATURE_LEVEL_12_2: return "12.2";
    case D3D_FEATURE_LEVEL_12_1: return "12.1";
    case D3D_FEATURE_LEVEL_12_0: return "12.0";
    case D3D_FEATURE_LEVEL_11_1: return "11.1";
    case D3D_FEATURE_LEVEL_11_0: return "11.0";
    case D3D_FEATURE_LEVEL_10_1: return "10.1";
    case D3D_FEATURE_LEVEL_10_0: return "10.0";
    case D3D_FEATURE_LEVEL_9_3: return "9.3";
    case D3D_FEATURE_LEVEL_9_2: return "9.2";
    case D3D_FEATURE_LEVEL_9_1: return "9.1";
    case D3D_FEATURE_LEVEL_1_0_CORE: return "1.0 Core";
    default: return "Unknown";
    }
}

inline std::string ShaderModelToString(D3D_SHADER_MODEL model)
{
    switch (model)
    {
    case D3D_SHADER_MODEL_6_10: return "6.10";
    case D3D_SHADER_MODEL_6_9: return "6.9";
    case D3D_SHADER_MODEL_6_8: return "6.8";
    case D3D_SHADER_MODEL_6_7: return "6.7";
    case D3D_SHADER_MODEL_6_6: return "6.6";
    case D3D_SHADER_MODEL_6_5: return "6.5";
    case D3D_SHADER_MODEL_6_4: return "6.4";
    case D3D_SHADER_MODEL_6_3: return "6.3";
    case D3D_SHADER_MODEL_6_2: return "6.2";
    case D3D_SHADER_MODEL_6_1: return "6.1";
    case D3D_SHADER_MODEL_6_0: return "6.0";
    case D3D_SHADER_MODEL_5_1: return "5.1";
    default: return "Unknown";
    }
}

std::vector<CD3DX12_STATIC_SAMPLER_DESC> GetStaticSamplers();
DXGI_FORMAT FormatToSRGB(DXGI_FORMAT format);