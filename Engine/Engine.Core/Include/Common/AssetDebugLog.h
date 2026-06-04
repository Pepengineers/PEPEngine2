// AssetDebugLog.h

#pragma once

#include <string>

#ifndef PEP_ENABLE_ASSET_DEBUG_LOGS
#define PEP_ENABLE_ASSET_DEBUG_LOGS 0
#endif

#if PEP_ENABLE_ASSET_DEBUG_LOGS

#include <Windows.h>

inline void PEPWriteAssetDebugLog(const std::wstring& message)
{
	OutputDebugStringW(message.c_str());
}

#define LOG(message) \
do \
{ \
PEPWriteAssetDebugLog((message)); \
} while (false)

#else

#define LOG(message) do { } while (false)

#endif