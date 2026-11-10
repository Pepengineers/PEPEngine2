#include "Rendering/Water/GDX12FluidSolver.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.Fluid.Enabled", true,
        L"Enable fluid components. Default true.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> AsyncCompute(L"Render.Fluid.AsyncCompute", false,
        L"Run fluid simulation on COMPUTE when available. Default false.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> Quality(L"Render.Fluid.Quality", GDX12Quality::Mid,
        L"0 Off, 1 Low, 2 Mid (default), 3 High.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> MultiGPU(L"Render.Fluid.MultiGPU", false,
        L"Request secondary fluid simulation on the next seed/reset epoch.");
    static AutoConsoleVariable<bool> MultiGPUExact(L"Render.Fluid.MultiGPU.Exact", false,
        L"Request Exact fluid snapshots on the next seed/reset epoch. Otherwise use Latest completed.");
    static AutoConsoleVariable<float> MultiGPUUpdateHz(L"Render.Fluid.MultiGPU.UpdateHz", 0.0f,
        L"Maximum secondary fluid producer update rate (0 uncapped, up to 240 Hz).",
        EConsoleVariableFlags::Default, [](float v) { return std::isfinite(v) && v >= 0 && v <= 240; });
    static AutoConsoleVariable<float> MultiGPUMaxAgeMs(L"Render.Fluid.MultiGPU.MaxAgeMs", 0.0f,
        L"Fluid snapshot age diagnostic threshold (0 disabled, up to 60000 ms).",
        EConsoleVariableFlags::Default, [](float v) { return std::isfinite(v) && v >= 0 && v <= 60000; });
    bool Finite(const Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    bool Range(const float value, const float low, const float high)
    {
        return std::isfinite(value) && value >= low && value <= high;
    }
}

GDX12FluidControls GetFluidControls()
{
    const int quality = Quality.GetInt();
    return { Enabled.GetValue() && quality != 0, quality, AsyncCompute.GetValue(),
        MultiGPU.GetValue(), MultiGPUExact.GetValue(), MultiGPUUpdateHz.GetValue(), MultiGPUMaxAgeMs.GetValue() };
}

GDX12FluidSettings ResolveFluidSettings(const GDX12FluidSettings& author, const int quality)
{
    if (quality < 0 || quality > 3) throw std::invalid_argument("Invalid fluid quality");
    auto result = author;
    const UINT level = static_cast<UINT>((std::max)(1, quality));
    if (!result.MaxSubsteps) result.MaxSubsteps = 16u << (level - 1);
    if (!result.FilterIterations) result.FilterIterations = level;
    if (!result.FilterRadiusPixels) result.FilterRadiusPixels = 8u << (level - 1);
    return result;
}

