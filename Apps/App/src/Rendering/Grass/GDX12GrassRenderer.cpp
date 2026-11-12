#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include "BistroArch.h"

namespace
{
    template<typename T> void Hash(uint64_t& hash, const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    uint64_t SettingsHash(const GDX12GrassInstance& s, uint64_t& rootsHash)
    {
        uint64_t hash = 14695981039346656037ull;
        Hash(hash, s.Implementation); Hash(hash, s.ExtentsCm); Hash(hash, s.DensityPerSquareMetre);
        Hash(hash, s.MaxBlades); Hash(hash, s.Segments); Hash(hash, s.Seed); Hash(hash, s.HeightCm);
        Hash(hash, s.HeightVariation); Hash(hash, s.WidthCm); Hash(hash, s.BendSeconds); Hash(hash, s.MaxBendCm);
        Hash(hash, s.LodDistanceCm); Hash(hash, s.FarCardDistanceCm); Hash(hash, s.MaxDistanceCm);
        Hash(hash, s.WindId); Hash(hash, s.MaterialIndex); Hash(hash, s.MaterialRevision); Hash(hash, s.ResetRevision);
        Hash(hash, s.CastShadows); Hash(hash, s.CaptureReflections);
        rootsHash = 14695981039346656037ull;
        Hash(rootsHash, s.RootsCm.size());
        for (const auto& root : s.RootsCm) Hash(rootsHash, root);
        Hash(hash, rootsHash);
        return hash;
    }
    uint64_t SettingsHash(const GDX12WindInstance& s)
    {
        uint64_t hash = 14695981039346656037ull;
        Hash(hash, s.Implementation); Hash(hash, s.ExtentsCm); Hash(hash, s.Direction);
        Hash(hash, s.SpeedCmPerSecond); Hash(hash, s.GustAmplitude); Hash(hash, s.GustFrequencyHz);
        Hash(hash, s.GustWavelengthCm); Hash(hash, s.ForcePerSecond); Hash(hash, s.DampingPerSecond);
        Hash(hash, s.MaxSpeedCmPerSecond); Hash(hash, s.TimeScale); Hash(hash, s.MaxStepSeconds);
        Hash(hash, s.MaxSubsteps); Hash(hash, s.GridResolution); Hash(hash, s.PressureIterations);
        Hash(hash, s.ResetRevision); Hash(hash, s.CenterCm);
        return hash;
    }
    bool ValidWorld(const Matrix& world)
    {
        const auto* values = &world._11;
        for (UINT i = 0; i < 16; ++i) if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7f) return false;
        const float determinant = world.Determinant();
        return std::isfinite(determinant) && std::abs(determinant) >= 1e-9f;
    }
}

GDX12GrassRenderer::GDX12GrassRenderer(GDX12DeviceResources* resources) : _resources(resources), _frames(resources->FrameConstants.size()) {}
GDX12GrassRenderer::~GDX12GrassRenderer()
{ if (_primitiveRegistry) RemovePrimitives(*_primitiveRegistry); }
GDX12GrassRenderer::Frame& GDX12GrassRenderer::CurrentFrame() { return _frames.at(_resources->CurrFrameConstantsIndex); }

template<typename T>
std::unique_ptr<GDX12UploadBuffer<T>> GDX12GrassRenderer::Buffer(UINT count, const bool srv, const bool uav)
{
    auto buffer = std::make_unique<GDX12UploadBuffer<T>>(_resources->Device, count, EBufferType::Default, false);
    auto* heap = _resources->SRV_UAV_Heap.get();
    if (srv) buffer->CreateSRV(heap, heap->GetAvailableIndex(ConstantsResources));
    if (uav) buffer->CreateUAV(heap, heap->GetAvailableIndex(ConstantsResources));
    return buffer;
}

