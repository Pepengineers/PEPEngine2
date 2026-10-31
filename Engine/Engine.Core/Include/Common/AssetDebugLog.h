// AssetDebugLog.h

#pragma once

#include <string>

#ifndef PEP_ENABLE_ASSET_DEBUG_LOGS
#define PEP_ENABLE_ASSET_DEBUG_LOGS 0
#endif

#if PEP_ENABLE_ASSET_DEBUG_LOGS

#include "Engine.Core/Logging/Logger.h"

inline void PEPWriteAssetDebugLog(const std::wstring& message)
{
	Log::Info("AssetDebug", message);
}

#define LOG(message) \
do \
{ \
PEPWriteAssetDebugLog((message)); \
} while (false)

#else

#define LOG(message) do { } while (false)

#endif