void ValidateFluidSettings(const GDX12FluidSettings& s)
{
    if (!Range(s.MaxStepSeconds, 0.000001f, 0.05f) || s.MaxSubsteps > 64 ||
        !Range(s.TimeScale, 0, 8) || !Range(s.SmoothingRadiusCm, 0.1f, 1000) ||
        !Range(s.ParticleRadiusCm, 0.01f, s.SmoothingRadiusCm) ||
        !Range(s.ParticleMassKg, 1e-9f, 100000) || !Range(s.RestDensityKgPerCm3, 1e-8f, s.MGPUReference ? 100000.0f : 100.0f) ||
        !Range(s.PressureStiffnessCm2PerSecond2, 1, 1e8f) ||
        !Range(s.NearPressureStiffnessCm2PerSecond2, 0, 1e8f) || !Range(s.PredictionSeconds, 0, 0.05f) ||
        !Range(s.ViscosityCm2PerSecond, 0, 100000) || !Finite(s.AccelerationCmPerSecond2) ||
        !Range(s.WaveAcceleration, 0, 10000) || !Range(s.WavePeriodSeconds, 0.1f, 60) ||
        (!s.MGPUReference && s.AccelerationCmPerSecond2.Length() > s.MaxAccelerationCmPerSecond2) ||
        !Range(s.MaxSpeedCmPerSecond, 1, 100000) || !Range(s.MaxAccelerationCmPerSecond2, 1, 1e8f) ||
        !Finite(s.ContainerHalfExtentCm) || s.ContainerHalfExtentCm.x <= s.ParticleRadiusCm ||
        s.ContainerHalfExtentCm.y <= s.ParticleRadiusCm || s.ContainerHalfExtentCm.z <= s.ParticleRadiusCm ||
        s.ContainerHalfExtentCm.x > 1e6f || s.ContainerHalfExtentCm.y > 1e6f || s.ContainerHalfExtentCm.z > 1e6f ||
        !Range(s.Restitution, 0, 1) || s.FilterIterations > 16 || s.FilterRadiusPixels > 64 ||
        !Range(s.FilterSigmaCm, 0.001f, 1000) || !Range(s.FilterDepthRangeCm, 0.001f, 100000) ||
        !Range(s.FilterEdgeCm, 0, s.FilterDepthRangeCm) || !Range(s.IOR, 1, 3) ||
        !Finite(s.AbsorptionPerCm) || !Range(s.AbsorptionPerCm.x, 0, 100) ||
        !Range(s.AbsorptionPerCm.y, 0, 100) || !Range(s.AbsorptionPerCm.z, 0, 100) ||
        !Finite(s.ScatteringColor) || !Range(s.ScatteringColor.x, 0, 100) ||
        !Range(s.ScatteringColor.y, 0, 100) || !Range(s.ScatteringColor.z, 0, 100) ||
        !Range(s.Roughness, 0, 1) || !Range(s.RefractionPixels, 0, 128) ||
        !Range(s.MaxOpticalThicknessCm, 0.01f, 1e6f))
        throw std::invalid_argument("Invalid fluid component settings");
}

void GDX12FluidComponent::SetSettings(const GDX12FluidSettings& settings)
{
    ValidateFluidSettings(settings);
    if (_settings.MGPUReference != settings.MGPUReference) Reset();
    _settings = settings;
    ++_settingsRevision;
}

void GDX12FluidComponent::SetParticles(std::vector<GDX12FluidParticle> particles)
{
    if (particles.size() > 1048576) throw std::invalid_argument("Fluid seed exceeds 1048576 particles");
    for (const auto& particle : particles)
        if (!Finite(particle.Position) || !Finite(particle.Velocity) ||
            particle.Position.Length() > 1e7f || particle.Velocity.Length() > 1e7f)
            throw std::invalid_argument("Invalid fluid seed particle");
    _particles = std::make_shared<const std::vector<GDX12FluidParticle>>(std::move(particles));
    Reset();
}

void ValidateFluidInstance(const GDX12FluidInstance& instance)
{
    ValidateFluidSettings(instance.Component.GetSettings());
    if (!std::isfinite(instance.DeltaSeconds) || instance.DeltaSeconds < 0 || instance.DeltaSeconds > 3600)
        throw std::invalid_argument("Invalid fluid frame time");
    const auto& matrix = instance.ContainerToWorld;
    const auto* values = reinterpret_cast<const float*>(&matrix);
    for (UINT i = 0; i < 16; ++i)
        if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7f)
            throw std::invalid_argument("Invalid fluid container transform");
    const float determinant = matrix.Determinant();
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f ||
        matrix._14 != 0 || matrix._24 != 0 || matrix._34 != 0 || matrix._44 != 1)
        throw std::invalid_argument("Fluid container requires an invertible affine transform");
    const auto extent = instance.Component.GetSettings().ContainerHalfExtentCm;
    for (UINT corner = 0; corner < 8; ++corner)
    {
        const Vector3 local((corner & 1) ? extent.x : -extent.x,
            (corner & 2) ? extent.y : -extent.y, (corner & 4) ? extent.z : -extent.z);
        const auto world = Vector3::Transform(local, matrix);
        if (!Finite(world) || world.Length() > 1e7f)
            throw std::invalid_argument("Fluid container outside supported world range");
    }
    const auto& s = instance.Component.GetSettings();
    if (s.MGPUReference) return;
    const Matrix inverse = matrix.Invert();
    const Vector3 margin = s.ParticleRadiusCm * Vector3(
        Vector3(inverse._11, inverse._21, inverse._31).Length(),
        Vector3(inverse._12, inverse._22, inverse._32).Length(),
        Vector3(inverse._13, inverse._23, inverse._33).Length());
    if (!Finite(margin) || margin.x >= s.ContainerHalfExtentCm.x ||
        margin.y >= s.ContainerHalfExtentCm.y || margin.z >= s.ContainerHalfExtentCm.z)
        throw std::invalid_argument("Fluid particles do not fit the transformed container");
}

