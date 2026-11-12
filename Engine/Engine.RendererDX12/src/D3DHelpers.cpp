#include <comdef.h>

#include <Engine.RendererDX12/D3DHelpers.h>
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <map>
#include <memory>

namespace
{
    AutoConsoleVariable<bool> ShadowPolicyEnabled(L"Render.Shadows.Policy.Enabled", false,
        L"Apply saved independent shadow placement and update intervals");
    struct ShadowPolicyVariables
    {
        AutoConsoleVariable<bool> Secondary, Latest;
        AutoConsoleVariable<UINT> Interval;
        explicit ShadowPolicyVariables(const std::wstring& prefix)
            : Secondary(prefix + L".Secondary", false, L"Execute this shadow on the secondary adapter"),
              Latest(prefix + L".Latest", false, L"Use the latest completed shadow snapshot"),
              Interval(prefix + L".Interval", 1, L"Frames between shadow updates, 1..4", EConsoleVariableFlags::Default,
                  [](UINT value) { return value >= 1 && value <= 4; }) {}
        GDX12ShadowUpdatePolicy Read() const { return { Secondary.GetValue(), Latest.GetValue(), Interval.GetValue() }; }
    };
    ShadowPolicyVariables SunPolicy(L"Render.Shadows.Policy.Sun");
    std::map<UINT, std::unique_ptr<ShadowPolicyVariables>>& LocalPolicies()
    {
        static std::map<UINT, std::unique_ptr<ShadowPolicyVariables>> policies;
        return policies;
    }
    AutoConsoleVariable<UINT> SunShadowSize(L"Render.Shadows.Sun.Resolution", SunShadowCascadeResolution,
        L"Texels per sun cascade: 0 Off, 512, 1024, 2048 or 4096. Applies at the frame boundary.",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || IsSunShadowResolution(value); });
    AutoConsoleVariable<UINT> LocalShadowSize(L"Render.Shadows.Local.Resolution", LocalShadowResolution,
        L"Texels per point or spot shadow face: 0 Off, 128, 256, 384, 512 or 1024. Applies at the frame boundary.",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || IsLocalShadowResolution(value); });
    AutoConsoleVariable<UINT> TextureAnisotropy(L"Render.Textures.Anisotropy", 8u,
        L"Anisotropic material filtering: 2, 4, 8 or 16. Restart required for static samplers.",
        EConsoleVariableFlags::ReadOnly, [](UINT value) { return value == 2 || value == 4 || value == 8 || value == 16; });
}

bool IsShadowPolicyEnabled() { return ShadowPolicyEnabled.GetValue(); }

void EnsureShadowUpdatePolicies(const std::vector<GDX12LocalLightConstants>& lights)
{
    auto& policies = LocalPolicies();
    for (const auto& light : lights)
        if (light.EntityId && policies.find(light.EntityId) == policies.end())
            policies.emplace(light.EntityId, std::make_unique<ShadowPolicyVariables>(
                L"Render.Shadows.Policy.Local." + std::to_wstring(light.EntityId)));
}

GDX12ShadowUpdatePolicy ReadShadowUpdatePolicy(UINT entity)
{
    if (!IsShadowPolicyEnabled()) return {};
    if (entity == 0) return SunPolicy.Read();
    const auto& policies = LocalPolicies();
    const auto found = policies.find(entity);
    return found == policies.end() ? GDX12ShadowUpdatePolicy{} : found->second->Read();
}

GDX12ShadowQuality ReadShadowQuality()
{
    return { SunShadowSize.GetValue(), LocalShadowSize.GetValue() };
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

