#include "App.Base/Rendering/CrossAdapterParticles.h"
#include "Common/ConsoleVariables.h"
#include "Common/GameTimer.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace
{
using Graph = GDX12RenderGraph;
using namespace GDX12Submission;
using namespace GDX12Snapshot;
using Clock = std::chrono::steady_clock;
using Checkpoint = GDX12ParticleSimulation::Checkpoint;

AutoConsoleVariable<bool> Secondary(L"Render.Particles.MultiGPU", false,
    L"Simulate particles on the secondary adapter; draw on primary");
AutoConsoleVariable<bool> Scheduled(L"Render.Particles.Scheduled", false,
    L"Use the benchmark particle cadence policy, including full simulation time on primary");
AutoConsoleVariable<bool> Exact(L"Render.Particles.Exact", true,
    L"Use the scheduled particle update immediately; false publishes it on the next frame");
AutoConsoleVariable<UINT> Cadence(L"Render.Particles.UpdateEveryNFrames", 1,
    L"Particle update cadence; all captured input steps are replayed in order", EConsoleVariableFlags::Default,
    [](UINT value) { return value >= 1 && value <= 4; });
AutoConsoleVariable<float> MaxAge(L"Render.Particles.MaxAgeMs", 33.333334f,
    L"Diagnostic age budget for the displayed particle simulation state", EConsoleVariableFlags::Default,
    [](float value) { return std::isfinite(value) && value > 0; });

void Require(bool value, const char* message) { if (!value) throw std::logic_error(message); }
UINT64 Align(UINT64 value, UINT64 alignment) { return (value + alignment - 1) / alignment * alignment; }

}

struct CrossAdapterParticles::Input
{
    std::vector<GDX12ParticleEmitter> Emitters;
    GDX12ParticleSettings Settings;
    RenderPipelineCommonData Common;
    std::unique_ptr<GDX12Texture> Depth;
    View Camera, Interaction;
    GDX12Descriptor InteractionSRV;
    GDX12ParticleInteractionField Field;
    double Time = 0, SourceTime = 0;
};

struct CrossAdapterParticles::History
{
    View Buffer;
    GDX12Descriptor SRV;
    UINT Capacity = 0;
    UINT64 Emitter = 0, Reset = 0;
    bool Valid = false;
};

ParticleMGPUControls GetParticleMGPUControls()
{
    return {Secondary.GetValue(), Exact.GetValue(), Cadence.GetValue(), MaxAge.GetValue(), Scheduled.GetValue()};
}

CrossAdapterParticles::CrossAdapterParticles(GDX12DeviceResources& primary, GDX12Device& secondary,
    RenderMGPURuntime& runtime, GDX12ParticleSimulationPass& simulation, GDX12ParticlesPass& draw)
    : _primary(primary), _runtime(runtime), _work(runtime), _simulation(simulation), _draw(draw),
      _inputChannel(std::make_unique<Channel>()), _outputChannel(std::make_unique<Channel>())
{
    _secondary.Device = &secondary;
    _secondary.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&secondary,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    _secondary.FrameConstants.push_back(std::make_unique<GDX12FrameConstants>(&secondary));
    _secondary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
}

CrossAdapterParticles::~CrossAdapterParticles() = default;

