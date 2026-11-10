#pragma once

#include "Components/BistroInteractionSource.h"
#include "Rendering/Grass/GDX12GrassWind.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>

// Device-local field simulation. Callers own command submission and slot retirement.
class GDX12GrassFieldProducer
{
public:
    using FieldBuffer = GDX12UploadBuffer<XMFLOAT4>;
    static constexpr UINT InteractionResolution = 64;
    static constexpr size_t MaxStepsPerInvocation = 4;

    struct Inputs
    {
        const std::vector<GDX12WindInstance>& Wind;
        const std::vector<BistroInteractionSource>& Actors;
        XMFLOAT4 InteractionArea;
        float DeltaSeconds = 0;
        int WindQuality = 2;
        bool ResetWind = false;
        bool NeedNeutralWind = false;
        uint64_t InteractionResetRevision = 0;
        bool CarryWindDebt = false;
    };
    struct Region
    {
        UINT FirstElement = 0;
        UINT ElementCount = 0;
    };
    struct WindRegion
    {
        uint64_t Owner = 0;
        Region Velocity;
        XMFLOAT4 Area = {};
        UINT Resolution = 0;
        GDX12WindImplementation Mode = GDX12WindImplementation::Analytic;
        uint64_t SettingsRevision = 0;
        float TimeSeconds = 0;
        double WindDebtSeconds = 0;
    };
    struct SnapshotLayout
    {
        XMFLOAT4 InteractionArea = {};
        UINT InteractionGrid = InteractionResolution;
        uint64_t InteractionRevision = 0;
        uint64_t InteractionResetRevision = 0;
        uint64_t ActorRevision = 0;
        double InteractionTimeSeconds = 0;
        Region Presence, Trail;
        std::vector<WindRegion> Wind;
        UINT ElementCount = 0;

        bool IsCompatibleWith(const SnapshotLayout& other) const;
        bool Matches(const Inputs& inputs) const;
    };
    struct WindView
    {
        FieldBuffer* Current = nullptr;
        FieldBuffer* Previous = nullptr;
        XMFLOAT4 Area = {};
        UINT Resolution = 0;
        uint64_t SettingsRevision = 0;
        float TimeSeconds = 0;
        float PreviousTimeSeconds = 0;
        bool ResetHistory = true;
    };

    GDX12GrassFieldProducer(GDX12Device& device, GDX12DescriptorHeap& heap, size_t slots);