GDX12FluidParameters MakeFluidParameters(const Matrix& container, const GDX12FluidSettings& settings)
{
    GDX12FluidParameters p{};
    XMStoreFloat4x4(&p.ContainerToWorld, XMMatrixTranspose(container));
    XMStoreFloat4x4(&p.WorldToContainer, XMMatrixTranspose(container.Invert()));
    p.HalfExtent = settings.ContainerHalfExtentCm;
    p.Radius = settings.ParticleRadiusCm;
    p.Gravity = settings.AccelerationCmPerSecond2;
    p.H = settings.SmoothingRadiusCm;
    p.Mass = settings.ParticleMassKg;
    p.RestDensity = settings.RestDensityKgPerCm3;
    p.Stiffness = settings.PressureStiffnessCm2PerSecond2;
    p.Viscosity = settings.ViscosityCm2PerSecond;
    p.MaxSpeed = settings.MaxSpeedCmPerSecond;
    p.MaxAcceleration = settings.MaxAccelerationCmPerSecond2;
    p.Restitution = settings.Restitution;
    p.Sigma = settings.FilterSigmaCm;
    p.DepthRange = settings.FilterDepthRangeCm;
    p.EdgeDepth = settings.FilterEdgeCm;
    p.IOR = settings.IOR;
    p.Roughness = settings.Roughness;
    p.Absorption = settings.AbsorptionPerCm;
    p.RefractionPixels = settings.RefractionPixels;
    p.Scattering = settings.ScatteringColor;
    p.MaxThickness = settings.MaxOpticalThicknessCm;
    p.FilterRadius = settings.FilterRadiusPixels;
    p.NearStiffness = settings.NearPressureStiffnessCm2PerSecond2;
    p.PredictionSeconds = settings.PredictionSeconds;
    p.LocalSimulation = settings.MGPUReference ? 1u : 0u;
    return p;
}

GDX12FluidProducerInput::GDX12FluidProducerInput(const GDX12FluidInstance& instance, int quality)
    : _instance(instance), _settings(ResolveFluidSettings(instance.Component.GetSettings(), quality)), _quality(quality)
{
    ValidateFluidInstance(instance);
    if (!std::isfinite(instance.WorldTimeScale) || instance.WorldTimeScale < 0)
        throw std::invalid_argument("Invalid fluid world time scale");
    if (_settings.MGPUReference) _instance.ContainerToWorld = Matrix::Identity;
}

double GDX12FluidProducerInput::Elapsed() const
{
    return _instance.Paused || !_settings.SimulationEnabled ? 0 :
        static_cast<double>(_instance.DeltaSeconds) * _settings.TimeScale;
}

double GDX12FluidProducerInput::LogicalTick() const
{
    const double scaledTick = static_cast<double>(_instance.WorldTimeScale) * _settings.TimeScale / 60;
    const double solverLimit = GDX12FluidSolver::SafeStep(_settings) * _settings.MaxSubsteps;
    return (std::min)(scaledTick, solverLimit);
}

