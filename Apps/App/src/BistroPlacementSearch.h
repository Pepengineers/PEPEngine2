#pragma once

#include "BistroPlacementMetrics.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12ReflectionSchedule.h"
#include <filesystem>
#include <iosfwd>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <tuple>

namespace BistroBenchmarkData { class Csv; }

namespace BistroPlacement
{
using Size = std::pair<unsigned, unsigned>;

struct Technique
{
    const wchar_t* Enable;
    const wchar_t* Quality;
    const char* Pass;
    bool ScaledOff = false;
};

inline constexpr Technique Techniques[]{
    {L"Render.AO.Enabled", L"Render.AO.Quality", "AO"},
    {L"Render.Grass.Enabled", L"Render.Grass.Quality", "grass"},
    {L"Render.Wind.Enabled", L"Render.Wind.Quality", "wind"},
    {L"Render.Particles.Enabled", L"Render.Particles.Quality", "particle"},
    {L"Render.Water.Enabled", L"Render.Water.Quality", "water"},
    {L"Render.Sky.Clouds.Enabled", L"Render.Sky.Clouds.Quality", "volumetric clouds"},
    {L"Render.ReflectionProbes.Enabled", L"Render.ReflectionProbes.Quality", "reflection probes"},
    {L"Render.SSR.Enabled", L"Render.SSR.Quality", "SSR"},
    {L"Render.DOF.Enabled", L"Render.DOF.Quality", "DoF"},
    {L"Render.MotionBlur.Enabled", L"Render.MotionBlur.Quality", "motion blur"},
    {L"Render.Sky.Atmosphere.Enabled", L"Render.Sky.Atmosphere.Quality", "atmosphere"},
    {L"Render.Sky.SkyBox.Enabled", L"Render.Sky.SkyBox.Quality", "sky"},
    {L"Render.Skinning.Enabled", L"Render.Skinning.Quality", "skin deformation"},
    {L"Render.GameHUD.Enabled", L"Render.GameHUD.Quality", "HUD"},
    {nullptr, L"Render.WorldCanvas.Quality", "canvas"},
    {nullptr, L"Render.Tree.Quality", "Linden"},
    {L"Render.CloudPicture.Enabled", L"Render.CloudPicture.Quality", "cloud picture"},
    {nullptr, L"Render.Shadows.Sun.Resolution", "sun shadow", true},
    {nullptr, L"Render.Shadows.Local.Resolution", "local shadow", true},
};

struct Placement
{
    std::wstring Variable;
    std::string Owner;
    std::wstring Sync;
    std::vector<int> Modes;
};

inline const Placement Placements[]{
    {L"Render.AO.Adapter", "AOAdapter", L"Render.AO.CrossSync", {1, 0}},
    {L"Render.LightGrid.Adapter", "LightGridAdapter"},
    {L"Render.Particles.Adapter", "ParticleAdapter", L"Render.Particles.CrossSync", {1, 0, 2, 3}},
    {L"Render.Water.Adapter", "WaterAdapter", L"Render.Water.CrossSync", {1, 0, 2}},
    {L"Render.Wind.Adapter", "GrassWindAdapter", L"Render.Wind.CrossSync", {1, 0}},
    {L"Render.CloudPicture.Adapter", "CloudPictureAdapter", L"Render.CloudPicture.CrossSync", {1, 0, 2}},
    {L"Render.Sky.SkyView.Adapter", "SkyViewAdapter", L"Render.Sky.SkyView.CrossSync", {1, 0, 3}},
    {L"Render.Sky.AerialPerspective.Adapter", "AerialPerspectiveAdapter", L"Render.Sky.AerialPerspective.CrossSync", {1}},
    {L"Render.WBOIT.Adapter", "TransparencyAdapter"},
    {L"Render.WorldCanvas.Atrium.Adapter", "AtriumCanvasAdapter", L"Render.WorldCanvas.Atrium.CrossSync", {1, 0}},
    {L"Render.WorldCanvas.Terrace.Adapter", "TerraceCanvasAdapter", L"Render.WorldCanvas.Terrace.CrossSync", {1, 0}},
    {L"Render.Post.Adapter", "PostAdapter"},
    {L"Render.GameHUD.Adapter", "GameHUDAdapter"},
    {L"Render.Shadows.Sun.Adapter", "SunShadowAdapter", L"Render.Shadows.Sun.CrossSync", {1, 3}},
    {L"Render.Sky.Clouds.Adapter", "VolumetricCloudsAdapter"},
};
inline constexpr size_t AO = 0, Water = 3, Wind = 4, Post = 11, HUD = 12, SunShadow = 13, Clouds = 14;
inline constexpr size_t FirstProbe = std::size(Placements);
inline constexpr size_t WaterQuality = 4, ProbeQuality = 6, SSRQuality = 7, SkinningQuality = 12;
inline constexpr size_t SunShadowQuality = std::size(Techniques) - 2, LocalShadowQuality = std::size(Techniques) - 1;

enum Level { Off, Low, Medium, High, Epic };
enum Profile { Performance, Quality };
using Frequency = GDX12ReflectionSchedule::Frequency;
inline constexpr double Threshold[]{60, 28};
inline constexpr const char* ProfileName[]{"Performance", "Quality"};

struct Config
{
    int Provider = 0, Preset = 0;
    std::array<Level, std::size(Techniques)> Levels{};
    std::vector<int> Owners;
    std::vector<int> Sync;
    Frequency ProbeFrequency = Frequency::Low;
    int ProbeCapture = 1, ProbeDelivery = 0;
    bool RemoteAOFilter = false;

