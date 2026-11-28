#pragma once

#include "App.Base/ECS/World.h"
#include "BistroBenchmarkRoute.h"
#include "BistroBenchmarkCsv.h"
#include "BistroBenchmarkLaunch.h"
#include "BistroPlacementSearch.h"
#include "BistroProbeFreshness.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "BistroEntrance.h"
#include "Components/BistroInteractionSource.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/ConsoleModule.h"
#include <cmath>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <bitset>
#include <deque>
#include <optional>
#include <tuple>

class Window;
namespace BistroCharacters { class Scene; }

class BistroBenchmark
{
    using Clock = std::chrono::steady_clock;
    using Point = BistroBenchmarkRoute::Point;
    struct Pose { Vector3 Position, Forward; };

    class Shot
    {
    public:
        template<size_t N> explicit Shot(const Point (&points)[N])
            : _points(points), _count(N)
        {
            for (size_t i = 0; i + 1 < N; ++i)
            {
                const auto delta = points[i+1].Position - points[i].Position;
                const float length = delta.Length();
                assert(length > 0);
                _distances.push_back(_distances.back() + length);
                _curve.Points.back().LeaveTangent = Tangent(i) * (length / 3);
                SplinePoint next{};
                next.Position = points[i+1].Position;
                next.ArriveTangent = Tangent(i+1) * (-length / 3);
                _curve.Points.push_back(next);
            }
            _times.resize(N);
        }

        float Distance() const { return _distances.back(); }
        float Distance(size_t begin, size_t end) const { return _distances[end] - _distances[begin]; }
        template<class Pace> void SetPace(Pace secondsPerCm, float paintingHold = 0)
        {
            _paintingHold = paintingHold;
            for (size_t i = 1; i < _times.size(); ++i)
                _times[i] = _times[i - 1] + Hold(i - 1) + Distance(i - 1, i) * secondsPerCm(i);
        }

        float Duration() const { return _times.back(); }
        float At(size_t i) const { return _times.at(i); }
        Pose Sample(float time) const;
    private:
        float _paintingHold = 0;
        float Hold(size_t i) const { return _points[i].PaintingStop ? _paintingHold : 0.f; }
        float SegmentSpeed(size_t i) const;
        float KnotSpeed(size_t i) const;
        Vector3 Tangent(size_t i) const;
        const Point* _points;
        size_t _count;
        SplineCurveComponent _curve = [&]
        {
            SplineCurveComponent curve;
            SplinePoint first{};
            first.Position = _points[0].Position;
            curve.Points.push_back(first);
            return curve;
        }();
        std::vector<float> _distances{0};
        std::vector<float> _times;
    };

    static constexpr std::array Techniques{
        L"Render.AO.AsyncCompute", L"Render.Sky.AsyncCompute", L"Render.LightGrid.AsyncCompute", L"Render.Particles.AsyncCompute",
        L"Render.Water.AsyncCompute", L"Render.CloudPicture.AsyncCompute"};
    static constexpr double Budget = 600, FinishReserve = 4;
    static constexpr unsigned Cases = 1u << Techniques.size();
    inline static AutoConsoleVariable<float> FlightSeconds{L"App.Benchmark.FlightSeconds", 100.f,
        L"Flight duration excluding warmup, including the painting stops.",
        EConsoleVariableFlags::Default,
        [](float seconds) { return std::isfinite(seconds) && seconds >= 1; }};
public:
    enum class Action { None, Automatic, Search, Exhaustive, SingleGPU, MultiGPU, PlacementRun, Cancel };
private:
    enum class Stage { Baseline, Control, Repeat, Singles, Pairs, Neighbors, Elimination, Tiebreak, FinalControl, Final, Exhaustive, Placement };
    using Stats = BistroBenchmarkData::Stats;
    using Csv = BistroBenchmarkData::Csv;
    using Config = std::vector<std::pair<std::string, std::string>>;
    struct Window { double Begin, End, Weight, AdditionalWarmup = 0; };
    using Windows = std::array<Window, 4>;
    using Measurements = std::array<Stats, 4>;
    using PhysicsStates = std::array<std::function<void()>, 4>;
    struct Checkpoint { double Time; std::function<void()> Restore; };
    struct Probe
    {
        Window Range{};
        Stats Reference;
        unsigned Groups = 0;
        size_t Phase = 0;
        std::function<void()> Restore;
    };
    struct Rating
    {
        std::array<double, 4> Gain{};
        std::array<unsigned, 4> Samples{};