bool operator==(const GDX12FluidProducerInput& a, const GDX12FluidProducerInput& b)
{
    const auto solver = [](const GDX12FluidSettings& s)
    {
        return std::tie(s.SimulationEnabled, s.MGPUReference, s.MaxStepSeconds, s.MaxSubsteps, s.TimeScale,
            s.SmoothingRadiusCm, s.ParticleRadiusCm, s.ParticleMassKg, s.RestDensityKgPerCm3,
            s.PressureStiffnessCm2PerSecond2, s.NearPressureStiffnessCm2PerSecond2, s.PredictionSeconds,
            s.ViscosityCm2PerSecond, s.AccelerationCmPerSecond2, s.WaveAcceleration, s.WavePeriodSeconds,
            s.MaxSpeedCmPerSecond, s.MaxAccelerationCmPerSecond2, s.ContainerHalfExtentCm, s.Restitution);
    };
    const auto& ac = a._instance.Component;
    const auto& bc = b._instance.Component;
    return a._instance.Id == b._instance.Id && a._quality == b._quality &&
        ac.GetResetRevision() == bc.GetResetRevision() && ac.GetSettingsRevision() == bc.GetSettingsRevision() &&
        ac.GetParticles() == bc.GetParticles() && a._instance.ContainerToWorld == b._instance.ContainerToWorld &&
        solver(a._settings) == solver(b._settings);
}

namespace
{
    template<class T>
    std::unique_ptr<GDX12UploadBuffer<T>> SolverBuffer(GDX12Device* device, GDX12DescriptorHeap* heap, UINT count)
    {
        auto buffer = std::make_unique<GDX12UploadBuffer<T>>(device, count, EBufferType::Default, false);
        buffer->CreateSRV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        buffer->CreateUAV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        return buffer;
    }

    struct SolverRecordingBuffer
    {
        GDX12Resource& Resource;
        bool Placed;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        CD3DX12_RESOURCE_BARRIER To(D3D12_RESOURCE_STATES next)
        {
            if (!Placed) return Resource.GetTransitionBarrier(next);
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(Resource.D3DResource.Get(), State, next);
            State = next;
            return barrier;
        }

        CD3DX12_RESOURCE_BARRIER UAV()
        {
            return Placed ? CD3DX12_RESOURCE_BARRIER::UAV(Resource.D3DResource.Get()) : Resource.GetUAVBarrier();
        }
    };
}

void GDX12FluidSolver::Initialize(GDX12Device* device)
{
    if (_root) return;
    GDX12RootSignatureDesc desc;
    desc.VolatileSRVData = true;
    desc.Constants = { 8 };
    desc.NumSingleCBVSlots = 1;
    desc.NumSingleSRVSlots = 4;
    desc.NumSingleUAVSlots = 4;
    auto root = std::make_unique<GDX12RootSignature>(device, desc);
    _save = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "SavePrevious");
    _hash = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Hash");
    _sort = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Sort");
    _offsets = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "BuildOffsets");
    _densities = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Densities");
    _integrate = GDX12RenderHelpers::CreateComputePipeline(device, root.get(), APP_SHADERS_FOLDER "FluidSolver.hlsl", "Integrate");
    _root = std::move(root);
}

GDX12FluidSolver::Buffers GDX12FluidSolver::CreateBuffers(GDX12Device* device, GDX12DescriptorHeap* heap,
    const GDX12FluidInstance& instance)
{
    static_assert(sizeof(Particle) == 36);
    ValidateFluidInstance(instance);
    const auto& seed = instance.Component.GetParticles();
    if (!device || !heap || !seed || seed->empty()) throw std::invalid_argument("Fluid solver requires device and seed");
    Buffers buffers;
    buffers.Count = static_cast<UINT>(seed->size());
    buffers.Capacity = 1;
    while (buffers.Capacity < buffers.Count) buffers.Capacity <<= 1;
    for (auto& particles : buffers.Particles) particles = SolverBuffer<Particle>(device, heap, buffers.Count);
    buffers.Density = SolverBuffer<XMFLOAT2>(device, heap, buffers.Count);
    buffers.Keys = SolverBuffer<XMUINT2>(device, heap, buffers.Capacity);
    buffers.Offsets = SolverBuffer<UINT>(device, heap, buffers.Capacity);
    buffers.SeedUpload = std::make_unique<GDX12UploadBuffer<Particle>>(device, buffers.Count, EBufferType::Upload, false);
    const auto& settings = instance.Component.GetSettings();
    for (UINT i = 0; i < buffers.Count; ++i)
    {
        const auto& initial = (*seed)[i];
        const auto position = settings.MGPUReference ? initial.Position : Vector3::Transform(initial.Position, instance.ContainerToWorld);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
            position.Length() > 1e7f) throw std::invalid_argument("Fluid seed outside supported world range");
        auto velocity = settings.MGPUReference ? initial.Velocity : Vector3::TransformNormal(initial.Velocity, instance.ContainerToWorld);
        const float speed = velocity.Length();
        if (!settings.MGPUReference && speed > settings.MaxSpeedCmPerSecond) velocity *= settings.MaxSpeedCmPerSecond / speed;
        buffers.SeedUpload->CopyData(i, {position, velocity, position});
    }
    return buffers;
}

