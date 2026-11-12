#include "Rendering/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <chrono>
#include <deque>
#include <cassert>

struct GDX12FluidSurfacePass::State
{
    GDX12FluidSolver::Buffers Solver;
    GDX12FluidSolver::Head SolverHead;
    std::unique_ptr<GDX12UploadBuffer<GDX12FluidParameters>> Constants;
    std::shared_ptr<const std::vector<GDX12FluidParticle>> Source;
    uint64_t Revision = 0;
    uint64_t SettingsRevision = 0;
    UINT SeedFrameSlot = UINT_MAX;
    bool WasRendered = false;
    bool ResetThisFrame = false;
    struct InputSpan { GDX12FluidProducerInput Input; double Remaining; };
    std::deque<InputSpan> Pending;
    double DesiredTime = 0, IssuedTime = 0, MissingTime = 0;
    std::chrono::steady_clock::time_point LastSolve{};
};

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
    static_assert(sizeof(GDX12FluidParameters) == 336);
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
    if (_surfaceRoot) return;
    GDX12RootSignatureDesc surface;
    surface.VolatileSRVData = true;
    surface.Constants = { 4 };
    surface.NumSingleCBVSlots = 2;
    surface.NumSingleSRVSlots = 15;
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
        if (active && (state.Source != source || state.Revision != instance.Component.GetResetRevision()) &&
            (instance.Id != _routedOwner || state.Solver.Count)) retire = true;
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
        const bool routed = _routedOwner && instance.Id == _routedOwner;
        if (state && state->Source == source && state->Revision == instance.Component.GetResetRevision())
        {
            if (!routed && !state->Solver.Count)
                state->Solver = GDX12FluidSolver::CreateBuffers(_resources->Device, _resources->SRV_UAV_Heap.get(), instance);
            continue;
        }
        if (routed && state && !state->Solver.Count)
        {
            state->Source = source;
            state->Revision = instance.Component.GetResetRevision();
            state->WasRendered = false;
            _commonData->ResetHistory = true;
            continue;
        }
        auto replacement = std::make_unique<State>();
        replacement->Source = source;
        replacement->Revision = instance.Component.GetResetRevision();
        if (!routed)
            replacement->Solver = GDX12FluidSolver::CreateBuffers(_resources->Device, _resources->SRV_UAV_Heap.get(), instance);
        replacement->Constants = std::make_unique<GDX12UploadBuffer<GDX12FluidParameters>>(_resources->Device,
            3 * static_cast<UINT>(_resources->FrameConstants.size()));
        state = std::move(replacement);
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
        if (frame->CameraCB) accesses.push_back({ &frame->CameraCB->GetResource(), GDX12RenderGraph::Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER });
        if (frame->ReflectionProbes) accesses.push_back({ &frame->ReflectionProbes->GetResource(), GDX12RenderGraph::Mode::Read });
    }
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12FluidSurfacePass::GetSimulationBufferAccesses() const
{
    std::vector<GDX12RenderGraph::BufferAccess> accesses;
    for (const auto& [id, state] : _states)
    {
        if (!state->Solver.Count || (_routedOwner && id == _routedOwner)) continue;
        accesses.push_back({ &state->Constants->GetResource(), GDX12RenderGraph::Mode::Read });
        for (const auto& particles : state->Solver.Particles)
            accesses.push_back({ &particles->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Solver.Density->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Solver.Keys->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        accesses.push_back({ &state->Solver.Offsets->GetResource(), GDX12RenderGraph::Mode::ReadWrite });
        if (state->SolverHead.SeedPending && state->Solver.SeedUpload) accesses.push_back({ &state->Solver.SeedUpload->GetResource(), GDX12RenderGraph::Mode::Read });
    }
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12FluidSurfacePass::GetBufferAccesses() const
{
    if (!_rendering) return {};
    auto accesses = GetResolveBufferAccesses();
    for (const auto& [id, state] : _states)
    {
        if (!state->Solver.Count || (_routedOwner && id == _routedOwner)) continue;
        for (const auto& particles : state->Solver.Particles)
            accesses.push_back({ &particles->GetResource(), GDX12RenderGraph::Mode::Read });
    }
    if (_routedDisplay)
    {
        accesses.push_back({ _routedDisplay->Current.Resource.get(), GDX12RenderGraph::Mode::Read });
        if (_routedDisplay->HistoryValid && _routedDisplay->Previous.Resource != _routedDisplay->Current.Resource)
            accesses.push_back({ _routedDisplay->Previous.Resource.get(), GDX12RenderGraph::Mode::Read });
    }
    return accesses;
}

void GDX12FluidSurfacePass::SurfaceBindings(GDX12CommandList* commands, D3D12_GPU_VIRTUAL_ADDRESS constants,
    const UINT owner, const UINT mode)
{
    commands->SetComputeRootSignature(_surfaceRoot.get());
    UINT dispatch[] = { _depth->GetWidth(), _depth->GetHeight(), owner, mode };
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, dispatch, 0);
    commands->SetComputeRootConstantBufferView(1, constants);
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

void GDX12FluidSurfacePass::DrawSurface(GDX12CommandList* commands, const ParticleView& particles,
    const GDX12FluidDisplay::View* previous, D3D12_GPU_VIRTUAL_ADDRESS constants, const UINT owner,
    const GDX12FluidSettings& settings)
{
    commands->ResourceBarrier({ particles.Resource.GetSRVBarrier(),
        _raw->GetResource()->GetRenderTargetBarrier(), _eyeDepth->GetResource()->GetRenderTargetBarrier(),
        _scratchDepth->GetResource()->GetDepthWriteBarrier(), _thickness->GetResource()->GetRenderTargetBarrier(),
        _inputs[0]->GetTexture()->GetResource()->GetSRVBarrier() });
    if (previous && previous->Resource.get() != &particles.Resource)
        commands->ResourceBarrier({previous->Resource->GetSRVBarrier()});
    commands->SetGraphicsRootSignature(_surfaceRoot.get());
    const UINT dispatch[] = { _depth->GetWidth(), _depth->GetHeight(), owner, 0 };
    commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 4, dispatch, 0);
    commands->SetGraphicsRootConstantBufferView(1, constants);
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetGraphicsRootConstantBufferView(2, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetGraphicsSRV(0, particles.SRV);
    commands->SetGraphicsSRV(14, previous ? previous->SRV : particles.SRV);
    commands->SetGraphicsSRV(4, _inputs[0]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetViewport(_raw->GetViewport());
    commands->SetScissorRect(_raw->GetScissorRect());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->ClearRenderTargetView(_raw.get());
    commands->ClearRenderTargetView(_eyeDepth.get());
    commands->ClearDepthStencilView(_scratchDepth.get());
    commands->SetRenderTargets({ _raw.get(), _eyeDepth.get() }, _scratchDepth.get());
    commands->SetPipelineState(_sphere);
    commands->DrawInstanced(6, particles.Count, 0, 0);
    commands->ClearRenderTargetView(_thickness.get());
    commands->SetRenderTargets({ _thickness.get() }, nullptr);
    commands->SetPipelineState(_thicknessDraw);
    commands->DrawInstanced(6, particles.Count, 0, 0);
    commands->ResourceBarrier({ _raw->GetResource()->GetNonPixelShaderResourceBarrier(),
        _thickness->GetResource()->GetNonPixelShaderResourceBarrier(),
        _eyeDepth->GetResource()->GetCopySourceBarrier(), _filtered[0]->GetResource()->GetCopyDestBarrier() });
    commands->CopyResource(_filtered[0]->GetResource()->D3DResource.Get(), _eyeDepth->GetResource()->D3DResource.Get());
    UINT current = 0;
    // Alternating 1D NRF passes followed by the paper's small 2D cleanup.
    for (UINT pass = 0; pass <= settings.FilterIterations * 2; ++pass)
    {
        const UINT mode = pass == settings.FilterIterations * 2 ? 2 : pass % 2;
        SurfaceBindings(commands, constants, owner, mode);
        commands->ResourceBarrier({ _filtered[current]->GetResource()->GetNonPixelShaderResourceBarrier(),
            _filtered[1 - current]->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(2, _filtered[current]->GetSRV()->GPUHandle);
        commands->SetComputeUAV(4, _filtered[1 - current]->GetUAV()->GPUHandle);
        commands->SetPipelineState(_filter);
        commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
        current = 1 - current;
    }
    commands->ResourceBarrier({ _filtered[current]->GetResource()->GetNonPixelShaderResourceBarrier() });
    SurfaceBindings(commands, constants, owner);
    commands->SetComputeSRV(2, _filtered[current]->GetSRV()->GPUHandle);
    commands->SetPipelineState(_merge);
    commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ _depth->GetResource()->GetUAVBarrier(), _motion->GetResource()->GetUAVBarrier(),
        _geometry->GetResource()->GetUAVBarrier(), _owner->GetResource()->GetUAVBarrier() });
}

void GDX12FluidSurfacePass::PrepareFrame()
{
    if (_prepared) return;
    _frameStarted = std::chrono::steady_clock::now();
    const auto previousControls = _controls;
    _controls = GetFluidControls();
    PrepareStates();
    // The frame-slot wait covers the final DIRECT fence and its COMPUTE join.
    for (auto& [id, state] : _states)
        if (!state->SolverHead.SeedPending && state->SeedFrameSlot == _resources->CurrFrameConstantsIndex)
            state->Solver.SeedUpload.reset();
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
        _simulating |= _controls.Enabled && settings.SimulationEnabled && (!_routedOwner || instance.Id != _routedOwner);
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
    if (rendering)
    {
        CreateTargets();
        // Publish storage before the graph resolves dynamic writes; fallback depth is read-only.
        _depthLink.Target = _depth.get();
        _motionLink.Target = _motion.get();
    }
    _prepared = true;
}

void GDX12FluidSurfacePass::SetInstances(std::vector<GDX12FluidInstance> instances)
{
    _instances = std::move(instances);
    _prepared = false;
}

void GDX12FluidSurfacePass::SetRoutedDisplay(std::uint64_t owner, std::optional<GDX12FluidDisplay> display)
{
    if (display && (!owner || !display->Current.Resource || !display->Count ||
        (display->HistoryValid && !display->Previous.Resource)))
        throw std::invalid_argument("Invalid fluid snapshot display binding");
    _routedOwner = owner;
    _routedDisplay = std::move(display);
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
        if (!state.Solver.Count || (_routedOwner && instance.Id == _routedOwner)) continue;
        const auto settings = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
        if (!settings.RenderEnabled && !settings.SimulationEnabled) continue;
        _solver.Initialize(_resources->Device);
        const auto parameters = MakeFluidParameters(instance.ContainerToWorld, settings);
        const bool seedPending = state.SolverHead.SeedPending;
        state.ResetThisFrame = seedPending;
        const UINT constants = 3 * _resources->CurrFrameConstantsIndex;
        if (_controls.UpdateHz >= 0)
        {
            const GDX12FluidProducerInput input(instance, _controls.Quality);
            const double elapsed = input.Elapsed();
            state.DesiredTime += elapsed;
            if (elapsed > 0)
            {
                if (state.MissingTime > 0) state.MissingTime += elapsed;
                else if (!state.Pending.empty() && state.Pending.back().Input == input)
                    state.Pending.back().Remaining += elapsed;
                else if (state.Pending.size() < 16) state.Pending.push_back({input, elapsed});
                else state.MissingTime += elapsed;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!seedPending && _controls.UpdateHz > 0 &&
                std::chrono::duration<double>(now - state.LastSolve).count() < 1.0 / _controls.UpdateHz) continue;
            UINT recorded = 0;
            while (!state.Pending.empty() && recorded < 2)
            {
                auto& pending = state.Pending.front();
                const auto quantum = pending.Input.LogicalTick();
                if (pending.Remaining < quantum && state.Pending.size() == 1 && !state.MissingTime) break;
                const auto seconds = (std::min)(pending.Remaining, quantum);
                const auto& captured = pending.Input;
                state.Constants->CopyData(constants + recorded,
                    MakeFluidParameters(captured.Instance().ContainerToWorld, captured.Settings()));
                const auto steps = GDX12FluidSolver::PlanSteps(captured.Settings(), static_cast<float>(seconds));
                state.SolverHead = _solver.Record(*commands, state.Solver, state.SolverHead, captured.Settings(), steps,
                    state.Constants->GetElementAddress(constants + recorded), GDX12FluidSolver::Boundary::LegacyTracked);
                ++recorded;
                state.IssuedTime += seconds;
                pending.Remaining -= seconds;
                if (pending.Remaining <= 0) state.Pending.pop_front();
            }
            if (seedPending && !recorded)
            {
                state.Constants->CopyData(constants, parameters);
                state.SolverHead = _solver.Record(*commands, state.Solver, state.SolverHead, settings, {},
                    state.Constants->GetElementAddress(constants), GDX12FluidSolver::Boundary::LegacyTracked);
            }
            if (recorded || seedPending) state.LastSolve = now;
            if (seedPending) state.SeedFrameSlot = _resources->CurrFrameConstantsIndex;
            double retained = state.MissingTime;
            for (const auto& span : state.Pending) retained += span.Remaining;
            assert(std::abs(state.DesiredTime - state.IssuedTime - retained) < 1e-7 * (1 + state.DesiredTime));
            continue;
        }
        state.Constants->CopyData(constants, parameters);
        const auto steps = GDX12FluidSolver::PlanSteps(settings, instance.DeltaSeconds * settings.TimeScale);
        state.SolverHead = _solver.Record(*commands, state.Solver, state.SolverHead, settings, steps,
            state.Constants->GetElementAddress(constants), GDX12FluidSolver::Boundary::LegacyTracked);
        if (seedPending) state.SeedFrameSlot = _resources->CurrFrameConstantsIndex;
    }
}

GDX12FluidSurfacePass::LocalSimulationStatus GDX12FluidSurfacePass::GetLocalSimulationStatus() const
{
    LocalSimulationStatus result;
    for (const auto& [id, state] : _states)
    {
        if (!state->Solver.Count || (_routedOwner && id == _routedOwner)) continue;
        result.Active = true;
        result.DebtSeconds = (std::max)(result.DebtSeconds, (std::max)(0.0, state->DesiredTime - state->IssuedTime));
        result.MissingSeconds += state->MissingTime;
    }
    if (result.Active)
        result.AgeMs = result.DebtSeconds * 1000 +
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _frameStarted).count();
    return result;
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
        const auto author = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
        if (!author.RenderEnabled && !author.SimulationEnabled) { state.WasRendered = false; continue; }
        const bool routed = _routedOwner && instance.Id == _routedOwner;
        const auto* display = routed && _routedDisplay ? &*_routedDisplay : nullptr;
        const auto& settings = display ? display->Settings : author;
        const auto& transform = display ? display->Transform : instance.ContainerToWorld;
        const bool drawing = author.RenderEnabled && (!routed || display);
        _commonData->ResetHistory |= drawing != state.WasRendered;
        auto parameters = MakeFluidParameters(transform, settings);
        if (settings.MGPUReference)
            parameters.Radius *= std::cbrt(std::abs(transform.Determinant()));
        parameters.ResetMotion = _commonData->ResetHistory || state.ResetThisFrame || !state.WasRendered ||
            (display && !display->HistoryValid);
        _commonData->ResetHistory |= state.ResetThisFrame && drawing;
        if (display)
        {
            parameters.SnapshotMotion = 1;
            XMStoreFloat4x4(&parameters.PreviousContainerToWorld,
                XMMatrixTranspose(display->HistoryValid ? display->PreviousTransform : transform));
        }
        const UINT surfaceSlot = 3 * _resources->CurrFrameConstantsIndex + 2;
        state.Constants->CopyData(surfaceSlot, parameters);
        const auto constants = state.Constants->GetElementAddress(surfaceSlot);
        if (author.RenderEnabled)
        {
            if (!initialized)
            {
                commands->ResourceBarrier({ _inputs[0]->GetTexture()->GetResource()->GetSRVBarrier(),
                    _inputs[1]->GetTexture()->GetResource()->GetNonPixelShaderResourceBarrier(),
                    _depth->GetResource()->GetUnorderedAccessBarrier(), _motion->GetResource()->GetUnorderedAccessBarrier(),
                    _geometry->GetResource()->GetUnorderedAccessBarrier(), _owner->GetResource()->GetUnorderedAccessBarrier() });
                SurfaceBindings(commands, constants, owner);
                commands->SetPipelineState(_initialize);
                commands->Dispatch((_depth->GetWidth() + 7) / 8, (_depth->GetHeight() + 7) / 8, 1);
                commands->ResourceBarrier({ _depth->GetResource()->GetUAVBarrier(), _motion->GetResource()->GetUAVBarrier(),
                    _geometry->GetResource()->GetUAVBarrier(), _owner->GetResource()->GetUAVBarrier() });
                initialized = true;
            }
            if (display)
            {
                DrawSurface(commands, {*display->Current.Resource, display->Current.SRV, display->Count},
                    display->HistoryValid ? &display->Previous : nullptr, constants, owner, settings);
                commands->ResourceBarrier({display->Current.Resource->GetCommonBarrier()});
                if (display->HistoryValid && display->Previous.Resource != display->Current.Resource)
                    commands->ResourceBarrier({display->Previous.Resource->GetCommonBarrier()});
            }
            else if (drawing)
            {
                auto& particles = *state.Solver.Particles[state.SolverHead.Current];
                DrawSurface(commands, {particles.GetResource(), particles.GetSRV()->GPUHandle, state.Solver.Count},
                    nullptr, constants, owner, settings);
            }
        }
        state.WasRendered = drawing;
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
        const auto constants = found->second->Constants->GetElementAddress(2 * _resources->CurrFrameConstantsIndex + 1);
        SurfaceBindings(commands, constants, owner);
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
    _routedDisplay.reset();
    _routedOwner = 0;
    ReleaseTargets();
    _solver = GDX12FluidSolver{};
    _surfaceRoot.reset();
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