        void Add(size_t phase, double gain)
        {
            Gain[phase] += gain;
            ++Samples[phase];
        }
    };

public:
    inline static AutoConsoleVariable<bool> AutoStart{L"App.Benchmark.AutoStart", false,
        L"Run quick Bistro queue tuning at Release startup."};
    inline static AutoConsoleVariable<bool> SkipBenchmark{L"App.Benchmark.SkipBenchmark", false,
        L"Skip automatic Bistro queue tuning at Release startup."};
    inline static AutoConsoleVariable<float> PlacementBudgetSeconds{L"App.Benchmark.PlacementBudgetSeconds", 600.f,
        L"Profile search budget in seconds per eligible physical GPU, including the scene GPU.",
        EConsoleVariableFlags::Default, [](float seconds) { return std::isfinite(seconds) && seconds > 0; }};

    static double PlacementBudget(size_t peers) { return double(PlacementBudgetSeconds.GetValue()) * (1 + peers); }

    inline static AutoConsoleVariable<float> WarmupSeconds{L"App.Benchmark.WarmupSeconds", 3.f,
        L"Base warmup in seconds for each benchmark pass; captured when a run starts.",
        EConsoleVariableFlags::Default, [](float seconds) { return std::isfinite(seconds) && seconds >= 0; }};

    inline static AutoConsoleVariable<float> PaintingHoldSeconds{L"App.Benchmark.PaintingHoldSeconds", 5.f,
        L"Seconds at each painting stop. Zero skips the pause. Flight time includes all painting stops.",
        EConsoleVariableFlags::Default, [](float seconds) { return std::isfinite(seconds) && seconds >= 0; }};

    struct CompletedPass
    {
        std::filesystem::path Path;
        BistroBenchmarkData::RunMode Mode;
    };
    static size_t ResultSlot(BistroBenchmarkData::RunMode mode, BistroPlacement::Profile profile);
    static const char* SectionName(size_t section);

    size_t LatestResultSlot() const { return _latestResultSlot; }
    const CompletedPass& LastPass(size_t slot) const { return _lastPasses.at(slot); }
    const auto& ReportPath() const { return _path; }
    const std::string& Message() const { return _message; }
    double FlightTime() const { return _active ? Duration() : FlightSeconds.GetValue(); }
    double BaseWarmup() const { return _active ? _warmup : WarmupSeconds.GetValue(); }
    float PaintingHold() const { return _active ? _paintingHold : PaintingHoldSeconds.GetValue(); }
    bool CanStart() const { return !_active && _action == Action::None; }
    void Launch(BistroRun::Launch launch, ConsoleModule& console);
    const BistroRun::Launch* PendingLaunch() const { return !_active && _action != Action::None ? &_launch : nullptr; }
    double HudRemainingSeconds() const;
    std::optional<double> HudPhaseRemainingSeconds() const;
    std::string HudPhase() const;
    bool ManualGpuTimings() const { return _stage == Stage::Placement && _manualPlacement && _manualGpuTimings; }
    enum class Outcome { Completed, Failed, Cancelled };
    void AbortLaunch(std::string message, Outcome outcome);
    std::optional<Outcome> TakeOutcome() { return std::exchange(_outcome, std::nullopt); }
    int RunPercent() const;
    bool Active() const { return _active; }

    static std::filesystem::path DefaultsPath();
    static int QueueGroup(std::string_view name);
    void Stop(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, const char* status = "cancelled");

    void Initialize(World& world, BistroCharacters::Scene& characters, BistroRenderPipeline& pipeline);
    void ReleaseScene();
    uint64_t LegNumber() const { return _legNumber; }
    void RequestCancel()
    {
        if (_active) _action = Action::Cancel;
        else if (_action != Action::None) AbortLaunch("Map loading cancelled", Outcome::Cancelled);
    }
    std::wstring Caption() const;

    void Update(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline, ::Window& window);
    void FrameRendered(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline,
        bool presented);
private:
    static float Ease(float t);
    static Vector3 BlendDirection(Vector3 a, Vector3 b, float t);
    float Duration() const
    {
        return 2 * _route->Red.Duration() + _route->Blue.Duration() + _route->Green.Duration() + _route->Yellow.Duration();
    }