float GDX12FluidSolver::SafeStep(const GDX12FluidSettings& settings)
{
    float safe = settings.MaxStepSeconds;
    if (settings.NearPressureStiffnessCm2PerSecond2 == 0)
    {
        const float h = settings.SmoothingRadiusCm;
        const float sound = std::sqrt(settings.PressureStiffnessCm2PerSecond2);
        safe = (std::min)(safe, 0.25f * h / (settings.MaxSpeedCmPerSecond + sound));
        safe = (std::min)(safe, 0.25f * std::sqrt(h / settings.MaxAccelerationCmPerSecond2));
        if (settings.ViscosityCm2PerSecond > 0)
            safe = (std::min)(safe, 0.125f * h * h / settings.ViscosityCm2PerSecond);
    }
    return safe;
}

GDX12FluidSolver::Steps GDX12FluidSolver::PlanSteps(const GDX12FluidSettings& settings, float scaledElapsed)
{
    if (!settings.SimulationEnabled || settings.TimeScale == 0 || !std::isfinite(scaledElapsed) ||
        scaledElapsed <= 0) return {};
    const float safe = SafeStep(settings);
    if (!settings.MaxSubsteps || !std::isfinite(safe) || safe <= 0)
        throw std::invalid_argument("Fluid solver requires resolved substep settings");
    const float simulated = (std::min)(scaledElapsed, safe * settings.MaxSubsteps);
    const UINT count = settings.NearPressureStiffnessCm2PerSecond2 > 0 ? settings.MaxSubsteps :
        (std::min)(settings.MaxSubsteps, (std::max)(1u, static_cast<UINT>(std::ceil(simulated / safe))));
    return {count, simulated / count};
}

