#include "Rendering/Grass/GDX12GrassFieldProducer.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
    static AutoConsoleVariable<bool> GrassAsync(L"Render.Grass.AsyncCompute", false, L"Run Bistro grass interaction and wind on COMPUTE");
    static AutoConsoleVariable<bool> GrassEnabled(L"Render.Grass.Enabled", true, L"Enable grass components", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> GrassQuality(L"Render.Grass.Quality", GDX12Quality::Mid, L"Grass: 0 Off, 1 Low, 2 Mid, 3 High, 4 Epic", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> WindEnabled(L"Render.Wind.Enabled", true, L"Enable wind components", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> WindQuality(L"Render.Wind.Quality", GDX12Quality::Mid, L"Wind: 0 Off, 1 Low, 2 Mid, 3 High, 4 Epic", EConsoleVariableFlags::Default);
    bool Range(const float value, const float low, const float high) { return std::isfinite(value) && value >= low && value <= high; }
    static AutoConsoleVariable<bool> GrassMultiGPU(L"Render.Grass.MultiGPU", false,
        L"Route the authoritative Bistro grass field producer through the secondary adapter");
    static AutoConsoleVariable<bool> GrassExact(L"Render.Grass.MultiGPU.Exact", false,
        L"Demand the current grass field version instead of selecting the latest completed local import");
    static AutoConsoleVariable<float> GrassUpdateHz(L"Render.Grass.MultiGPU.UpdateHz", 0,
        L"Grass producer invocations per second [0, 240]; 0 uncapped", EConsoleVariableFlags::Default,
        [](float value) { return Range(value, 0, 240); });
    static AutoConsoleVariable<float> LocalUpdateHz(L"Render.Grass.UpdateHz", -1,
        L"Primary field cadence: -1 legacy, 0 uncapped ordered inputs, positive Hz retains pending inputs.",
        EConsoleVariableFlags::Default, [](float value) { return Range(value, -1, 240); });
    static AutoConsoleVariable<float> GrassMaxAgeMs(L"Render.Grass.MultiGPU.MaxAgeMs", 0,
        L"Grass display age diagnostic [0, 60000] ms; 0 disables the threshold", EConsoleVariableFlags::Default,
        [](float value) { return Range(value, 0, 60000); });
    bool Point(const XMFLOAT3& v) { return Range(v.x, -1e7f, 1e7f) && Range(v.y, -1e7f, 1e7f) && Range(v.z, -1e7f, 1e7f); }
}

GDX12GrassWindControls GetGrassWindControls()
{
    return { GrassEnabled.GetValue(), GrassQuality.GetInt(), WindEnabled.GetValue(), WindQuality.GetInt(), GrassAsync.GetValue(), LocalUpdateHz.GetValue() };
}

GDX12GrassMGPUControls GetGrassMGPUControls()
{
    return { GrassMultiGPU.GetValue(), GrassExact.GetValue(), GrassUpdateHz.GetValue(), GrassMaxAgeMs.GetValue() };
}

GDX12GrassSettings ResolveGrassSettings(const GDX12GrassSettings& author, const int quality)
{
    auto settings = author;
    const int q = (std::clamp)(quality, 1, 4) - 1;
    const UINT budgets[] = { 16384, 65536, 262144, 262144 };
    const UINT segments[] = { 1, 3, 5, 8 };
    if (!settings.MaxBlades) settings.MaxBlades = budgets[q];
    if (!settings.Segments) settings.Segments = segments[q];
    return settings;
}
GDX12WindSettings ResolveWindSettings(const GDX12WindSettings& author, const int quality)
{
    auto settings = author;
    const int q = (std::clamp)(quality, 1, 4) - 1;
    const UINT grids[] = { 32, 64, 128, 256 };
    const UINT iterations[] = { 8, 20, 40, 80 };
    if (!settings.GridResolution) settings.GridResolution = grids[q];
    if (!settings.PressureIterations) settings.PressureIterations = iterations[q];
    return settings;
}

bool IsValidGrassSettings(const GDX12GrassSettings& s)
{
    if (s.Implementation != GDX12GrassImplementation::Ribbon && s.Implementation != GDX12GrassImplementation::CrossedRibbons) return false;
    if (!Range(s.ExtentsCm.x, 1, 1e6f) || !Range(s.ExtentsCm.y, 1, 1e6f) ||
        !Range(s.DensityPerSquareMetre, 0, 100000) || s.MaxBlades > 262144 || s.Segments > 8 ||
        !Range(s.HeightCm, 0.1f, 10000) || !Range(s.HeightVariation, 0, 1) || !Range(s.WidthCm, 0.01f, 1000) ||
        !Range(s.BendSeconds, 0, 10) || !Range(s.MaxBendCm, 0, 10000) || !Range(s.LodDistanceCm, 1, 1e7f) ||
        !Range(s.FarCardDistanceCm, s.LodDistanceCm, 1e7f) || !Range(s.MaxDistanceCm, s.FarCardDistanceCm, 1e7f) ||
        s.RootsCm.size() > 262144) return false;
    for (const auto& root : s.RootsCm) if (!Point(root)) return false;
    return true;
}

bool IsValidWindSettings(const GDX12WindSettings& s)
{
    if (s.Implementation != GDX12WindImplementation::Analytic && s.Implementation != GDX12WindImplementation::ProjectedGrid) return false;
    return Range(s.ExtentsCm.x, 1, 1e6f) && Range(s.ExtentsCm.y, 1, 1e6f) &&
        Range(s.Direction.x, -1, 1) && Range(s.Direction.y, -1, 1) && s.Direction.LengthSquared() > 1e-8f &&
        Range(s.SpeedCmPerSecond, 0, s.MaxSpeedCmPerSecond) && Range(s.GustAmplitude, 0, 1) &&
        Range(s.GustFrequencyHz, 0, 100) && Range(s.GustWavelengthCm, 1, 1e6f) &&
        Range(s.ForcePerSecond, 0, 100) && Range(s.DampingPerSecond, 0, 100) &&
        Range(s.MaxSpeedCmPerSecond, 0.01f, 10000) && Range(s.TimeScale, 0, 10) &&
        Range(s.MaxStepSeconds, 0.0001f, 0.1f) && s.MaxSubsteps >= 1 && s.MaxSubsteps <= 16 &&
        (s.GridResolution == 0 || (s.GridResolution >= 8 && s.GridResolution <= 256)) && s.PressureIterations <= 128;
}

UINT GrassBladeCount(const GDX12GrassSettings& s)
{
    const double count = s.RootsCm.empty() ? std::ceil(4.0 * s.ExtentsCm.x * s.ExtentsCm.y * 0.0001 * s.DensityPerSquareMetre) :
        static_cast<double>(s.RootsCm.size());
    return static_cast<UINT>((std::min)(count, static_cast<double>(s.MaxBlades)));
}
BoundingBox GrassLocalBounds(const GDX12GrassSettings& s)
{
    Vector3 low(-s.ExtentsCm.x, 0, -s.ExtentsCm.y), high(s.ExtentsCm.x, 0, s.ExtentsCm.y);
    if (!s.RootsCm.empty())
    {
        low = high = Vector3(s.RootsCm.front());
        for (const auto& root : s.RootsCm)
        {
            low = Vector3::Min(low, Vector3(root));
            high = Vector3::Max(high, Vector3(root));
        }
    }
    const float bend = s.MaxBendCm + s.WidthCm;
    low -= Vector3(bend, bend, bend);
    high += Vector3(bend, s.HeightCm * (1 + s.HeightVariation) + bend, bend);
    BoundingBox result;
    BoundingBox::CreateFromPoints(result, low, high);
    return result;
}

namespace
{
    template<typename T> void Hash(uint64_t& hash, const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    }

    XMFLOAT4 WindArea(const GDX12WindInstance& s)
    {
        return {s.CenterCm.x, s.CenterCm.y, s.ExtentsCm.x, s.ExtentsCm.y};
    }

    bool SameArea(const XMFLOAT4& a, const XMFLOAT4& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
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
}

GDX12GrassFieldProducer::GDX12GrassFieldProducer(GDX12Device& device, GDX12DescriptorHeap& heap, size_t slots)
    : _device(device), _heap(heap), _frames(slots)
{
    GDX12RootSignatureDesc wind;
    wind.VolatileSRVData = true;
    wind.NumSingleCBVSlots = 1;
    wind.NumSingleSRVSlots = 2;
    wind.NumSingleUAVSlots = 1;
    _windRoot = std::make_unique<GDX12RootSignature>(&_device, wind);
    GDX12RootSignatureDesc interaction;
    interaction.VolatileSRVData = true;
    interaction.NumSingleCBVSlots = 1;
    interaction.NumSingleSRVSlots = 1;
    interaction.NumSingleUAVSlots = 3;
    _interactionRoot = std::make_unique<GDX12RootSignature>(&_device, interaction);
    _interactionPSO = GDX12RenderHelpers::CreateComputePipeline(&_device, _interactionRoot.get(),
        APP_SHADERS_FOLDER "GrassInteraction.hlsl", "CS");
    for (UINT stage = 0; stage < _windPSO.size(); ++stage)
    {
        const auto number = std::to_string(stage);
        D3D_SHADER_MACRO defines[] = { { "WIND_STAGE", number.c_str() }, { nullptr, nullptr } };
        _windPSO[stage] = GDX12RenderHelpers::CreateComputePipeline(&_device, _windRoot.get(), APP_SHADERS_FOLDER "WindField.hlsl", "CS", defines);
    }
}

std::unique_ptr<GDX12GrassFieldProducer::FieldBuffer> GDX12GrassFieldProducer::MakeField(UINT count) const
{
    auto buffer = std::make_unique<FieldBuffer>(&_device, count, EBufferType::Default, false);
    buffer->CreateSRV(&_heap, _heap.GetAvailableIndex(ConstantsResources));
    buffer->CreateUAV(&_heap, _heap.GetAvailableIndex(ConstantsResources));
    return buffer;
}

void GDX12GrassFieldProducer::Prepare(UINT slot, const Inputs& inputs)
{
    PrepareBatch(slot, &inputs, 1);
}

void GDX12GrassFieldProducer::PrepareBatch(UINT slot, const Inputs* inputs, size_t count)
{
    if (_pendingSlot) throw std::logic_error("Grass field invocation already prepared");
    if (!inputs || !count || count > MaxStepsPerInvocation)
        throw std::invalid_argument("Invalid grass field step count");
    auto& frame = _frames.at(slot);
    _preparedSlot.reset();
    frame.Wind = _wind;
    frame.IsSeed = false;
    frame.WindJobs.clear();
    frame.WindCursor = 0;
    frame.InteractionCount = 0;
    frame.HasNeutralWind = _neutralInitialized;
    frame.Layout.InteractionArea = _interactionArea;
    frame.Layout.InteractionRevision = _interactionRevision;
    frame.Layout.InteractionResetRevision = _interactionResetRevision;
    frame.Layout.InteractionTimeSeconds = _interactionTimeSeconds;

    AppendStep(frame, inputs[0]);
    for (size_t i = 1; i < count; ++i)
    {
        const auto previous = frame.Layout;
        if (inputs[i].ResetWind) throw std::invalid_argument("Wind reset must start an invocation");
        AppendStep(frame, inputs[i]);
        if (!previous.IsCompatibleWith(frame.Layout))
            throw std::invalid_argument("Grass field batch crosses an incompatible input");
    }
    _preparedSlot = _pendingSlot = slot;
}

void GDX12GrassFieldProducer::AppendStep(Frame& frame, const Inputs& inputs)
{
    const auto& area = inputs.InteractionArea;
    if (!std::isfinite(area.x) || !std::isfinite(area.y) ||
        !std::isfinite(area.z) || !std::isfinite(area.w) || area.z <= 0 || area.w <= 0)
        throw std::invalid_argument("Invalid grass interaction area");
    const float deltaSeconds = inputs.DeltaSeconds;
    const auto& sources = inputs.Actors;
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
        if ((std::min)(p.x, q.x) - p.w > area.x + area.z ||
            (std::max)(p.x, q.x) + p.w < area.x - area.z ||
            (std::min)(p.z, q.z) - p.w > area.y + area.w ||
            (std::max)(p.z, q.z) + p.w < area.y - area.w) continue;
        nearby.push_back(source);
    }
    if (nearby.size() > UINT_MAX / sizeof(BistroInteractionSource))
        throw std::length_error("Too many grass interaction sources");
    const UINT sourceCount = static_cast<UINT>(nearby.size());

    std::unordered_set<uint64_t> activeWind;
    for (const auto& source : inputs.Wind)
    {
        if (!source.Id || !activeWind.insert(source.Id).second)
            throw std::invalid_argument("Invalid or duplicate wind owner");
        if (!IsValidWindSettings(source) || !std::isfinite(source.CenterCm.x) || !std::isfinite(source.CenterCm.y) ||
            !std::isfinite(source.DeltaSeconds) || source.DeltaSeconds < 0)
            throw std::invalid_argument("Invalid wind instance");
    }
    auto& layout = frame.Layout;
    const bool resetInteraction = !layout.InteractionRevision || !SameArea(area, layout.InteractionArea) ||
        inputs.InteractionResetRevision != layout.InteractionResetRevision;
    constexpr UINT cells = InteractionResolution * InteractionResolution;
    if (!_presence)
    {
        auto presence = MakeField(cells);
        auto trail = MakeField(cells);
        auto previousTrail = MakeField(cells);
        _presence = std::move(presence);
        _trail = std::move(trail);
        _previousTrail = std::move(previousTrail);
    }
    auto& interaction = frame.InteractionJobs.at(frame.InteractionCount++);
    if (!interaction.Sources || interaction.Sources->GetElementCount() < (std::max)(1u, sourceCount))
    {
        interaction.Sources = std::make_unique<GDX12UploadBuffer<BistroInteractionSource>>(&_device,
            (std::max)(1u, sourceCount), EBufferType::Upload, false);
        interaction.Sources->CreateSRV(&_heap, _heap.GetAvailableIndex(ConstantsResources));
    }
    if (sourceCount) interaction.Sources->CopyData(0, nearby.data(), sourceCount);
    if (!interaction.Constants)
        interaction.Constants = std::make_unique<GDX12UploadBuffer<InteractionParameters>>(&_device, 1);

    interaction.Constants->CopyData(0, InteractionParameters{
        area, deltaSeconds, sourceCount, resetInteraction, InteractionResolution });
    if (inputs.NeedNeutralWind && !frame.HasNeutralWind)
    {
        if (!_neutralWind) _neutralWind = MakeField(1);
        WindParameters neutral = {};
        neutral.Resolution = 1;
        WindDispatch(frame, 0, neutral, nullptr, nullptr, _neutralWind.get());
        frame.HasNeutralWind = true;
    }

    layout.InteractionArea = area;
    if (deltaSeconds > 0 || resetInteraction) ++layout.InteractionRevision;
    layout.InteractionResetRevision = inputs.InteractionResetRevision;
    layout.InteractionTimeSeconds = (resetInteraction ? 0 : layout.InteractionTimeSeconds) + deltaSeconds;
    layout.ActorRevision = 14695981039346656037ull;
    Hash(layout.ActorRevision, sourceCount);
    for (const auto& actor : nearby) Hash(layout.ActorRevision, actor);
    layout.Presence = { 0, cells };
    layout.Trail = { cells, cells };
    layout.Wind.clear();
    layout.ElementCount = 2 * cells;

    for (auto it = frame.Wind.begin(); it != frame.Wind.end();)
    {
        if (activeWind.count(it->first)) ++it;
        else it = frame.Wind.erase(it);
    }
    for (auto source : inputs.Wind)
    {
        static_cast<GDX12WindSettings&>(source) = ResolveWindSettings(source, inputs.WindQuality);
        const auto signature = SettingsHash(source);
        auto& state = frame.Wind[source.Id];
        state.ResetHistory = inputs.ResetWind || !state.Resolution || signature != state.SettingsHash;
        state.Source = source;
        state.SettingsHash = signature;
        UpdateWind(frame, state, state.ResetHistory, inputs.CarryWindDebt);
        layout.Wind.push_back({ source.Id, { 0, state.Resolution * state.Resolution }, WindArea(source),
            state.Resolution, source.Implementation, signature, state.Time, state.DebtSeconds });
    }
    std::sort(layout.Wind.begin(), layout.Wind.end(), [](const WindRegion& a, const WindRegion& b) { return a.Owner < b.Owner; });
    for (auto& field : layout.Wind)
    {
        if (field.Velocity.ElementCount > UINT_MAX - layout.ElementCount)
            throw std::length_error("Grass field snapshot too large");
        field.Velocity.FirstElement = layout.ElementCount;
        layout.ElementCount += field.Velocity.ElementCount;
    }
}

void GDX12GrassFieldProducer::PrepareSeed(UINT slot, const Inputs& inputs, const SnapshotLayout& layout)
{
    if (_interactionRevision || !layout.InteractionRevision || !std::isfinite(layout.InteractionTimeSeconds) ||
        layout.InteractionTimeSeconds < 0)
        throw std::invalid_argument("Grass handoff requires a fresh solver and a valid checkpoint");

    // Build matching allocations without consuming the next application tick.
    auto wind = inputs.Wind;
    for (auto& field : wind) field.DeltaSeconds = 0;
    Prepare(slot, { wind, {}, inputs.InteractionArea, 0, inputs.WindQuality, false, false,
        inputs.InteractionResetRevision, inputs.CarryWindDebt });
    auto& frame = _frames.at(slot);
    const auto sameRegion = [](const Region& a, const Region& b)
    {
        return a.FirstElement == b.FirstElement && a.ElementCount == b.ElementCount;
    };
    const auto validWind = [&](const WindRegion& actual, const WindRegion& expected)
    {
        return sameRegion(actual.Velocity, expected.Velocity) && std::isfinite(actual.TimeSeconds) &&
            actual.TimeSeconds >= 0 && std::isfinite(actual.WindDebtSeconds) && actual.WindDebtSeconds >= 0;
    };
    if (!frame.Layout.IsCompatibleWith(layout) || !sameRegion(layout.Presence, frame.Layout.Presence) ||
        !sameRegion(layout.Trail, frame.Layout.Trail) ||
        !std::equal(layout.Wind.begin(), layout.Wind.end(), frame.Layout.Wind.begin(), validWind))
    {
        Cancel(slot);
        throw std::invalid_argument("Grass checkpoint is incompatible with the seed inputs");
    }
    frame.Layout = layout;
    frame.WindJobs.clear();
    frame.IsSeed = true;
    for (const auto& field : layout.Wind)
    {
        auto& state = frame.Wind.at(field.Owner);
        state.Time = state.PreviousTime = field.TimeSeconds;
        state.DebtSeconds = field.WindDebtSeconds;
    }
}

void GDX12GrassFieldProducer::RecordSeed(GDX12CommandList& commands, UINT slot, ID3D12Resource& packed) const
{
    if (_pendingSlot != slot || !_frames.at(slot).IsSeed)
        throw std::logic_error("Grass handoff seed is not prepared");
    const auto& frame = _frames.at(slot);
    const auto& layout = frame.Layout;
    const auto desc = packed.GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc.Width != UINT64(layout.ElementCount) * sizeof(XMFLOAT4))
        throw std::invalid_argument("Grass handoff resource does not match its layout");

    const auto copy = [&](FieldBuffer& target, const Region& region)
    {
        auto& resource = target.GetResource();
        commands.ResourceBarrier({ resource.GetCopyDestBarrier() });
        commands.GetCommandList()->CopyBufferRegion(resource.D3DResource.Get(), 0, &packed,
            UINT64(region.FirstElement) * sizeof(XMFLOAT4), UINT64(region.ElementCount) * sizeof(XMFLOAT4));
        commands.ResourceBarrier({ resource.GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
    };
    copy(*_presence, layout.Presence);
    copy(*_trail, layout.Trail);
    copy(*_previousTrail, layout.Trail);
    for (const auto& field : layout.Wind)
        for (const auto& velocity : frame.Wind.at(field.Owner).Velocity)
            copy(*velocity, field.Velocity);
}

void GDX12GrassFieldProducer::Commit(UINT slot)
{
    if (_pendingSlot != slot) throw std::logic_error("Grass field commit without matching preparation");
    auto& frame = _frames.at(slot);
    _wind.swap(frame.Wind);
    _interactionArea = frame.Layout.InteractionArea;
    _interactionRevision = frame.Layout.InteractionRevision;
    _interactionResetRevision = frame.Layout.InteractionResetRevision;
    _interactionTimeSeconds = frame.Layout.InteractionTimeSeconds;
    _neutralInitialized = frame.HasNeutralWind;
    _pendingSlot.reset();
}

void GDX12GrassFieldProducer::Cancel(UINT slot)
{
    if (_pendingSlot != slot) throw std::logic_error("Grass field cancel without matching preparation");
    auto& frame = _frames.at(slot);
    frame.WindJobs.clear();
    frame.Wind.clear();
    frame.Layout = {};
    _pendingSlot.reset();
    _preparedSlot.reset();
}

std::optional<GDX12GrassFieldProducer::WindView> GDX12GrassFieldProducer::FindWind(uint64_t owner) const
{
    const auto& wind = _pendingSlot ? _frames.at(*_pendingSlot).Wind : _wind;
    const auto found = wind.find(owner);
    if (found == wind.end()) return std::nullopt;
    const auto& state = found->second;
    return WindView{ state.Velocity[0].get(), state.Velocity[1].get(), WindArea(state.Source),
        state.Resolution, state.SettingsHash, state.Time, state.PreviousTime, state.ResetHistory };
}

void GDX12GrassFieldProducer::WindDispatch(Frame& frame, const UINT stage, const WindParameters& parameters,
    FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output)
{
    if (frame.WindCursor == frame.WindConstants.size())
        frame.WindConstants.push_back(std::make_unique<GDX12UploadBuffer<WindParameters>>(&_device, 1));
    auto& constants = frame.WindConstants[frame.WindCursor++];
    constants->CopyData(0, parameters);
    frame.WindJobs.push_back({ stage, parameters.Resolution, constants.get(), input, pressure, output });
}

void GDX12GrassFieldProducer::Record(GDX12CommandList& list, UINT slot, ID3D12Resource* packed) const
{
    if (_preparedSlot != slot || _frames.at(slot).IsSeed) throw std::logic_error("Grass solve slot is not prepared");
    const auto& frame = _frames.at(slot);
    if (packed)
    {
        const auto desc = packed->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            desc.Width != UINT64(frame.Layout.ElementCount) * sizeof(XMFLOAT4))
            throw std::invalid_argument("Grass snapshot buffer does not match its layout");
    }
    auto* commands = &list;
    commands->SetDescriptorHeaps({ &_heap });
    commands->SetComputeRootSignature(_interactionRoot.get());
    commands->SetPipelineState(_interactionPSO);
    for (size_t i = 0; i < frame.InteractionCount; ++i)
    {
        const auto& interaction = frame.InteractionJobs[i];
        commands->SetComputeRootConstantBufferView(0, interaction.Constants->GetElementAddress(0));
        commands->SetComputeSRV(0, interaction.Sources->GetSRV()->GPUHandle);
        commands->ResourceBarrier({ _trail->GetResource().GetUnorderedAccessBarrier(),
            _previousTrail->GetResource().GetUnorderedAccessBarrier(), _presence->GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, _trail->GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, _previousTrail->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2, _presence->GetUAV()->GPUHandle);
        commands->Dispatch((InteractionResolution + 7) / 8, (InteractionResolution + 7) / 8, 1);
        commands->ResourceBarrier({ _trail->GetResource().GetUAVBarrier(), _previousTrail->GetResource().GetUAVBarrier(),
            _presence->GetResource().GetUAVBarrier() });
    }
    for (const auto& job : frame.WindJobs)
    {
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
    if (packed)
    {
        RecordPacked(list, frame.Layout, *packed);
        // Isolated placed work releases all private solver buffers between invocations.
        for (const auto& job : frame.WindJobs)
            commands->ResourceBarrier({ job.Output->GetResource().GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
        commands->ResourceBarrier({ _trail->GetResource().GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON),
            _previousTrail->GetResource().GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON),
            _presence->GetResource().GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
    }
}

void GDX12GrassFieldProducer::RecordPacked(GDX12CommandList& commands, const SnapshotLayout& layout,
    ID3D12Resource& packed) const
{
    const auto copy = [&](FieldBuffer& source, const Region& region)
    {
        auto& resource = source.GetResource();
        commands.ResourceBarrier({ resource.GetCopySourceBarrier() });
        commands.GetCommandList()->CopyBufferRegion(&packed, UINT64(region.FirstElement) * sizeof(XMFLOAT4),
            resource.D3DResource.Get(), 0, UINT64(region.ElementCount) * sizeof(XMFLOAT4));
        commands.ResourceBarrier({ resource.GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
    };
    copy(*_presence, layout.Presence);
    copy(*_trail, layout.Trail);
    const auto& wind = _pendingSlot ? _frames.at(*_pendingSlot).Wind : _wind;
    for (const auto& field : layout.Wind) copy(*wind.at(field.Owner).Velocity[0], field.Velocity);
}

void GDX12GrassFieldProducer::AppendAccesses(UINT slot, std::vector<GDX12RenderGraph::BufferAccess>& accesses) const
{
    const auto& frame = _frames.at(slot);
    using Mode = GDX12RenderGraph::Mode;
    for (size_t i = 0; i < frame.InteractionCount; ++i)
    {
        const auto& interaction = frame.InteractionJobs[i];
        accesses.push_back({ &interaction.Sources->GetResource(), Mode::Read });
        accesses.push_back({ &interaction.Constants->GetResource(), Mode::Read });
    }
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
}

void GDX12GrassFieldProducer::UpdateWind(Frame& frame, WindState& state, const bool reset, const bool carryDebt)
{
    const auto& s = state.Source;
    WindParameters p = {};
    auto direction = s.Direction;
    direction.Normalize();
    p.Area = WindArea(s);
    p.Force = XMFLOAT4(direction.x, direction.y, s.SpeedCmPerSecond, s.GustAmplitude);
    p.Gust = XMFLOAT4(s.GustFrequencyHz, s.GustWavelengthCm, 0, 0);
    p.Solver = XMFLOAT4(s.ForcePerSecond, s.DampingPerSecond, s.MaxSpeedCmPerSecond, 0);
    p.Resolution = s.GridResolution;
    if (reset)
    {
        state.Time = 0;
        state.DebtSeconds = 0;
    }
    state.PreviousTime = state.Time;
    const float scaledDt = s.DeltaSeconds * s.TimeScale;
    const float budget = s.MaxSubsteps * s.MaxStepSeconds;
    const bool advancing = scaledDt > 0;
    const double currentElapsed = advancing ? double(s.DeltaSeconds) * s.TimeScale : 0;
    const double elapsed = carryDebt ? state.DebtSeconds + currentElapsed : scaledDt;
    // Debt stays pending until the field's simulation clock advances again.
    float remaining = advancing ? static_cast<float>((std::min)(elapsed, double(budget))) : 0;
    state.DebtSeconds = carryDebt ? (std::max)(0.0, elapsed - remaining) : 0;
    if (s.Implementation == GDX12WindImplementation::Analytic)
    {
        if (state.Resolution != s.GridResolution || state.Advected)
        {
            state.Pressure = {};
            state.Advected.reset();
            state.Divergence.reset();
            state.Initialized = false;
            for (auto& buffer : state.Velocity) buffer = MakeField(p.Resolution * p.Resolution);
            state.Resolution = s.GridResolution;
        }
        state.Time += remaining;
        p.Gust.z = state.Time;
        WindDispatch(frame, 6, p, nullptr, nullptr, state.Velocity[0].get());
        p.Gust.z = reset ? state.Time : state.PreviousTime;
        WindDispatch(frame, 6, p, nullptr, nullptr, state.Velocity[1].get());
        return;
    }
    if (state.Resolution != s.GridResolution || !state.Advected)
    {
        state.Initialized = false;
        for (auto& buffer : state.Velocity) buffer = MakeField(p.Resolution * p.Resolution);
        for (auto& buffer : state.Pressure) buffer = MakeField(p.Resolution * p.Resolution);
        state.Advected = MakeField(p.Resolution * p.Resolution);
        state.Divergence = MakeField(p.Resolution * p.Resolution);
        state.Resolution = p.Resolution;
    }
    if (reset || !state.Initialized)
    {
        for (auto& buffer : state.Velocity) WindDispatch(frame, 0, p, nullptr, nullptr, buffer.get());
        state.Initialized = true;
    }
    WindDispatch(frame, 5, p, state.Velocity[0].get(), nullptr, state.Velocity[1].get());
    const float cellSize = 2 * (std::min)(s.ExtentsCm.x, s.ExtentsCm.y) / s.GridResolution;
    const float stepLimit = (std::min)(s.MaxStepSeconds, 0.5f * cellSize / s.MaxSpeedCmPerSecond);
    // Legacy frames keep their original substep limit. Placed frames retain solver debt.
    for (UINT step = 0; step < s.MaxSubsteps && remaining > 0; ++step)
    {
        p.Gust.w = (std::min)(remaining, stepLimit);
        state.Time += p.Gust.w;
        p.Gust.z = state.Time;
        remaining -= p.Gust.w;
        WindDispatch(frame, 1, p, state.Velocity[0].get(), nullptr, state.Advected.get());
        WindDispatch(frame, 2, p, state.Advected.get(), nullptr, state.Divergence.get());
        WindDispatch(frame, 0, p, nullptr, nullptr, state.Pressure[0].get());
        UINT read = 0;
        for (UINT iteration = 0; iteration < s.PressureIterations; ++iteration)
        {
            WindDispatch(frame, 3, p, state.Divergence.get(), state.Pressure[read].get(), state.Pressure[read ^ 1].get());
            read ^= 1;
        }
        WindDispatch(frame, 4, p, state.Advected.get(), state.Pressure[read].get(), state.Velocity[0].get());
    }
    if (carryDebt) state.DebtSeconds += (std::max)(0.0f, remaining);
}


bool GDX12GrassFieldProducer::SnapshotLayout::IsCompatibleWith(const SnapshotLayout& other) const
{
    if (InteractionGrid != other.InteractionGrid || InteractionResetRevision != other.InteractionResetRevision ||
        !SameArea(InteractionArea, other.InteractionArea) || Wind.size() != other.Wind.size() ||
        ElementCount != other.ElementCount) return false;
    return std::equal(Wind.begin(), Wind.end(), other.Wind.begin(), [](const WindRegion& a, const WindRegion& b)
    {
        return a.Owner == b.Owner && a.Mode == b.Mode && a.Resolution == b.Resolution &&
            a.SettingsRevision == b.SettingsRevision && SameArea(a.Area, b.Area);
    });
}

bool GDX12GrassFieldProducer::SnapshotLayout::Matches(const Inputs& inputs) const
{
    if (inputs.Wind.size() != Wind.size() || !SameArea(InteractionArea, inputs.InteractionArea) ||
        InteractionResetRevision != inputs.InteractionResetRevision) return false;
    std::unordered_set<uint64_t> owners;
    for (auto source : inputs.Wind)
    {
        if (!owners.insert(source.Id).second) return false;
        static_cast<GDX12WindSettings&>(source) = ResolveWindSettings(source, inputs.WindQuality);
        const auto it = std::lower_bound(Wind.begin(), Wind.end(), source.Id,
            [](const WindRegion& field, uint64_t owner) { return field.Owner < owner; });
        if (it == Wind.end() || it->Owner != source.Id || it->SettingsRevision != SettingsHash(source)) return false;
    }
    return true;
}

GDX12GrassFieldSnapshot::GDX12GrassFieldSnapshot(GDX12DescriptorHeap& heap, GDX12Resource& local, Layout layout,
    std::shared_ptr<GDX12Resource> owner)
    : _heap(heap), _local(local), _owner(std::move(owner)), _layout(std::move(layout))
{
    if ((_owner && _owner.get() != &local) || !local.D3DResource) throw std::invalid_argument("Missing grass snapshot resource");
    ComPtr<ID3D12Device> heapDevice, localDevice;
    ThrowIfFailed(heap.GetHeap()->GetDevice(IID_PPV_ARGS(&heapDevice)));
    ThrowIfFailed(local.D3DResource->GetDevice(IID_PPV_ARGS(&localDevice)));
    const auto resource = local.D3DResource->GetDesc();
    if (heapDevice.Get() != localDevice.Get() || heap.GetType() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        heap.GetFlags() != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE || resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        resource.Width != UINT64(_layout.ElementCount) * sizeof(XMFLOAT4) ||
        (resource.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        !std::isfinite(_layout.InteractionArea.x) || !std::isfinite(_layout.InteractionArea.y) ||
        !std::isfinite(_layout.InteractionArea.z) || !std::isfinite(_layout.InteractionArea.w) ||
        _layout.InteractionArea.z <= 0 || _layout.InteractionArea.w <= 0 ||
        _layout.InteractionGrid != GDX12GrassFieldProducer::InteractionResolution)
        throw std::invalid_argument("Invalid local grass snapshot binding");

    UINT end = 0;
    const auto validate = [&](const GDX12GrassFieldProducer::Region& region, UINT count)
    {
        if (region.FirstElement != end || region.ElementCount != count || count > _layout.ElementCount - end)
            throw std::invalid_argument("Invalid grass snapshot region");
        end += count;
    };
    const UINT interactionCells = _layout.InteractionGrid * _layout.InteractionGrid;
    validate(_layout.Presence, interactionCells);
    validate(_layout.Trail, interactionCells);
    uint64_t previousOwner = 0;
    for (const auto& field : _layout.Wind)
    {
        if (field.Owner <= previousOwner || field.Resolution < 8 || field.Resolution > 256)
            throw std::invalid_argument("Invalid grass snapshot wind field");
        validate(field.Velocity, field.Resolution * field.Resolution);
        previousOwner = field.Owner;
    }
    if (end != _layout.ElementCount) throw std::invalid_argument("Unbound grass snapshot cells");

    const auto makeView = [&](const GDX12GrassFieldProducer::Region& region)
    {
        auto descriptor = std::make_unique<GDX12Descriptor>();
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = region.FirstElement;
        desc.Buffer.NumElements = region.ElementCount;
        desc.Buffer.StructureByteStride = sizeof(XMFLOAT4);
        descriptor->InitAsSRV(local.D3DResource.Get(), &desc, &heap, heap.GetAvailableIndex(ConstantsResources));
        return descriptor;
    };
    _presence = makeView(_layout.Presence);
    _trail = makeView(_layout.Trail);
    for (const auto& field : _layout.Wind) _wind.push_back(makeView(field.Velocity));
}

const GDX12GrassFieldSnapshot::WindRegion* GDX12GrassFieldSnapshot::FindWind(uint64_t owner) const
{
    const auto found = std::lower_bound(_layout.Wind.begin(), _layout.Wind.end(), owner,
        [](const WindRegion& field, uint64_t id) { return field.Owner < id; });
    return found != _layout.Wind.end() && found->Owner == owner ? &*found : nullptr;
}

bool GDX12GrassFieldSnapshot::MatchesWind(const GDX12WindInstance& wind, int quality) const
{
    const auto* field = FindWind(wind.Id);
    if (!field) return false;
    auto effective = wind;
    static_cast<GDX12WindSettings&>(effective) = ResolveWindSettings(wind, quality);
    return field->SettingsRevision == SettingsHash(effective);
}

GDX12GrassFieldSnapshot::View GDX12GrassFieldSnapshot::Wind(uint64_t owner) const
{
    const auto* field = FindWind(owner);
    if (!field) throw std::invalid_argument("Missing wind in grass snapshot");
    return { &_local, _wind.at(field - _layout.Wind.data())->GPUHandle };
}

GDX12GrassLatestSelection::GDX12GrassLatestSelection(GDX12Submission::TransferId stream,
    GDX12Submission::Epoch epoch, Stamp seed)
    : _stream(stream), _epoch(epoch), _displayed(std::move(seed))
{
    if (stream == GDX12Submission::TransferId::Invalid || epoch == GDX12Submission::Epoch::Invalid ||
        !_displayed.Sequence || !_displayed.Layout.ElementCount)
        throw std::invalid_argument("Latest grass display requires an authoritative handoff seed");
}

bool GDX12GrassLatestSelection::TryAdopt(GDX12Submission::TransferVersion version, Stamp completed)
{
    if (!version.IsValid() || version.Transfer != _stream || version.VersionEpoch != _epoch ||
        version.Invocation <= _invocation || completed.Sequence <= _displayed.Sequence ||
        completed.CapturedAt < _displayed.CapturedAt)
        return false;
    const auto& previous = _displayed.Layout;
    const auto& next = completed.Layout;
    if (!previous.IsCompatibleWith(next) || next.InteractionRevision < previous.InteractionRevision ||
        !(next.InteractionTimeSeconds >= previous.InteractionTimeSeconds))
        return false;
    for (size_t i = 0; i < next.Wind.size(); ++i)
        if (!(next.Wind[i].TimeSeconds >= previous.Wind[i].TimeSeconds)) return false;
    _displayed = std::move(completed);
    _invocation = version.Invocation;
    return true;
}

double GDX12GrassLatestSelection::AgeMs(std::chrono::steady_clock::time_point now) const
{
    return (std::max)(0.0, std::chrono::duration<double, std::milli>(now - _displayed.CapturedAt).count());
}

bool GDX12GrassLatestSelection::OverAge(std::chrono::steady_clock::time_point now, float maxAgeMs) const
{
    return maxAgeMs > 0 && AgeMs(now) > maxAgeMs;
}

bool GDX12GrassFieldInputQueue::Push(Tick tick)
{
    if (!tick.Sequence || (_lastSeen && tick.Sequence <= _lastSeen) ||
        !std::isfinite(tick.DeltaSeconds) || tick.DeltaSeconds < 0)
        throw std::invalid_argument("Invalid grass field input sequence");
    if (!_firstMissing && _lastSeen && tick.Sequence - _lastSeen > 1) _firstMissing = _lastSeen + 1;
    _lastSeen = tick.Sequence;
    if (_firstMissing || _ticks.size() == Capacity)
    {
        if (!_firstMissing) _firstMissing = tick.Sequence;
        _missingSeconds += tick.DeltaSeconds;
        return false;
    }
    const float dt = tick.DeltaSeconds;
    _ticks.push_back(std::move(tick));
    _pendingSeconds += dt;
    return true;
}

void GDX12GrassFieldInputQueue::Commit(uint64_t sequence)
{
    if (_ticks.empty() || _ticks.front().Sequence != sequence)
        throw std::logic_error("Grass field input commit out of order");
    _pendingSeconds -= _ticks.front().DeltaSeconds;
    _ticks.pop_front();
    if (_ticks.empty()) _pendingSeconds = 0;
}

void GDX12GrassFieldInputQueue::Reset()
{
    _supersededSeconds += _pendingSeconds;
    _ticks.clear();
    _pendingSeconds = 0;
    _missingSeconds = 0;
    _lastSeen = _firstMissing = 0;
}

void GDX12GrassFieldInputQueue::StartAfter(uint64_t sequence)
{
    if (!sequence || _lastSeen || !_ticks.empty())
        throw std::logic_error("Grass backlog requires a fresh checkpoint boundary");
    _lastSeen = sequence;
}
