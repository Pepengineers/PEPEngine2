#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "../../../External/External.Libraries/XeGTAO/XeGTAO.h"
#define FFX_CACAO_ENABLE_D3D12
#include "../../../External/External.Libraries/cacao/inc/ffx_cacao_impl.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace
{
    void CheckCACAO(FFX_CACAO_Status status)
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

GDX12AmbientOcclusionPass::~GDX12AmbientOcclusionPass() { DestroyCACAO(); }

void GDX12AmbientOcclusionPass::Initialize()
{
    CreateTextures();
    const UINT frames = static_cast<UINT>(_resources->FrameConstants.size());
    _constants = std::make_unique<GDX12UploadBuffer<ShaderConstants>>(_resources->Device, frames);
    _xeConstants = std::make_unique<GDX12UploadBuffer<XeGTAO::GTAOConstants>>(_resources->Device, frames);
    GDX12RootSignatureDesc root;
    root.Constants.push_back(4);
    root.NumSingleCBVSlots = 3;
    root.NumSingleSRVSlots = 5;
    root.NumSingleUAVSlots = 9;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const char* entries[] = { "PrepareNormalsCS", "SSAOCS", "HBAOCS", "BlurCS", "ResolveCS", "XePrefilterCS", "XeMainCS", "XeDenoiseCS", "NeutralCS" };
    for (UINT i = 0; i < PipelineCount; ++i)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "AmbientOcclusion.hlsl", nullptr, entries[i], "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _root->GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pipelines[i])));
    }
    static_assert(sizeof(ShaderConstants) == 64);
}

void GDX12AmbientOcclusionPass::CreateTexture(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, UINT mips)
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
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &allocation, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
    texture.Initialize(desc);
}

