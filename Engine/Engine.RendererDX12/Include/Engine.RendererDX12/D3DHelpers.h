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

using namespace DirectX;
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

inline std::wstring AnsiToWString (const std::string& sourceString)
{
	WCHAR Buffer[512];
	MultiByteToWideChar(CP_ACP, 0, sourceString.c_str(), -1, Buffer, 512);
	return std::wstring(Buffer);
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
	std::wstring FileName = AnsiToWString(__FILE__);                  \
	if (FAILED(HrResult))                                             \
	{                                                                 \
		throw DxException(HrResult, L#x, FileName, __LINE__);         \
	}                                                                 \
}
#endif