GDX12FluidSolver::Head GDX12FluidSolver::Record(GDX12CommandList& commands, Buffers& buffers, Head head,
    const GDX12FluidSettings& settings, Steps steps, D3D12_GPU_VIRTUAL_ADDRESS constants, Boundary boundary) const
{
    if (!_root || head.Current > 1 || buffers.Count == 0 || (head.SeedPending && !buffers.SeedUpload))
        throw std::logic_error("Fluid solver state is not ready for recording");
    if (steps.Count > settings.MaxSubsteps || (steps.Count && (!std::isfinite(steps.Dt) || steps.Dt < 0)))
        throw std::invalid_argument("Invalid fluid substep plan");
    const bool placed = boundary == Boundary::PlacedUav;
    SolverRecordingBuffer particles[] = {
        {buffers.Particles[0]->GetResource(), placed}, {buffers.Particles[1]->GetResource(), placed}
    };
    SolverRecordingBuffer density{buffers.Density->GetResource(), placed};
    SolverRecordingBuffer keys{buffers.Keys->GetResource(), placed};
    SolverRecordingBuffer offsets{buffers.Offsets->GetResource(), placed};
    auto& current = particles[head.Current];
    if (head.SeedPending)
    {
        commands.ResourceBarrier({current.To(D3D12_RESOURCE_STATE_COPY_DEST)});
        commands.CopyResource(current.Resource.D3DResource.Get(), buffers.SeedUpload->GetResource().D3DResource.Get());
        head.SeedPending = false;
    }

    commands.SetComputeRootSignature(_root.get());
    commands.SetComputeRootConstantBufferView(1, constants);
    struct Dispatch { UINT Count, Capacity, K, J; float Dt; UINT ReferenceUnits; float WaveAcceleration = 0; UINT Padding = 0; };
    Dispatch dispatch = {buffers.Count, buffers.Capacity, 0, 0, 0, settings.MGPUReference ? 1u : 0u};
    const auto setDispatch = [&] { commands.GetCommandList()->SetComputeRoot32BitConstants(0, 8, &dispatch, 0); };
    setDispatch();
    commands.ResourceBarrier({current.To(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
    commands.SetComputeUAV(0, buffers.Particles[head.Current]->GetUAV()->GPUHandle);
    commands.SetPipelineState(_save);
    commands.Dispatch((buffers.Count + 127) / 128, 1, 1);
    commands.ResourceBarrier({current.UAV()});

    dispatch.Dt = steps.Dt;
    for (UINT step = 0; step < steps.Count; ++step)
    {
        if (settings.WaveAcceleration > 0)
        {
            dispatch.WaveAcceleration = settings.WaveAcceleration * std::cos(XM_2PI *
                (head.WaveTime + dispatch.Dt * 0.5f) / settings.WavePeriodSeconds);
            head.WaveTime = std::fmod(head.WaveTime + dispatch.Dt, settings.WavePeriodSeconds);
        }
        auto& input = particles[head.Current];
        auto& output = particles[1 - head.Current];
        commands.ResourceBarrier({input.To(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            keys.To(D3D12_RESOURCE_STATE_UNORDERED_ACCESS), offsets.To(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        commands.SetComputeSRV(0, buffers.Particles[head.Current]->GetSRV()->GPUHandle);
        commands.SetComputeUAV(2, buffers.Keys->GetUAV()->GPUHandle);
        commands.SetComputeUAV(3, buffers.Offsets->GetUAV()->GPUHandle);
        dispatch.K = dispatch.J = 0;
        setDispatch();
        commands.SetPipelineState(_hash);
        commands.Dispatch((buffers.Capacity + 127) / 128, 1, 1);
        commands.ResourceBarrier({keys.UAV(), offsets.UAV()});
        commands.SetPipelineState(_sort);
        for (UINT k = 2; k <= buffers.Capacity; k <<= 1)
        for (UINT j = k >> 1; j > 0; j >>= 1)
        {
            dispatch.K = k;
            dispatch.J = j;
            setDispatch();
            commands.Dispatch((buffers.Capacity + 127) / 128, 1, 1);
            commands.ResourceBarrier({keys.UAV()});
        }
        commands.ResourceBarrier({keys.To(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(2, buffers.Keys->GetSRV()->GPUHandle);
        commands.SetPipelineState(_offsets);
        commands.Dispatch((buffers.Count + 127) / 128, 1, 1);
        commands.ResourceBarrier({offsets.To(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            density.To(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        commands.SetComputeSRV(3, buffers.Offsets->GetSRV()->GPUHandle);
        commands.SetComputeUAV(1, buffers.Density->GetUAV()->GPUHandle);
        commands.SetPipelineState(_densities);
        commands.Dispatch((buffers.Count + 127) / 128, 1, 1);
        commands.ResourceBarrier({density.To(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            output.To(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        commands.SetComputeSRV(1, buffers.Density->GetSRV()->GPUHandle);
        commands.SetComputeUAV(0, buffers.Particles[1 - head.Current]->GetUAV()->GPUHandle);
        commands.SetPipelineState(_integrate);
        commands.Dispatch((buffers.Count + 127) / 128, 1, 1);
        commands.ResourceBarrier({output.UAV()});
        head.Current = 1 - head.Current;
    }
    if (placed)
    {
        constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commands.ResourceBarrier({particles[0].To(uav), particles[1].To(uav),
            density.To(uav), keys.To(uav), offsets.To(uav)});
    }
    return head;
}

void GDX12FluidSolver::RecordPublish(GDX12CommandList& commands, ID3D12Resource& particles,
    ID3D12Resource& destination, UINT count)
{
    const auto src = particles.GetDesc(), dst = destination.GetDesc();
    const auto bytes = static_cast<UINT64>(count) * sizeof(Particle);
    if (!bytes || &particles == &destination || src.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        dst.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || src.Width != bytes || dst.Width != bytes)
        throw std::invalid_argument("Fluid publish requires matching distinct particle buffers");
    commands.GetCommandList()->CopyBufferRegion(&destination, 0, &particles, 0, bytes);
}