    // Prepare copies input data and stages a state head. Reuse a slot only after it retires.
    // An empty submit retries the staged invocation. Commit only after admission.
    // The legacy renderer commits at frame preparation, as it did before placed execution.
    void Prepare(UINT slot, const Inputs& inputs);
    void PrepareBatch(UINT slot, const Inputs* inputs, size_t count);
    // Seed a fresh device-local solver from the last authoritative packed checkpoint.
    // The caller provides the matching captured settings and a graph-owned COPY_SOURCE.
    void PrepareSeed(UINT slot, const Inputs& inputs, const SnapshotLayout& layout);
    void RecordSeed(GDX12CommandList& commands, UINT slot, ID3D12Resource& packed) const;
    void Commit(UINT slot);
    // Only for a proven no-submit. After successful Record, discard only packed recordings.
    void Cancel(UINT slot);
    // Optional packed buffer is a graph-owned, device-local COPY_DEST buffer.
    void Record(GDX12CommandList& commands, UINT slot, ID3D12Resource* packed = nullptr) const;
    void AppendAccesses(UINT slot, std::vector<GDX12RenderGraph::BufferAccess>& accesses) const;
    const SnapshotLayout& Snapshot(UINT slot) const { return _frames.at(slot).Layout; }
    bool MatchesCurrent(const Inputs& inputs) const
    { return _preparedSlot && Snapshot(*_preparedSlot).Matches(inputs); }
    bool CanInstall(const GDX12Device& device, const GDX12DescriptorHeap& heap, size_t slots) const
    {
        return &device == &_device && &heap == &_heap && slots == _frames.size() &&
            _interactionRevision != 0 && !_pendingSlot;
    }
    FieldBuffer& Presence() const { return *_presence; }
    FieldBuffer& Trail() const { return *_trail; }
    FieldBuffer& PreviousTrail() const { return *_previousTrail; }
    FieldBuffer* NeutralWind() const { return _neutralWind.get(); }
    std::optional<WindView> FindWind(uint64_t owner) const;

private:
    struct WindParameters
    {
        XMFLOAT4 Area, Force, Gust, Solver;
        UINT Resolution, Padding0, Padding1, Padding2;
    };
    static_assert(sizeof(WindParameters) == 80);
    struct InteractionParameters
    {
        XMFLOAT4 Area;
        float DeltaSeconds;
        UINT SourceCount, Reset, Resolution;
    };
    static_assert(sizeof(InteractionParameters) == 32);
    struct WindState
    {
        GDX12WindInstance Source;
        std::array<std::shared_ptr<FieldBuffer>, 2> Velocity, Pressure;
        std::shared_ptr<FieldBuffer> Advected, Divergence;
        float Time = 0;
        float PreviousTime = 0;
        double DebtSeconds = 0;
        UINT Resolution = 0;
        bool Initialized = false;
        bool ResetHistory = true;
        uint64_t SettingsHash = 0;
    };
    struct WindJob
    {
        UINT Stage, Resolution;
        GDX12UploadBuffer<WindParameters>* Constants;
        FieldBuffer *Input, *Pressure, *Output;
    };
    struct InteractionJob
    {
        std::unique_ptr<GDX12UploadBuffer<BistroInteractionSource>> Sources;
        std::unique_ptr<GDX12UploadBuffer<InteractionParameters>> Constants;
    };
    struct Frame
    {
        std::vector<std::unique_ptr<GDX12UploadBuffer<WindParameters>>> WindConstants;
        std::unordered_map<uint64_t, WindState> Wind;
        std::vector<WindJob> WindJobs;
        std::array<InteractionJob, MaxStepsPerInvocation> InteractionJobs;
        size_t InteractionCount = 0;
        UINT WindCursor = 0;
        SnapshotLayout Layout;
        bool HasNeutralWind = false;
        bool IsSeed = false;
    };

    std::unique_ptr<FieldBuffer> MakeField(UINT count) const;
    void AppendStep(Frame& frame, const Inputs& inputs);
    void UpdateWind(Frame& frame, WindState& state, bool reset, bool carryDebt);
    void WindDispatch(Frame& frame, UINT stage, const WindParameters& parameters,
        FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output);
    void RecordPacked(GDX12CommandList& commands, const SnapshotLayout& layout, ID3D12Resource& packed) const;

    GDX12Device& _device;
    GDX12DescriptorHeap& _heap;
    std::vector<Frame> _frames;
    std::optional<UINT> _preparedSlot, _pendingSlot;
    std::unordered_map<uint64_t, WindState> _wind;
    std::unique_ptr<GDX12RootSignature> _windRoot, _interactionRoot;
    std::array<ComPtr<ID3D12PipelineState>, 7> _windPSO;
    ComPtr<ID3D12PipelineState> _interactionPSO;
    std::unique_ptr<FieldBuffer> _presence, _trail, _previousTrail, _neutralWind;
    XMFLOAT4 _interactionArea = {};
    uint64_t _interactionRevision = 0;
    uint64_t _interactionResetRevision = 0;
    double _interactionTimeSeconds = 0;
    bool _neutralInitialized = false;
};

// Per-device descriptors for a local packed resource. The transport/replica owner must
// retain the resource and its reader lease until the last submitted use of these views.
class GDX12GrassFieldSnapshot
{
public:
    using Layout = GDX12GrassFieldProducer::SnapshotLayout;
    using WindRegion = GDX12GrassFieldProducer::WindRegion;
    struct View
    {
        GDX12Resource* Resource = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE SRV = {};
    };

