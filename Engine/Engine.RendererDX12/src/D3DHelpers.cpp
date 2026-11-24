#include <comdef.h>

#include <Engine.RendererDX12/D3DHelpers.h>
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.Core/Logging/Logger.h"
#include <mutex>

namespace
{
    AutoConsoleVariable<UINT> SunShadowSize(L"Render.Shadows.Sun.Resolution", SunShadowCascadeResolution,
        L"Texels per sun cascade: 0 Off, 512, 1024, 2048 or 4096. Applies at the frame boundary.",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || IsSunShadowResolution(value); });
    AutoConsoleVariable<UINT> LocalShadowSize(L"Render.Shadows.Local.Resolution", LocalShadowResolution,
        L"Point and spot shadow face resolution. In Bistro this caps adaptive maps. 0 Off, 128, 256, 384, 512 or 1024.",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || IsLocalShadowResolution(value); });
    AutoConsoleVariable<UINT> SunCascadeCount(L"Render.Shadows.Sun.CascadeCount", 4u,
        L"Number of sun cascades, 1..4. Applies at the frame boundary.",
        EConsoleVariableFlags::Default, [](UINT value) { return value >= 1 && value <= 4; });
    AutoConsoleVariable<float> SunMaxDistance(L"Render.Shadows.Sun.MaxDistance", 6000.0f,
        L"Maximum sun shadow distance in scene units (Bistro: centimetres), also limited by camera far plane.",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0 && value <= 1e9f; });
    AutoConsoleVariable<bool> SunAutomaticSplits(L"Render.Shadows.Sun.AutomaticSplits", true,
        L"Use the original logarithmic/linear cascade distribution. False uses normalized CascadeSplit1..3.");
    AutoConsoleVariable<float> SunSplit1(L"Render.Shadows.Sun.CascadeSplit1", 0.067f,
        L"First cascade boundary as a fraction of the shadow distance, 0..1 exclusive. Used with AutomaticSplits=false.",
        EConsoleVariableFlags::Default, [](float value) { return value > 0 && value < 1; });
    AutoConsoleVariable<float> SunSplit2(L"Render.Shadows.Sun.CascadeSplit2", 0.2f,
        L"Second cascade boundary; requires three or four cascades and Split1 < Split2 < 1.",
        EConsoleVariableFlags::Default, [](float value) { return value > 0 && value < 1; });
    AutoConsoleVariable<float> SunSplit3(L"Render.Shadows.Sun.CascadeSplit3", 0.467f,
        L"Third cascade boundary; requires four cascades and Split2 < Split3 < 1.",
        EConsoleVariableFlags::Default, [](float value) { return value > 0 && value < 1; });
    AutoConsoleVariable<UINT> TextureAnisotropy(L"Render.Textures.Anisotropy", 8u,
        L"Anisotropic material filtering: 2, 4, 8 or 16. Restart required for static samplers.",
        EConsoleVariableFlags::ReadOnly, [](UINT value) { return value == 2 || value == 4 || value == 8 || value == 16; });
}

GDX12ShadowQuality ReadShadowQuality()
{
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    GDX12ShadowQuality result{SunShadowSize.GetValue(), LocalShadowSize.GetValue()};
    auto& cascades = result.SunCascades;
    cascades.Count = SunCascadeCount.GetValue();
    cascades.MaxDistance = SunMaxDistance.GetValue();
    cascades.AutomaticSplits = SunAutomaticSplits.GetValue();
    cascades.Splits = {SunSplit1.GetValue(), SunSplit2.GetValue(), SunSplit3.GetValue()};
    static auto lastValid = [] { std::array<XMFLOAT3, 4> values; values.fill(GDX12SunCascadeSettings{}.Splits); return values; }();
    static GDX12SunCascadeSettings lastRejected;
    static bool warned = false;
    if (!cascades.AutomaticSplits)
    {
        const float splits[] = {cascades.Splits.x, cascades.Splits.y, cascades.Splits.z};
        bool valid = true;
        float previous = 0;
        for (UINT i = 0; i + 1 < cascades.Count; ++i) { valid &= splits[i] > previous && splits[i] < 1; previous = splits[i]; }
        if (valid) { lastValid[cascades.Count - 1] = cascades.Splits; warned = false; }
        else
        {
            if (!warned || !(lastRejected == cascades))
                Log::Warning("Shadows", "Rejected unordered active CSM split boundaries; retaining the last valid splits for ", cascades.Count, " cascades");
            lastRejected = cascades;
            warned = true;
            cascades.Splits = lastValid[cascades.Count - 1];
        }
    }
    else warned = false;
    return result;
}

DxException::DxException (const HRESULT hr, const std::wstring& functionName, const std::wstring& fileName, const int lineNumber)
	: ErrorCode(hr)
	, FunctionName(functionName)
	, FileName(fileName)
	, LineNumber(lineNumber)
{
}

std::wstring DxException::ToString () const
{
	// Get the string description of the error code.
	_com_error Error(ErrorCode);
	std::wstring ErrorMessage = Error.ErrorMessage();

	return FunctionName + L" failed in " + FileName + L"; line " + std::to_wstring(LineNumber) + L"; error: " + ErrorMessage;
}

std::vector<CD3DX12_STATIC_SAMPLER_DESC> GetStaticSamplers()
{
    const UINT anisotropy = TextureAnisotropy.GetValue();
    CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, // mipLODBias     
        anisotropy);   // maxAnisotropy
    CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f,
        anisotropy);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp };
}

DXGI_FORMAT FormatToSRGB(const DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM:     return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R8_UNORM:           return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_UNORM:         return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_BC1_UNORM:          return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM:          return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM:          return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_BC7_UNORM:          return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
    {
        OutputDebugStringA("WARNING: Unable to convert format into it's SRGB version. Returning original format\n");
        return format;
    }
    }
}