void GDX12GrassRenderer::Initialize()
{
    GDX12RootSignatureDesc compute;
    compute.VolatileSRVData = true;
    compute.NumSingleCBVSlots = 1;
    compute.NumSingleSRVSlots = 5;
    compute.NumSingleUAVSlots = 3;
    _computeRoot = std::make_unique<GDX12RootSignature>(_resources->Device, compute);
    GDX12RootSignatureDesc wind;
    wind.VolatileSRVData = true;
    wind.NumSingleCBVSlots = 1;
    wind.NumSingleSRVSlots = 2;
    wind.NumSingleUAVSlots = 1;
    _windRoot = std::make_unique<GDX12RootSignature>(_resources->Device, wind);
    GDX12RootSignatureDesc interaction;
    interaction.VolatileSRVData = true;
    interaction.NumSingleCBVSlots = 1;
    interaction.NumSingleSRVSlots = 1;
    interaction.NumSingleUAVSlots = 3;
    _interactionRoot = std::make_unique<GDX12RootSignature>(_resources->Device, interaction);
    _interactionPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _interactionRoot.get(),
        APP_SHADERS_FOLDER "GrassInteraction.hlsl", "CS");
    GDX12RootSignatureDesc graphics;
    graphics.VolatileSRVData = true;
    graphics.NumSingleCBVSlots = 4;
    graphics.NumSingleSRVSlots = 8;
    graphics.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    graphics.StaticSamplers = GetStaticSamplers();
    CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    graphics.StaticSamplers.push_back(shadowSampler);
    _graphicsRoot = std::make_unique<GDX12RootSignature>(_resources->Device, graphics);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    _expandPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ExpandCS");
    _clearPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ClearCS");
    for (UINT stage = 0; stage < _windPSO.size(); ++stage)
    {
        const auto number = std::to_string(stage);
        D3D_SHADER_MACRO defines[] = { { "WIND_STAGE", number.c_str() }, { nullptr, nullptr } };
        _windPSO[stage] = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _windRoot.get(), APP_SHADERS_FOLDER "WindField.hlsl", "CS", defines);
    }
    for (UINT i = 0; i < _graphicsPSO.size(); ++i)
    {
        const auto stage = static_cast<Stage>(i);
        const bool depth = stage == Stage::Depth || stage == Stage::Shadow;
        const bool capture = stage == Stage::Capture;
        D3D_SHADER_MACRO captureDefines[] = { { "GRASS_CAPTURE", "1" }, { nullptr, nullptr } };
        auto vs = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
            depth ? "DepthVS" : "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
            depth ? "DepthPS" : (capture ? "CapturePS" : (stage == Stage::PrepassNormals ? "PrepassPS" : "PS")), "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _graphicsRoot->GetRootSignature().Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.FrontCounterClockwise = TRUE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = stage == Stage::Shadow ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        if (stage == Stage::GBuffer)
        {
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        if (stage == Stage::Shadow)
        {
            desc.RasterizerState.DepthBias = 250;
            desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        }
        desc.NumRenderTargets = depth ? 0 : (capture || stage == Stage::PrepassNormals ? 1 : 5);
        if (capture || stage == Stage::PrepassNormals) desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        else if (!depth)
        {
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
            desc.RTVFormats[2] = desc.RTVFormats[3] = desc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleMask = UINT_MAX;
        desc.SampleDesc.Count = 1;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_graphicsPSO[i])));
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&_signature)));
    _initialized = true;
}

void GDX12GrassRenderer::RetireWind(WindState& state)
{
    auto& retired = CurrentFrame().RetiredFields;
    for (auto& buffer : state.Velocity) if (buffer) retired.push_back(std::move(buffer));
    for (auto& buffer : state.Pressure) if (buffer) retired.push_back(std::move(buffer));
    if (state.Advected) retired.push_back(std::move(state.Advected));
    if (state.Divergence) retired.push_back(std::move(state.Divergence));
    state.Initialized = false;
    state.Resolution = 0;
}

