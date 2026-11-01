#include "Engine.RendererDX12/GDX12GrassRenderer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace
{
    template<typename T> void Hash(uint64_t& hash, const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    uint64_t SettingsHash(const GDX12GrassInstance& s)
    {
        uint64_t hash = 14695981039346656037ull;
        Hash(hash, s.Implementation); Hash(hash, s.ExtentsCm); Hash(hash, s.DensityPerSquareMetre);
        Hash(hash, s.MaxBlades); Hash(hash, s.Segments); Hash(hash, s.Seed); Hash(hash, s.HeightCm);
        Hash(hash, s.HeightVariation); Hash(hash, s.WidthCm); Hash(hash, s.BendSeconds); Hash(hash, s.MaxBendCm);
        Hash(hash, s.LodDistanceCm); Hash(hash, s.FarCardDistanceCm); Hash(hash, s.MaxDistanceCm);
        Hash(hash, s.WindId); Hash(hash, s.MaterialIndex); Hash(hash, s.MaterialRevision); Hash(hash, s.ResetRevision);
        Hash(hash, s.CastShadows); Hash(hash, s.CaptureReflections);
        Hash(hash, s.RootsCm.size());
        for (const auto& root : s.RootsCm) Hash(hash, root);
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
GDX12GrassRenderer::~GDX12GrassRenderer() = default;
GDX12GrassRenderer::Frame& GDX12GrassRenderer::CurrentFrame() { return _frames.at(_resources->CurrFrameConstantsIndex); }

template<typename T>
std::unique_ptr<GDX12UploadBuffer<T>> GDX12GrassRenderer::Buffer(UINT count, bool srv, bool uav)
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
    compute.NumSingleSRVSlots = 3;
    compute.NumSingleUAVSlots = 3;
    _computeRoot = std::make_unique<GDX12RootSignature>(_resources->Device, compute);
    GDX12RootSignatureDesc wind;
    wind.VolatileSRVData = true;
    wind.NumSingleCBVSlots = 1;
    wind.NumSingleSRVSlots = 2;
    wind.NumSingleUAVSlots = 1;
    _windRoot = std::make_unique<GDX12RootSignature>(_resources->Device, wind);
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
    const auto createCompute = [&](const wchar_t* file, const char* entry, GDX12RootSignature* root,
        const D3D_SHADER_MACRO* defines, ComPtr<ID3D12PipelineState>& target)
    {
        auto cs = compiler.CompileShader(_resources->Device, file, defines, entry, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root->GetRootSignature().Get();
        desc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&target)));
    };
    createCompute(SHADERS_FOLDER "GrassExpand.hlsl", "ExpandCS", _computeRoot.get(), nullptr, _expandPSO);
    createCompute(SHADERS_FOLDER "GrassExpand.hlsl", "ClearCS", _computeRoot.get(), nullptr, _clearPSO);
    for (UINT stage = 0; stage < _windPSO.size(); ++stage)
    {
        const auto number = std::to_string(stage);
        D3D_SHADER_MACRO defines[] = { { "WIND_STAGE", number.c_str() }, { nullptr, nullptr } };
        createCompute(SHADERS_FOLDER "WindField.hlsl", "CS", _windRoot.get(), defines, _windPSO[stage]);
    }
    for (UINT i = 0; i < _graphicsPSO.size(); ++i)
    {
        const auto stage = static_cast<Stage>(i);
        const bool depth = stage == Stage::Depth || stage == Stage::Shadow;
        const bool capture = stage == Stage::Capture;
        D3D_SHADER_MACRO captureDefines[] = { { "GRASS_CAPTURE", "1" }, { nullptr, nullptr } };
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
            depth ? "DepthVS" : "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
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

void GDX12GrassRenderer::BeginFrame(GDX12CommandList* commands, const RenderPipelineCommonData& common,
    std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls)
{
    auto& frame = CurrentFrame();
    frame.RetiredFields.clear();
    frame.RetiredVertices.clear();
    frame.RetiredIndices.clear();
    frame.RetiredGrass.clear();
    frame.DrawCursor = frame.WindCursor = 0;
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
            const auto signature = SettingsHash(source);
            state.ResetHistory = common.ResetHistory || controlChange || state.SettingsHash != signature;
            state.PreviousWorld = state.ResetHistory ? source.World : state.Source.World;
            state.SettingsHash = signature;
            state.Source = std::move(source);
            if (state.Source.WindId) activeWind.insert(state.Source.WindId);
            _drawOrder.push_back(state.Source.Id);
            const UINT ribbons = state.Source.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2u : 1u;
            vertexCapacity = (std::max)(vertexCapacity, count * (state.Source.Segments + 1) * 2 * ribbons);
            indexCapacity = (std::max)(indexCapacity, count * state.Source.Segments * 6 * ribbons);
            if (state.Roots.empty()) state.Roots.resize(_frames.size());
            auto& roots = state.Roots[_resources->CurrFrameConstantsIndex];
            const UINT rootCount = (std::max)(1u, static_cast<UINT>(state.Source.RootsCm.size()));
            if (!roots || roots->GetElementCount() < rootCount)
            {
                roots = std::make_unique<GDX12UploadBuffer<XMFLOAT3>>(_resources->Device, rootCount, EBufferType::Upload, false);
                roots->CreateSRV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            }
            if (state.Source.RootsCm.empty()) roots->CopyData(0, XMFLOAT3(0, 0, 0));
            else roots->CopyData(0, state.Source.RootsCm.data(), static_cast<UINT>(state.Source.RootsCm.size()));
        }
    }
    bool changed = controlChange;
    for (auto it = _grass.begin(); it != _grass.end();)
    {
        if (activeGrass.count(it->first)) { ++it; continue; }
        frame.RetiredGrass.push_back(std::move(it->second));
        it = _grass.erase(it);
        changed = true;
    }
    for (auto it = _wind.begin(); it != _wind.end();)
    {
        if (activeWind.count(it->first) && controls.WindEnabled && controls.WindQuality > 0) { ++it; continue; }
        RetireWind(it->second);
        it = _wind.erase(it);
        changed = true;
    }
    if (!_drawOrder.empty() && !_initialized) Initialize();
    if (!vertexCapacity)
    {
        frame.DrawParameters.clear();
        frame.WindParametersBuffers.clear();
        if (_vertices) frame.RetiredVertices.push_back(std::move(_vertices));
        if (_indices) frame.RetiredIndices.push_back(std::move(_indices));
        if (_arguments) frame.RetiredIndices.push_back(std::move(_arguments));
        if (_neutralWind) frame.RetiredFields.push_back(std::move(_neutralWind));
        if (changed) ++_revision;
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
        WindDispatch(commands, 0, neutral, nullptr, nullptr, _neutralWind.get());
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
            UpdateWind(commands, state, reset);
            changed |= reset || state.Time != state.PreviousTime;
        }
    }
    // Missing/disabled fields are neutral; never sample a retained owner's old field.
    for (auto it = _wind.begin(); it != _wind.end();)
    {
        if (submittedWind.count(it->first)) { ++it; continue; }
        RetireWind(it->second);
        it = _wind.erase(it);
        changed = true;
    }
    for (const auto id : _drawOrder)
    {
        auto& state = _grass.at(id);
        const auto field = _wind.find(state.Source.WindId);
        if (field != _wind.end()) state.ResetHistory |= field->second.ResetHistory;
        changed |= state.ResetHistory || state.Source.World != state.PreviousWorld;
    }
    if (changed) ++_revision;
}

