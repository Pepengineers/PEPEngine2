#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "../../../External/External.Libraries/XeGTAO/XeGTAO.h"
#define FFX_CACAO_ENABLE_D3D12
#include "../../../External/External.Libraries/cacao/inc/ffx_cacao_impl.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace
{
    void CheckCACAO(const FFX_CACAO_Status status)
    {
        if (status != FFX_CACAO_STATUS_OK)
            throw std::runtime_error("FidelityFX CACAO failed: " + std::to_string(status));
    }

}

GDX12AmbientOcclusionPass::GDX12AmbientOcclusionPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 2; // reverse-Z depth and world-space shading normals
    _numOutputs = 1;
    _outputs.push_back(&_output);
}

GDX12AmbientOcclusionPass::~GDX12AmbientOcclusionPass()
{
    for (auto& grid : _grids) DestroyCACAO(grid);
}

void GDX12AmbientOcclusionPass::Initialize()
{
    CreateTextures();
    const UINT frames = static_cast<UINT>(_resources->FrameConstants.size());
    _constants = std::make_unique<GDX12UploadBuffer<ShaderConstants>>(_resources->Device, frames);
    _xeConstants = std::make_unique<GDX12UploadBuffer<XeGTAO::GTAOConstants>>(_resources->Device, frames);
    static_assert(sizeof(ShaderConstants) == 64);
}

GDX12AmbientOcclusionPass::Pipelines::Pipelines(GDX12Device* device)
{
    GDX12RootSignatureDesc root;
    root.Constants.push_back(8);
    root.NumSingleCBVSlots = 3;
    root.NumSingleSRVSlots = 9;
    root.NumSingleUAVSlots = 10;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    Root = std::make_unique<GDX12RootSignature>(device, root);
    const char* entries[PipelineCount] = { "PrepareNormalsCS", "SSAOCS", "HBAOCS", "BlurCS", "ResolveCS",
        "XePrefilterCS", "XeMainCS", "XeDenoiseCS", "NeutralCS", "PrepareHalfCS", "UpsampleCS",
        "PadCACAOCS", "ReprojectCS", "PackFullCS", "ReprojectXeCS" };
    for (UINT i = 0; i < PipelineCount; ++i)
    {
        States[i] = GDX12RenderHelpers::CreateComputePipeline(device, Root.get(),
            SHADERS_FOLDER "AmbientOcclusion.hlsl", entries[i]);
    }
}

void GDX12AmbientOcclusionPass::CreateTexture(GDX12Texture& texture, const DXGI_FORMAT format, const UINT width, const UINT height, const UINT mips)
{
    auto* heap = _resources->SRV_UAV_Heap.get();
    GDX12TextureDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = format;
    desc.Semantic = ETextureSemantic::Data;
    desc.CreateUAV = true;
    desc.SRV_UAV_Heap = heap;
    desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex : heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.UAVHeapIndex = texture.IsInitialized() ? texture.GetUAV()->HeapIndex : heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.SRVDesc.Texture2D.MipLevels = mips;
    desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    const auto allocation = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1,
        static_cast<UINT16>(mips), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    desc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, allocation,
        D3D12_RESOURCE_STATE_COMMON, nullptr);
    texture.Initialize(desc);
}

void GDX12AmbientOcclusionPass::CreateTextures()
{
    const UINT width = _inputs[0]->GetTexture()->GetWidth(), height = _inputs[0]->GetTexture()->GetHeight();
    if (width == 0 || height == 0 || _inputs[1]->GetTexture()->GetWidth() != width || _inputs[1]->GetTexture()->GetHeight() != height)
        throw std::invalid_argument("AO requires equally sized, nonempty depth and normal textures");
    CreateTexture(_output, DXGI_FORMAT_R32_FLOAT, width, height);
    CreateGrid(_grids[0], width, height, false);
    if (_grids[1].Normals.IsInitialized())
        CreateGrid(_grids[1], (width + 1) / 2, (height + 1) / 2, true);
}

void GDX12AmbientOcclusionPass::CreateGrid(WorkGrid& grid, UINT width, UINT height, bool half)
{
    if (half)
    {
        CreateTexture(grid.Depth, DXGI_FORMAT_R32_FLOAT, width, height);
        CreateTexture(grid.Visibility, DXGI_FORMAT_R32_FLOAT, width, height);
    }
    CreateTexture(grid.Normals, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    for (auto& texture : grid.Scratch) CreateTexture(texture, DXGI_FORMAT_R32_FLOAT, width, height);
    for (auto& texture : grid.XeTerms) CreateTexture(texture, DXGI_FORMAT_R8_UINT, width, height);
    CreateTexture(grid.XeEdges, DXGI_FORMAT_R8_UNORM, width, height);
    // Every prefilter group writes a complete 16x16 tile through five mips.
    CreateTexture(grid.XeDepth, DXGI_FORMAT_R32_FLOAT, (width + 15) / 16 * 16, (height + 15) / 16 * 16, 5);
    if ((std::max)(width, height) < 16)
    {
        // The native CACAO deinterleaved depth needs four mips, so extend one axis.
        CreateTexture(grid.CacaoDepth, DXGI_FORMAT_R32_FLOAT, 16, height);
        CreateTexture(grid.CacaoNormals, DXGI_FORMAT_R16G16B16A16_FLOAT, 16, height);
        CreateTexture(grid.CacaoVisibility, DXGI_FORMAT_R32_FLOAT, 16, height);
    }
    for (UINT mip = 0; mip < 5; ++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
        auto* heap = _resources->SRV_UAV_Heap.get();
        const UINT index = grid.DepthUAVs[mip].DescType != DESC_TYPE_NONE ? grid.DepthUAVs[mip].HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        grid.DepthUAVs[mip].InitAsUAV(grid.XeDepth.GetResource()->D3DResource.Get(), nullptr, &view, heap, index);
    }
}

void GDX12AmbientOcclusionPass::DestroyCACAO(WorkGrid& grid)
{
    if (grid.CacaoScreenReady) FFX_CACAO_D3D12DestroyScreenSizeDependentResources(grid.Cacao);
    if (grid.Cacao) FFX_CACAO_D3D12DestroyContext(grid.Cacao);
    grid.CacaoScreenReady = false;
    grid.Cacao = nullptr;
    grid.CacaoMemory.clear();
}

void GDX12AmbientOcclusionPass::Resize()
{
    // Renderer drains the queue before replacing size-dependent resources.
    for (auto& grid : _grids)
    {
        if (grid.CacaoScreenReady) CheckCACAO(FFX_CACAO_D3D12DestroyScreenSizeDependentResources(grid.Cacao));
        grid.CacaoScreenReady = false;
    }
    CreateTextures();
    _hasPrevious = false;
}

void GDX12AmbientOcclusionPass::ClearDenendencies()
{
    for (auto& grid : _grids) DestroyCACAO(grid);
    GDX12RenderPass::ClearDenendencies();
}
