#pragma once

#include "Engine.UI/WorldCanvas.h"
#include "Engine.RendererDX12/GDX12Submission.h"
#include <optional>
#include <string_view>
#include <vector>

class GDX12Resource;
class RenderMGPURuntime;

namespace Engine::UI
{
    enum class UIOffloadScope { Whole, Canvas, Count };
    enum class UIVisualFreshness { Exact, Latest, Count };

    struct UIOffloadSettings
    {
        bool Enabled = false;
        UIOffloadScope Scope = UIOffloadScope::Whole;
        CanvasId Selected = CanvasId::Invalid;
        UIVisualFreshness Freshness = UIVisualFreshness::Exact;
        double UpdateHz = 0;
        double MaxAgeMs = 0;
    };

    UIOffloadSettings ReadUIOffloadSettings();
    bool RequestsUIOffload(const UIOffloadSettings& settings, CanvasId surface);
    // The caller supplies monotonic elapsed seconds and validated controls.
    bool UIUpdateDue(double nowSeconds, std::optional<double> lastAdmissionSeconds, double updateHz);

    struct UISnapshotAge
    {
        double Milliseconds = 0;
        bool OverAge = false;
    };

    UISnapshotAge GetUISnapshotAge(double nowSeconds, double capturedSeconds, double maxAgeMs);

    struct UIPlacementStatus
    {
        bool Requested = false;
        GDX12Submission::AdapterId ActiveAdapter = GDX12Submission::AdapterId::Invalid;
        GDX12Submission::SnapshotReadMode Mode = GDX12Submission::SnapshotReadMode::ExactVersion;
        GDX12Submission::TransferVersion Displayed;
        std::uint64_t FrameAge = 0;
        double CapturedSeconds = 0;
        UISnapshotAge Age;
        std::uint64_t PayloadBytes = 0;
        std::uint64_t FootprintBytes = 0;
        std::uint64_t MaterialCopyBytes = 0;
        std::uint64_t Deferred = 0;
        std::uint64_t Dropped = 0;
        std::string_view Fallback;
    };

    // The serialized render owner uses CanvasId::Invalid as the screen key.
    // Devices and runtime outlive this controller. Drain the runtime before UI teardown.
    class CrossAdapterUI
    {
    public:
        CrossAdapterUI(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary,
            const std::vector<CanvasId>& canvases);
        ~CrossAdapterUI();
        CrossAdapterUI(const CrossAdapterUI&) = delete;
        CrossAdapterUI& operator=(const CrossAdapterUI&) = delete;

        void BeginFrame(GDX12Submission::FrameSubmissionId frame, UIOffloadSettings settings, double nowSeconds);
        void Invalidate(CanvasId surface);
        UIPlacementStatus GetStatus(CanvasId surface) const;

        // Call outside a legacy replacement after runtime Poll. Pausing stops new
        // publications while allowing already issued optional work to retire.
        void PumpLatest(std::shared_ptr<const CanvasFrame> frame, bool allowNewPublication);

        // Primary target is the retained legacy output tracker in COMMON. An empty
        // result leaves the current frame eligible for local recording.
        std::optional<GDX12Submission::CompletionPoint> SubmitCanvas(const CanvasFrame& frame,
            std::shared_ptr<GDX12Resource> materialTarget, GDX12Submission::CompletionPoint prefix);
        std::optional<GDX12Submission::CompletionPoint> SubmitScreen(std::shared_ptr<const DrawData> draw,
            std::shared_ptr<GDX12Resource> backbuffer, GDX12Submission::CompletionPoint prefix);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
