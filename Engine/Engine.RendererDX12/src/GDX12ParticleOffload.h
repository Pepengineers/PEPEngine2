#pragma once
#include "GDX12ParticleResources.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

// All marked emitters share one solver device, worker and publication ring.
class GDX12ParticleOffload
{
public:
    struct Entry
    {
        uint64_t Id, ResetRevision;
        UINT Capacity, Words, Base;
    };
    struct Statistics
    {
        uint64_t Steps = 0, Publications = 0, Imports = 0;
        uint64_t ExportCopies = 0, ImportCopies = 0, InputCopies = 0;
        uint64_t LastImported = 0, PayloadBytes = 0;
        std::array<uint64_t, 3> SlotPublications{};
    };
    GDX12ParticleOffload(GDX12DeviceResources*, ComPtr<IDXGIAdapter4>);
    ~GDX12ParticleOffload();
    void Configure(const std::vector<GDX12ParticleEmitter>&, UINT interactionResolution,
        const std::unordered_map<uint64_t, uint64_t>& burstSeeds = {});
    void Record(GDX12CommandList*, const std::vector<GDX12ParticleEmitter>&,
        const GDX12ParticleInteractionField&, bool resetMotion);
    bool Find(uint64_t id, UINT& base) const;
    uint64_t ConsumedBurst(uint64_t id) const;
    void Stop();
    ParticleResources::Buffer<ParticleResources::Particle>& Display() { return *_display; }
    std::vector<GDX12Resource*> Resources() const;
    Statistics GetStatistics() const;
    // Diagnostics drain only; never used by the ordinary frame path.
    std::vector<UINT> ReadPayload(bool sent);
    std::vector<ParticleResources::Particle> ReadSolver(uint64_t id);
    ID3D12Device* SecondaryDevice() { return _secondary.GetDevice().Get(); }
private:
    struct Exchange
    {
        ComPtr<ID3D12Heap> SecondaryHeap, PrimaryHeap;
        ComPtr<ID3D12Resource> Secondary, Primary;
    };
    struct Slot : Exchange { uint64_t Sequence = 0; bool Published = false; };
    void Run();
    void Simulate(const std::vector<GDX12ParticleEmitter>&, GDX12ParticleInteractionField);
    Exchange CreateExchange(uint64_t bytes);
    void Start();
    std::array<UINT, 16> Parameters(const Entry&, size_t, uint64_t sequence) const;

    GDX12DeviceResources* _primary;
    GDX12Device _secondary;
    GDX12DeviceResources _resources;
    GDX12ParticleSimulation _simulation;
    RenderPipelineCommonData _common;
    std::unique_ptr<GDX12RootSignature> _packRoot, _displayRoot;
    ComPtr<ID3D12PipelineState> _packPSO, _displayPSO;
    std::unique_ptr<ParticleResources::Buffer<UINT>> _packed, _received;
    std::unique_ptr<ParticleResources::Buffer<ParticleResources::Particle>> _display;
    std::unique_ptr<ParticleResources::Buffer<XMFLOAT4>> _field;
    Exchange _inputField;
    std::array<Slot, 3> _slots;
    ComPtr<ID3D12Fence> _readySecondary, _readyPrimary, _freePrimary, _freeSecondary;
    ComPtr<ID3D12Fence> _inputReadyPrimary, _inputReadySecondary;
    std::vector<Entry> _entries;
    UINT _words = 0, _resolution = 0;
    size_t _nextSlot = 0;
    uint64_t _generation = 0, _inputSequence = 0;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _worker;
    bool _stop = false, _busy = false;
    bool _jobNeedsInput = false;
    std::vector<GDX12ParticleEmitter> _job;
    GDX12ParticleInteractionField _jobField;
    std::exception_ptr _failure;
    Statistics _statistics;
};