    void SetFlightDuration(float seconds);
    Pose Flight(float time) const;
    void AnimateDoor(WorldECS& ecs, float time);

    static ConsoleVariablePatch Placement(unsigned mask);
    static ConsoleVariablePatch EnabledPlacement(unsigned mask);
    static unsigned CurrentMask(const ConsoleModule& console);
    static void Apply(ConsoleModule& console, const ConsoleVariablePatch& patch);

    static double Seconds(Clock::time_point begin, Clock::time_point end);
    static double SecondsSince(Clock::time_point begin) { return Seconds(begin, Clock::now()); }
    double Remaining() const { return Budget - SecondsSince(_started); }
    bool TimedSearch() const { return _search && _stage != Stage::Exhaustive; }
    double LegWarmup() const { return _warmup + _window.AdditionalWarmup; }
    bool Measuring() const { return _elapsed >= LegWarmup() + _window.Begin - _preroll; }

    void Request(Action action, BistroPlacement::Profile profile = BistroPlacement::Performance);
    const char* StageName() const;
    static std::string PlacementName(unsigned mask);
    size_t Section(double time) const;
    size_t Phase(double time) const;
    void SetWindows();

    static Config EffectiveConfig(RenderModule& render);
    static ConsoleVariablePatch RenderConfig(ConsoleModule& console);
    static ConsoleVariablePatch SearchConfig(ConsoleModule& console);
    void WriteRequestedConfig(ConsoleModule& console);
    static ConsoleVariablePatch LocalAdapters(ConsoleModule& console);

