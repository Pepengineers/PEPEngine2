#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

namespace BistroRender::ShadowTexture
{
inline auto Depth(UINT side, UINT slices, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
{
    return CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, side, side, UINT16(slices), 1, 1, 0, flags);
}

inline auto View(UINT slices)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = slices;
    return view;
}
}
