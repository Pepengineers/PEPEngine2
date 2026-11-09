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

    // Preserve the original pixel rays when extending the viewport to the right or bottom.
    void ExpandProjection(Matrix& projection, float xScale, float yScale)
    {
        projection._11 *= xScale;
        projection._22 *= yScale;
        projection._31 = xScale * projection._31 + (1.0f - xScale);
        projection._32 = yScale * projection._32 - (1.0f - yScale);
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
    GDX12RootSignatureDesc root;
    root.Constants.push_back(4);
    root.NumSingleCBVSlots = 3;
    root.NumSingleSRVSlots = 7;
    root.NumSingleUAVSlots = 10;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const char* entries[PipelineCount] = { "PrepareNormalsCS", "SSAOCS", "HBAOCS", "BlurCS", "ResolveCS",
        "XePrefilterCS", "XeMainCS", "XeDenoiseCS", "NeutralCS", "PrepareHalfCS", "UpsampleCS", "PadCACAOCS" };
    for (UINT i = 0; i < PipelineCount; ++i)
    {
        _pipelines[i] = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
            SHADERS_FOLDER "AmbientOcclusion.hlsl", entries[i]);
    }
    static_assert(sizeof(ShaderConstants) == 64);
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

void GDX12AmbientOcclusionPass::Bind(GDX12CommandList* commands)
{
    const UINT frame = _resources->CurrFrameConstantsIndex;
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_root.get());
    struct { UINT Axis, Final; float XeScale; UINT Padding; } parameters = { 0, 0, 1.0f, 0 };
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, &parameters, 0);
    commands->SetComputeRootConstantBufferView(1, _resources->FrameConstants[frame]->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetComputeRootConstantBufferView(2, _constants->GetElementAddress(frame));
    commands->SetComputeRootConstantBufferView(3, _xeConstants->GetElementAddress(frame));
}