bool CrossAdapterParticles::PrepareFrame(RenderPipelineCommonData& common)
{
    _frameStarted = std::chrono::steady_clock::now();
    _common = &common;
    _requested = _primary.ParticleEmitters;
    const auto controls = GetParticleMGPUControls();
    const bool active = GetParticleSettings().Enabled && !(_requested.empty()) &&
        (controls.Scheduled || controls.Secondary || controls.UpdateEveryNFrames != 1);
    if ((active || _active || _publishPending) && _primary.ParticleSimulation)
    {
        auto* queue = _primary.Device->GetCommandQueue();
        GDX12CommandList* release = nullptr;
        for (const auto& pool : _primary.ParticleSimulation->CaptureCheckpoint().Pools)
            for (const auto& buffer : pool.Buffers)
                if (buffer->GetCurrentState() != D3D12_RESOURCE_STATE_COMMON)
                {
                    if (!release) release = queue->GetCommandList();
                    release->ResourceBarrier({buffer->GetCommonBarrier()});
                }
        // The previous draw and this release share the primary DIRECT timeline.
        if (release) queue->ExecuteCommandList(release);
    }
    if (active) CaptureDisplayHistory();
    if (_publishPending) Publish();
    if (_active && (!active || controls.Secondary != _ownsSecondary))
    {
        Advance();
        Publish();
    }
    if (_ownsSecondary && (!active || !controls.Secondary)) MoveOwner(false);
    _controls = controls;
    _diagnostics.Requested = controls.Secondary;
    _diagnostics.Exact = controls.Exact;
    _diagnostics.Fallback.clear();
    _active = active;
    if (!active)
    {
        _initialized = false;
        return false;
    }
    if (!_primary.ParticleSimulation) _primary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    if (!_initialized)
    {
        _primary.ParticleSimulation->BeginFrame(&_primary, common);
        std::vector<Use> uses;
        const auto checkpoint = _primary.ParticleSimulation->CaptureCheckpoint();
        for (const auto& pool : checkpoint.Pools)
            for (const auto& buffer : pool.Buffers)
                uses.push_back({_work.Identify(buffer, *_primary.Device), Graph::Mode::ReadWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        _work.Run("Particle initial pools", *_primary.Device, std::move(uses), [&](GDX12CommandList& commands)
            { _primary.ParticleSimulation->InitializePools(&commands, &_primary); });
        _initialized = true;
    }
    if (controls.Secondary != _ownsSecondary) MoveOwner(controls.Secondary);
    return true;
}

void CrossAdapterParticles::MoveOwner(bool secondary)
{
    if (secondary == _ownsSecondary) return;
    Advance();
    if (!secondary)
    {
        Publish();
        _ownsSecondary = false;
        _diagnostics.Secondary = false;
        return;
    }
    const auto source = _primary.ParticleSimulation->CaptureCheckpoint();
    _secondary.ParticleEmitters = source.Emitters;
    auto settings = GetParticleSettings();
    settings.Enabled = settings.SimulationEnabled = true;
    _secondary.ParticleSimulation->BeginFrame(&_secondary, *_common, &settings);
    auto target = _secondary.ParticleSimulation->CaptureCheckpoint();
    std::vector<Use> reads, writes;
    for (const auto& pool : source.Pools)
        for (const auto& buffer : pool.Buffers)
            reads.push_back({_work.Identify(buffer, *_primary.Device), Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
    for (const auto& pool : target.Pools)
        for (const auto& buffer : pool.Buffers)
            writes.push_back({_work.Identify(buffer, *_secondary.Device), Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
    if (source.Bytes)
        _work.Transfer(*_inputChannel, *_primary.Device, *_secondary.Device, source.Bytes, std::move(reads), std::move(writes),
            [&](GDX12CommandList& commands, GDX12Resource& payload)
                { GDX12ParticleSimulation::RecordCheckpoint(commands, source, payload, false); },
            [&](GDX12CommandList& commands, GDX12Resource& payload)
                { GDX12ParticleSimulation::RecordCheckpoint(commands, target, payload, true); });
    _secondary.ParticleSimulation->RestoreCheckpointMetadata(source);
    _ownsSecondary = true;
    _diagnostics.InputBytes += source.Bytes;
}

std::optional<std::vector<Graph::LegacyExternalView>> CrossAdapterParticles::Boundary()
{
    if (!_active) return std::nullopt;
    std::vector<Graph::LegacyExternalView> result;
    const auto camera = _primary.FrameConstants[_primary.CurrFrameConstantsIndex]->CameraCB->ShareResource();
    const auto view = _work.Identify(camera, *_primary.Device);
    result.push_back({view.Identity, view.Resource, Graph::Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_STATE_GENERIC_READ});
    if (auto* interaction = _primary.ParticleSimulation->GetInteractionResource())
    {
        auto borrowed = std::shared_ptr<GDX12Resource>(interaction, [](GDX12Resource*) {});
        const auto field = _work.Identify(std::move(borrowed), *_primary.Device);
        result.push_back({field.Identity, field.Resource, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, {}});
    }
    for (const auto& pool : _primary.ParticleSimulation->CaptureCheckpoint().Pools)
        for (const auto& buffer : pool.Buffers)
        {
            const auto resource = _work.Identify(buffer, *_primary.Device);
            result.push_back({resource.Identity, resource.Resource, Graph::Mode::ReadWrite,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON});
        }
    return result;
}

void CrossAdapterParticles::Configure(Graph& graph, RenderPipelineBindings& bindings, IRenderPassLink* depth)
{
    graph.AddPass(_simulation);
    graph.SetOptionalLegacySplit(_simulation, {depth}, {}, [this] { return Boundary(); });
    bindings.LegacyReplacements.push_back({&_simulation,
        [this](const auto& boundary, auto prefix) { return Submit(boundary, prefix); }, {}, {}});
}

std::optional<CompletionPoint> CrossAdapterParticles::Submit(const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
{
    Require(_active && _common && boundary.Replaced == &_simulation && boundary.Inputs.size() == 1,
        "Particle route called outside its frame");
    auto input = std::make_shared<Input>();
    input->Emitters = _requested;
    input->Settings = GetParticleSettings();
    input->Common = *_common;
    input->Common.ActiveCameraCBufferIndex = 0;
    double elapsed = 0, sourceDelta = 0;
    for (const auto& emitter : _requested)
    {
        sourceDelta = (std::max)(sourceDelta, double(emitter.DeltaTime));
        if (!emitter.Paused && input->Settings.SimulationEnabled)
            elapsed = (std::max)(elapsed, double(emitter.DeltaTime) * emitter.TimeScale);
    }
    if (_common->GameTimer) sourceDelta = _common->GameTimer->DeltaTime();
    Require(std::isfinite(sourceDelta) && sourceDelta >= 0, "Invalid particle source time");
    _diagnostics.Desired += elapsed;
    _diagnostics.SourceTime += sourceDelta;
    input->Time = _diagnostics.Desired;
    input->SourceTime = _diagnostics.SourceTime;
    auto& target = _ownsSecondary ? _secondary : _primary;
    auto& device = *target.Device;
    auto& camera = *_primary.FrameConstants[_primary.CurrFrameConstantsIndex]->CameraCB;
    const UINT64 cameraBytes = sizeof(GDX12CameraConstants);
    input->Camera = _work.Allocate(device, Align(cameraBytes, 256));
    const auto& depth = boundary.Inputs[0];
    auto desc = depth.Native->D3DResource->GetDesc();
    Require(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
        (desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT || desc.Format == DXGI_FORMAT_R32_FLOAT),
        "Particle depth transfer requires a single R32 depth plane");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    _primary.Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1,
        Align(cameraBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT), &footprint, nullptr, nullptr, nullptr);
    GDX12TextureDesc texture;
    texture.Width = static_cast<UINT>(desc.Width);
    texture.Height = desc.Height;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.SRVDesc.Texture2D.MipLevels = 1;
    texture.SRV_UAV_Heap = target.SRV_UAV_Heap.get();
    texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    input->Depth = std::make_unique<GDX12Texture>();
    input->Depth->Initialize(texture);
    const auto targetDepth = _work.Identify(input->Depth->GetResourceOwner(), device);
    auto field = _primary.ParticleSimulation->GetInteractionField();
    const UINT64 fieldOffset = Align(footprint.Offset + UINT64(footprint.Footprint.RowPitch) *
        footprint.Footprint.Height * footprint.Footprint.Depth, 256);
    UINT64 fieldBytes = 0;
    std::vector<Use> reads{{_work.Identify(depth.Native, *_primary.Device), Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
        {_work.Identify(camera.ShareResource(), *_primary.Device), Graph::Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}};
    std::vector<Use> writes{{targetDepth, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST},
        {input->Camera, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}};
    if (field.Resource)
    {
        const auto shape = field.Resource->D3DResource->GetDesc();
        Require(shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && field.Resolution &&
            shape.Width >= UINT64(field.Resolution) * field.Resolution * sizeof(XMFLOAT4),
            "Invalid particle interaction field layout");
        fieldBytes = UINT64(field.Resolution) * field.Resolution * sizeof(XMFLOAT4);
        input->Interaction = _work.Allocate(device, fieldBytes);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = field.Resolution * field.Resolution;
        srv.Buffer.StructureByteStride = sizeof(XMFLOAT4);
        input->InteractionSRV.InitAsSRV(input->Interaction.Resource->D3DResource.Get(), &srv,
            target.SRV_UAV_Heap.get(), target.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        input->Field = field;
        input->Field.Resource = input->Interaction.Resource.get();
        input->Field.SRV = input->InteractionSRV.GPUHandle;
        const auto loan = std::find_if(boundary.ExternalViews.begin(), boundary.ExternalViews.end(),
            [&](const auto& view) { return view.Native.get() == field.Resource; });
        Require(loan != boundary.ExternalViews.end(), "Particle interaction field was not loaned by the graph");
        reads.push_back({{loan->Native, loan->Identity}, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, _interactionVersion});
        writes.push_back({input->Interaction, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
    }
    const UINT64 cameraOffset = UINT64(_common->ActiveCameraCBufferIndex) * camera.GetElementSize();
    const auto unpack = [&](GDX12CommandList& commands, GDX12Resource& payload)
    {
        commands.GetCommandList()->CopyBufferRegion(input->Camera.Resource->D3DResource.Get(), 0,
            payload.D3DResource.Get(), 0, cameraBytes);
        const CD3DX12_TEXTURE_COPY_LOCATION from(payload.D3DResource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION to(input->Depth->GetResource()->D3DResource.Get(), 0);
        commands.GetCommandList()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (fieldBytes) commands.GetCommandList()->CopyBufferRegion(input->Interaction.Resource->D3DResource.Get(), 0,
            payload.D3DResource.Get(), fieldOffset, fieldBytes);
    };
    if (_ownsSecondary)
    {
        const UINT64 bytes = fieldOffset + fieldBytes;
        _work.Transfer(*_inputChannel, *_primary.Device, device, bytes, std::move(reads), std::move(writes),
            [&](GDX12CommandList& commands, GDX12Resource& payload)
            {
                commands.GetCommandList()->CopyBufferRegion(payload.D3DResource.Get(), 0,
                    camera.GetResource().D3DResource.Get(), cameraOffset, cameraBytes);
                const CD3DX12_TEXTURE_COPY_LOCATION from(depth.Native->D3DResource.Get(), 0);
                const CD3DX12_TEXTURE_COPY_LOCATION to(payload.D3DResource.Get(), footprint);
                commands.GetCommandList()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                if (fieldBytes) commands.GetCommandList()->CopyBufferRegion(payload.D3DResource.Get(), fieldOffset,
                    field.Resource->D3DResource.Get(), 0, fieldBytes);
            }, unpack);
        _diagnostics.InputBytes += bytes;
    }
    else
    {
        reads.insert(reads.end(), writes.begin(), writes.end());
        _work.Run("Particle local input capture", device, std::move(reads), [&](GDX12CommandList& commands)
        {
            commands.GetCommandList()->CopyBufferRegion(input->Camera.Resource->D3DResource.Get(), 0,
                camera.GetResource().D3DResource.Get(), cameraOffset, cameraBytes);
            const CD3DX12_TEXTURE_COPY_LOCATION from(depth.Native->D3DResource.Get(), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION to(input->Depth->GetResource()->D3DResource.Get(), 0);
            commands.GetCommandList()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (fieldBytes) commands.GetCommandList()->CopyBufferRegion(input->Interaction.Resource->D3DResource.Get(), 0,
                field.Resource->D3DResource.Get(), 0, fieldBytes);
        });
    }
    _inputs.push_back(input);
    if (!_diagnostics.Ready || _inputs.size() >= _controls.UpdateEveryNFrames)
    {
        Advance();
        if (_controls.Exact || !_ownsSecondary || !_diagnostics.Ready) Publish();
    }
    _diagnostics.AgeMs = (_diagnostics.SourceTime - _diagnostics.DisplayedSourceTime) * 1000;
    _diagnostics.Debt = _diagnostics.Desired - _diagnostics.Completed;
    _diagnostics.OverAge = _diagnostics.AgeMs > _controls.MaxAgeMs;
    ApplyDisplayHistory();
    _draw.BeginFrame();
    const auto last = _primary.Device->GetCommandQueue()->GetLastCompletionPoint();
    return last.IsValid() ? last : prefix;
}

void CrossAdapterParticles::Advance()
{
    auto& resources = _ownsSecondary ? _secondary : _primary;
    auto& device = *resources.Device;
    for (const auto& input : _inputs)
    {
        const auto current = std::move(resources.ParticleEmitters);
        resources.ParticleEmitters = input->Emitters;
        auto& simulation = *resources.ParticleSimulation;
        simulation.BeginFrame(&resources, input->Common, &input->Settings);
        const auto previousField = simulation.GetInteractionField();
        simulation.SetInteractionField(input->Field);
        std::vector<Use> uses{
            {_work.Identify(input->Depth->GetResourceOwner(), device), Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
            {input->Camera, Graph::Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER}};
        if (input->Interaction.Resource) uses.push_back({input->Interaction, Graph::Mode::Read,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        for (const auto& pool : simulation.CaptureCheckpoint().Pools)
            for (const auto& buffer : pool.Buffers)
                uses.push_back({_work.Identify(buffer, device), Graph::Mode::ReadWrite, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        for (const auto& constants : simulation.GetConstantResources(resources.CurrFrameConstantsIndex))
            uses.push_back({_work.Identify(constants, device), Graph::Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        _diagnostics.Producer = _work.Run("Particle scheduled simulation", device, std::move(uses), [&](GDX12CommandList& commands)
        {
            simulation.InitializePools(&commands, &resources);
            simulation.Execute(&commands, &resources, input->Common, input->Depth.get(),
                input->Camera.Resource->D3DResource->GetGPUVirtualAddress(), true);
        });
        simulation.SetInteractionField(previousField);
        resources.ParticleEmitters = current;
        _diagnostics.Issued = _diagnostics.Completed = input->Time;
        _completedSourceTime = input->SourceTime;
        _publishPending = true;
    }
    _inputs.clear();
    _diagnostics.Debt = _diagnostics.Desired - _diagnostics.Completed;
}

void CrossAdapterParticles::Publish()
{
    if (!_publishPending) return;
    if (_ownsSecondary)
    {
        const auto source = _secondary.ParticleSimulation->CaptureCheckpoint();
        const auto requested = std::move(_primary.ParticleEmitters);
        _primary.ParticleEmitters = source.Emitters;
        auto settings = GetParticleSettings();
        settings.Enabled = settings.SimulationEnabled = true;
        _primary.ParticleSimulation->BeginFrame(&_primary, *_common, &settings);
        const auto target = _primary.ParticleSimulation->CaptureCheckpoint();
        _primary.ParticleEmitters = requested;
        std::vector<Use> reads, writes;
        for (const auto& pool : source.Pools)
            for (const auto& buffer : pool.Buffers)
                reads.push_back({_work.Identify(buffer, *_secondary.Device), Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
        for (const auto& pool : target.Pools)
            for (const auto& buffer : pool.Buffers)
                writes.push_back({_work.Identify(buffer, *_primary.Device), Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
        if (source.Bytes)
            _diagnostics.Imported = _work.Transfer(*_outputChannel, *_secondary.Device, *_primary.Device, source.Bytes,
                std::move(reads), std::move(writes),
                [&](GDX12CommandList& commands, GDX12Resource& payload)
                    { GDX12ParticleSimulation::RecordCheckpoint(commands, source, payload, false); },
                [&](GDX12CommandList& commands, GDX12Resource& payload)
                    { GDX12ParticleSimulation::RecordCheckpoint(commands, target, payload, true); });
        _primary.ParticleSimulation->RestoreCheckpointMetadata(source);
        _diagnostics.OutputBytes += source.Bytes;
    }
    _diagnostics.Displayed = _diagnostics.Completed;
    _diagnostics.DisplayedSourceTime = _completedSourceTime;
    _diagnostics.AgeMs = (_diagnostics.SourceTime - _diagnostics.DisplayedSourceTime) * 1000;
    _diagnostics.OverAge = _diagnostics.AgeMs > _controls.MaxAgeMs;
    _diagnostics.Secondary = _ownsSecondary;
    _diagnostics.Ready = true;
    _publishPending = false;
}

void CrossAdapterParticles::ResetWorld()
{
    _inputs.clear();
    _history.clear();
    for (auto* channel : {_inputChannel.get(), _outputChannel.get()})
    {
        if (channel->Bank) _runtime.RetireBank(*channel->Bank);
        *channel = {};
    }
    _secondary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    _active = _ownsSecondary = _initialized = _publishPending = false;
    _completedSourceTime = 0;
    _diagnostics = {};
    _work.WaitMilliseconds = 0;
}

void CrossAdapterParticles::CaptureDisplayHistory()
{
    for (const auto& history : _history) history->Valid = false;
    if (!_primary.ParticleSimulation) return;
    const auto checkpoint = _primary.ParticleSimulation->CaptureCheckpoint();
    _history.erase(std::remove_if(_history.begin(), _history.end(), [&](const auto& history)
        {
            return std::none_of(checkpoint.Emitters.begin(), checkpoint.Emitters.end(),
                [&](const auto& emitter) { return emitter.Id == history->Emitter; });
        }), _history.end());
    std::vector<Use> uses;
    std::vector<std::pair<View, View>> copies;
    for (size_t i = 0; i < checkpoint.Pools.size(); ++i)
    {
        const auto& pool = checkpoint.Pools[i];
        const auto id = checkpoint.Emitters[i].Id;
        auto found = std::find_if(_history.begin(), _history.end(),
            [id](const auto& history) { return history->Emitter == id; });
        if (found != _history.end() && (*found)->Capacity != pool.Capacity)
        {
            _history.erase(found);
            found = _history.end();
        }
        if (found == _history.end())
        {
            auto history = std::make_shared<History>();
            history->Emitter = id;
            history->Capacity = pool.Capacity;
            history->Buffer = _work.Allocate(*_primary.Device, pool.Buffers[0]->D3DResource->GetDesc().Width);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = pool.Capacity;
            srv.Buffer.StructureByteStride = static_cast<UINT>(history->Buffer.Resource->D3DResource->GetDesc().Width / pool.Capacity);
            history->SRV.InitAsSRV(history->Buffer.Resource->D3DResource.Get(), &srv, _primary.SRV_UAV_Heap.get(),
                _primary.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
            _history.push_back(std::move(history));
            found = _history.end() - 1;
        }
        auto& history = **found;
        history.Valid = pool.Initialized;
        history.Reset = pool.ResetRevision;
        const auto source = _work.Identify(pool.Buffers[0], *_primary.Device);
        uses.push_back({source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
        uses.push_back({history.Buffer, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
        copies.emplace_back(source, history.Buffer);
    }
    if (!copies.empty()) _work.Run("Particle previous displayed state", *_primary.Device, std::move(uses),
        [&](GDX12CommandList& commands)
        {
            for (const auto& copy : copies) commands.GetCommandList()->CopyBufferRegion(
                copy.second.Resource->D3DResource.Get(), 0, copy.first.Resource->D3DResource.Get(), 0,
                copy.first.Resource->D3DResource->GetDesc().Width);
        });
}

void CrossAdapterParticles::ApplyDisplayHistory()
{
    auto checkpoint = _primary.ParticleSimulation->CaptureCheckpoint();
    std::vector<Use> uses;
    struct Repair { UINT64 Id; std::shared_ptr<History> Previous; bool Valid; };
    std::vector<Repair> repairs;
    for (size_t i = 0; i < checkpoint.Pools.size(); ++i)
    {
        const auto& pool = checkpoint.Pools[i];
        const auto id = checkpoint.Emitters[i].Id;
        auto found = std::find_if(_history.begin(), _history.end(), [&](const auto& history)
            { return history->Emitter == id && history->Capacity == pool.Capacity; });
        std::shared_ptr<History> previous;
        if (found != _history.end()) previous = *found;
        else
        {
            previous = std::make_shared<History>();
            previous->Buffer = _work.Allocate(*_primary.Device, pool.Buffers[0]->D3DResource->GetDesc().Width);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = pool.Capacity;
            srv.Buffer.StructureByteStride = static_cast<UINT>(pool.Buffers[0]->D3DResource->GetDesc().Width / pool.Capacity);
            previous->SRV.InitAsSRV(previous->Buffer.Resource->D3DResource.Get(), &srv, _primary.SRV_UAV_Heap.get(),
                _primary.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        }
        uses.push_back({_work.Identify(pool.Buffers[0], *_primary.Device), Graph::Mode::ReadWrite,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        uses.push_back({previous->Buffer, Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        repairs.push_back({id, previous, previous->Valid && previous->Reset == pool.ResetRevision && !_common->ResetHistory});
    }
    if (!repairs.empty())
    {
        _work.Run("Particle displayed motion", *_primary.Device, std::move(uses), [&](GDX12CommandList& commands)
            {
                for (const auto& repair : repairs) _primary.ParticleSimulation->RecordDisplayHistory(commands, _primary,
                    repair.Id, repair.Previous->SRV.GPUHandle, repair.Valid);
            });
        checkpoint.ResetMotion = _common->ResetHistory || std::any_of(repairs.begin(), repairs.end(),
            [](const auto& repair) { return !repair.Valid; });
        _primary.ParticleSimulation->RestoreCheckpointMetadata(checkpoint);
    }
}