    GDX12GrassFieldSnapshot(GDX12DescriptorHeap& heap, GDX12Resource& local, Layout layout,
        std::shared_ptr<GDX12Resource> owner = {});

    const Layout& GetLayout() const { return _layout; }
    const GDX12DescriptorHeap* GetHeap() const { return &_heap; }
    View Presence() const { return { &_local, _presence->GPUHandle }; }
    View Trail() const { return { &_local, _trail->GPUHandle }; }
    const WindRegion* FindWind(uint64_t owner) const;
    bool MatchesWind(const GDX12WindInstance& wind, int quality) const;
    View Wind(uint64_t owner) const;

private:
    GDX12DescriptorHeap& _heap;
    GDX12Resource& _local;
    std::shared_ptr<GDX12Resource> _owner;
    Layout _layout;
    std::unique_ptr<GDX12Descriptor> _presence, _trail;
    std::vector<std::unique_ptr<GDX12Descriptor>> _wind;
};

// One routed epoch, initialized with the frozen authoritative primary field. Latest offers
// only completed local imports. Exact uses the same cursor with its actual readiness edge.
class GDX12GrassLatestSelection
{
public:
    struct Stamp
    {
        uint64_t Sequence = 0;
        GDX12GrassFieldProducer::SnapshotLayout Layout;
        std::chrono::steady_clock::time_point CapturedAt = {};
    };

    GDX12GrassLatestSelection(GDX12Submission::TransferId stream, GDX12Submission::Epoch epoch, Stamp seed);
    bool TryAdopt(GDX12Submission::TransferVersion version, Stamp completed);
    const Stamp& Displayed() const { return _displayed; }
    double AgeMs(std::chrono::steady_clock::time_point now) const;
    bool OverAge(std::chrono::steady_clock::time_point now, float maxAgeMs) const;

private:
    GDX12Submission::TransferId _stream;
    GDX12Submission::Epoch _epoch;
    GDX12Submission::ProducerInvocationId _invocation = GDX12Submission::ProducerInvocationId::Invalid;
    Stamp _displayed;
};

// One immutable sample per application tick. A full queue latches the first missing
// sequence until the owner explicitly starts a new epoch.
class GDX12GrassFieldInputQueue
{
public:
    static constexpr size_t Capacity = 16;
    struct Tick
    {
        uint64_t Sequence = 0;
        std::vector<GDX12WindInstance> Wind;
        std::vector<BistroInteractionSource> Actors;
        XMFLOAT4 InteractionArea = {};
        float DeltaSeconds = 0;
        int WindQuality = 2;
        bool ResetWind = false;
        uint64_t InteractionResetRevision = 0;
        std::chrono::steady_clock::time_point CapturedAt = {};

        GDX12GrassFieldProducer::Inputs Inputs() const
        {
            return { Wind, Actors, InteractionArea, DeltaSeconds, WindQuality, ResetWind, false,
                InteractionResetRevision, true };
        }
    };

    bool Push(Tick tick);
    const Tick* Front() const { return _ticks.empty() ? nullptr : &_ticks.front(); }
    const Tick& At(size_t index) const { return _ticks.at(index); }
    size_t Size() const { return _ticks.size(); }
    uint64_t LastSeen() const { return _lastSeen; }
    void Commit(uint64_t sequence);
    void Reset();
    void StartAfter(uint64_t sequence);
    uint64_t FirstMissing() const { return _firstMissing; }
    double PendingSeconds() const { return _pendingSeconds; }
    double MissingSeconds() const { return _missingSeconds; }
    double SupersededSeconds() const { return _supersededSeconds; }

private:
    std::deque<Tick> _ticks;
    uint64_t _lastSeen = 0;
    uint64_t _firstMissing = 0;
    double _pendingSeconds = 0, _missingSeconds = 0, _supersededSeconds = 0;
};
