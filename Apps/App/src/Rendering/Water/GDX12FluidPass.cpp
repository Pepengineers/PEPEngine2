#include "Rendering/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

struct GDX12FluidSurfacePass::Parameters
{
    XMFLOAT4X4 ContainerToWorld, WorldToContainer;
    XMFLOAT3 HalfExtent; float Radius;
    XMFLOAT3 Gravity; float H;
    float Mass, RestDensity, Stiffness, Viscosity;
    float MaxSpeed, MaxAcceleration, Restitution, Sigma;
    float DepthRange, EdgeDepth, IOR, Roughness;
    XMFLOAT3 Absorption; float RefractionPixels;
    XMFLOAT3 Scattering; float MaxThickness;
    UINT FilterRadius, ResetMotion;
    float NearStiffness, PredictionSeconds;
};

struct GDX12FluidSurfacePass::State
{
    struct Particle { XMFLOAT3 Position, Velocity, PreviousPosition; };
    std::unique_ptr<GDX12UploadBuffer<Particle>> Particles[2], SeedUpload;
    std::unique_ptr<GDX12UploadBuffer<XMFLOAT2>> Density;
    std::unique_ptr<GDX12UploadBuffer<XMUINT2>> Keys;
    std::unique_ptr<GDX12UploadBuffer<UINT>> Offsets;
    std::unique_ptr<GDX12UploadBuffer<Parameters>> Constants;
    std::shared_ptr<const std::vector<GDX12FluidParticle>> Source;
    uint64_t Revision = 0;
    uint64_t SettingsRevision = 0;
    UINT Count = 0, Capacity = 0, Current = 0;
    bool SeedPending = true;
    UINT SeedFrameSlot = UINT_MAX;
    bool WasRendered = false;
    bool ResetThisFrame = false;
    Parameters FrameParameters = {};
};

namespace
{
    template<class T>
    std::unique_ptr<GDX12UploadBuffer<T>> Buffer(GDX12DeviceResources* resources, UINT count)
    {
        auto buffer = std::make_unique<GDX12UploadBuffer<T>>(resources->Device, count, EBufferType::Default, false);
        auto* heap = resources->SRV_UAV_Heap.get();
        buffer->CreateSRV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        buffer->CreateUAV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        return buffer;
    }
}

GDX12FluidSurfacePass::GDX12FluidSurfacePass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 2;
    _numOutputs = 4;
    _outputs = { &_depthLink, &_motionLink, &_geometryLink, &_ownerLink };
}
GDX12FluidSurfacePass::~GDX12FluidSurfacePass() = default;

void GDX12FluidSurfacePass::Initialize()
{
    static_assert(sizeof(Parameters) == 256);
    static_assert(sizeof(State::Particle) == 36);
    auto* depth = _inputs[0]->GetTexture();
    auto* motion = _inputs[1]->GetTexture();
    if (!depth->GetWidth() || !depth->GetHeight() || depth->GetWidth() != motion->GetWidth() ||
        depth->GetHeight() != motion->GetHeight() || motion->GetFormat() != DXGI_FORMAT_R16G16_FLOAT)
        throw std::invalid_argument("Fluid surface requires matching reverse-Z depth and RG16F motion grids");
    _depthLink.Target = depth;
    _motionLink.Target = motion;
}

