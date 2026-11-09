#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "../../../External/External.Libraries/XeGTAO/XeGTAO.h"
#define FFX_CACAO_ENABLE_D3D12
#include "../../../External/External.Libraries/cacao/inc/ffx_cacao_impl.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

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
    InitializePlacedDevice(*_resources);
}

void GDX12AmbientOcclusionPass::InitializePlacedDevice(GDX12DeviceResources& resources)
{
    _resources = &resources;
    GDX12RootSignatureDesc root;
    root.Constants.push_back(4);
    root.NumSingleCBVSlots = 3;
    root.NumSingleSRVSlots = 7;
    root.NumSingleUAVSlots = 10;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const char* entries[PipelineCount] = { "PrepareNormalsCS", "SSAOCS", "HBAOCS", "BlurCS", "ResolveCS",
        "XePrefilterCS", "XeMainCS", "XeDenoiseCS", "NeutralCS", "PrepareHalfCS", "UpsampleCS", "PadCACAOCS", "PackFullCS" };
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
    CreateScratch(grid, width, height);
    CreateTexture(grid.Normals, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
}

void GDX12AmbientOcclusionPass::CreateScratch(WorkGrid& grid, UINT width, UINT height)
{
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

GDX12AmbientOcclusionPass::PlacedTexture GDX12AmbientOcclusionPass::BindTexture(GDX12Texture& texture)
{
    return { texture.GetResource(), texture.HasSRV() ? texture.GetSRV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{},
        texture.HasUAV() ? texture.GetUAV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{} };
}

void GDX12AmbientOcclusionPass::Bind(GDX12CommandList* commands, ConstantBindings constants)
{
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_root.get());
    struct { UINT Axis, Final; float XeScale; UINT Padding; } parameters = { 0, 0, 1.0f, 0 };
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, &parameters, 0);
    if (constants.View) commands->SetComputeRootConstantBufferView(1, constants.View);
    if (constants.AO) commands->SetComputeRootConstantBufferView(2, constants.AO);
    if (constants.Xe) commands->SetComputeRootConstantBufferView(3, constants.Xe);
}

void GDX12AmbientOcclusionPass::Dispatch(GDX12CommandList* commands, const Pipeline pipeline, const UINT width, const UINT height)
{
    commands->SetPipelineState(_pipelines[pipeline]);
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

struct GDX12AmbientOcclusionPass::RecordingConstants
{
    GDX12AOSettings Settings;
    ShaderConstants Shader;
    XeGTAO::GTAOConstants Xe;
    Matrix Projection;
    UINT Width, Height;
};

GDX12AmbientOcclusionPass::RecordingConstants GDX12AmbientOcclusionPass::MakeRecordingConstants(const FrameInput& frame)
{
    const auto& settings = frame.Settings;
    const bool half = settings.Resolution == GDX12AOResolution::Half;
    const UINT width = half ? (frame.Width + 1) / 2 : frame.Width;
    const UINT height = half ? (frame.Height + 1) / 2 : frame.Height;
    const ShaderConstants shader = { settings.RadiusCm, settings.Intensity, settings.BiasCm, settings.HorizonBias,
        settings.FadeStartCm, settings.FadeEndCm, settings.Sharpness, static_cast<UINT>(settings.Samples),
        static_cast<UINT>(settings.Directions), static_cast<UINT>(settings.Steps), static_cast<UINT>(settings.DenoisePasses),
        static_cast<UINT>(settings.Implementation), static_cast<float>(settings.GTAOSlices), static_cast<float>(settings.GTAOSteps), half, 0 };
    Matrix projection = frame.JitteredProjection;
    if (half)
        ExpandProjection(projection, frame.Width / (2.0f * width), frame.Height / (2.0f * height));
    XeGTAO::GTAOSettings gtao;
    gtao.Radius = settings.RadiusCm;
    gtao.DenoisePasses = settings.DenoisePasses;
    gtao.FalloffRange = settings.GTAOFalloff;
    gtao.ThinOccluderCompensation = settings.GTAOThinOccluder;
    XeGTAO::GTAOConstants xe = {};
    XeGTAO::GTAOUpdateConstants(xe, width, height, gtao, &projection._11, true, frame.NoiseIndex);
    xe.NDCToViewAdd.x += projection._31 / projection._11;
    xe.NDCToViewAdd.y += projection._32 / projection._22;
    xe.DenoiseBlurBeta = settings.DenoisePasses ? 0.2f + settings.Sharpness * 1.0f : 1e4f;
    return { settings, shader, xe, projection, width, height };
}

class GDX12AmbientOcclusionPass::ScratchStates
{
public:
    ScratchStates(GDX12CommandList* commands, bool placed) : _commands(commands), _placed(placed) {}

    void Transition(GDX12Resource& resource, D3D12_RESOURCE_STATES state)
    {
        if (!_placed)
        {
            _commands->ResourceBarrier({ resource.GetTransitionBarrier(state) });
            return;
        }
        auto& old = State(resource);
        if (old != state)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.D3DResource.Get(), old, state);
            _commands->GetCommandList()->ResourceBarrier(1, &barrier);
            old = state;
        }
    }

    void ObserveVendorOutput(GDX12Resource& resource)
    {
        const auto state = _commands->GetCommandList()->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ?
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_GENERIC_READ;
        if (_placed) State(resource) = state;
        else resource.SetCurrentState(state);
    }

    void Finish()
    {
        for (auto& [resource, state] : _states)
            Transition(*resource, D3D12_RESOURCE_STATE_COMMON);
    }