void GDX12AmbientOcclusionPass::Dispatch(GDX12CommandList* commands, const Pipeline pipeline, const UINT width, const UINT height)
{
    commands->SetPipelineState(_pipelines[pipeline]);
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void GDX12AmbientOcclusionPass::Execute(GDX12CommandList* commands)
{
    const auto settings = GetAmbientOcclusionSettings();
    // All fields are four-byte scalars; no padding participates in this comparison.
    static_assert(sizeof(GDX12AOSettings) == 24 * 4);
    if (!_hasPrevious || std::memcmp(&_previous, &settings, sizeof(settings)) != 0)
        _commonData->ResetUpscalerHistory = true;
    _previous = settings;
    _hasPrevious = true;
    GDX12_SCOPED_EVENT(commands, "Ambient occlusion", Colors::DarkSlateBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    const bool half = settings.Resolution == GDX12AOResolution::Half;
    auto& grid = _grids[half];
    const UINT width = half ? (_output.GetWidth() + 1) / 2 : _output.GetWidth();
    const UINT height = half ? (_output.GetHeight() + 1) / 2 : _output.GetHeight();
    if (!settings.IsActive() || (width == 1 && height == 1))
    {
        commands->ResourceBarrier({ _output.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeRootSignature(_root.get());
        commands->SetComputeUAV(0, _output.GetUAV()->GPUHandle);
        Dispatch(commands, Neutral, _output.GetWidth(), _output.GetHeight());
        commands->ResourceBarrier({ _output.GetResource()->GetNonPixelShaderResourceBarrier() });
        return;
    }
    // First Half use writes new descriptor slots. Full and already queued frames keep theirs.
    if (!grid.Normals.IsInitialized())
        CreateGrid(grid, width, height, half);

    auto& fullDepth = *_inputs[0]->GetTexture();
    auto& fullNormals = *_inputs[1]->GetTexture();
    auto& depth = half ? grid.Depth : fullDepth;
    const UINT frame = _resources->CurrFrameConstantsIndex;
    ShaderConstants constants = { settings.RadiusCm, settings.Intensity, settings.BiasCm, settings.HorizonBias,
        settings.FadeStartCm, settings.FadeEndCm, settings.Sharpness, static_cast<UINT>(settings.Samples),
        static_cast<UINT>(settings.Directions), static_cast<UINT>(settings.Steps), static_cast<UINT>(settings.DenoisePasses),
        static_cast<UINT>(settings.Implementation), static_cast<float>(settings.GTAOSlices), static_cast<float>(settings.GTAOSteps), half, 0 };
    _constants->CopyData(frame, constants);
    Matrix projection = _commonData->CameraViewToClip;
    projection._31 -= _commonData->ActiveCameraJitterOffsetX * 2.0f / _output.GetWidth();
    projection._32 += _commonData->ActiveCameraJitterOffsetY * 2.0f / _output.GetHeight();
    if (half)
        ExpandProjection(projection, _output.GetWidth() / (2.0f * width), _output.GetHeight() / (2.0f * height));
    XeGTAO::GTAOSettings gtao;
    gtao.Radius = settings.RadiusCm;
    gtao.DenoisePasses = settings.DenoisePasses;
    gtao.FalloffRange = settings.GTAOFalloff;
    gtao.ThinOccluderCompensation = settings.GTAOThinOccluder;
    XeGTAO::GTAOConstants xe = {};
    XeGTAO::GTAOUpdateConstants(xe, width, height, gtao, &projection._11, true,
        _commonData->Upscaler ? _frameIndex++ : 0);
    xe.NDCToViewAdd.x += projection._31 / projection._11;
    xe.NDCToViewAdd.y += projection._32 / projection._22;
    xe.DenoiseBlurBeta = settings.DenoisePasses ? 0.2f + settings.Sharpness * 1.0f : 1e4f;
    _xeConstants->CopyData(frame, xe);
    commands->ResourceBarrier({ fullDepth.GetResource()->GetNonPixelShaderResourceBarrier(),
        fullNormals.GetResource()->GetNonPixelShaderResourceBarrier(), grid.Normals.GetResource()->GetUnorderedAccessBarrier() });
    Bind(commands);
    commands->SetComputeSRV(0, fullDepth.GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, fullNormals.GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, grid.Normals.GetUAV()->GPUHandle);
    if (half)
    {
        commands->ResourceBarrier({ depth.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(9, depth.GetUAV()->GPUHandle);
    }
    Dispatch(commands, half ? PrepareHalf : PrepareNormals, width, height);
    commands->ResourceBarrier({ depth.GetResource()->GetNonPixelShaderResourceBarrier(),
        grid.Normals.GetResource()->GetNonPixelShaderResourceBarrier() });
    commands->SetComputeSRV(0, depth.GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, grid.Normals.GetSRV()->GPUHandle);

    UINT source = 0;
    GDX12Texture* visibility = nullptr;
    if (settings.Implementation == 3)
    {
        visibility = &DrawCACAO(commands, grid, depth, settings, projection);
        Bind(commands);
        commands->SetComputeSRV(0, depth.GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, grid.Normals.GetSRV()->GPUHandle);
    }
    else if (settings.Implementation == 2)
    {
        commands->ResourceBarrier({ grid.XeDepth.GetResource()->GetUnorderedAccessBarrier() });
        for (UINT mip = 0; mip < 5; ++mip) commands->SetComputeUAV(2 + mip, grid.DepthUAVs[mip].GPUHandle);
        Dispatch(commands, XePrefilter, grid.XeDepth.GetWidth() / 2, grid.XeDepth.GetHeight() / 2);
        commands->ResourceBarrier({ grid.XeDepth.GetResource()->GetNonPixelShaderResourceBarrier(),
            grid.XeTerms[0].GetResource()->GetUnorderedAccessBarrier(), grid.XeEdges.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(2, grid.XeDepth.GetSRV()->GPUHandle);
        commands->SetComputeUAV(7, grid.XeTerms[0].GetUAV()->GPUHandle);
        commands->SetComputeUAV(8, grid.XeEdges.GetUAV()->GPUHandle);
        Dispatch(commands, XeMain, width, height);
        commands->ResourceBarrier({ grid.XeTerms[0].GetResource()->GetNonPixelShaderResourceBarrier(), grid.XeEdges.GetResource()->GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(4, grid.XeEdges.GetSRV()->GPUHandle);
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            commands->ResourceBarrier({ grid.XeTerms[target].GetResource()->GetUnorderedAccessBarrier() });
            commands->SetComputeSRV(3, grid.XeTerms[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(7, grid.XeTerms[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass + 1 == settings.DenoisePasses, 1);
            Dispatch(commands, XeDenoise, (width + 1) / 2, height);
            commands->ResourceBarrier({ grid.XeTerms[target].GetResource()->GetNonPixelShaderResourceBarrier() });
            source = target;
        }
        const float scale = settings.DenoisePasses ? 1.0f : XE_GTAO_OCCLUSION_TERM_SCALE;
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 1, &scale, 2);
        commands->SetComputeSRV(3, grid.XeTerms[source].GetSRV()->GPUHandle);
        visibility = &grid.XeDepth;
    }
    else
    {
        commands->ResourceBarrier({ grid.Scratch[0].GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, grid.Scratch[0].GetUAV()->GPUHandle);
        Dispatch(commands, settings.Implementation == 0 ? SSAO : HBAO, width, height);
        commands->ResourceBarrier({ grid.Scratch[0].GetResource()->GetNonPixelShaderResourceBarrier() });
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            commands->ResourceBarrier({ grid.Scratch[target].GetResource()->GetUnorderedAccessBarrier() });
            commands->SetComputeSRV(2, grid.Scratch[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(0, grid.Scratch[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass % 2, 0);
            Dispatch(commands, Blur, width, height);
            commands->ResourceBarrier({ grid.Scratch[target].GetResource()->GetNonPixelShaderResourceBarrier() });
            source = target;
        }
        visibility = &grid.Scratch[source];
    }
    commands->SetComputeSRV(2, visibility->GetSRV()->GPUHandle);
    if (settings.Implementation != 2)
    {
        commands->ResourceBarrier({ grid.XeTerms[0].GetResource()->GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(3, grid.XeTerms[0].GetSRV()->GPUHandle);
    }
    auto& resolved = half ? grid.Visibility : _output;
    commands->ResourceBarrier({ resolved.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, resolved.GetUAV()->GPUHandle);
    Dispatch(commands, Resolve, width, height);
    if (half)
    {
        commands->ResourceBarrier({ resolved.GetResource()->GetNonPixelShaderResourceBarrier(),
            _output.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(2, resolved.GetSRV()->GPUHandle);
        commands->SetComputeSRV(5, fullDepth.GetSRV()->GPUHandle);
        commands->SetComputeSRV(6, fullNormals.GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, _output.GetUAV()->GPUHandle);
        Dispatch(commands, Upsample, _output.GetWidth(), _output.GetHeight());
    }
    commands->ResourceBarrier({ _output.GetResource()->GetNonPixelShaderResourceBarrier() });
}

GDX12Texture& GDX12AmbientOcclusionPass::DrawCACAO(GDX12CommandList* commands, WorkGrid& grid, GDX12Texture& depth,
    const GDX12AOSettings& s, Matrix projection)
{
    const bool pad = (std::max)(depth.GetWidth(), depth.GetHeight()) < 16;
    auto& inputDepth = pad ? grid.CacaoDepth : depth;
    auto& inputNormals = pad ? grid.CacaoNormals : grid.Normals;
    auto& output = pad ? grid.CacaoVisibility : grid.Scratch[0];
    if (pad)
    {
        commands->ResourceBarrier({ inputDepth.GetResource()->GetUnorderedAccessBarrier(),
            inputNormals.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(9, inputDepth.GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, inputNormals.GetUAV()->GPUHandle);
        Dispatch(commands, PadCACAO, inputDepth.GetWidth(), inputDepth.GetHeight());
        ExpandProjection(projection, static_cast<float>(depth.GetWidth()) / inputDepth.GetWidth(), 1.0f);
    }
    if (!grid.Cacao)
    {
        grid.CacaoMemory.resize(FFX_CACAO_D3D12GetContextSize());
        auto* context = reinterpret_cast<FFX_CACAO_D3D12Context*>(grid.CacaoMemory.data());
        CheckCACAO(FFX_CACAO_D3D12InitContext(context, _resources->Device->GetDevice().Get(),
            static_cast<UINT>(_resources->FrameConstants.size())));
        grid.Cacao = context;
    }
    if (!grid.CacaoScreenReady)
    {
        // The graph already chose the input grid. Keep the native downsample flag off.
        FFX_CACAO_D3D12ScreenSizeInfo info = {};
        info.width = inputDepth.GetWidth();
        info.height = inputDepth.GetHeight();
        info.depthBufferResource = inputDepth.GetResource()->D3DResource.Get();
        info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
        info.normalBufferResource = inputNormals.GetResource()->D3DResource.Get();
        info.normalBufferSrvDesc = info.depthBufferSrvDesc;
        info.normalBufferSrvDesc.Format = inputNormals.GetFormat();
        info.outputResource = output.GetResource()->D3DResource.Get();
        info.outputUavDesc.Format = output.GetFormat();
        info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(grid.Cacao, &info));
        grid.CacaoScreenReady = true;
    }
    auto settings = FFX_CACAO_DEFAULT_SETTINGS;
    // CACAO stores linear depth in FP16; metres keep centimetre-scale scenes in range.
    settings.radius = s.RadiusCm * 0.01f;
    settings.shadowMultiplier = s.Intensity;
    settings.shadowPower = s.CACAOPower;
    settings.shadowClamp = s.CACAOClamp;
    settings.horizonAngleThreshold = s.HorizonBias;
    settings.fadeOutFrom = s.FadeStartCm * 0.01f;
    settings.fadeOutTo = s.FadeEndCm * 0.01f;
    settings.qualityLevel = static_cast<FFX_CACAO_Quality>(s.CACAOQuality);
    settings.adaptiveQualityLimit = s.CACAOAdaptiveLimit;
    settings.blurPassCount = s.DenoisePasses;
    settings.sharpness = s.Sharpness;
    settings.detailShadowStrength = s.CACAODetail;
    CheckCACAO(FFX_CACAO_D3D12UpdateSettings(grid.Cacao, &settings));
    commands->ResourceBarrier({ inputDepth.GetResource()->GetNonPixelShaderResourceBarrier(),
        inputNormals.GetResource()->GetNonPixelShaderResourceBarrier(), output.GetResource()->GetUnorderedAccessBarrier() });
    FFX_CACAO_Matrix4x4 proj, identity = {};
    std::memcpy(&proj, &projection, sizeof(proj));
    proj.elements[3][2] *= 0.01f;
    for (int i = 0; i < 4; ++i) identity.elements[i][i] = 1;
    CheckCACAO(FFX_CACAO_D3D12Draw(grid.Cacao, commands->GetCommandList().Get(), &proj, &identity));
    output.GetResource()->GDX12Resource::SetCurrentState(commands->GetCommandList()->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ?
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_GENERIC_READ);
    commands->ResourceBarrier({ output.GetResource()->GetNonPixelShaderResourceBarrier() });
    commands->InvalidateCachedState();
    return output;
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

std::vector<GDX12RenderGraph::BufferAccess> GDX12AmbientOcclusionPass::GetBufferAccesses() const
{
    const auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    std::vector<GDX12RenderGraph::BufferAccess> accesses = {
        { &_constants->GetResource(), GDX12RenderGraph::Mode::Read },
        { &_xeConstants->GetResource(), GDX12RenderGraph::Mode::Read }};
    if (frame->CameraCB && frame->CameraCB->GetResource().D3DResource)
        accesses.push_back({ &frame->CameraCB->GetResource(), GDX12RenderGraph::Mode::Read,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER });
    return accesses;
}