    bool operator<(const Config& rhs) const
    {
        return std::tie(Provider, Preset, Levels, Owners, Sync, ProbeFrequency, ProbeCapture, ProbeDelivery, RemoteAOFilter) <
            std::tie(rhs.Provider, rhs.Preset, rhs.Levels, rhs.Owners, rhs.Sync, rhs.ProbeFrequency,
                rhs.ProbeCapture, rhs.ProbeDelivery, rhs.RemoteAOFilter);
    }
    bool operator==(const Config& rhs) const
    {
        return !(*this < rhs) && !(rhs < *this);
    }
};

struct Result
{
    size_t Id = 0;
    Config Settings;
    Config ConfirmationSettings;
    Size Output{}, Render{};
    double Fps = 0, P95 = 0, ThresholdFps = 0;
    std::optional<double> ShadowTexels;
    std::array<double, 7> SectionShadowTexels{{-1, -1, -1, -1, -1, -1, -1}};
    bool Valid = false, Remote = false, EquivalentInactiveOwners = true;
    bool CommonPlacementsConsumed = false, CoveredExtraWindows = false;
    PlacementCosts CommonCosts;
    bool Reused = false;
    std::array<bool, std::size(Techniques)> Activity{};
    PlacementCosts Costs;
    std::map<size_t, ProducerTimings> ProducerWindows;
    int AOImplementation = -1, AOResolution = -1;
    std::string SourceCsv;
};

struct Trial
{
    size_t Id = 0;
    Config Settings;
    std::string Source;
    bool Full = false;
    bool RequireLive = false;
    bool InspectImage = false;
    size_t Comparison = 0, Pass = 0, PassCount = 0;
};

class Search
{
public:
    explicit Search(Profile profile = Performance, decltype(Techniques)& techniques = Techniques)
        : _profile(profile), _techniques(techniques) {}

    void LinkBaseline(const std::filesystem::path& output, std::string conditions);
    void Start(ConsoleModule& console, Size output, std::vector<int> adapters, bool remote,
        bool fixedWaterSubsteps, bool usesSkinningPreset, const std::set<std::wstring>& shadowFixtures, BistroBenchmarkData::Csv& csv,
        bool allowUpscalers, std::map<int, int> preferredProviders);

    Size Output() const { return _output; }
    const Result* Winner() const;
    void RecordImageMode(const Trial& trial, bool available);
    Trial CompareImageMode(Trial trial, BistroBenchmarkData::Csv& csv);
    std::optional<Trial> SavedComparison();
    std::optional<Trial> NextComparisonTrial();
    size_t PendingComparisonPasses() const { return _comparisonTrials.size(); }
    void InterruptComparison(BistroBenchmarkData::Csv& csv) const;
    const auto& Candidates() const { return _placements; }
    Config EffectiveConfig(Config config) const { return Canonical(std::move(config)); }
    int QualityValue(size_t technique, Level level) const { return _values[technique][level]; }

