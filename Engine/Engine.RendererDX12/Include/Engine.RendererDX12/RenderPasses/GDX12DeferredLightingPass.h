#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12DeferredLightingPass final : public GDX12RenderPass
{
public:
    GDX12DeferredLightingPass()
    {
        _flags = RENDER_PASS_FLAG_USE_CAMERAS;
        _numInputs = 12;
        _numOutputs = 1;
        _color = std::make_unique<GDX12Texture>();
        _outputs.push_back(_color.get());
    }
    void Initialize() override
    {
        GDX12TextureDesc texture;
        texture.Width = _inputs[0]->GetTexture()->GetWidth();
        texture.Height = _inputs[0]->GetTexture()->GetHeight();
        texture.Format = texture.SRVDesc.Format = texture.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.CreateUAV = true;
        texture.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        texture.UAVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        texture.SRVDesc.Texture2D.MipLevels = 1;
        texture.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _color->Initialize(texture);
        GDX12RootSignatureDesc root;
        root.NumSingleCBVSlots = 3;
        root.NumSingleSRVSlots = 15;
