#pragma once
#include "Rendering/Water/GDX12FluidSolver.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

// One persistent solver and a three-slot position-only exchange, for one water instance.
class GDX12WaterOffload
{
public:
    struct Result
    {
        uint64_t Sequence = 0, Steps = 0;
        GDX12FluidSolver::Steps Batch;
        GDX12FluidInstance Input;
    };
    GDX12WaterOffload(GDX12Device* primary, GDX12DescriptorHeap* primaryHeap,
        ComPtr<IDXGIAdapter4> secondaryAdapter, const GDX12FluidInstance& seed);
    ~GDX12WaterOffload();
    void Start();
    void Stop();
    void Update(const GDX12FluidInstance& input);
    // The worker (or deterministic validation driver) owns secondary queue calls.
    void Simulate(const GDX12FluidInstance& input, GDX12FluidSolver::Steps steps);
    bool UpdateDisplay();
    GDX12UploadBuffer<GDX12FluidSolver::Particle>& DisplayParticles() { return *_display; }
    Result LastResult() const;
    uint64_t GetPayloadBytes() const { return uint64_t(_buffers.Count) * sizeof(XMFLOAT3); }
    std::vector<GDX12FluidSolver::Particle> ReadSolverState();
    std::vector<XMFLOAT3> ReadSentPositions();
private:
    struct Slot
    {
        ComPtr<ID3D12Heap> SecondaryHeap, PrimaryHeap;
        ComPtr<ID3D12Resource> Secondary, Primary;
        Result Produced;
        bool Published = false;
    };
    void Run();
    GDX12Device* _primary;
    GDX12DescriptorHeap* _displayHeap;
    GDX12Device _secondary;
    std::unique_ptr<GDX12DescriptorHeap> _heap;
    GDX12FluidSolver _solver;
    GDX12FluidSolver::Buffers _buffers;
    GDX12FluidSolver::Head _head;
    std::unique_ptr<GDX12UploadBuffer<GDX12FluidParameters>> _constants;
    std::unique_ptr<GDX12UploadBuffer<XMFLOAT3>> _packed, _received;
    std::unique_ptr<GDX12UploadBuffer<GDX12FluidSolver::Particle>> _display, _seed;
    std::unique_ptr<GDX12RootSignature> _packRoot, _displayRoot;
    ComPtr<ID3D12PipelineState> _packPSO, _displayPSO;
    ComPtr<ID3D12Fence> _readySecondary, _readyPrimary, _freePrimary, _freeSecondary;
    std::array<Slot, 3> _slots;
    uint64_t _sequence = 0, _steps = 0;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    GDX12FluidInstance _input;
    Result _last;
    std::exception_ptr _failure;
    bool _stop = false;
    std::thread _worker;
};