void GDX12GrassRenderer::BeginFrame(const RenderPipelineCommonData& common,
    std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls,
    const std::vector<BistroInteractionSource>& sources, float deltaSeconds)
{
    auto& frame = CurrentFrame();
    frame.RetiredFields.clear();
    frame.RetiredVertices.clear();
    frame.RetiredIndices.clear();
    frame.RetiredGrass.clear();
    frame.DrawCursor = frame.WindCursor = 0;
    frame.WindJobs.clear();
    frame.RootUploads.clear();
    frame.Outputs.clear();
    frame.OutputIndex.clear();
    _arenaOutput = nullptr;
    if (!_initialized) Initialize();
    if (!_presence)
    {
        _presence = Buffer<XMFLOAT4>(InteractionResolution * InteractionResolution, true, true);
        _trail = Buffer<XMFLOAT4>(InteractionResolution * InteractionResolution, true, true);
        _previousTrail = Buffer<XMFLOAT4>(InteractionResolution * InteractionResolution, true, true);
        BoundingBox area;
        BistroArch::SwarmBounds.Transform(area, Matrix::CreateTranslation(-Vector3(BistroArch::SwarmBounds.Center)) *
            Matrix::CreateRotationY(XMConvertToRadians(BistroArch::YawDegrees)) *
            Matrix::CreateTranslation(Vector3(BistroArch::SwarmBounds.Center)));
        _interactionArea = XMFLOAT4(area.Center.x, area.Center.z, area.Extents.x + 100, area.Extents.z + 100);
    }
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0) throw std::invalid_argument("Invalid interaction dt");
    std::vector<BistroInteractionSource> nearby;
    for (const auto& source : sources)
    {
        const auto& p = source.PositionRadius;
        const auto& q = source.PreviousHeight;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.w) ||
            !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
            p.w <= 0 || p.w > 1000 || q.w <= 0 || q.w > 10000)
            throw std::invalid_argument("Invalid interaction source");
        if ((std::min)(p.x, q.x) - p.w > _interactionArea.x + _interactionArea.z ||
            (std::max)(p.x, q.x) + p.w < _interactionArea.x - _interactionArea.z ||
            (std::min)(p.z, q.z) - p.w > _interactionArea.y + _interactionArea.w ||
            (std::max)(p.z, q.z) + p.w < _interactionArea.y - _interactionArea.w) continue;
        nearby.push_back(source);
    }
    const UINT sourceCount = static_cast<UINT>(nearby.size());
    if (!frame.Sources || frame.Sources->GetElementCount() < (std::max)(1u, sourceCount))
    {
        frame.Sources = std::make_unique<GDX12UploadBuffer<BistroInteractionSource>>(_resources->Device,
            (std::max)(1u, sourceCount), EBufferType::Upload, false);
        frame.Sources->CreateSRV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }
    if (sourceCount) frame.Sources->CopyData(0, nearby.data(), sourceCount);
    if (!frame.InteractionConstants)
        frame.InteractionConstants = std::make_unique<GDX12UploadBuffer<InteractionParameters>>(_resources->Device, 1);
    const bool controlChange = controls.GrassEnabled != _controls.GrassEnabled || controls.GrassQuality != _controls.GrassQuality ||
        controls.WindEnabled != _controls.WindEnabled || controls.WindQuality != _controls.WindQuality;
    _controls = controls;
    _previousEye = _eye;
    _eye = common.CameraView.Invert().Translation();
    if (common.ResetHistory) _previousEye = _eye;
    _drawOrder.clear();
    std::unordered_set<uint64_t> activeGrass, activeWind;
    UINT vertexCapacity = 0, indexCapacity = 0;
    if (controls.GrassEnabled && controls.GrassQuality > 0)
    {
        for (auto& source : grass)
        {
            if (!source.Id || !IsValidGrassSettings(source) || !ValidWorld(source.World) ||
                source.MaterialIndex >= _resources->FrameConstants[_resources->CurrFrameConstantsIndex]->MaterialCache->GetElementCount())
                throw std::invalid_argument("Invalid grass instance");
            static_cast<GDX12GrassSettings&>(source) = ResolveGrassSettings(source, controls.GrassQuality);
            const UINT count = GrassBladeCount(source);
            if (!count) continue;
            if (!activeGrass.insert(source.Id).second) throw std::invalid_argument("Duplicate grass owner");
            auto& state = _grass[source.Id];
            uint64_t rootsHash;
            const auto signature = SettingsHash(source, rootsHash);
            state.ResetHistory = common.ResetHistory || controlChange || state.SettingsHash != signature;
            _resetInteraction |= state.SettingsHash != signature;
            state.PreviousWorld = state.ResetHistory ? source.World : state.Source.World;
            state.SettingsHash = signature;
            state.Source = std::move(source);
            GrassLocalBounds(state.Source).Transform(state.Bounds, state.Source.World);
            state.Bounds.Extents.x += state.Source.MaxBendCm;
            state.Bounds.Extents.z += state.Source.MaxBendCm;
            const float interactionReach = state.Source.HeightCm * (1 + state.Source.HeightVariation) *
                Vector3::TransformNormal(Vector3::UnitY, state.Source.World).Length();
            state.Bounds.Extents.x += interactionReach;
            state.Bounds.Extents.z += interactionReach;
            if (state.Source.WindId) activeWind.insert(state.Source.WindId);
            _drawOrder.push_back(state.Source.Id);
            const UINT ribbons = state.Source.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2u : 1u;
            vertexCapacity = (std::max)(vertexCapacity, count * (state.Source.Segments + 1) * 2 * ribbons);
            indexCapacity = (std::max)(indexCapacity, count * state.Source.Segments * 6 * ribbons);
            if (state.Roots.empty())
            {
                state.Roots.resize(_frames.size());
                state.RootsUpload.resize(_frames.size());
                state.RootSignatures.resize(_frames.size());
            }
            const auto slot = _resources->CurrFrameConstantsIndex;
            auto& roots = state.Roots[slot];
            auto& upload = state.RootsUpload[slot];
            const UINT rootCount = (std::max)(1u, static_cast<UINT>(state.Source.RootsCm.size()));
            bool dirty = state.RootSignatures[slot] != rootsHash;
            if (!roots || roots->GetElementCount() < rootCount)
            {
                roots = Buffer<XMFLOAT3>(rootCount, true, false);
                upload = std::make_unique<GDX12UploadBuffer<XMFLOAT3>>(_resources->Device, rootCount, EBufferType::Upload, false);
                dirty = true;
            }
            if (dirty)
            {
                if (state.Source.RootsCm.empty()) upload->CopyData(0, XMFLOAT3(0, 0, 0));
                else upload->CopyData(0, state.Source.RootsCm.data(), static_cast<UINT>(state.Source.RootsCm.size()));
                frame.RootUploads.push_back({ roots.get(), upload.get() });
                state.RootSignatures[slot] = rootsHash;
            }
        }
    }
    for (auto it = _grass.begin(); it != _grass.end();)
    {
        if (activeGrass.count(it->first)) { ++it; continue; }
        _removedPrimitives.push_back(it->second.Primitive);
        frame.RetiredGrass.push_back(std::move(it->second));
        it = _grass.erase(it);
    }
    for (auto it = _wind.begin(); it != _wind.end();)
    {
        if (activeWind.count(it->first) && controls.WindEnabled && controls.WindQuality > 0) { ++it; continue; }
        RetireWind(it->second);
        it = _wind.erase(it);
    }
    InteractionParameters interaction = {};
    interaction.Area = _interactionArea;
    interaction.DeltaSeconds = deltaSeconds;
    interaction.SourceCount = sourceCount;
    interaction.Reset = _resetInteraction;
    if (deltaSeconds > 0 || _resetInteraction) ++_interactionRevision;
    interaction.Resolution = InteractionResolution;
    frame.InteractionConstants->CopyData(0, interaction);
    _resetInteraction = false;
    if (!vertexCapacity)
    {
        frame.DrawParameters.clear();
        frame.WindParametersBuffers.clear();
        if (_vertices) frame.RetiredVertices.push_back(std::move(_vertices));
        if (_indices) frame.RetiredIndices.push_back(std::move(_indices));
        if (_arguments) frame.RetiredIndices.push_back(std::move(_arguments));
        if (_neutralWind) frame.RetiredFields.push_back(std::move(_neutralWind));
        return;
    }
    if (!_vertices || _vertices->GetElementCount() < vertexCapacity)
    {
        if (_vertices) frame.RetiredVertices.push_back(std::move(_vertices));
        _vertices = Buffer<Vertex>(vertexCapacity, true, true);
    }
    if (!_indices || _indices->GetElementCount() < indexCapacity)
    {
        if (_indices) frame.RetiredIndices.push_back(std::move(_indices));
        _indices = Buffer<UINT>(indexCapacity, false, true);
    }
    if (!_arguments) _arguments = Buffer<UINT>(8, false, true);
    if (!_neutralWind)
    {
        _neutralWind = Buffer<XMFLOAT4>(1, true, true);
        WindParameters neutral = {};
        neutral.Resolution = 1;
        WindDispatch(0, neutral, nullptr, nullptr, _neutralWind.get());
    }
    std::unordered_set<uint64_t> submittedWind;
    if (controls.WindEnabled && controls.WindQuality > 0)
    {
        for (auto& source : wind)
        {
            if (!activeWind.count(source.Id)) continue;
            if (!IsValidWindSettings(source) || !std::isfinite(source.CenterCm.x) || !std::isfinite(source.CenterCm.y) ||
                !std::isfinite(source.DeltaSeconds) || source.DeltaSeconds < 0) throw std::invalid_argument("Invalid wind instance");
            if (!submittedWind.insert(source.Id).second) throw std::invalid_argument("Duplicate wind owner");
            static_cast<GDX12WindSettings&>(source) = ResolveWindSettings(source, controls.WindQuality);
            const auto signature = SettingsHash(source);
            auto& state = _wind[source.Id];
            const bool reset = controlChange || signature != state.SettingsHash;
            state.Source = std::move(source);
            state.SettingsHash = signature;
            state.ResetHistory = reset;
            UpdateWind(state, reset);
        }
    }
    // Missing/disabled fields are neutral; never sample a retained owner's old field.
    for (auto it = _wind.begin(); it != _wind.end();)
    {
        if (submittedWind.count(it->first)) { ++it; continue; }
        RetireWind(it->second);
        it = _wind.erase(it);
    }
    for (const auto id : _drawOrder)
    {
        auto& state = _grass.at(id);
        const auto field = _wind.find(state.Source.WindId);
        if (field != _wind.end()) state.ResetHistory |= field->second.ResetHistory;
    }
}