    ConsoleVariablePatch Patch(const Config& config) const;
    ConsoleVariablePatch ControlledSettings() const { return Patch(_high); }

    bool LowFirst() const { return _profile == Performance && !RemotePhase(); }
    bool HighFirst() const { return _profile == Quality && !RemotePhase(); }

    Trial Calibration();
    bool NeedsPhase(unsigned phase) const;
    bool HasReference(bool upscaled) const { return BestReference(upscaled) != nullptr; }
    bool BeginPhase(unsigned phase);

    std::optional<Trial> NextProbe(BistroBenchmarkData::Csv& csv);
    void RetryChangedEstimates();
    std::optional<Trial> PerformanceConfirmation(BistroBenchmarkData::Csv& csv);
    std::optional<Trial> NextQualityTrial(BistroBenchmarkData::Csv& csv, bool canProbe);
    const Result* Finalist() const;
    Trial Confirm(const Result& result);

    void Record(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv, std::string_view rejection);
    void Report(BistroBenchmarkData::Csv& csv, std::string_view reason,
        ConsoleModule& console, const std::filesystem::path& output) const;

    bool Upscaled() const { return (_phase & 1u) != 0; }
    bool RemotePhase() const { return _phase >= 2; }

private:
    enum class QualityStage { High, Epic, Reduce, Reflections, Fallback, Remove, RaiseDetail, Done };

    struct QualityProgress
    {
        QualityStage Stage = QualityStage::High;
        bool RecheckSaved = true;
        Config Anchor, Candidate;
        std::vector<Config> Alternatives;
    };

    enum class PerformanceStage { Lower, Remove, RemovalResults, Raise, Refine, High, Reduce,
        ReductionResults, NeighborHigh, Improve, Done };

    struct PerformanceProgress
    {
        PerformanceStage Stage = PerformanceStage::Lower;
        Config Anchor, Quality;
        size_t RefineStep = 0;
        double RemainingDeficit = 0;
        bool ConfirmingHigh = false;
        bool DeepReduction = false;
        std::vector<Config> Alternatives;
        std::deque<std::pair<Config, std::string>> Full;
    };

    using ImageMode = std::tuple<int, int, int>;
    using ImageSteps = std::vector<std::pair<int, std::vector<int>>>;
    std::optional<Trial> InspectNextImage();
    void QueueDiscovery(int owner);
    void SeedRemotePhase();
    const ImageSteps& Steps(int owner) const;
    bool SupportsImage(const Config& config) const;
    size_t StepCount(int owner = -1) const { return Upscaled() ? Steps(owner).size() : 1; }
    static Config ComparisonKey(Config config);
    void RecordComparison(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv);
    size_t Step(const Config& settings) const;
    std::vector<Config> AtStep(const Config& base, size_t step) const;
    const Result* SavedPreset(bool upscaled) const;
    std::optional<Config> SavedCompetitor(const Result& saved) const;

    static double ShortFps(const Result& result) { return result.ThresholdFps; }

    const Result* QualityFull(const Config& settings) const;
    const Result* QualityShort(const Config& settings) const;
    const Result* QualityFinalist(const std::vector<Config>* alternatives = nullptr, const Result* reductionBase = nullptr) const;
    void QueueQualityAlternative(Config settings, const std::string& source);
    std::optional<Trial> AdvanceQuality(BistroBenchmarkData::Csv& csv, bool canProbe);

    const Result* PerformanceMeasure(const Config& settings, bool reuseCalibration = false) const;
    Config PerformanceHigh(Config settings) const;
    void ReducePerformanceQuality(const Result* full);
    void WriteRemovalEvidence(const Result& base, BistroBenchmarkData::Csv& csv) const;
    void AdvancePerformance(BistroBenchmarkData::Csv& csv);
    void ImprovePerformance(const Config& settings);

    Config EffectiveSettings(Config config) const;
    Config Canonical(Config config) const;
    Config FullComposition(Config config) const;
    static Config WithMediumShadows(Config config);
    bool KeepsComposition(const Config& config) const;
    void ObserveComposition(const Result& result);

    static bool ReadReference(std::istream& file, Result& r);
    void SaveReferences() const;
    static bool SameImage(const Result& a, const Result& b);