    void StartPlacement(World& world, ConsoleModule& console, const RenderModule& render,
        BistroRenderPipeline& pipeline, const std::vector<int>& adapters, bool manual);
    void CheckPlacementReconstruction(const BistroRenderPipeline&, GDX12UpscalerProvider);
    void ObservePlacementImage(RenderModule& render, BistroRenderPipeline& pipeline);
    void BeginPlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline,
        BistroPlacement::Trial trial, bool first = false);
    void CapturePlacementWindows(World& world, BistroRenderPipeline& pipeline);
    Window PlacementWindow(size_t index) const;
    void BeginPlacementLeg(World& world, BistroRenderPipeline& pipeline);

    struct ScheduleEvidence
    {
        std::shared_ptr<const BistroRender::Schedule> Plan;
        std::array<bool, std::size(BistroPlacement::Techniques)> Activity{};
        BistroRenderPipeline::PacketRoles Roles;
        std::set<std::pair<BistroRender::Id, BistroRender::Id>> Links;
        unsigned Groups = 0;
    };

    ScheduleEvidence& ObserveSchedule(const BistroRenderPipeline& pipeline);
    void ObservePlacement(BistroRenderPipeline& pipeline);
    void RecordPlacementFrame(RenderModule& render, BistroRenderPipeline& pipeline, Clock::time_point now);

    void CompletePlacementLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void WritePlacementWaits(BistroRenderPipeline& pipeline);
    void ReportPlacementEvidence(BistroRenderPipeline& pipeline);
    void CompletePlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void WritePlacementTrialReport(BistroRenderPipeline& pipeline, std::string_view status);
    bool StartPlacementConfirmation(World&, ConsoleModule&, BistroRenderPipeline&, BistroPlacement::Trial);
    void ExtendPlacementBudget(double seconds);
    void SettleComparisonPass(bool interrupted = false);
    void WritePlacementBudget();
    double PlacementFullCost() const { return _warmup + Duration() + _placementSetupReserve; }
    double PlacementProbeCost() const { return _placementPhysics.size() * (_warmup + 1 + 2) + 3 + _placementSetupReserve; }

    bool CheckPlacementDeadline(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, Clock::time_point now);
    void BeginNextPlacementPhase(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void NextPlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline,
        Clock::time_point now = Clock::now());

    void Start(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline,
        ::Window& window, Action action);

    uint64_t _legNumber = 0;
    void BeginLeg(World& world, BistroRenderPipeline& pipeline, Window window);

    void CaptureWindows(World& world, BistroRenderPipeline& pipeline, const Windows& windows, PhysicsStates& snapshots);
    std::function<void()> CaptureScene(World& world, BistroRenderPipeline& pipeline);
    void CaptureCheckpoint(World& world, BistroRenderPipeline& pipeline);
    void CaptureActivity(unsigned groups);
    void WriteLeg(const char* status);
    static bool Comparable(Stats stats) { return stats.Count >= 3 && stats.Mean > 0 && stats.P95 > 0; }

    void ReportBaselineWindows(const Windows& windows);
    void SetProbeActivity(Probe& probe);
    void PrepareEliminationWindows();
    void Report(std::string_view name, double value) { _csv.Config(name, Csv::Number(value)); }

    double Score(const Rating& rating) const;
    void Rank(std::vector<unsigned>& masks) const;
    std::vector<unsigned> Leaders() const;
    void AddPairs();
    void AddNeighbors();

    double NominalCost(const Windows& windows, size_t first = 0) const;
    bool Fits(double seconds) const { return Remaining() > seconds * _probeScale + 1 + _finalReserve; }
    double ProbeCost(const Probe& probe) const;
    void NextProbe(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void StartFinal(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    Probe* BoundaryProbe(unsigned missing);
    void StartRoundLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    bool TryBoundaryCheck(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, unsigned missing);
    void CutRound(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void AdvanceRound(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline);
    void CompleteRoundLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, Stats stats);

    void CompleteLeg(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline);

    void RestoreSettings(ConsoleModule& console, std::optional<unsigned> selected);
    void Finish(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, const char* status,
        std::optional<unsigned> selected = {}, double gain = 0, Outcome outcome = Outcome::Completed);
    void Fail(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, std::string error);

    struct Route
    {
        Shot Red{BistroBenchmarkRoute::Red}, Blue{BistroBenchmarkRoute::Blue};
        Shot Green{BistroBenchmarkRoute::Green}, Yellow{BistroBenchmarkRoute::Yellow};
    };
    std::optional<Route> _route;
    std::array<Entity, BistroEntrance::Names.size()> _doors{InvalidEntity, InvalidEntity};
    Entity _fountain = InvalidEntity, _camera = InvalidEntity;
    Pose _previousPose{};
    BistroInteractionSource _previousInfluence{};
    ConsoleVariablePatch _previous, _searchConfig;
    Config _effective, _pendingAdapters;
    bool _frameConfigPending = false;
    std::function<void()> _initialScene;
    PhysicsStates _windowPhysics, _extraPhysics;
    BistroCharacters::Scene* _characters = nullptr;
    std::optional<Outcome> _outcome;
    BistroRun::Launch _launch;
    Action _action = Action::None;
    BistroPlacement::Profile _placementProfile = BistroPlacement::Performance;
    Stage _stage = Stage::Baseline;
    bool _active = false, _search = false, _legDone = false, _legOpen = false;
    bool _configured = false, _legConfigured = false, _hadInfluence = false, _repeatComparable = false;
    unsigned _mask = 0, _userMask = 0, _activeGroups = 0, _legGroups = 0, _runId = 0;
    size_t _windowIndex = 0;
    double _elapsed = 0, _routeTime = 0, _preroll = 0, _finalReserve = 0, _probeScale = 1;
    double _meanThreshold = .01, _p95Threshold = .02;
    Clock::time_point _started, _legStarted, _frameStart, _lastPresent, _probeStarted;
    std::filesystem::path _path;
    Csv _csv;
    Window _window{};
    Windows _windows{}, _extraWindows{};
    std::array<double, 6> _sections{};
    std::array<double, 4> _phaseEnds{};
    std::array<unsigned, 4> _activity{};
    Stats _baseline{};
    Stats _sweepBest{};
    unsigned _sweepBestMask = 0;
    std::optional<unsigned> _startupDefault;
    Measurements _reference{}, _measurements{};
    std::bitset<Cases> _seen;
    std::array<std::optional<double>, Cases> _scores;
    std::array<Rating, Cases> _ratings;
    std::array<double, Cases> _roundGains;
    std::array<Probe, 4> _primaryProbes, _extraProbes;
    std::array<Probe, Techniques.size()> _activityProbes;
    std::bitset<4> _usedExpansions;
    std::deque<Checkpoint> _checkpoints;
    Probe* _roundProbe = nullptr;
    bool _roundNeedsControl = false;
    std::vector<unsigned> _ranked, _roundMasks;
    unsigned _round = 0, _coveredGroups = 0;
    size_t _cut = 0, _roundIndex = 0;
    double _observedMeanNoise = 0;
    std::deque<unsigned> _pending;
    std::vector<BistroBenchmarkData::Frame> _frames;
    std::string _message, _savedPath;
    BistroBenchmarkData::RunMode _runMode = BistroBenchmarkData::RunMode::Manual;
    size_t _latestResultSlot = 0;
    std::array<CompletedPass, 6> _lastPasses{};
    double _warmup = 3;
    float _paintingHold = 5;
    bool _multiGPU = false;
    bool _inspectorWasVisible = false;
    bool _manualPlacement = false, _manualGpuTimings = false, _calibrating = false, _placementWaitsOpen = false;
    bool _placementInitialProbesReady = false;
    BistroRender::CompletionToken _placementPreparationFence;
    unsigned _placementPhase = 0;
    size_t _placementWindowIndex = 0, _trialWindowCount = 0;
    Clock::time_point _trialStarted, _placementLegStarted, _placementDeadline, _singleDeadline, _phaseDeadline;
    double _basePlacementBudget = 0, _comparisonEstimate = 0, _activeComparisonEstimate = 0;
    double _phaseBaseBudget = 0;
    size_t _plannedComparisonPasses = 0;
    std::array<double, 4> _addedPlacementBudget{};
    BistroRender::Id _trialPostDevice = 0;
    double _placementSetupReserve = 2;
    std::array<std::function<void()>, 8> _placementPhysics;
    std::function<void()> _replayScene;
    size_t _startupBodies = 0;
    uint64_t _startupLayout = 14695981039346656037ull;
    BistroPlacement::Search _placementSearch;
    BistroPlacement::Trial _trial;
    BistroPlacement::Result _trialResult;
    std::array<bool, std::size(BistroPlacement::Techniques)> _placementSeenOnRoute{};
    std::vector<ScheduleEvidence> _scheduleEvidence;
    BistroRenderPipeline::FramePlacement _framePlacement;
    struct TrialMeasurements
    {
        struct SimulationStats
        {
            uint64_t Generation = 0, LastVersion = 0, Frames = 0, UsedVersions = 0;
            double SimulatedSeconds = 0;
        };

        size_t Frames = 0, PrimaryFrames = 0;
        double Seconds = 0, PrimarySeconds = 0;
        double FullP95 = 0;
        std::vector<double> PrimaryIntervals;
        std::array<uint64_t, 4> TransferBytes{};
        std::array<uint64_t, 7> SectionShadowTexels{}, SectionShadowSamples{};
        std::set<std::string> Passes;
        std::set<std::tuple<std::string, BistroRender::Id, std::string>> Publications;
        std::map<std::string, std::set<BistroRender::Id>> ActiveOwners, UsefulOwners;
        BistroRenderPipeline::Owners PreviousOwners;
        std::pair<BistroRender::Id, BistroRender::Id> PostPublication{BistroRender::Invalid, BistroRender::Invalid};
        bool HasPublishedProbes = false;
        BistroPlacement::ProbeFreshness Freshness;
        std::map<std::tuple<std::string, BistroRender::Id, size_t>, SimulationStats> Simulations;
        std::array<bool, std::size(BistroPlacement::Techniques)> Activity{};
        BistroPlacement::PlacementCosts Costs;
    };
    TrialMeasurements _trialMeasurements, _legMeasurements;
    void AccumulatePlacementWindow(bool primary);
    void OpenPlacementMeasurements(const std::string& conditions);
    std::string PlacementMeasurementKey(const BistroRenderPipeline& pipeline) const;
    bool RestorePlacementWindow();
    bool CompatibleRemotePaths(const TrialMeasurements& saved) const;
    void SavePlacementWindow() const;
    struct WindowRecord { std::filesystem::path File; std::streampos Offset; };
    std::map<std::string, WindowRecord> _measurementIndex;
    std::map<std::wstring, std::string> _measurementDefaults;
    std::filesystem::path _measurementFile;
    std::string _measurementKey, _reusedPass;
    Config _trialConfig;
    std::string _trialFallback;
    std::optional<double> _preparationSeconds;
    ConsoleVariablePatch _trialRequested;
    GDX12UpscalerProvider _trialProvider{};
    GDX12FrameGenerationProvider _trialFrameGeneration{};
    std::string _trialPrefix, _trialReason, _trialLastPass;
    size_t _trialCompletedPasses = 0;
    bool _trialPending = false, _trialReconstructionFailed = false;
};