void GDX12FluidSurfacePass::CreatePipelines()
{
    if (_solverRoot) return;
    GDX12RootSignatureDesc solver;
    solver.VolatileSRVData = true;
    solver.Constants = { 8 };
    solver.NumSingleCBVSlots = 1;
    solver.NumSingleSRVSlots = 4;
    solver.NumSingleUAVSlots = 4;
    _solverRoot = std::make_unique<GDX12RootSignature>(_resources->Device, solver);
    _save = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "SavePrevious");
    _hash = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Hash");
    _sort = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Sort");
    _offsets = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "BuildOffsets");
    _densities = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Densities");
    _integrate = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _solverRoot.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Integrate");
    GDX12RootSignatureDesc surface;
    surface.VolatileSRVData = true;
    surface.Constants = { 4 };
    surface.NumSingleCBVSlots = 2;
    surface.NumSingleSRVSlots = 14;
    surface.NumSingleUAVSlots = 6;
    surface.StaticSamplers = GetStaticSamplers();
    _surfaceRoot = std::make_unique<GDX12RootSignature>(_resources->Device, surface);
    _initialize = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _surfaceRoot.get(), APP_SHADERS_FOLDER "FluidSurface.hlsl", "InitializeSurface");
    _filter = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _surfaceRoot.get(), APP_SHADERS_FOLDER "FluidSurface.hlsl", "NarrowRange");
    _merge = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _surfaceRoot.get(), APP_SHADERS_FOLDER "FluidSurface.hlsl", "MergeSurface");
    _resolve = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _surfaceRoot.get(), APP_SHADERS_FOLDER "FluidSurface.hlsl", "ResolveWater");
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    auto vs = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "FluidSurface.hlsl", nullptr, "SphereVS", "vs");
    auto ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "FluidSurface.hlsl", nullptr, "SpherePS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _surfaceRoot->GetRootSignature().Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_sphere)));
    ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "FluidSurface.hlsl", nullptr, "ThicknessPS", "ps");
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.DepthStencilState.DepthEnable = false;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    desc.BlendState.RenderTarget[0].BlendEnable = true;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_thicknessDraw)));
}

