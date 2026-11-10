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
    struct Diagnostics
    {
        GDX12Submission::TransferVersion Version;
        GDX12Submission::CompletionPoint Import, Acquire, Tail;
        std::uint64_t Sequence = 0;
        double AgeMs = 0;
        bool Exact = false, OverAge = false, LocalAuthoritative = false;
    };
    const Diagnostics& GetDiagnostics() const { return _diagnostics; }
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
    std::uint64_t _resetRevision = 0;
};