void GDX12GrassRenderer::WindDispatch(GDX12CommandList* commands, UINT stage, const WindParameters& parameters,
    FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output)
{
    auto& frame = CurrentFrame();
    if (frame.WindCursor == frame.WindParametersBuffers.size())
        frame.WindParametersBuffers.push_back(std::make_unique<GDX12UploadBuffer<WindParameters>>(_resources->Device, 1));
    auto& constants = frame.WindParametersBuffers[frame.WindCursor++];
    constants->CopyData(0, parameters);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(_windRoot.get());
    commands->SetComputeRootConstantBufferView(0, constants->GetElementAddress(0));
    commands->SetPipelineState(_windPSO.at(stage));
    if (input)
    {
        commands->ResourceBarrier({ input->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(0, input->GetSRV()->GPUHandle);
    }
    if (pressure)
    {
        commands->ResourceBarrier({ pressure->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(1, pressure->GetSRV()->GPUHandle);
    }
    commands->ResourceBarrier({ output->GetResource().GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, output->GetUAV()->GPUHandle);
    commands->Dispatch((parameters.Resolution + 7) / 8, (parameters.Resolution + 7) / 8, 1);
    commands->ResourceBarrier({ output->GetResource().GetUAVBarrier() });
}

void GDX12GrassRenderer::UpdateWind(GDX12CommandList* commands, WindState& state, bool reset)
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
        if (state.Resolution) RetireWind(state);
        state.Time += remaining;
        return;
    }
    if (state.Resolution != s.GridResolution)
    {
        RetireWind(state);
        for (auto& buffer : state.Velocity) buffer = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        for (auto& buffer : state.Pressure) buffer = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Advected = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Divergence = Buffer<XMFLOAT4>(p.Resolution * p.Resolution, true, true);
        state.Resolution = p.Resolution;
    }
    GDX12_SCOPED_EVENT(commands, "Wind advection and projection", Colors::DarkCyan);
    if (reset || !state.Initialized)
    {
        for (auto& buffer : state.Velocity) WindDispatch(commands, 0, p, nullptr, nullptr, buffer.get());
        state.Initialized = true;
    }
    WindDispatch(commands, 5, p, state.Velocity[0].get(), nullptr, state.Velocity[1].get());
    const float cellSize = 2 * (std::min)(s.ExtentsCm.x, s.ExtentsCm.y) / s.GridResolution;
    const float stepLimit = (std::min)(s.MaxStepSeconds, 0.5f * cellSize / s.MaxSpeedCmPerSecond);
    // shortcut: excess dt is dropped; raise the component substep budget if stalls must catch up.
    for (UINT step = 0; step < s.MaxSubsteps && remaining > 0; ++step)
    {
        p.Gust.w = (std::min)(remaining, stepLimit);
        state.Time += p.Gust.w;
        p.Gust.z = state.Time;
        remaining -= p.Gust.w;
        WindDispatch(commands, 1, p, state.Velocity[0].get(), nullptr, state.Advected.get());
        WindDispatch(commands, 2, p, state.Advected.get(), nullptr, state.Divergence.get());
        WindDispatch(commands, 0, p, nullptr, nullptr, state.Pressure[0].get());
        UINT read = 0;
        for (UINT iteration = 0; iteration < s.PressureIterations; ++iteration)
        {
            WindDispatch(commands, 3, p, state.Divergence.get(), state.Pressure[read].get(), state.Pressure[read ^ 1].get());
            read ^= 1;
        }
        WindDispatch(commands, 4, p, state.Advected.get(), state.Pressure[read].get(), state.Velocity[0].get());
    }
}

bool GDX12GrassRenderer::GetBounds(BoundingBox& bounds) const
{
    bool found = false;
    for (const auto id : _drawOrder)
    {
        const auto& source = _grass.at(id).Source;
        BoundingBox box;
        GrassLocalBounds(source).Transform(box, source.World);
        box.Extents.x += source.MaxBendCm;
        box.Extents.z += source.MaxBendCm;
        if (found) BoundingBox::CreateMerged(bounds, bounds, box);
        else bounds = box;
        found = true;
    }
    return found;
}

bool GDX12GrassRenderer::HasCasters() const
{
    for (const auto id : _drawOrder) if (_grass.at(id).Source.CastShadows) return true;
    return false;
}

void GDX12GrassRenderer::Draw(GDX12CommandList* commands, Stage stage, const Matrix& viewProjection,
    Vector3 eye, D3D12_GPU_VIRTUAL_ADDRESS camera, const CaptureLighting* lighting)
{
    if (_drawOrder.empty()) return;
    if (stage == Stage::Capture && (!lighting || !camera)) throw std::invalid_argument("Grass capture requires lighting and camera");
    GDX12_SCOPED_EVENT(commands, stage == Stage::Shadow ? "Grass shadow expansion and draw" : "Grass expansion and draw", Colors::ForestGreen);
    auto& frame = CurrentFrame();
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    for (const auto id : _drawOrder)
    {
        const auto& state = _grass.at(id);
        const auto& s = state.Source;
        if ((stage == Stage::Shadow && !s.CastShadows) || (stage == Stage::Capture && !s.CaptureReflections)) continue;
        Parameters p = {};
        XMStoreFloat4x4(&p.World, s.World.Transpose());
        XMStoreFloat4x4(&p.PreviousWorld, state.PreviousWorld.Transpose());
        XMStoreFloat4x4(&p.ViewProjection, viewProjection.Transpose());
        p.EyeMaxDistance = XMFLOAT4(eye.x, eye.y, eye.z, s.MaxDistanceCm);
        p.PreviousEye = XMFLOAT4(_previousEye.x, _previousEye.y, _previousEye.z, 0);
        p.Shape = XMFLOAT4(s.HeightCm, s.WidthCm, s.HeightVariation, s.MaxBendCm);
        p.Lod = XMFLOAT4(s.LodDistanceCm, s.FarCardDistanceCm, s.BendSeconds, 0);
        p.Patch = XMFLOAT4(s.ExtentsCm.x, s.ExtentsCm.y, 0, 0);
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
            p.WindMode = source.Implementation == GDX12WindImplementation::ProjectedGrid ? 2u : 1u;
            p.GridResolution = source.GridResolution;
            if (p.WindMode == 2)
            {
                currentWind = field.Velocity[0].get();
                previousWind = field.Velocity[1].get();
            }
        }
        if (frame.DrawCursor == frame.DrawParameters.size())
            frame.DrawParameters.push_back(std::make_unique<GDX12UploadBuffer<Parameters>>(_resources->Device, 1));
        auto& parameters = frame.DrawParameters[frame.DrawCursor++];
        parameters->CopyData(0, p);
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(_computeRoot.get());
        commands->SetComputeRootConstantBufferView(0, parameters->GetElementAddress(0));
        commands->ResourceBarrier({ _vertices->GetResource().GetUnorderedAccessBarrier(),
            _indices->GetResource().GetUnorderedAccessBarrier(), _arguments->GetResource().GetUnorderedAccessBarrier(),
            currentWind->GetResource().GetNonPixelShaderResourceBarrier(), previousWind->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(0, state.Roots[_resources->CurrFrameConstantsIndex]->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, currentWind->GetSRV()->GPUHandle);
        commands->SetComputeSRV(2, previousWind->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, _vertices->GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, _indices->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2, _arguments->GetUAV()->GPUHandle);
        commands->SetPipelineState(_clearPSO);
        commands->Dispatch(1, 1, 1);
        commands->ResourceBarrier({ _arguments->GetResource().GetUAVBarrier() });
        commands->SetPipelineState(_expandPSO);
        commands->Dispatch((p.Count + 63) / 64, 1, 1);
        commands->ResourceBarrier({ _vertices->GetResource().GetSRVBarrier(),
            _indices->GetResource().GetGenericReadBarrier(), _arguments->GetResource().GetIndirectArgsBarrier() });
        commands->SetGraphicsRootSignature(_graphicsRoot.get());
        commands->SetPipelineState(_graphicsPSO.at(static_cast<size_t>(stage)));
        commands->SetGraphicsRootConstantBufferView(0, parameters->GetElementAddress(0));
        // Shadow VS uses only b0; all other stages consume the supplied unjittered history camera.
        if (camera) commands->SetGraphicsRootConstantBufferView(1, camera);
        commands->SetGraphicsRootConstantBufferView(2, constants->MainCB->GetElementAddress(0));
        commands->SetGraphicsSRV(0, _vertices->GetSRV()->GPUHandle);
        commands->SetGraphicsSRV(1, constants->MaterialCache->GetSRV()->GPUHandle);
        commands->SetGraphicsSRV(8, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
        if (stage == Stage::Capture)
        {
            commands->SetGraphicsRootConstantBufferView(3, lighting->ShadowConstants);
            commands->SetGraphicsSRV(2, lighting->SunShadow->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(3, lighting->Irradiance->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(4, lighting->Prefiltered->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(5, lighting->BRDF->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(6, constants->LocalLightCache->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(7, lighting->LocalShadows->GetSRV()->GPUHandle);
        }
        const D3D12_INDEX_BUFFER_VIEW indexView = { _indices->GetElementAddress(0),
            static_cast<UINT>(_indices->GetElementCount() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
        commands->GetCommandList()->IASetIndexBuffer(&indexView);
        commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->ExecuteIndirect(_signature.Get(), 1, _arguments->GetResource().D3DResource.Get(), 0, nullptr);
    }
    // IA was set directly; the following mesh pass must bind its own geometry again.
    commands->InvalidateCachedState();
}