void GDX12GrassRenderer::WindDispatch(const UINT stage, const WindParameters& parameters,
    FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output)
{
    auto& frame = CurrentFrame();
    if (frame.WindCursor == frame.WindParametersBuffers.size())
        frame.WindParametersBuffers.push_back(std::make_unique<GDX12UploadBuffer<WindParameters>>(_resources->Device, 1));
    auto& constants = frame.WindParametersBuffers[frame.WindCursor++];
    constants->CopyData(0, parameters);
    frame.WindJobs.push_back({ stage, parameters.Resolution, constants.get(), input, pressure, output });
}

void GDX12GrassRenderer::PrepareGPU(GDX12CommandList* commands)
{
    auto& frame = CurrentFrame();
    for (const auto& [roots, upload] : frame.RootUploads)
    {
        commands->ResourceBarrier({ roots->GetResource().GetCopyDestBarrier() });
        commands->GetCommandList()->CopyBufferRegion(roots->GetResource().D3DResource.Get(), 0,
            upload->GetResource().D3DResource.Get(), 0, UINT64(roots->GetElementCount()) * sizeof(XMFLOAT3));
        commands->ResourceBarrier({ roots->GetResource().GetNonPixelShaderResourceBarrier() });
    }
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_interactionRoot.get());
    commands->SetPipelineState(_interactionPSO);
    commands->SetComputeRootConstantBufferView(0, frame.InteractionConstants->GetElementAddress(0));
    commands->SetComputeSRV(0, frame.Sources->GetSRV()->GPUHandle);
    commands->ResourceBarrier({ _trail->GetResource().GetUnorderedAccessBarrier(),
        _previousTrail->GetResource().GetUnorderedAccessBarrier(), _presence->GetResource().GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, _trail->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _previousTrail->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _presence->GetUAV()->GPUHandle);
    commands->Dispatch((InteractionResolution + 7) / 8, (InteractionResolution + 7) / 8, 1);
    commands->ResourceBarrier({ _trail->GetResource().GetUAVBarrier(), _previousTrail->GetResource().GetUAVBarrier(),
        _presence->GetResource().GetUAVBarrier() });
    for (const auto& job : frame.WindJobs)
    {
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(_windRoot.get());
        commands->SetComputeRootConstantBufferView(0, job.Constants->GetElementAddress(0));
        commands->SetPipelineState(_windPSO.at(job.Stage));
        if (job.Input)
        {
            commands->ResourceBarrier({ job.Input->GetResource().GetNonPixelShaderResourceBarrier() });
            commands->SetComputeSRV(0, job.Input->GetSRV()->GPUHandle);
        }
        if (job.Pressure)
        {
            commands->ResourceBarrier({ job.Pressure->GetResource().GetNonPixelShaderResourceBarrier() });
            commands->SetComputeSRV(1, job.Pressure->GetSRV()->GPUHandle);
        }
        commands->ResourceBarrier({ job.Output->GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, job.Output->GetUAV()->GPUHandle);
        commands->Dispatch((job.Resolution + 7) / 8, (job.Resolution + 7) / 8, 1);
        commands->ResourceBarrier({ job.Output->GetResource().GetUAVBarrier() });
    }
}

void GDX12GrassRenderer::UpdateWind(WindState& state, const bool reset)
{
    const auto& s = state.Source;
    WindParameters p = {};
    auto direction = s.Direction;
    direction.Normalize();
    p.Area = XMFLOAT4(s.CenterCm.x, s.CenterCm.y, s.ExtentsCm.x, s.ExtentsCm.y);
    p.Force = XMFLOAT4(direction.x, direction.y, s.SpeedCmPerSecond, s.GustAmplitude);
    p.Gust = XMFLOAT4(s.GustFrequencyHz, s.GustWavelengthCm, 0, 0);
    p.Solver = XMFLOAT4(s.ForcePerSecond, s.DampingPerSecond, s.MaxSpeedCmPerSecond, 0);
    p.Resolution = s.GridResolution;
    if (reset) state.Time = 0;
    state.PreviousTime = state.Time;
    float remaining = (std::min)(s.DeltaSeconds * s.TimeScale, s.MaxSubsteps * s.MaxStepSeconds);
    if (s.Implementation == GDX12WindImplementation::Analytic)
    {
        if (state.Resolution != s.GridResolution || state.Advected)
        {
            RetireWind(state);
            for (auto& buffer : state.Velocity) buffer = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
            state.Resolution = s.GridResolution;
        }
        state.Time += remaining;
        p.Gust.z = state.Time;
        WindDispatch(6, p, nullptr, nullptr, state.Velocity[0].get());
        p.Gust.z = reset ? state.Time : state.PreviousTime;
        WindDispatch(6, p, nullptr, nullptr, state.Velocity[1].get());
        return;
    }
    if (state.Resolution != s.GridResolution || !state.Advected)
    {
        RetireWind(state);
        for (auto& buffer : state.Velocity) buffer = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        for (auto& buffer : state.Pressure) buffer = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Advected = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Divergence = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Resolution = p.Resolution;
    }
    if (reset || !state.Initialized)
    {
        for (auto& buffer : state.Velocity) WindDispatch(0, p, nullptr, nullptr, buffer.get());
        state.Initialized = true;
    }
    WindDispatch(5, p, state.Velocity[0].get(), nullptr, state.Velocity[1].get());
    const float cellSize = 2 * (std::min)(s.ExtentsCm.x, s.ExtentsCm.y) / s.GridResolution;
    const float stepLimit = (std::min)(s.MaxStepSeconds, 0.5f * cellSize / s.MaxSpeedCmPerSecond);
    // shortcut: excess dt is dropped; raise the component substep budget if stalls must catch up.
    for (UINT step = 0; step < s.MaxSubsteps && remaining > 0; ++step)
    {
        p.Gust.w = (std::min)(remaining, stepLimit);
        state.Time += p.Gust.w;
        p.Gust.z = state.Time;
        remaining -= p.Gust.w;
        WindDispatch(1, p, state.Velocity[0].get(), nullptr, state.Advected.get());
        WindDispatch(2, p, state.Advected.get(), nullptr, state.Divergence.get());
        WindDispatch(0, p, nullptr, nullptr, state.Pressure[0].get());
        UINT read = 0;
        for (UINT iteration = 0; iteration < s.PressureIterations; ++iteration)
        {
            WindDispatch(3, p, state.Divergence.get(), state.Pressure[read].get(), state.Pressure[read ^ 1].get());
            read ^= 1;
        }
        WindDispatch(4, p, state.Advected.get(), state.Pressure[read].get(), state.Velocity[0].get());
    }
}

bool GDX12GrassRenderer::HasCasters() const
{
    for (const auto id : _drawOrder) if (_grass.at(id).Source.CastShadows) return true;
    return false;
}

GDX12GrassRenderer::Stage GDX12GrassRenderer::RenderStage(GDX12GeometryStage stage)
{
    switch (stage)
    {
    case GDX12GeometryStage::Depth: return Stage::Depth;
    case GDX12GeometryStage::DepthNormals: return Stage::PrepassNormals;
    case GDX12GeometryStage::GBuffer: return Stage::GBuffer;
    case GDX12GeometryStage::Forward: return Stage::Full;
    case GDX12GeometryStage::Shadow: return Stage::Shadow;
    case GDX12GeometryStage::ReflectionCapture: return Stage::Capture;
    default: throw std::invalid_argument("Grass has no transparent material batches");
    }
}

GDX12GrassRenderer::Output* GDX12GrassRenderer::FindOutput(const GDX12GeometryContext& view, UINT patch) const
{
    if (view.FrameSlot != _resources->CurrFrameConstantsIndex) throw std::invalid_argument("Grass view uses an inactive frame slot");
    const auto& index = _frames.at(view.FrameSlot).OutputIndex;
    const auto found = index.find({ view.Key, view.Stage, patch });
    return found == index.end() ? nullptr : found->second;
}

void GDX12GrassRenderer::PrepareView(const GDX12GeometryContext& view, UINT patch)
{
    auto& frame = CurrentFrame();
    frame.OutputIndex.erase({ view.Key, view.Stage, patch });
    if (view.Filter == GDX12GeometryFilter::Static || view.Stage == GDX12GeometryStage::Transparency || patch >= _drawOrder.size()) return;
    const auto stage = RenderStage(view.Stage);
    const auto id = _drawOrder.at(patch);
    const auto& state = _grass.at(id);
    const auto& s = state.Source;
    if ((stage == Stage::Shadow && !s.CastShadows) || (stage == Stage::Capture && !s.CaptureReflections)) return;
    if (stage == Stage::Capture && (!view.CaptureLighting || !view.CameraConstants))
        throw std::invalid_argument("Grass capture requires lighting and camera");
    XMFLOAT4X4 clip;
    XMStoreFloat4x4(&clip, view.ViewProjection.Transpose());
    if (!LocalShadowFaceIntersects(clip, state.Bounds)) return;
    Parameters p = {};
    XMStoreFloat4x4(&p.World, s.World.Transpose());
    XMStoreFloat4x4(&p.PreviousWorld, state.PreviousWorld.Transpose());
    p.ViewProjection = clip;
    p.EyeMaxDistance = XMFLOAT4(view.Eye.x, view.Eye.y, view.Eye.z, s.MaxDistanceCm);
    p.PreviousEye = XMFLOAT4(_previousEye.x, _previousEye.y, _previousEye.z, 0);
    p.Shape = XMFLOAT4(s.HeightCm, s.WidthCm, s.HeightVariation, s.MaxBendCm);
    p.Lod = XMFLOAT4(s.LodDistanceCm, s.FarCardDistanceCm, s.BendSeconds, 0);
    p.Patch = XMFLOAT4(s.ExtentsCm.x, s.ExtentsCm.y, 0, 0);
    p.InteractionArea = _interactionArea;
    p.Padding0 = InteractionResolution;
    p.Count = GrassBladeCount(s);
    p.Segments = s.Segments;
    p.Implementation = static_cast<UINT>(s.Implementation);
    p.Seed = s.Seed;
    p.PointSource = !s.RootsCm.empty();
    p.Material = s.MaterialIndex;
    p.Shadow = stage == Stage::Shadow;
    p.ResetHistory = state.ResetHistory;
    FieldBuffer* currentWind = _neutralWind.get();
    FieldBuffer* previousWind = _neutralWind.get();
    const auto wind = _wind.find(s.WindId);
    if (wind != _wind.end())
    {
        const auto& field = wind->second;
        const auto& source = field.Source;
        auto direction = source.Direction;
        direction.Normalize();
        p.WindArea = XMFLOAT4(source.CenterCm.x, source.CenterCm.y, source.ExtentsCm.x, source.ExtentsCm.y);
        p.WindForce = XMFLOAT4(direction.x, direction.y, source.SpeedCmPerSecond, source.GustAmplitude);
        p.WindGust = XMFLOAT4(source.GustFrequencyHz, source.GustWavelengthCm, source.MaxSpeedCmPerSecond, 0);
        p.Patch.z = field.Time;
        p.Patch.w = field.PreviousTime;
        p.WindMode = 2u;
        p.GridResolution = source.GridResolution;
        currentWind = field.Velocity[0].get();
        previousWind = field.Velocity[1].get();
    }
    const UINT block = frame.DrawCursor / ParametersPerBuffer;
    const UINT element = frame.DrawCursor++ % ParametersPerBuffer;
    if (block == frame.DrawParameters.size())
        frame.DrawParameters.push_back(std::make_unique<GDX12UploadBuffer<Parameters>>(_resources->Device, ParametersPerBuffer));
    auto& parameters = frame.DrawParameters[block];
    parameters->CopyData(element, p);

    auto output = std::make_unique<Output>();
    output->View = view;
    if (view.CaptureLighting)
    {
        output->Lighting = *view.CaptureLighting;
        output->View.CaptureLighting = &output->Lighting;
    }
    output->Patch = patch;
    output->Owner = id;
    output->Constants = parameters.get();
    output->ConstantIndex = element;
    output->CurrentWind = currentWind;
    output->PreviousWind = previousWind;
    auto& binding = output->Binding;
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    binding.Root = _graphicsRoot.get();
    binding.Pipeline = _graphicsPSO.at(static_cast<size_t>(stage));
    binding.IndirectSignature = _signature.Get();
    binding.IndirectStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    binding.Stage = view.Stage;
    if (stage == Stage::Full || stage == Stage::GBuffer)
    {
        binding.ColorTargetCount = 5;
        binding.ColorFormats = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    }
    else if (stage == Stage::PrepassNormals || stage == Stage::Capture)
    {
        binding.ColorTargetCount = 1;
        binding.ColorFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
    binding.Heap = _resources->SRV_UAV_Heap.get();
    binding.CBVs.push_back({ 0, parameters->GetElementAddress(element) });
    if (view.CameraConstants) binding.CBVs.push_back({ 1, view.CameraConstants });
    binding.CBVs.push_back({ 2, constants->MainCB->GetElementAddress(0) });
    binding.SRVs = { { 0, _vertices->GetSRV()->GPUHandle },
        { 1, constants->ShadowMaterials->GetSRV()->GPUHandle },
        { 8, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) } };
    if (stage == Stage::Capture)
    {
        const auto& lighting = output->Lighting;
        binding.CBVs.push_back({ 3, lighting.ShadowConstants });
        binding.SRVs.insert(binding.SRVs.end(), {
            { 2, lighting.SunShadow->GetSRV()->GPUHandle }, { 3, lighting.Irradiance->GetSRV()->GPUHandle },
            { 4, lighting.Prefiltered->GetSRV()->GPUHandle }, { 5, lighting.BRDF->GetSRV()->GPUHandle },
            { 6, constants->ShadowLights->GetSRV()->GPUHandle }, { 7, lighting.LocalShadows->GetSRV()->GPUHandle } });
    }
    auto& batch = output->Batch;
    batch.Binding = &binding;
    batch.ExplicitGeometry = true;
    batch.IndexBuffer = { _indices->GetElementAddress(0), static_cast<UINT>(_indices->GetElementCount() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
    batch.InstanceCount = 1;
    batch.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(), _arguments->GetResource().D3DResource.Get(),
        0, nullptr, 0, 1, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) };
    batch.BufferAccesses = {
        { &_vertices->GetResource(), GDX12RenderGraph::Mode::Read },
        { &_indices->GetResource(), GDX12RenderGraph::Mode::Read },
        { &_arguments->GetResource(), GDX12RenderGraph::Mode::Read },
        { &constants->ShadowMaterials->GetResource(), GDX12RenderGraph::Mode::Read },
        { &constants->ShadowLights->GetResource(), GDX12RenderGraph::Mode::Read },
        { &parameters->GetResource(), GDX12RenderGraph::Mode::Read } };
    frame.OutputIndex[{view.Key, view.Stage, patch}] = output.get();
    frame.Outputs.push_back(std::move(output));
}

bool GDX12GrassRenderer::Expand(GDX12CommandList* commands, const GDX12GeometryContext& view, UINT patch)
{
    GDX12_SCOPED_EVENT(commands, "Bistro grass expansion");
    auto* output = FindOutput(view, patch);
    if (!output) return false;
    const auto& state = _grass.at(output->Owner);
    const auto& s = state.Source;
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_computeRoot.get());
    commands->SetComputeRootConstantBufferView(0, output->Constants->GetElementAddress(output->ConstantIndex));
    commands->ResourceBarrier({ _vertices->GetResource().GetUnorderedAccessBarrier(),
        _indices->GetResource().GetUnorderedAccessBarrier(), _arguments->GetResource().GetUnorderedAccessBarrier(),
        output->CurrentWind->GetResource().GetNonPixelShaderResourceBarrier(),
        output->PreviousWind->GetResource().GetNonPixelShaderResourceBarrier() });
    commands->SetComputeSRV(0, state.Roots[view.FrameSlot]->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, output->CurrentWind->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, output->PreviousWind->GetSRV()->GPUHandle);
    commands->ResourceBarrier({ _trail->GetResource().GetNonPixelShaderResourceBarrier(),
        _previousTrail->GetResource().GetNonPixelShaderResourceBarrier() });
    commands->SetComputeSRV(3, _trail->GetSRV()->GPUHandle);
    commands->SetComputeSRV(4, _previousTrail->GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, _vertices->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _indices->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _arguments->GetUAV()->GPUHandle);
    commands->SetPipelineState(_clearPSO);
    commands->Dispatch(1, 1, 1);
    commands->ResourceBarrier({ _arguments->GetResource().GetUAVBarrier() });
    commands->SetPipelineState(_expandPSO);
    commands->Dispatch((GrassBladeCount(s) + 63) / 64, 1, 1);
    commands->ResourceBarrier({ _vertices->GetResource().GetSRVBarrier(),
        _indices->GetResource().GetGenericReadBarrier(), _arguments->GetResource().GetIndirectArgsBarrier() });
    _arenaOutput = output;
    return true;
}