void GDX12AmbientOcclusionPass::CreateTextures()
{
    const UINT width = _inputs[0]->GetTexture()->GetWidth(), height = _inputs[0]->GetTexture()->GetHeight();
    if (width == 0 || height == 0 || _inputs[1]->GetTexture()->GetWidth() != width || _inputs[1]->GetTexture()->GetHeight() != height)
        throw std::invalid_argument("AO requires equally sized, nonempty depth and normal textures");
    CreateTexture(_output, DXGI_FORMAT_R32_FLOAT, width, height);
    CreateTexture(_normals, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    for (auto& texture : _scratch) CreateTexture(texture, DXGI_FORMAT_R32_FLOAT, width, height);
    for (auto& texture : _xeTerms) CreateTexture(texture, DXGI_FORMAT_R8_UINT, width, height);
    CreateTexture(_xeEdges, DXGI_FORMAT_R8_UNORM, width, height);
    CreateTexture(_xeDepth, DXGI_FORMAT_R32_FLOAT, (std::max)(16u, width), (std::max)(16u, height), 5);
    for (UINT mip = 0; mip < 5; ++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
        auto* heap = _resources->SRV_UAV_Heap.get();
        const UINT index = _depthUAVs[mip].DescType != DESC_TYPE_NONE ? _depthUAVs[mip].HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        _depthUAVs[mip].InitAsUAV(_xeDepth.GetResource()->D3DResource.Get(), nullptr, &view, heap, index);
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
    commands->SetComputeSRV(0, _inputs[0]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, _inputs[1]->GetTexture()->GetSRV()->GPUHandle);
}

void GDX12AmbientOcclusionPass::Dispatch(GDX12CommandList* commands, Pipeline pipeline, UINT width, UINT height)
{
    commands->SetPipelineState(_pipelines[pipeline]);
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void GDX12AmbientOcclusionPass::Execute(GDX12CommandList* commands)
{
    const auto settings = GetAmbientOcclusionSettings();
    // All fields are four-byte scalars; no padding participates in this comparison.
    static_assert(sizeof(GDX12AOSettings) == 23 * 4);
    if (!_hasPrevious || std::memcmp(&_previous, &settings, sizeof(settings)) != 0)
        _commonData->ResetUpscalerHistory = true;
    _previous = settings;
    _hasPrevious = true;
    commands->BeginPixEvent("Ambient occlusion", Colors::DarkSlateBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->ResourceBarrier({ _output.GetResource()->GetUnorderedAccessBarrier() });
    // shortcut: sub-icon viewports use neutral AO; extend vendor mip setup if these are needed.
    if (!settings.IsActive() || (std::max)(_output.GetWidth(), _output.GetHeight()) < 16)
    {
        commands->SetComputeRootSignature(_root.get());
        commands->SetComputeUAV(0, _output.GetUAV()->GPUHandle);
        Dispatch(commands, Neutral, _output.GetWidth(), _output.GetHeight());
        commands->ResourceBarrier({ _output.GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
        return;
    }

    const UINT frame = _resources->CurrFrameConstantsIndex;
    ShaderConstants constants = { settings.RadiusCm, settings.Intensity, settings.BiasCm, settings.HorizonBias,
        settings.FadeStartCm, settings.FadeEndCm, settings.Sharpness, static_cast<UINT>(settings.Samples),
        static_cast<UINT>(settings.Directions), static_cast<UINT>(settings.Steps), static_cast<UINT>(settings.DenoisePasses),
        static_cast<UINT>(settings.Implementation), static_cast<float>(settings.GTAOSlices), static_cast<float>(settings.GTAOSteps), {} };
    _constants->CopyData(frame, constants);
    Matrix projection = _commonData->CameraViewToClip;
    projection._31 -= _commonData->ActiveCameraJitterOffsetX * 2.0f / _output.GetWidth();
    projection._32 += _commonData->ActiveCameraJitterOffsetY * 2.0f / _output.GetHeight();
    XeGTAO::GTAOSettings gtao;
    gtao.Radius = settings.RadiusCm;
    gtao.DenoisePasses = settings.DenoisePasses;
    gtao.FalloffRange = settings.GTAOFalloff;
    gtao.ThinOccluderCompensation = settings.GTAOThinOccluder;
    XeGTAO::GTAOConstants xe = {};
    XeGTAO::GTAOUpdateConstants(xe, _output.GetWidth(), _output.GetHeight(), gtao, &projection._11, true,
        _commonData->Upscaler ? _frameIndex++ : 0);
    xe.NDCToViewAdd.x += projection._31 / projection._11;
    xe.NDCToViewAdd.y += projection._32 / projection._22;
    xe.DenoiseBlurBeta = settings.DenoisePasses ? 0.2f + settings.Sharpness * 1.0f : 1e4f;
    _xeConstants->CopyData(frame, xe);
    commands->ResourceBarrier({ _inputs[0]->GetTexture()->GetResource()->GetSRVBarrier(),
        _inputs[1]->GetTexture()->GetResource()->GetSRVBarrier(), _normals.GetResource()->GetUnorderedAccessBarrier() });
    Bind(commands);
    commands->SetComputeSRV(1, _inputs[1]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, _normals.GetUAV()->GPUHandle);
    Dispatch(commands, PrepareNormals, _output.GetWidth(), _output.GetHeight());
    commands->ResourceBarrier({ _normals.GetResource()->GetSRVBarrier() });
    commands->SetComputeSRV(1, _normals.GetSRV()->GPUHandle);

    UINT source = 0;
    if (settings.Implementation == 3)
    {
        DrawCACAO(commands, settings, projection);
        Bind(commands);
    }
    else if (settings.Implementation == 2)
    {
        commands->ResourceBarrier({ _xeDepth.GetResource()->GetUnorderedAccessBarrier() });
        for (UINT mip = 0; mip < 5; ++mip) commands->SetComputeUAV(2 + mip, _depthUAVs[mip].GPUHandle);
        Dispatch(commands, XePrefilter, (_xeDepth.GetWidth() + 1) / 2, (_xeDepth.GetHeight() + 1) / 2);
        commands->ResourceBarrier({ _xeDepth.GetResource()->GetSRVBarrier(), _xeTerms[0].GetResource()->GetUnorderedAccessBarrier(),
            _xeEdges.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(2, _xeDepth.GetSRV()->GPUHandle);
        commands->SetComputeUAV(7, _xeTerms[0].GetUAV()->GPUHandle);
        commands->SetComputeUAV(8, _xeEdges.GetUAV()->GPUHandle);
        Dispatch(commands, XeMain, _output.GetWidth(), _output.GetHeight());
        commands->ResourceBarrier({ _xeTerms[0].GetResource()->GetSRVBarrier(), _xeEdges.GetResource()->GetSRVBarrier() });
        commands->SetComputeSRV(4, _xeEdges.GetSRV()->GPUHandle);
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            commands->ResourceBarrier({ _xeTerms[target].GetResource()->GetUnorderedAccessBarrier() });
            commands->SetComputeSRV(3, _xeTerms[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(7, _xeTerms[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass + 1 == settings.DenoisePasses, 1);
            Dispatch(commands, XeDenoise, (_output.GetWidth() + 1) / 2, _output.GetHeight());
            commands->ResourceBarrier({ _xeTerms[target].GetResource()->GetSRVBarrier() });
            source = target;
        }
        const float scale = settings.DenoisePasses ? 1.0f : XE_GTAO_OCCLUSION_TERM_SCALE;
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 1, &scale, 2);
        commands->SetComputeSRV(3, _xeTerms[source].GetSRV()->GPUHandle);
    }
    else
    {
        commands->ResourceBarrier({ _scratch[0].GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, _scratch[0].GetUAV()->GPUHandle);
        Dispatch(commands, settings.Implementation == 0 ? SSAO : HBAO, _output.GetWidth(), _output.GetHeight());
        commands->ResourceBarrier({ _scratch[0].GetResource()->GetSRVBarrier() });
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            commands->ResourceBarrier({ _scratch[target].GetResource()->GetUnorderedAccessBarrier() });
            commands->SetComputeSRV(2, _scratch[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(0, _scratch[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass % 2, 0);
            Dispatch(commands, Blur, _output.GetWidth(), _output.GetHeight());
            commands->ResourceBarrier({ _scratch[target].GetResource()->GetSRVBarrier() });
            source = target;
        }
    }
    commands->ResourceBarrier({ _scratch[source].GetResource()->GetSRVBarrier() });
    commands->SetComputeSRV(2, _scratch[source].GetSRV()->GPUHandle);
    if (settings.Implementation != 2)
    {
        commands->ResourceBarrier({ _xeTerms[0].GetResource()->GetSRVBarrier() });
        commands->SetComputeSRV(3, _xeTerms[0].GetSRV()->GPUHandle);
    }
    commands->SetComputeUAV(0, _output.GetUAV()->GPUHandle);
    Dispatch(commands, Resolve, _output.GetWidth(), _output.GetHeight());
    commands->ResourceBarrier({ _output.GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}

void GDX12AmbientOcclusionPass::DrawCACAO(GDX12CommandList* commands, const GDX12AOSettings& s, const Matrix& projection)
{
    if (!_cacao)
    {
        _cacaoMemory.resize(FFX_CACAO_D3D12GetContextSize());
        auto* context = reinterpret_cast<FFX_CACAO_D3D12Context*>(_cacaoMemory.data());
        CheckCACAO(FFX_CACAO_D3D12InitContext(context, _resources->Device->GetDevice().Get(),
            static_cast<UINT>(_resources->FrameConstants.size())));
        _cacao = context;
    }
    if (!_cacaoScreenReady)
    {
        FFX_CACAO_D3D12ScreenSizeInfo info = {};
        info.width = _output.GetWidth();
        info.height = _output.GetHeight();
        info.depthBufferResource = _inputs[0]->GetTexture()->GetResource()->D3DResource.Get();
        info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
        info.normalBufferResource = _normals.GetResource()->D3DResource.Get();
        info.normalBufferSrvDesc = info.depthBufferSrvDesc;
        info.normalBufferSrvDesc.Format = _normals.GetFormat();
        info.outputResource = _scratch[0].GetResource()->D3DResource.Get();
        info.outputUavDesc.Format = _scratch[0].GetFormat();
        info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(_cacao, &info));
        _cacaoScreenReady = true;
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
    CheckCACAO(FFX_CACAO_D3D12UpdateSettings(_cacao, &settings));
    commands->ResourceBarrier({ _inputs[0]->GetTexture()->GetResource()->GetNonPixelShaderResourceBarrier(),
        _normals.GetResource()->GetNonPixelShaderResourceBarrier(), _scratch[0].GetResource()->GetUnorderedAccessBarrier() });
    FFX_CACAO_Matrix4x4 proj, identity = {};
    std::memcpy(&proj, &projection, sizeof(proj));
    proj.elements[3][2] *= 0.01f;
    for (int i = 0; i < 4; ++i) identity.elements[i][i] = 1;
    CheckCACAO(FFX_CACAO_D3D12Draw(_cacao, commands->GetCommandList().Get(), &proj, &identity));
    _scratch[0].GetResource()->GDX12Resource::SetCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ);
    commands->InvalidateCachedState();
}

void GDX12AmbientOcclusionPass::DestroyCACAO()
{
    if (_cacaoScreenReady) FFX_CACAO_D3D12DestroyScreenSizeDependentResources(_cacao);
    if (_cacao) FFX_CACAO_D3D12DestroyContext(_cacao);
    _cacaoScreenReady = false;
    _cacao = nullptr;
    _cacaoMemory.clear();
}

void GDX12AmbientOcclusionPass::Resize()
{
    // Renderer drains the queue before replacing size-dependent resources.
    if (_cacaoScreenReady) CheckCACAO(FFX_CACAO_D3D12DestroyScreenSizeDependentResources(_cacao));
    _cacaoScreenReady = false;
    CreateTextures();
    _hasPrevious = false;
}

void GDX12AmbientOcclusionPass::ClearDenendencies()
{
    DestroyCACAO();
    GDX12RenderPass::ClearDenendencies();
}
