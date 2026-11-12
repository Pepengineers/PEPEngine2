#pragma once

#include "Rendering/Water/GDX12FluidPass.h"

class RenderMGPURuntime;

class CrossAdapterWater final
{
public:
    struct Diagnostics
    {
        bool Requested = false, SecondaryOwns = false, Exact = false, ResetRequired = false, OverAge = false;
        const char* Status = "local";
        double Desired = 0, Issued = 0, Completed = 0;
        double AcceptedDebt = 0, InFlight = 0, Unrepresented = 0, ResetSuperseded = 0;
        double SnapshotAgeSeconds = -1, DisplaySimulationLag = 0;
        double DisplayedSimulationTime = -1, SourceAgeSeconds = -1;
        std::uint64_t FirstMissingInput = 0, SolverInvocations = 0, ExportBytes = 0, ImportBytes = 0;
        std::uint64_t SkippedPublications = 0, DiscardedReady = 0, ExactDeferrals = 0;
    };

    CrossAdapterWater();
    ~CrossAdapterWater();
    CrossAdapterWater(const CrossAdapterWater&) = delete;
    CrossAdapterWater& operator=(const CrossAdapterWater&) = delete;

    // Surface eligibility describes the frame graph. Input capture still runs without Surface.
    void Tick(const GDX12FluidInstance* instance, const GDX12FluidControls& controls,
        RenderMGPURuntime* runtime, GDX12DeviceResources& primary, GDX12Device* secondary, bool surfaceEligible);
    void SelectFrame(GDX12Submission::FrameSubmissionId frame);
    bool WantsExactBoundary() const;
    std::optional<GDX12Submission::CompletionPoint> SubmitExact(GDX12Submission::CompletionPoint prefix);
    void RetireFrame(GDX12Submission::CompletionPoint tail);

    std::uint64_t RoutedOwner() const;
    const std::optional<GDX12FluidDisplay>& Display() const;
    Diagnostics GetDiagnostics() const;

private:
    struct State;
    std::unique_ptr<State> _state;
};
