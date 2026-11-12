#include "Rendering/Grass/BistroGrassRoute.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/Logging/Logger.h"

namespace
{
using namespace GDX12Submission;
using Graph = GDX12RenderGraph;

class GrassExactConsumer final : public GDX12RenderPass
{
public:
    GrassExactConsumer(Placement direct, Graph::Resource view, TransferVersion version)
        : _direct(direct), _view(view), _version(version) {}

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Bistro grass Exact field acquire");
        graph.SetPlacement(_direct);
        graph.DeclareCompleteAccesses();
        graph.HasSideEffects();
        graph.UseVersion(_view, Graph::Mode::Read, _version, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    bool SupportsGraphQueue(QueueKind queue) const override { return queue == QueueKind::Direct; }
    void RecordPlacedCommands(GDX12CommandList*, const Graph::BoundResources&) override {}

private:
    Placement _direct;
    Graph::Resource _view;
    TransferVersion _version;
};

CompletionPoint OrderGrassExact(RenderMGPURuntime& runtime, const CrossAdapterGrass::Frame& display)
{
    const auto& view = display.CurrentView;
    if (!display.Exact || !view.Version.IsValid() || !view.Resource)
        throw std::logic_error("Grass pending field requires an exact imported view");
    const Placement direct{ view.Identity.Adapter, QueueKind::Direct };
    Graph graph;
    const auto resource = graph.Import("Bistro grass primary packed field");
    graph.RegisterResource(resource, view.Identity);
    graph.ImportVersion(resource, view.Version);
    GrassExactConsumer acquire(direct, resource, view.Version);
    graph.AddPass(acquire);
    graph.CompilePlan();
    Graph::ExecutionBindings bindings;
    bindings.Views.push_back({ resource, view.Identity, view.Resource });
    bindings.ExactInputs.push_back({ resource, view.Version, display.Ready, display.Fields.Current });
    // The scene DIRECT list is still recording. Its first grass consumer and any later
    // COMPUTE fork are FIFO after this issued DIRECT point.
    const auto receipt = runtime.TrySubmit(graph, std::move(bindings));
    return receipt ? receipt->Batches.at(0) : CompletionPoint{};
}
}

bool BistroGrassRoute::PrepareFrame(RenderPipelineContext& context, BistroGrassGeometry& grass, bool visible)
{
    if (_display) throw std::logic_error("Grass readers were not retired on the previous frame");
    _diagnostics = {};
    auto controls = GetGrassMGPUControls();
    auto* runtime = context.Runtime.GetMGPURuntime();
    auto* secondary = context.Runtime.GetMGPUDevices().Secondary;
    if (controls.Enabled && runtime && secondary && !_controller)
        _controller = std::make_unique<CrossAdapterGrass>(*runtime, context.Primary, *secondary);

    using State = CrossAdapterGrass::State;
    const auto frame = context.Runtime.GetFrameSubmissionId();
    const auto sequence = static_cast<std::uint64_t>(frame);
    const auto now = CrossAdapterGrass::Clock::now();
    _selectedAt = now;
    if (visible || (_controller && _controller->GetState() != State::Primary))
        grass.CollectScene(context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0);

    if (_controller)
    {
        if (auto continuation = _controller->TakeLocalContinuation())
        {
            _resetRevision = continuation->ResetRevision;
            grass.ResumeLocalSolver(std::move(continuation->Solver), _resetRevision, std::move(continuation->Pending));
        }
        if (visible && controls.Enabled && _checkpointReady.IsValid() && _controller->GetState() == State::Primary)
            _controller->TryStart(_checkpoint, _checkpointTick, _checkpointReady);
        _checkpoint.reset();
        _checkpointReady = {};

        if (_controller->GetState() != State::Primary)
        {
            auto input = grass.CaptureFieldInputs(sequence, _resetRevision);
            _controller->Capture(std::move(input));
            _controller->Pump(controls, now);
            const auto inputStatus = _controller->GetInputStatus();
            _diagnostics.ExportBytes = inputStatus.ExportBytes;
            _diagnostics.ImportBytes = inputStatus.ImportBytes;
            _diagnostics.PendingSeconds = inputStatus.PendingSeconds;
            _diagnostics.MissingSeconds = inputStatus.MissingSeconds;
            if (_controller->GetState() == State::ResetRequired)
            {
                _diagnostics.ResetRequired = true;
                Log::Warning("Renderer", "Grass field epoch reset. Pending seconds ", inputStatus.PendingSeconds,
                    ", missing seconds ", inputStatus.MissingSeconds, ", first missing sequence ", inputStatus.FirstMissing);
                _controller->Reset();
                grass.RestartLocalSolver();
                context.Common.RequestHistoryReset = true;
            }
        }

        if (_controller->GetState() != State::Primary)
        {
            if (!visible) return false;
            _display = _controller->Select(frame, sequence, now);
            if (_display && _display->Ready.IsValid() && !(_display->CurrentView.Version == _orderedVersion))
            {
                _orderedReady = OrderGrassExact(*runtime, *_display);
                if (_orderedReady.IsValid())
                    _orderedVersion = _display->CurrentView.Version;
                else
                {
                    _controller->CancelFrame();
                    _display = _controller->Select(frame, sequence, now, false);
                    if (_display && _display->Ready.IsValid())
                        throw std::logic_error("Grass fallback exposed a pending field");
                }
            }
            if (!_display) return false;
            _diagnostics.Version = _display->CurrentView.Version;
            _diagnostics.Import = _display->Ready;
            if (_diagnostics.Version == _orderedVersion) _diagnostics.Acquire = _orderedReady;
            _diagnostics.Sequence = _display->Sequence;
            _diagnostics.Exact = _display->Exact;
            _diagnostics.AgeMs = _display->AgeMs;
            _diagnostics.OverAge = _display->OverAge;
            for (const auto& wind : _display->Fields.Current->GetLayout().Wind)
                _diagnostics.WindDebtSeconds = (std::max)(_diagnostics.WindDebtSeconds, wind.WindDebtSeconds);
            grass.PrepareFrame(context.Primary, context.Common, &_display->Fields);
            return true;
        }
    }

    if (!visible) return false;
    _diagnostics.LocalAuthoritative = true;
    grass.PrepareFrame(context.Primary, context.Common);
    if (_controller && controls.Enabled)
    {
        _checkpoint = grass.PrepareLocalCheckpoint();
        if (_checkpoint)
        {
            _resetRevision = _checkpoint->Layout.InteractionResetRevision;
            _checkpointTick = grass.CaptureFieldInputs(sequence, _resetRevision);
        }
    }
    return true;
}

void BistroGrassRoute::ResetWorld()
{
    if (_display) throw std::logic_error("Grass readers must retire before a world reset");
    if (_controller) _controller->Reset();
    _checkpoint.reset();
    _checkpointReady = _orderedReady = {};
    _orderedVersion = {};
    _resetRevision = 0;
    _diagnostics = {};
}

void BistroGrassRoute::RecordTail(GDX12CommandList& commands)
{
    if (_display) _controller->RecordFrameRelease(commands);
}

void BistroGrassRoute::RetireFrame(GDX12Submission::CompletionPoint tail)
{
    if (_display)
    {
        _controller->RetireFrame(tail);
        _diagnostics.Tail = tail;
        _display.reset();
    }
    if (_checkpoint) _checkpointReady = tail;
}