void GDX12FluidSurfacePass::CreateTargets()
{
    if (_depth) return;
    const UINT width = _inputs[0]->GetTexture()->GetWidth();
    const UINT height = _inputs[0]->GetTexture()->GetHeight();
    const auto texture = [&](const DXGI_FORMAT format, const bool renderTarget = false)
    {
        auto result = std::make_unique<GDX12Texture>();
        GDX12TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = desc.RTVDesc.Format = format;
        desc.CreateUAV = true;
        desc.CreateRTV = renderTarget;
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        if (renderTarget)
        {
            desc.RTVHeap = _resources->RTVHeap.get();
            desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
            desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
        desc.ClearValue = { 0, 0, 0, 0 };
        result->Initialize(desc);
        return result;
    };
    _raw = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    _eyeDepth = texture(DXGI_FORMAT_R32_FLOAT, true);
    _thickness = texture(DXGI_FORMAT_R32_FLOAT, true);
    for (auto& target : _filtered) target = texture(DXGI_FORMAT_R32_FLOAT);
    _depth = texture(DXGI_FORMAT_R32_FLOAT);
    _motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    _geometry = texture(DXGI_FORMAT_R32G32B32A32_FLOAT);
    _owner = texture(DXGI_FORMAT_R32_UINT);
    _geometryLink.Target = _geometry.get();
    _ownerLink.Target = _owner.get();
    _color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
    _scratchDepth = std::make_unique<GDX12Texture>();
    GDX12TextureDesc depth;
    depth.Width = width;
    depth.Height = height;
    depth.Format = depth.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depth.CreateSRV = false;
    depth.CreateDSV = true;
    depth.DSVHeap = _resources->DSVHeap.get();
    depth.DSVHeapIndex = depth.DSVHeap->GetAvailableIndex();
    depth.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depth.ClearValue = { 0, 0, 0, 0 };
    _scratchDepth->Initialize(depth);
}

void GDX12FluidSurfacePass::PrepareStates()
{
    std::set<uint64_t> alive;
    bool retire = false;
    for (const auto& instance : _instances)
    {
        ValidateFluidInstance(instance);
        const auto& source = instance.Component.GetParticles();
        if (!source || source->empty()) continue;
        if (!alive.insert(instance.Id).second) throw std::invalid_argument("Duplicate fluid instance id");
        auto found = _states.find(instance.Id);
        if (found == _states.end()) continue;
        const auto& author = instance.Component.GetSettings();
        auto& state = *found->second;
        const bool active = _controls.Enabled && (author.SimulationEnabled || author.RenderEnabled);
        if (active && (state.Source != source || state.Revision != instance.Component.GetResetRevision())) retire = true;
        if (state.WasRendered && (!_controls.Enabled || !author.RenderEnabled ||
            state.SettingsRevision != instance.Component.GetSettingsRevision())) _commonData->ResetHistory = true;
    }
    for (const auto& [id, state] : _states)
        if (!alive.count(id))
        {
            retire = true;
            _commonData->ResetHistory |= state->WasRendered;
        }
    // Old buffers, seed uploads and descriptor slots may still be referenced by queued frames.
    if (retire) _resources->Device->GetCommandQueue()->Flush();
    for (auto it = _states.begin(); it != _states.end();)
        if (!alive.count(it->first)) it = _states.erase(it);
        else ++it;
    for (const auto& instance : _instances)
    {
        const auto& source = instance.Component.GetParticles();
        const auto& author = instance.Component.GetSettings();
        if (!source || source->empty() || !_controls.Enabled ||
            (!author.SimulationEnabled && !author.RenderEnabled)) continue;
        auto& state = _states[instance.Id];
        if (state && state->Source == source && state->Revision == instance.Component.GetResetRevision()) continue;
        state = std::make_unique<State>();
        state->Source = source;
        state->Revision = instance.Component.GetResetRevision();
        state->Count = static_cast<UINT>(source->size());
        state->Capacity = 1;
        while (state->Capacity < state->Count) state->Capacity <<= 1;
        for (auto& buffer : state->Particles) buffer = Buffer<State::Particle>(_resources, state->Count);
        state->Density = Buffer<XMFLOAT2>(_resources, state->Count);
        state->Keys = Buffer<XMUINT2>(_resources, state->Capacity);
        state->Offsets = Buffer<UINT>(_resources, state->Capacity);
        state->Constants = std::make_unique<GDX12UploadBuffer<Parameters>>(_resources->Device,
            2 * static_cast<UINT>(_resources->FrameConstants.size()));
        state->SeedUpload = std::make_unique<GDX12UploadBuffer<State::Particle>>(_resources->Device,
            state->Count, EBufferType::Upload, false);
        const auto& settings = instance.Component.GetSettings();
        for (UINT i = 0; i < state->Count; ++i)
        {
            const auto& seed = (*source)[i];
            const auto position = Vector3::Transform(seed.Position, instance.ContainerToWorld);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
                position.Length() > 1e7f) throw std::invalid_argument("Fluid seed outside supported world range");
            auto velocity = Vector3::TransformNormal(seed.Velocity, instance.ContainerToWorld);
            const float speed = velocity.Length();
            if (speed > settings.MaxSpeedCmPerSecond) velocity *= settings.MaxSpeedCmPerSecond / speed;
            state->SeedUpload->CopyData(i, { position, velocity, position });
        }
    }
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12FluidSurfacePass::GetResolveBufferAccesses() const
{
    std::vector<GDX12RenderGraph::BufferAccess> accesses;
    for (const auto& [id, state] : _states)
        accesses.push_back({ &state->Constants->GetResource(), GDX12RenderGraph::Mode::Read });
    if (_resources && _resources->CurrFrameConstantsIndex < _resources->FrameConstants.size())
    {
        const auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        if (frame->CameraCB) accesses.push_back({ &frame->CameraCB->GetResource(), GDX12RenderGraph::Mode::Read });
        if (frame->ReflectionProbes) accesses.push_back({ &frame->ReflectionProbes->GetResource(), GDX12RenderGraph::Mode::Read });
    }
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12FluidSurfacePass::GetSimulationBufferAccesses() const
{
    std::vector<GDX12RenderGraph::BufferAccess> accesses;
    for (const auto& [id, state] : _states)
    {
        accesses.push_back({ &state->Constants->GetResource(), GDX12RenderGraph::Mode::Read });
        for (const auto& particles : state->Particles)
            accesses.push_back({ &particles->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Density->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Keys->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Offsets->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        if (state->SeedPending && state->SeedUpload) accesses.push_back({ &state->SeedUpload->GetResource(), GDX12RenderGraph::Mode::Read });
    }
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12FluidSurfacePass::GetBufferAccesses() const
{
    if (!_rendering) return {};
    auto accesses = GetResolveBufferAccesses();
    for (const auto& [id, state] : _states)
        for (const auto& particles : state->Particles)
            accesses.push_back({ &particles->GetResource(), GDX12RenderGraph::Mode::Read });
    return accesses;
}

void GDX12FluidSurfacePass::Simulate(GDX12CommandList* commands, State& state, const GDX12FluidSettings& settings, const float elapsed)
{
    if (state.SeedPending)
    {
        auto& target = state.Particles[state.Current]->GetResource();
        commands->ResourceBarrier({ target.GetCopyDestBarrier() });
        commands->CopyResource(target.D3DResource.Get(), state.SeedUpload->GetResource().D3DResource.Get());
        state.SeedPending = false;
        state.SeedFrameSlot = _resources->CurrFrameConstantsIndex;
    }
    commands->SetComputeRootSignature(_solverRoot.get());
    commands->SetComputeRootConstantBufferView(1, state.Constants->GetElementAddress(2 * _resources->CurrFrameConstantsIndex));
    struct Dispatch { UINT Count, Capacity, K, J; float Dt; UINT Padding[3] = {}; };
    Dispatch dispatch = { state.Count, state.Capacity, 0, 0, 0 };
    const auto setDispatch = [&]() { commands->GetCommandList()->SetComputeRoot32BitConstants(0, 8, &dispatch, 0); };
    setDispatch();
    auto& current = state.Particles[state.Current]->GetResource();
    commands->ResourceBarrier({ current.GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, state.Particles[state.Current]->GetUAV()->GPUHandle);
    commands->SetPipelineState(_save);
    commands->Dispatch((state.Count + 127) / 128, 1, 1);
    commands->ResourceBarrier({ current.GetUAVBarrier() });
    if (!settings.SimulationEnabled || settings.TimeScale == 0) return;
    if (!std::isfinite(elapsed) || elapsed <= 0) return;
    const float h = settings.SmoothingRadiusCm;
    const float sound = std::sqrt(settings.PressureStiffnessCm2PerSecond2);
    float safeStep = settings.MaxStepSeconds;
    // MGPU authors fixed substeps and a frame-time cap; legacy SPH retains its CFL bound.
    if (settings.NearPressureStiffnessCm2PerSecond2 == 0)
    {
        safeStep = (std::min)(safeStep, 0.25f * h / (settings.MaxSpeedCmPerSecond + sound));
        safeStep = (std::min)(safeStep, 0.25f * std::sqrt(h / settings.MaxAccelerationCmPerSecond2));
        if (settings.ViscosityCm2PerSecond > 0)
            safeStep = (std::min)(safeStep, 0.125f * h * h / settings.ViscosityCm2PerSecond);
    }
    // Bounded catch-up: drop excess wall time instead of taking an unstable oversized step.
    const float simulatedTime = (std::min)(elapsed * settings.TimeScale, safeStep * settings.MaxSubsteps);
    const UINT steps = settings.NearPressureStiffnessCm2PerSecond2 > 0 ? settings.MaxSubsteps :
        (std::min)(settings.MaxSubsteps, (std::max)(1u, static_cast<UINT>(std::ceil(simulatedTime / safeStep))));
    dispatch.Dt = simulatedTime / steps;
    for (UINT step = 0; step < steps; ++step)
    {
        auto& input = *state.Particles[state.Current];
        auto& output = *state.Particles[1 - state.Current];
        commands->ResourceBarrier({ input.GetResource().GetNonPixelShaderResourceBarrier(),
            state.Keys->GetResource().GetUnorderedAccessBarrier(), state.Offsets->GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(0, input.GetSRV()->GPUHandle);
        commands->SetComputeUAV(2, state.Keys->GetUAV()->GPUHandle);
        commands->SetComputeUAV(3, state.Offsets->GetUAV()->GPUHandle);
        dispatch.K = dispatch.J = 0;
        setDispatch();
        commands->SetPipelineState(_hash);
        commands->Dispatch((state.Capacity + 127) / 128, 1, 1);
        commands->ResourceBarrier({ state.Keys->GetResource().GetUAVBarrier(), state.Offsets->GetResource().GetUAVBarrier() });
        commands->SetPipelineState(_sort);
        for (UINT k = 2; k <= state.Capacity; k <<= 1)
        for (UINT j = k >> 1; j > 0; j >>= 1)
        {
            dispatch.K = k;
            dispatch.J = j;
            setDispatch();
            commands->Dispatch((state.Capacity + 127) / 128, 1, 1);
            commands->ResourceBarrier({ state.Keys->GetResource().GetUAVBarrier() });
        }
        commands->ResourceBarrier({ state.Keys->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(2, state.Keys->GetSRV()->GPUHandle);
        commands->SetPipelineState(_offsets);
        commands->Dispatch((state.Count + 127) / 128, 1, 1);
        commands->ResourceBarrier({ state.Offsets->GetResource().GetNonPixelShaderResourceBarrier(),
            state.Density->GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(3, state.Offsets->GetSRV()->GPUHandle);
        commands->SetComputeUAV(1, state.Density->GetUAV()->GPUHandle);
        commands->SetPipelineState(_densities);
        commands->Dispatch((state.Count + 127) / 128, 1, 1);
        commands->ResourceBarrier({ state.Density->GetResource().GetNonPixelShaderResourceBarrier(),
            output.GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(1, state.Density->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, output.GetUAV()->GPUHandle);
        commands->SetPipelineState(_integrate);
        commands->Dispatch((state.Count + 127) / 128, 1, 1);
        commands->ResourceBarrier({ output.GetResource().GetUAVBarrier() });
        state.Current = 1 - state.Current;
    }
}

void GDX12FluidSurfacePass::SurfaceBindings(GDX12CommandList* commands, State& state, const UINT owner, const UINT mode)
{
    commands->SetComputeRootSignature(_surfaceRoot.get());
    UINT dispatch[] = { _depth->GetWidth(), _depth->GetHeight(), owner, mode };
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, dispatch, 0);
    commands->SetComputeRootConstantBufferView(1, state.Constants->GetElementAddress(2 * _resources->CurrFrameConstantsIndex + 1));
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetComputeRootConstantBufferView(2, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(1, _raw->GetSRV()->GPUHandle);
    commands->SetComputeSRV(3, _thickness->GetSRV()->GPUHandle);
    commands->SetComputeSRV(4, _inputs[0]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetComputeSRV(5, _inputs[1]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, _depth->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _motion->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _geometry->GetUAV()->GPUHandle);
    commands->SetComputeUAV(3, _owner->GetUAV()->GPUHandle);
}

void GDX12FluidSurfacePass::DrawSurface(GDX12CommandList* commands, State& state, const UINT owner,
    const GDX12FluidSettings& settings)
{
    commands->ResourceBarrier({ state.Particles[state.Current]->GetResource().GetSRVBarrier(),
        _raw->GetResource()->GetRenderTargetBarrier(), _eyeDepth->GetResource()->GetRenderTargetBarrier(),
        _scratchDepth->GetResource()->GetDepthWriteBarrier(), _thickness->GetResource()->GetRenderTargetBarrier(),
        _inputs[0]->GetTexture()->GetResource()->GetSRVBarrier() });
    commands->SetGraphicsRootSignature(_surfaceRoot.get());
    const UINT dispatch[] = { _depth->GetWidth(), _depth->GetHeight(), owner, 0 };
    commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 4, dispatch, 0);
    commands->SetGraphicsRootConstantBufferView(1, state.Constants->GetElementAddress(2 * _resources->CurrFrameConstantsIndex + 1));
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetGraphicsRootConstantBufferView(2, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetGraphicsSRV(0, state.Particles[state.Current]->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(4, _inputs[0]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetViewport(_raw->GetViewport());
    commands->SetScissorRect(_raw->GetScissorRect());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->ClearRenderTargetView(_raw.get());
    commands->ClearRenderTargetView(_eyeDepth.get());
    commands->ClearDepthStencilView(_scratchDepth.get());
    commands->SetRenderTargets({ _raw.get(), _eyeDepth.get() }, _scratchDepth.get());
    commands->SetPipelineState(_sphere);
    commands->DrawInstanced(6, state.Count, 0, 0);
    commands->ClearRenderTargetView(_thickness.get());
    commands->SetRenderTargets({ _thickness.get() }, nullptr);
    commands->SetPipelineState(_thicknessDraw);
    commands->DrawInstanced(6, state.Count, 0, 0);
    commands->ResourceBarrier({ _raw->GetResource()->GetNonPixelShaderResourceBarrier(),
        _thickness->GetResource()->GetNonPixelShaderResourceBarrier(),
        _eyeDepth->GetResource()->GetCopySourceBarrier(), _filtered[0]->GetResource()->GetCopyDestBarrier() });
    commands->CopyResource(_filtered[0]->GetResource()->D3DResource.Get(), _eyeDepth->GetResource()->D3DResource.Get());
    UINT current = 0;
    // Alternating 1D NRF passes followed by the paper's small 2D cleanup.
    for (UINT pass = 0; pass <= settings.FilterIterations * 2; ++pass)
    {
        const UINT mode = pass == settings.FilterIterations * 2 ? 2 : pass % 2;
        SurfaceBindings(commands, state, owner, mode);
        commands->ResourceBarrier({ _filtered[current]->GetResource()->GetNonPixelShaderResourceBarrier(),
            _filtered[1 - current]->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(2, _filtered[current]->GetSRV()->GPUHandle);
        commands->SetComputeUAV(4, _filtered[1 - current]->GetUAV()->GPUHandle);
        commands->SetPipelineState(_filter);
        commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
        current = 1 - current;
    }
    commands->ResourceBarrier({ _filtered[current]->GetResource()->GetNonPixelShaderResourceBarrier() });
    SurfaceBindings(commands, state, owner);
    commands->SetComputeSRV(2, _filtered[current]->GetSRV()->GPUHandle);
    commands->SetPipelineState(_merge);
    commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ _depth->GetResource()->GetUAVBarrier(), _motion->GetResource()->GetUAVBarrier(),
        _geometry->GetResource()->GetUAVBarrier(), _owner->GetResource()->GetUAVBarrier() });
}

void GDX12FluidSurfacePass::PrepareFrame()
{
    if (_prepared) return;
    const auto previousControls = _controls;
    _controls = GetFluidControls();
    PrepareStates();
    // The frame-slot wait covers the final DIRECT fence and its COMPUTE join.
    for (auto& [id, state] : _states)
        if (!state->SeedPending && state->SeedFrameSlot == _resources->CurrFrameConstantsIndex)
            state->SeedUpload.reset();
    _depthLink.Target = _inputs[0]->GetTexture();
    _motionLink.Target = _inputs[1]->GetTexture();
    bool rendering = false;
    _simulating = false;
    for (const auto& instance : _instances)
    {
        const auto& seed = instance.Component.GetParticles();
        if (!seed || seed->empty()) continue;
        const auto& settings = instance.Component.GetSettings();
        rendering |= _controls.Enabled && settings.RenderEnabled;
        _simulating |= _controls.Enabled && settings.SimulationEnabled;
    }
    const bool changed = rendering != _rendering || previousControls.Enabled != _controls.Enabled ||
        previousControls.Quality != _controls.Quality;
    _commonData->ResetHistory |= changed;
    _rendering = rendering;
    if (!rendering && _depth)
    {
        _resources->Device->GetCommandQueue()->Flush();
        ReleaseTargets();
    }
    if (!rendering && !_simulating)
    {
        for (auto& [id, state] : _states) state->WasRendered = false;
        _prepared = true;
        return;
    }
    CreatePipelines();
    if (rendering) CreateTargets();
    _prepared = true;
}

void GDX12FluidSurfacePass::SetInstances(std::vector<GDX12FluidInstance> instances)
{
    _instances = std::move(instances);
    _prepared = false;
}

void GDX12FluidSurfacePass::ExecuteSimulation(GDX12CommandList* commands)
{
    if (!_prepared) PrepareFrame();
    if (!_rendering && !_simulating) return;
    GDX12_SCOPED_EVENT(commands, "Fluid SPH simulation", Colors::Aqua);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    for (const auto& instance : _instances)
    {
        auto found = _states.find(instance.Id);
        if (found == _states.end()) continue;
        auto& state = *found->second;
        const auto settings = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
        if (!settings.RenderEnabled && !settings.SimulationEnabled) continue;
        Parameters parameters = {};
        XMStoreFloat4x4(&parameters.ContainerToWorld, XMMatrixTranspose(instance.ContainerToWorld));
        XMStoreFloat4x4(&parameters.WorldToContainer, XMMatrixTranspose(instance.ContainerToWorld.Invert()));
        parameters.HalfExtent = settings.ContainerHalfExtentCm;
        parameters.Radius = settings.ParticleRadiusCm;
        parameters.Gravity = settings.AccelerationCmPerSecond2;
        parameters.H = settings.SmoothingRadiusCm;
        parameters.Mass = settings.ParticleMassKg;
        parameters.RestDensity = settings.RestDensityKgPerCm3;
        parameters.Stiffness = settings.PressureStiffnessCm2PerSecond2;
        parameters.Viscosity = settings.ViscosityCm2PerSecond;
        parameters.MaxSpeed = settings.MaxSpeedCmPerSecond;
        parameters.MaxAcceleration = settings.MaxAccelerationCmPerSecond2;
        parameters.Restitution = settings.Restitution;
        parameters.Sigma = settings.FilterSigmaCm;
        parameters.DepthRange = settings.FilterDepthRangeCm;
        parameters.EdgeDepth = settings.FilterEdgeCm;
        parameters.IOR = settings.IOR;
        parameters.Roughness = settings.Roughness;
        parameters.Absorption = settings.AbsorptionPerCm;
        parameters.RefractionPixels = settings.RefractionPixels;
        parameters.Scattering = settings.ScatteringColor;
        parameters.MaxThickness = settings.MaxOpticalThicknessCm;
        parameters.FilterRadius = settings.FilterRadiusPixels;
        parameters.ResetMotion = _commonData->ResetHistory || state.SeedPending || !state.WasRendered;
        parameters.NearStiffness = settings.NearPressureStiffnessCm2PerSecond2;
        parameters.PredictionSeconds = settings.PredictionSeconds;
        state.FrameParameters = parameters;
        state.ResetThisFrame = state.SeedPending;
        state.Constants->CopyData(2 * _resources->CurrFrameConstantsIndex, parameters);
        Simulate(commands, state, settings, instance.DeltaSeconds);
    }
}

void GDX12FluidSurfacePass::Execute(GDX12CommandList* commands)
{
    if (!_prepared) PrepareFrame();
    _prepared = false;
    if (!_rendering && !_simulating) return;
    GDX12_SCOPED_EVENT(commands, "Fluid NRF surface", Colors::Aqua);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    bool initialized = false;
    UINT owner = 0;
    for (const auto& instance : _instances)
    {
        ++owner;
        auto found = _states.find(instance.Id);
        if (found == _states.end()) continue;
        auto& state = *found->second;
        const auto settings = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
        if (!settings.RenderEnabled && !settings.SimulationEnabled) { state.WasRendered = false; continue; }
        _commonData->ResetHistory |= settings.RenderEnabled && !state.WasRendered;
        auto parameters = state.FrameParameters;
        parameters.ResetMotion = _commonData->ResetHistory || state.ResetThisFrame || !state.WasRendered;
        _commonData->ResetHistory |= state.ResetThisFrame && settings.RenderEnabled;
        state.Constants->CopyData(2 * _resources->CurrFrameConstantsIndex + 1, parameters);
        if (settings.RenderEnabled)
        {
            if (!initialized)
            {
                commands->ResourceBarrier({ _inputs[0]->GetTexture()->GetResource()->GetSRVBarrier(),
                    _inputs[1]->GetTexture()->GetResource()->GetNonPixelShaderResourceBarrier(),
                    _depth->GetResource()->GetUnorderedAccessBarrier(), _motion->GetResource()->GetUnorderedAccessBarrier(),
                    _geometry->GetResource()->GetUnorderedAccessBarrier(), _owner->GetResource()->GetUnorderedAccessBarrier() });
                SurfaceBindings(commands, state, owner);
                commands->SetPipelineState(_initialize);
                commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
                commands->ResourceBarrier({ _depth->GetResource()->GetUAVBarrier(), _motion->GetResource()->GetUAVBarrier(),
                    _geometry->GetResource()->GetUAVBarrier(), _owner->GetResource()->GetUAVBarrier() });
                initialized = true;
            }
            DrawSurface(commands, state, owner, settings);
        }
        state.WasRendered = settings.RenderEnabled;
        state.SettingsRevision = instance.Component.GetSettingsRevision();
    }
    if (initialized)
    {
        commands->ResourceBarrier({ _depth->GetResource()->GetSRVBarrier(), _motion->GetResource()->GetSRVBarrier(),
            _geometry->GetResource()->GetNonPixelShaderResourceBarrier(), _owner->GetResource()->GetNonPixelShaderResourceBarrier() });
        _depthLink.Target = _depth.get();
        _motionLink.Target = _motion.get();
    }
}

GDX12Texture* GDX12FluidSurfacePass::Resolve(GDX12CommandList* commands, GDX12Texture* background,
    GDX12Texture* environment)
{
    if (!_rendering) return background;
    if (background->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        background->GetWidth() != _color->GetWidth() || background->GetHeight() != _color->GetHeight())
        throw std::invalid_argument("Water resolve requires matching linear RGBA16F HDR");
    GDX12_SCOPED_EVENT(commands, "Water HDR optics", Colors::Aqua);
    commands->ResourceBarrier({ background->GetResource()->GetCopySourceBarrier(), _color->GetResource()->GetCopyDestBarrier() });
    commands->CopyResource(_color->GetResource()->D3DResource.Get(), background->GetResource()->D3DResource.Get());
    commands->ResourceBarrier({ background->GetResource()->GetNonPixelShaderResourceBarrier(),
        _color->GetResource()->GetUnorderedAccessBarrier(), environment->GetResource()->GetSRVBarrier(),
        _depth->GetResource()->GetSRVBarrier(), _inputs[0]->GetTexture()->GetResource()->GetNonPixelShaderResourceBarrier() });
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    UINT owner = 0;
    for (const auto& instance : _instances)
    {
        ++owner;
        auto found = _states.find(instance.Id);
        if (found == _states.end() || !instance.Component.GetSettings().RenderEnabled) continue;
        SurfaceBindings(commands, *found->second, owner);
        commands->SetComputeSRV(6, background->GetSRV()->GPUHandle);
        commands->SetComputeSRV(7, environment->GetSRV()->GPUHandle);
        commands->SetComputeSRV(9, _resources->ReflectionRadianceSRV.GPUHandle);
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        commands->SetComputeSRV(10, frame->ReflectionProbes->GetSRV()->GPUHandle);
        commands->SetComputeSRV(11, _geometry->GetSRV()->GPUHandle);
        commands->SetComputeSRV(12, _depth->GetSRV()->GPUHandle);
        commands->SetComputeSRV(13, _owner->GetSRV()->GPUHandle);
        commands->SetComputeUAV(5, _color->GetUAV()->GPUHandle);
        commands->SetPipelineState(_resolve);
        commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
        commands->ResourceBarrier({ _color->GetResource()->GetUAVBarrier() });
    }
    commands->ResourceBarrier({ _color->GetResource()->GetSRVBarrier() });
    return _color.get();
}

void GDX12FluidSurfacePass::Resize()
{
    Initialize();
    if (!_depth) return;
    const UINT width = _inputs[0]->GetTexture()->GetWidth();
    const UINT height = _inputs[0]->GetTexture()->GetHeight();
    for (auto* texture : { _raw.get(), _eyeDepth.get(), _scratchDepth.get(), _thickness.get(),
        _filtered[0].get(), _filtered[1].get(), _depth.get(), _motion.get(), _geometry.get(), _owner.get(), _color.get() })
        texture->Resize(width, height);
    for (auto& [id, state] : _states) state->WasRendered = false;
    _commonData->ResetHistory = true;
}

void GDX12FluidSurfacePass::ReleaseTargets()
{
    _raw.reset();
    _eyeDepth.reset();
    _scratchDepth.reset();
    _thickness.reset();
    for (auto& texture : _filtered) texture.reset();
    _depth.reset();
    _motion.reset();
    _geometry.reset();
    _owner.reset();
    _geometryLink.Target = _ownerLink.Target = nullptr;
    _color.reset();
}

void GDX12FluidSurfacePass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _states.clear();
    _instances.clear();
    ReleaseTargets();
    _solverRoot.reset();
    _surfaceRoot.reset();
    _save.Reset();
    _hash.Reset();
    _sort.Reset();
    _offsets.Reset();
    _densities.Reset();
    _integrate.Reset();
    _sphere.Reset();
    _thicknessDraw.Reset();
    _initialize.Reset();
    _filter.Reset();
    _merge.Reset();
    _resolve.Reset();
    _depthLink.Target = _motionLink.Target = nullptr;
    _rendering = _simulating = _prepared = false;
}

GDX12FluidResolvePass::GDX12FluidResolvePass(GDX12FluidSurfacePass* surface) : _surface(surface)
{
    if (!surface) throw std::invalid_argument("Fluid resolve requires its surface pass");
    _numInputs = 2;
    _numOutputs = 1;
    _outputs = { &_color };
}
void GDX12FluidResolvePass::Initialize() { PrepareFrame(); }
void GDX12FluidResolvePass::PrepareFrame() { _color.Target = _inputs[0]->GetTexture(); }
void GDX12FluidResolvePass::Execute(GDX12CommandList* commands)
{
    _color.Target = _surface->Resolve(commands, _inputs[0]->GetTexture(), _inputs[1]->GetTexture());
}
void GDX12FluidResolvePass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _color.Target = nullptr;
}
