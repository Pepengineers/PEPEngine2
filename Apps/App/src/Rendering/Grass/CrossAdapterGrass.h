#pragma once

#include "Rendering/Grass/GDX12GrassRenderer.h"

class RenderMGPURuntime;

// Serialized by the render thread. Native work is submitted only by P4. The integrator
// captures primary checkpoints and binds exact readiness/last readers at the legacy boundary.
class CrossAdapterGrass
{
public:
    using Clock = std::chrono::steady_clock;
    using Tick = GDX12GrassFieldInputQueue::Tick;
    using CompletionPoint = GDX12Submission::CompletionPoint;
    using FrameId = GDX12Submission::FrameSubmissionId;

    enum class State { Primary, Secondary, Returning, ResetRequired };
    struct LocalView
    {
        GDX12Submission::ResourceIdentity Identity;
        std::shared_ptr<GDX12Resource> Resource;
        GDX12Submission::TransferVersion Version;
    };
    struct Frame
    {
        GDX12GrassRenderer::DisplayedFields Fields;
        // Canonical primary-local owners and version for the P2d/P4 consumer.
        LocalView CurrentView, PreviousView;
        // Only Exact can require a dependency. Latest/anchor are already local.
        CompletionPoint Ready;
        uint64_t Sequence = 0;
        double AgeMs = 0;
        bool OverAge = false;
        bool Exact = false;
    };
    struct LocalContinuation
    {
        std::shared_ptr<GDX12GrassFieldProducer> Solver;
        GDX12GrassFieldInputQueue Pending;
        uint64_t Sequence = 0;
        uint64_t ResetRevision = 0;
    };
    struct InputStatus
    {
        uint64_t LastIssued = 0;
        uint64_t FirstMissing = 0;
        double PendingSeconds = 0;
        double MissingSeconds = 0;
        double SupersededSeconds = 0;
    };

    CrossAdapterGrass(RenderMGPURuntime& runtime, GDX12DeviceResources& primary, GDX12Device& secondary);
    ~CrossAdapterGrass();
    CrossAdapterGrass(const CrossAdapterGrass&) = delete;
    CrossAdapterGrass& operator=(const CrossAdapterGrass&) = delete;

    State GetState() const;
    // checkpointReady is the issued primary DIRECT tail of the checkpoint's own frame.
    bool TryStart(std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> checkpoint,
        const Tick& checkpointInput, CompletionPoint checkpointReady);
    bool Capture(Tick input);
    void Pump(const GDX12GrassMGPUControls& controls, Clock::time_point now);
    // One staged selection per frame. RetireFrame commits it after binding the actual tail.
    // CancelFrame preserves the last displayed snapshot and can retry an outstanding Exact demand.
    // A same-frame retry can exclude pending Exact after a proven no-submit bridge refusal.
    // Empty indicates no compatible completed binding or no reader credit for a routed frame.
    std::optional<Frame> Select(FrameId frame, uint64_t demandedSequence, Clock::time_point now,
        bool allowPendingExact = true);
    // Record after the final grass/particle use and the legacy async join.
    void RecordFrameRelease(GDX12CommandList& commands) const;
    void RetireFrame(CompletionPoint tail);
    void CancelFrame();
    std::optional<LocalContinuation> TakeLocalContinuation();
    // An authored incompatible reset closes the old epoch and invalidates display.
    // Call at a frame boundary after retiring or cancelling the selected readers.
    void Reset();
    InputStatus GetInputStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