void GDX12GrassRenderer::GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const
{
    if (view.Filter == GDX12GeometryFilter::Static || view.Stage == GDX12GeometryStage::Transparency) return;
    const auto count = static_cast<UINT>(_drawOrder.size());
    const UINT end = view.BatchCount > count - (std::min)(view.BatchOffset, count) ? count : view.BatchOffset + view.BatchCount;
    for (UINT patch = view.BatchOffset; patch < end; ++patch)
    {
        const auto* output = FindOutput(view, patch);
        if (!output) continue;
        if (_arenaOutput != output) throw std::logic_error("Consume a prepared geometry range before reusing its direct-queue arena");
        batches.push_back(output->Batch);
    }
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::PreparationAccesses() const
{
    using Mode = GDX12RenderGraph::Mode;
    std::vector<GDX12RenderGraph::BufferAccess> accesses;
    const auto& frame = _frames.at(_resources->CurrFrameConstantsIndex);
    for (const auto& [roots, upload] : frame.RootUploads)
    {
        accesses.push_back({ &roots->GetResource(), Mode::Write });
        accesses.push_back({ &upload->GetResource(), Mode::Read });
    }
    accesses.push_back({ &frame.Sources->GetResource(), Mode::Read });
    accesses.push_back({ &frame.InteractionConstants->GetResource(), Mode::Read });
    accesses.push_back({ &_presence->GetResource(), Mode::Write });
    accesses.push_back({ &_trail->GetResource(), Mode::ReadWrite });
    accesses.push_back({ &_previousTrail->GetResource(), Mode::Write });
    for (const auto& job : frame.WindJobs)
    {
        accesses.push_back({ &job.Constants->GetResource(), Mode::Read });
        if (job.Input) accesses.push_back({ &job.Input->GetResource(), Mode::ReadWrite });
        if (job.Pressure) accesses.push_back({ &job.Pressure->GetResource(), Mode::ReadWrite });
        accesses.push_back({ &job.Output->GetResource(), Mode::ReadWrite });
    }
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::ExpansionAccesses(const GDX12GeometryContext& view, UINT patch) const
{
    using Mode = GDX12RenderGraph::Mode;
    const auto* output = FindOutput(view, patch);
    if (!output) return {};
    return { { &_trail->GetResource(), Mode::Read }, { &_previousTrail->GetResource(), Mode::Read },
        { &_grass.at(output->Owner).Roots[view.FrameSlot]->GetResource(), Mode::Read },
        { &output->Constants->GetResource(), Mode::Read },
        { &output->CurrentWind->GetResource(), Mode::Read }, { &output->PreviousWind->GetResource(), Mode::Read },
        { &_vertices->GetResource(), Mode::Write }, { &_indices->GetResource(), Mode::Write }, { &_arguments->GetResource(), Mode::Write } };
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::DrawAccesses() const
{
    using Mode = GDX12RenderGraph::Mode;
    if (!_vertices) return {};
    const auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    std::vector<GDX12RenderGraph::BufferAccess> accesses = {
        { &_vertices->GetResource(), Mode::Read }, { &_indices->GetResource(), Mode::Read }, { &_arguments->GetResource(), Mode::Read },
        { &constants->ShadowMaterials->GetResource(), Mode::Read }, { &constants->ShadowLights->GetResource(), Mode::Read } };
    const auto& frame = _frames.at(_resources->CurrFrameConstantsIndex);
    const UINT blocks = (frame.DrawCursor + ParametersPerBuffer - 1) / ParametersPerBuffer;
    for (UINT block = 0; block < blocks; ++block)
        accesses.push_back({ &frame.DrawParameters[block]->GetResource(), Mode::Read });
    return accesses;
}

void GDX12GrassRenderer::PublishPrimitives(GDX12RenderPrimitiveRegistry& registry, const IRenderGeometrySource* source)
{
    _primitiveRegistry = &registry;
    for (auto handle : _removedPrimitives) registry.Remove(handle);
    _removedPrimitives.clear();
    for (const auto id : _drawOrder)
    {
        auto& state = _grass.at(id);
        const auto& s = state.Source;
        GDX12RenderPrimitive primitive;
        primitive.Bounds = state.Bounds;
        primitive.Dynamic = true;
        primitive.CastsShadows = s.CastShadows;
        primitive.CapturedInReflections = s.CaptureReflections;
        primitive.MaterialIndex = s.MaterialIndex;
        primitive.GeometryRevision = state.SettingsHash;
        primitive.MaterialRevision = s.MaterialRevision;
        primitive.TransformRevision = 14695981039346656037ull;
        Hash(primitive.TransformRevision, s.World);
        const auto wind = _wind.find(s.WindId);
        if (wind != _wind.end())
        {
            primitive.DeformationRevision = wind->second.SettingsHash;
            Hash(primitive.DeformationRevision, wind->second.Time);
        }
        Hash(primitive.DeformationRevision, _interactionRevision);
        primitive.Source = source;
        if (registry.Get(state.Primitive)) registry.Update(state.Primitive, primitive);
        else state.Primitive = registry.Add(primitive);
    }
}

void GDX12GrassRenderer::RemovePrimitives(GDX12RenderPrimitiveRegistry& registry)
{
    for (const auto& [id, state] : _grass) registry.Remove(state.Primitive);
    for (auto handle : _removedPrimitives) registry.Remove(handle);
    _removedPrimitives.clear();
    _primitiveRegistry = nullptr;
}

uint64_t GDX12GrassRenderer::GeometryAllocationBytes() const
{
    uint64_t bytes = 0;
    const auto add = [&](const auto& buffer)
    {
        if (!buffer) return;
        const auto desc = buffer->GetResource().D3DResource->GetDesc();
        bytes += _resources->Device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    };
    add(_vertices); add(_indices); add(_arguments);
    return bytes;
}

GDX12ParticleInteractionField GDX12GrassRenderer::InteractionField() const
{
    return { &_presence->GetResource(), _presence->GetSRV()->GPUHandle, _interactionArea,
        InteractionResolution, BistroArch::GrassPosition.y };
}