    const Result* BestReference(bool upscaled) const;
    const Result* ClosestReference(bool upscaled) const;
    const Result* DetailedReference(bool upscaled) const;
    Trial Make(const Config& config, std::string source, bool full);

    void Schedule(Config config, const std::string& source, bool priority = false);
    void ScheduleReflectionVariants(const Config& config, const std::string& source, bool priority = false);
    void Seed(const Config& config, bool postOnly = false, bool priority = false);
    void SeedAdapters(const Result& reference);
    enum class ProposalKind { Refinement, Mixed, CompareLight, CompareHeavy };
    void ScheduleMeasured(Config config, std::string source, double cost, ProposalKind kind);
    void CombinePlacements(const Result& result, double cost);
    double PlacementRank(const Result& result) const;

    void ObserveCosts(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv);
    bool SkipEstimated(const Trial& trial, BistroBenchmarkData::Csv& csv);
    void ExpandPolicies(const Trial& trial, const Result& result);
    void Expand(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv);

    bool HasQualityScale(size_t i) const;
    bool PerformanceScale(size_t i) const;
    bool HighMajority(const Config& config, const std::array<bool, std::size(Techniques)>& active) const;
    bool QualityComplete(const Result& result) const;
    auto QualityImageRank(const Result& result) const;
    bool QualityOutranks(const Result& full, const Result& sample) const;

    bool Better(const Result& a, const Result& b, bool preferDetail = false, bool shortWindow = false) const;
    static bool Dominates(const Result& a, const Result& b);
    const Result* Best(bool upscaled, bool remote) const;

    void ReportResult(BistroBenchmarkData::Csv& csv, const std::string& prefix, const Result* r) const;

    unsigned _phase = 0;
    Profile _profile;
    const Technique* _techniques;
    bool _remote = false, _aoDepthNormals = false;
    unsigned _probeIndex = 0;
    size_t _nextId = 0;
    bool _allowUpscalers = false;
    Size _output{};
    std::deque<ImageMode> _inspect;
    std::map<int, int> _preferredProviders;
    std::map<int, ImageSteps> _images;
    std::deque<Trial> _comparisonTrials;
    std::set<Config> _compared;
    std::optional<Result> _savedCalibration, _comparisonWinner;
    size_t _nextComparison = 0;
    bool _comparisonOpen = false;
    bool _seedRemoteAfterDiscovery = false, _comparedSavedCalibration = false;
    std::string _windAlgorithm;
    std::mt19937 _random{20261122};
    Config _high, _minimum;
    PerformanceProgress _performance;
    QualityProgress _quality;
    size_t _calibrationId = 0;
    std::optional<Result> _calibration;
    std::vector<int> _adapters;
    std::vector<Placement> _placements;
    size_t _firstLocalShadow = 0;
    std::array<std::array<int, Epic + 1>, std::size(Techniques)> _values{};
    std::array<bool, std::size(Techniques)> _fixedQuality{};
    std::array<bool, std::size(Techniques)> _available{};
    std::array<std::array<bool, std::size(Techniques)>, 2> _qualityAvailable{};
    std::deque<Trial> _pending, _priority;
    std::deque<std::pair<int, Config>> _initialPlacements;
    struct PeerCoverage
    {
        size_t Attempt = 0, Completed = 0, Consumed = 0;
    };
    std::array<std::map<int, PeerCoverage>, 2> _peerCoverage;
    struct MeasuredProposal
    {
        double Cost;
        ProposalKind Kind;
        std::string Source;
    };
    std::map<Config, MeasuredProposal> _proposals;
    std::set<Config> _dispatched;
    std::set<Config> _scheduled;
    std::set<Config> _estimated;
    std::map<Config, Result> _short;
    std::set<Config> _failedProbes;
    std::set<Config> _fullSettings, _retryFull;
    std::vector<Result> _confirmed, _comparedResults;
    std::vector<Result> _references;
    std::filesystem::path _baselineFile;
    std::string _conditions;
    std::optional<Result> _savedSelection;
    bool _triedClosest = false;
    std::array<std::optional<Result>, 2> _nearest, _detail, _nearMiss;
    std::array<unsigned, 2> _rejected{};
};
}
