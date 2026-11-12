#pragma once

#include "Rendering/Grass/CrossAdapterGrass.h"

class BistroGrassGeometry;
struct RenderPipelineContext;

class BistroGrassRoute final
{
public:
    bool PrepareFrame(RenderPipelineContext& context, BistroGrassGeometry& grass, bool visible);
    void RecordTail(GDX12CommandList& commands);
    void RetireFrame(GDX12Submission::CompletionPoint tail);
    void ResetWorld();
    struct Diagnostics
    {
        GDX12Submission::TransferVersion Version;
        GDX12Submission::CompletionPoint Import, Acquire, Tail;
        std::uint64_t Sequence = 0, ExportBytes = 0, ImportBytes = 0;
        double AgeMs = 0;
        double PendingSeconds = 0, MissingSeconds = 0, WindDebtSeconds = 0;
        bool ResetRequired = false;
        bool Exact = false, OverAge = false, LocalAuthoritative = false;
    };
    Diagnostics GetDiagnostics() const
    {
        auto result = _diagnostics;
        if (result.Version.IsValid())
            result.AgeMs += std::chrono::duration<double, std::milli>(CrossAdapterGrass::Clock::now() - _selectedAt).count();
        result.OverAge = result.AgeMs > GetGrassMGPUControls().MaxAgeMs;
        return result;
    }
    const CrossAdapterGrass::Frame* GetDisplay() const { return _display ? &*_display : nullptr; }

private:
    std::unique_ptr<CrossAdapterGrass> _controller;
    std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> _checkpoint;
    CrossAdapterGrass::Tick _checkpointTick{};
    GDX12Submission::CompletionPoint _checkpointReady;
    std::optional<CrossAdapterGrass::Frame> _display;
    GDX12Submission::TransferVersion _orderedVersion;
    GDX12Submission::CompletionPoint _orderedReady;
    Diagnostics _diagnostics;
    CrossAdapterGrass::Clock::time_point _selectedAt{};
    std::uint64_t _resetRevision = 0;
};