private:
    D3D12_RESOURCE_STATES& State(GDX12Resource& resource)
    {
        for (auto& [existing, state] : _states)
            if (existing == &resource) return state;
        return _states.emplace_back(&resource, D3D12_RESOURCE_STATE_COMMON).second;
    }

    GDX12CommandList* _commands;
    bool _placed;
    std::vector<std::pair<GDX12Resource*, D3D12_RESOURCE_STATES>> _states;
};

void GDX12AmbientOcclusionPass::RecordPrepare(GDX12CommandList* commands, UINT width, UINT height, Pipeline pipeline,
    PlacedTexture depth, PlacedTexture normals, PlacedTexture preparedDepth, PlacedTexture preparedNormals)
{
    commands->SetComputeSRV(0, depth.SRV);
    commands->SetComputeSRV(1, normals.SRV);
    if (pipeline != PrepareNormals) commands->SetComputeUAV(9, preparedDepth.UAV);
    commands->SetComputeUAV(1, preparedNormals.UAV);
    Dispatch(commands, pipeline, width, height);
}

void GDX12AmbientOcclusionPass::RecordEvaluate(GDX12CommandList* commands, WorkGrid& grid,
    const RecordingConstants& frame, ConstantBindings bindings,
    PlacedTexture depth, PlacedTexture normals, PlacedTexture resolved, ScratchStates& states)
{
    const auto& settings = frame.Settings;
    const UINT width = frame.Width, height = frame.Height;
    Bind(commands, bindings);
    commands->SetComputeSRV(0, depth.SRV);
    commands->SetComputeSRV(1, normals.SRV);
    UINT source = 0;
    GDX12Texture* visibility = nullptr;
    if (settings.Implementation == 3)
    {
        visibility = &DrawCACAO(commands, grid, frame, states);
        Bind(commands, bindings);
        commands->SetComputeSRV(0, depth.SRV);
        commands->SetComputeSRV(1, normals.SRV);
    }
    else if (settings.Implementation == 2)
    {
        states.Transition(*grid.XeDepth.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (UINT mip = 0; mip < 5; ++mip) commands->SetComputeUAV(2 + mip, grid.DepthUAVs[mip].GPUHandle);
        Dispatch(commands, XePrefilter, grid.XeDepth.GetWidth() / 2, grid.XeDepth.GetHeight() / 2);
        states.Transition(*grid.XeDepth.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.Transition(*grid.XeTerms[0].GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.Transition(*grid.XeEdges.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeSRV(2, grid.XeDepth.GetSRV()->GPUHandle);
        commands->SetComputeUAV(7, grid.XeTerms[0].GetUAV()->GPUHandle);
        commands->SetComputeUAV(8, grid.XeEdges.GetUAV()->GPUHandle);
        Dispatch(commands, XeMain, width, height);
        states.Transition(*grid.XeTerms[0].GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.Transition(*grid.XeEdges.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands->SetComputeSRV(4, grid.XeEdges.GetSRV()->GPUHandle);
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            states.Transition(*grid.XeTerms[target].GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetComputeSRV(3, grid.XeTerms[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(7, grid.XeTerms[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass + 1 == settings.DenoisePasses, 1);
            Dispatch(commands, XeDenoise, (width + 1) / 2, height);
            states.Transition(*grid.XeTerms[target].GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            source = target;
        }
        const float scale = settings.DenoisePasses ? 1.0f : XE_GTAO_OCCLUSION_TERM_SCALE;
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 1, &scale, 2);
        commands->SetComputeSRV(3, grid.XeTerms[source].GetSRV()->GPUHandle);
        visibility = &grid.XeDepth;
    }
    else
    {
        states.Transition(*grid.Scratch[0].GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeUAV(0, grid.Scratch[0].GetUAV()->GPUHandle);
        Dispatch(commands, settings.Implementation == 0 ? SSAO : HBAO, width, height);
        states.Transition(*grid.Scratch[0].GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (int pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            states.Transition(*grid.Scratch[target].GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetComputeSRV(2, grid.Scratch[source].GetSRV()->GPUHandle);
            commands->SetComputeUAV(0, grid.Scratch[target].GetUAV()->GPUHandle);
            commands->GetCommandList()->SetComputeRoot32BitConstant(0, pass % 2, 0);
            Dispatch(commands, Blur, width, height);
            states.Transition(*grid.Scratch[target].GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            source = target;
        }
        visibility = &grid.Scratch[source];
    }
    commands->SetComputeSRV(2, visibility->GetSRV()->GPUHandle);
    if (settings.Implementation != 2)
    {
        states.Transition(*grid.XeTerms[0].GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands->SetComputeSRV(3, grid.XeTerms[0].GetSRV()->GPUHandle);
    }
    commands->SetComputeUAV(0, resolved.UAV);
    Dispatch(commands, Resolve, width, height);
    states.Finish();
}

void GDX12AmbientOcclusionPass::RecordUpsample(GDX12CommandList* commands, UINT width, UINT height,
    const PlacedInputs& inputs)
{
    commands->SetComputeSRV(0, inputs.PreparedDepth.SRV);
    commands->SetComputeSRV(1, inputs.PreparedNormals.SRV);
    commands->SetComputeSRV(2, inputs.Visibility.SRV);
    commands->SetComputeSRV(5, inputs.Depth.SRV);
    commands->SetComputeSRV(6, inputs.Normals.SRV);
    commands->SetComputeUAV(0, inputs.Output.UAV);
    Dispatch(commands, Upsample, width, height);
}

GDX12AmbientOcclusionPass::FrameInput GDX12AmbientOcclusionPass::CaptureFrameInput()
{
    const auto settings = GetAmbientOcclusionSettings();
    FrameInput input = { settings, _output.GetWidth(), _output.GetHeight(), _commonData->CameraView,
        _commonData->CameraViewToClip, 0, _commonData->ActiveCameraCBufferIndex };
    input.JitteredProjection._31 -= _commonData->ActiveCameraJitterOffsetX * 2.0f / input.Width;
    input.JitteredProjection._32 += _commonData->ActiveCameraJitterOffsetY * 2.0f / input.Height;
    const UINT minimum = settings.Resolution == GDX12AOResolution::Half ? 2 : 1;
    if (_commonData->Upscaler && settings.IsActive() && (std::max)(input.Width, input.Height) > minimum)
        input.NoiseIndex = _frameIndex++;
    return input;
}

void GDX12AmbientOcclusionPass::UpdateHistory(const FrameInput& input, bool placed)
{
    const auto& settings = input.Settings;
    // All fields are four-byte scalars; no padding participates in this comparison.
    static_assert(sizeof(GDX12AOSettings) == 24 * 4);
    if (!_hasPrevious || placed != _previousPlaced || std::memcmp(&_previous, &settings, sizeof(settings)) != 0)
        _commonData->ResetUpscalerHistory = true;
    _previous = settings;
    _previousPlaced = placed;
    _hasPrevious = true;
}

void GDX12AmbientOcclusionPass::Execute(GDX12CommandList* commands)
{
    RecordLocalFrame(commands, CaptureFrameInput());
}

void GDX12AmbientOcclusionPass::RecordLocalFrame(GDX12CommandList* commands, const FrameInput& input)
{
    if (input.Width != _output.GetWidth() || input.Height != _output.GetHeight())
        throw std::invalid_argument("Local AO frame does not match the render extent");
    const auto& settings = input.Settings;
    UpdateHistory(input, false);
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
    const UINT index = _resources->CurrFrameConstantsIndex;
    const auto frame = MakeRecordingConstants(input);
    _constants->CopyData(index, frame.Shader);
    _xeConstants->CopyData(index, frame.Xe);
    const ConstantBindings bindings = {
        _resources->FrameConstants[index]->CameraCB->GetElementAddress(input.LocalCameraIndex),
        _constants->GetElementAddress(index), _xeConstants->GetElementAddress(index) };

    commands->ResourceBarrier({ fullDepth.GetResource()->GetNonPixelShaderResourceBarrier(),
        fullNormals.GetResource()->GetNonPixelShaderResourceBarrier(), grid.Normals.GetResource()->GetUnorderedAccessBarrier() });
    if (half) commands->ResourceBarrier({ depth.GetResource()->GetUnorderedAccessBarrier() });
    Bind(commands, bindings);
    RecordPrepare(commands, width, height, half ? PrepareHalf : PrepareNormals, BindTexture(fullDepth),
        BindTexture(fullNormals), half ? BindTexture(depth) : PlacedTexture{}, BindTexture(grid.Normals));
    commands->ResourceBarrier({ depth.GetResource()->GetNonPixelShaderResourceBarrier(),
        grid.Normals.GetResource()->GetNonPixelShaderResourceBarrier() });

    const auto preparedDepth = BindTexture(depth), preparedNormals = BindTexture(grid.Normals);
    if (settings.Implementation == 3) PrepareCACAO(grid, preparedDepth, preparedNormals, false);
    auto& resolved = half ? grid.Visibility : _output;
    commands->ResourceBarrier({ resolved.GetResource()->GetUnorderedAccessBarrier() });
    ScratchStates scratch(commands, false);
    RecordEvaluate(commands, grid, frame, bindings, preparedDepth, preparedNormals, BindTexture(resolved), scratch);
    if (half)
    {
        commands->ResourceBarrier({ resolved.GetResource()->GetNonPixelShaderResourceBarrier(),
            _output.GetResource()->GetUnorderedAccessBarrier() });
        RecordUpsample(commands, input.Width, input.Height, { BindTexture(fullDepth), BindTexture(fullNormals),
            preparedDepth, preparedNormals, BindTexture(resolved), BindTexture(_output) });
    }
    commands->ResourceBarrier({ _output.GetResource()->GetNonPixelShaderResourceBarrier() });
}

void GDX12AmbientOcclusionPass::PrepareCACAO(WorkGrid& grid, PlacedTexture depth, PlacedTexture normals, bool placed)
{
    if (grid.CacaoScreenReady && !placed) return;
    const auto extent = depth.Resource->D3DResource->GetDesc();
    const bool pad = (std::max)(extent.Width, static_cast<UINT64>(extent.Height)) < 16;
    const auto inputDepth = pad ? BindTexture(grid.CacaoDepth) : depth;
    const auto inputNormals = pad ? BindTexture(grid.CacaoNormals) : normals;
    auto& output = pad ? grid.CacaoVisibility : grid.Scratch[0];
    if (!grid.Cacao)
    {
        grid.CacaoMemory.resize(FFX_CACAO_D3D12GetContextSize());
        auto* context = reinterpret_cast<FFX_CACAO_D3D12Context*>(grid.CacaoMemory.data());
        const UINT pages = placed ? 1 : static_cast<UINT>(_resources->FrameConstants.size());
        CheckCACAO(FFX_CACAO_D3D12InitContext(context, _resources->Device->GetDevice().Get(), pages));
        grid.Cacao = context;
    }

    FFX_CACAO_D3D12ScreenSizeInfo info = {};
    info.width = output.GetWidth();
    info.height = output.GetHeight();
    info.depthBufferResource = inputDepth.Resource->D3DResource.Get();
    info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
    info.normalBufferResource = inputNormals.Resource->D3DResource.Get();
    info.normalBufferSrvDesc = info.depthBufferSrvDesc;
    info.normalBufferSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    info.outputResource = output.GetResource()->D3DResource.Get();
    info.outputUavDesc.Format = output.GetFormat();
    info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (grid.CacaoScreenReady)
        CheckCACAO(FFX_CACAO_D3D12RebindPlacedViews(grid.Cacao, &info));
    else
    {
        CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(grid.Cacao, &info));
        grid.CacaoScreenReady = true;
    }
}

GDX12Texture& GDX12AmbientOcclusionPass::DrawCACAO(GDX12CommandList* commands, WorkGrid& grid,
    const RecordingConstants& frame, ScratchStates& states)
{
    const bool pad = (std::max)(frame.Width, frame.Height) < 16;
    auto& output = pad ? grid.CacaoVisibility : grid.Scratch[0];
    Matrix projection = frame.Projection;
    if (pad)
    {
        states.Transition(*grid.CacaoDepth.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.Transition(*grid.CacaoNormals.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeUAV(9, grid.CacaoDepth.GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, grid.CacaoNormals.GetUAV()->GPUHandle);
        Dispatch(commands, PadCACAO, grid.CacaoDepth.GetWidth(), grid.CacaoDepth.GetHeight());
        states.Transition(*grid.CacaoDepth.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.Transition(*grid.CacaoNormals.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ExpandProjection(projection, static_cast<float>(frame.Width) / grid.CacaoDepth.GetWidth(), 1.0f);
    }
    const auto& s = frame.Settings;
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
    states.Transition(*output.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FFX_CACAO_Matrix4x4 proj, identity = {};
    std::memcpy(&proj, &projection, sizeof(proj));
    proj.elements[3][2] *= 0.01f;
    for (int i = 0; i < 4; ++i) identity.elements[i][i] = 1;
    CheckCACAO(FFX_CACAO_D3D12Draw(grid.Cacao, commands->GetCommandList().Get(), &proj, &identity));
    states.ObserveVendorOutput(*output.GetResource());
    states.Transition(*output.GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

class GDX12AmbientOcclusionPass::PlacedSlot
{
public:
    ~PlacedSlot()
    {
        if (Grid) DestroyCACAO(*Grid);
    }

private:
    friend class GDX12AmbientOcclusionPass;
    PlacedSlot(GDX12AmbientOcclusionPass& owner, PlacedStage stage, UINT width, UINT height, GDX12AOResolution resolution)
        : Owner(owner), Stage(stage), Width(width), Height(height), Resolution(resolution) {}

    GDX12AmbientOcclusionPass& Owner;
    const PlacedStage Stage;
    const UINT Width, Height;
    const GDX12AOResolution Resolution;
    RecordingConstants Frame{};
    PlacedInputs Inputs{};
    std::unique_ptr<WorkGrid> Grid;
    std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> ViewUpload;
    std::unique_ptr<GDX12UploadBuffer<ShaderConstants>> AOUpload;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> XeUpload;
    ConstantBindings Constants{};
    std::vector<PlacedAccess> Accesses;
    bool Ready = false;
};

std::shared_ptr<GDX12AmbientOcclusionPass::PlacedSlot> GDX12AmbientOcclusionPass::CreatePlacedSlot(
    PlacedStage stage, UINT width, UINT height, GDX12AOResolution resolution)
{
    if (!_root || stage < PlacedStage::Pack || stage > PlacedStage::Finalize ||
        width == 0 || height == 0 || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        (resolution != GDX12AOResolution::Full && resolution != GDX12AOResolution::Half))
        throw std::invalid_argument("Invalid placed AO device or layout");
    return std::shared_ptr<PlacedSlot>(new PlacedSlot(*this, stage, width, height, resolution));
}

void GDX12AmbientOcclusionPass::PreparePlacedSlot(const std::shared_ptr<PlacedSlot>& owner,
    const FrameInput& input, const PlacedInputs& inputs)
{
    if (!owner || &owner->Owner != this || owner.use_count() != 1)
        throw std::invalid_argument("Placed AO slot is retained by another invocation");
    auto& slot = *owner;
    slot.Ready = false;
    slot.Accesses.clear();
    if (input.Width != slot.Width || input.Height != slot.Height || input.Settings.Resolution != slot.Resolution ||
        !input.Settings.IsActive() || input.Settings.Implementation < 0 || input.Settings.Implementation > 3 ||
        input.Settings.DenoisePasses < 0 || input.Settings.DenoisePasses > 8)
        throw std::invalid_argument("Invalid placed AO frame snapshot");

    const auto frame = MakeRecordingConstants(input);
    if (frame.Width == 1 && frame.Height == 1)
        throw std::invalid_argument("Single-texel AO uses the local neutral path");
    const bool half = slot.Resolution == GDX12AOResolution::Half;
    auto* device = _resources->Device;

    using Mode = GDX12RenderGraph::Mode;
    constexpr auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::vector<PlacedAccess> accesses;
    const auto declare = [&](PlacedTexture texture, Mode mode, D3D12_RESOURCE_STATES state,
        UINT width, UINT height, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN)
    {
        if (!texture.Resource || !texture.Resource->D3DResource ||
            (state == srv && !texture.SRV.ptr) || (state == uav && !texture.UAV.ptr))
            throw std::invalid_argument("Placed AO texture or descriptor missing");
        auto* native = texture.Resource->D3DResource.Get();
        const auto desc = native->GetDesc();
        ComPtr<ID3D12Device14> actual;
        ThrowIfFailed(native->GetDevice(IID_PPV_ARGS(&actual)));
        if (actual.Get() != device->GetDevice().Get() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != width || desc.Height != height || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || (format != DXGI_FORMAT_UNKNOWN && desc.Format != format) ||
            (state == uav && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
            (state == srv && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
            throw std::invalid_argument("Placed AO texture has the wrong device, layout or access");
        for (const auto& access : accesses)
            if (access.Resource->D3DResource.Get() == native)
                throw std::invalid_argument("Placed AO roles must refer to distinct resources");
        accesses.push_back({ texture.Resource, texture.Resource->D3DResource, mode, state });
    };

    if (slot.Stage == PlacedStage::Pack || (slot.Stage == PlacedStage::Finalize && half))
    {
        declare(inputs.Depth, Mode::Read, srv, slot.Width, slot.Height);
        declare(inputs.Normals, Mode::Read, srv, slot.Width, slot.Height);
        if (!slot.ViewUpload) slot.ViewUpload = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(device, 1);
        GDX12CameraConstants camera{};
        camera.View = input.View.Transpose();
        slot.ViewUpload->CopyData(0, camera);
    }
    if (slot.Stage != PlacedStage::Finalize || half)
    {
        const bool pack = slot.Stage == PlacedStage::Pack;
        declare(inputs.PreparedDepth, pack ? Mode::Write : Mode::Read, pack ? uav : srv,
            frame.Width, frame.Height, DXGI_FORMAT_R32_FLOAT);
        declare(inputs.PreparedNormals, pack ? Mode::Write : Mode::Read, pack ? uav : srv,
            frame.Width, frame.Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    if (slot.Stage == PlacedStage::Evaluate)
    {
        declare(inputs.Visibility, Mode::Write, uav, frame.Width, frame.Height, DXGI_FORMAT_R32_FLOAT);
        if (!slot.Grid)
        {
            auto grid = std::make_unique<WorkGrid>();
            CreateScratch(*grid, frame.Width, frame.Height);
            slot.Grid = std::move(grid);
        }
        if (input.Settings.Implementation == 3)
            PrepareCACAO(*slot.Grid, inputs.PreparedDepth, inputs.PreparedNormals, true);
    }
    if (slot.Stage == PlacedStage::Finalize)
    {
        declare(inputs.Visibility, Mode::Read, half ? srv : D3D12_RESOURCE_STATE_COPY_SOURCE,
            frame.Width, frame.Height, DXGI_FORMAT_R32_FLOAT);
        declare(inputs.Output, Mode::Write, half ? uav : D3D12_RESOURCE_STATE_COPY_DEST,
            slot.Width, slot.Height, DXGI_FORMAT_R32_FLOAT);
    }
    if (slot.Stage == PlacedStage::Evaluate || (slot.Stage == PlacedStage::Finalize && half))
    {
        if (!slot.AOUpload) slot.AOUpload = std::make_unique<GDX12UploadBuffer<ShaderConstants>>(device, 1);
        if (!slot.XeUpload) slot.XeUpload = std::make_unique<GDX12UploadBuffer<XeGTAO::GTAOConstants>>(device, 1);
        slot.AOUpload->CopyData(0, frame.Shader);
        slot.XeUpload->CopyData(0, frame.Xe);
    }
    slot.Constants = { slot.ViewUpload ? slot.ViewUpload->GetElementAddress(0) : 0,
        slot.AOUpload ? slot.AOUpload->GetElementAddress(0) : 0, slot.XeUpload ? slot.XeUpload->GetElementAddress(0) : 0 };
    slot.Inputs = inputs;
    slot.Frame = frame;
    slot.Accesses = std::move(accesses);
    slot.Ready = true;
}

const std::vector<GDX12AmbientOcclusionPass::PlacedAccess>& GDX12AmbientOcclusionPass::GetPlacedAccesses(
    const PlacedSlot& slot) const
{
    if (&slot.Owner != this || !slot.Ready)
        throw std::logic_error("Placed AO slot must be prepared before graph registration");
    return slot.Accesses;
}

void GDX12AmbientOcclusionPass::RecordPlacedSlot(GDX12CommandList* commands, const PlacedSlot& slot)
{
    for (const auto& access : GetPlacedAccesses(slot))
        if (access.Resource->D3DResource.Get() != access.Native.Get())
            throw std::logic_error("Placed AO resource changed after slot preparation");
    auto* native = commands->GetCommandList().Get();
    const auto queue = slot.Stage == PlacedStage::Evaluate ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (native->GetType() != queue)
        throw std::invalid_argument("Placed AO recorded on the wrong queue");
    const auto& inputs = slot.Inputs;
    const auto& frame = slot.Frame;
    const bool half = slot.Resolution == GDX12AOResolution::Half;
    switch (slot.Stage)
    {
    case PlacedStage::Pack:
        Bind(commands, slot.Constants);
        RecordPrepare(commands, frame.Width, frame.Height, half ? PrepareHalf : PackFull,
            inputs.Depth, inputs.Normals, inputs.PreparedDepth, inputs.PreparedNormals);
        break;
    case PlacedStage::Evaluate:
    {
        ScratchStates scratch(commands, true);
        RecordEvaluate(commands, *slot.Grid, frame, slot.Constants,
            inputs.PreparedDepth, inputs.PreparedNormals, inputs.Visibility, scratch);
        break;
    }
    case PlacedStage::Finalize:
        if (half)
        {
            Bind(commands, slot.Constants);
            RecordUpsample(commands, slot.Width, slot.Height, inputs);
        }
        else
            native->CopyResource(inputs.Output.Resource->D3DResource.Get(), inputs.Visibility.Resource->D3DResource.Get());
        break;
    }
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
