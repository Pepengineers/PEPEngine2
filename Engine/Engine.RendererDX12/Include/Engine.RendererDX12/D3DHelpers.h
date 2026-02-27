#pragma once

#include <windows.h>
#include <DirectXMath.h>
#include <string>

static DirectX::XMFLOAT4X4 Identity4x4 ()
{
	static DirectX::XMFLOAT4X4 IdentityMatrix(
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

#ifndef ReleaseCom
#define ReleaseCom(x)                  \
{                                      \
	if (x)                             \
	{                                  \
		x->Release();                  \
		x = 0;                         \
	}                                  \
}
#endif