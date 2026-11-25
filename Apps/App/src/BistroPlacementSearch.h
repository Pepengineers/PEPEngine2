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
    {L"App.UI.GameHUD.Enabled", L"App.UI.GameHUD.Quality", "HUD"},
    {nullptr, L"App.UI.WorldCanvas.Quality", "canvas"},
    {nullptr, L"Render.Tree.Quality", "Linden"},
    {L"App.UI.CloudPicture.Enabled", L"App.UI.CloudPicture.Quality", "cloud picture"},
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
    {L"App.UI.CloudPicture.Adapter", "CloudPictureAdapter", L"App.UI.CloudPicture.CrossSync", {1, 0, 2}},
    {L"Render.Sky.SkyView.Adapter", "SkyViewAdapter", L"Render.Sky.SkyView.CrossSync", {1, 0, 3}},
    {L"Render.Sky.AerialPerspective.Adapter", "AerialPerspectiveAdapter", L"Render.Sky.AerialPerspective.CrossSync", {1}},
    {L"Render.WBOIT.Adapter", "TransparencyAdapter"},
    {L"App.UI.WorldCanvas.Atrium.Adapter", "AtriumCanvasAdapter", L"App.UI.WorldCanvas.Atrium.CrossSync", {1, 0}},
    {L"App.UI.WorldCanvas.Terrace.Adapter", "TerraceCanvasAdapter", L"App.UI.WorldCanvas.Terrace.CrossSync", {1, 0}},
    {L"Render.Post.Adapter", "PostAdapter"},
    {L"App.UI.GameHUD.Adapter", "GameHUDAdapter"},
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
    size_t Resolution = 0;
    int Provider = 0, Preset = 0;
    std::array<Level, std::size(Techniques)> Levels{};
    std::vector<int> Owners;
    std::vector<int> Sync;
    Frequency ProbeFrequency = Frequency::Low;
    int ProbeCapture = 1, ProbeDelivery = 0;
    bool RemoteAOFilter = false;

    bool operator<(const Config& rhs) const
    {
        return std::tie(Resolution, Provider, Preset, Levels, Owners, Sync, ProbeFrequency, ProbeCapture, ProbeDelivery, RemoteAOFilter) <
            std::tie(rhs.Resolution, rhs.Provider, rhs.Preset, rhs.Levels, rhs.Owners, rhs.Sync, rhs.ProbeFrequency,
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
    std::array<bool, std::size(Techniques)> Activity{};
    PlacementCosts Costs;
    std::string SourceCsv;
};

struct Trial
{
    size_t Id = 0;
    Config Settings;
    std::string Source;
    bool Full = false;
};

class Search
{
public:
    explicit Search(Profile profile = Performance) : _profile(profile) {}

    void LinkBaseline(const std::filesystem::path& output, std::string conditions);
    void Start(ConsoleModule& console, Size screen, std::vector<int> adapters, bool remote,
        bool fixedWaterSubsteps, bool usesSkinningPreset, const std::set<std::wstring>& shadowFixtures, BistroBenchmarkData::Csv& csv);

    Size Resolution(const Config& config) const { return _sizes.at(config.Resolution); }
    const auto& Resolutions() const { return _sizes; }
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
    void EndPhase();

    std::optional<Trial> NextProbe(BistroBenchmarkData::Csv& csv);
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
    enum class QualityStage { High, Epic, Reduce, Reflections, Fallback, Remove, RaiseOutput };

    struct QualityProgress
    {
        QualityStage Stage = QualityStage::High;
        size_t Mode = 0;
        std::vector<Config> Modes;
        Config Anchor, Candidate;
        std::vector<Config> Alternatives;
    };

    enum class PerformanceStage { Saved, Lower, Remove, RemovalResults, Raise, Refine, Neighbor, High, Reduce,
        ReductionResults, NeighborHigh, Improve, Done };

    struct PerformanceProgress
    {
        PerformanceStage Stage = PerformanceStage::Lower;
        Config Anchor, Quality;
        size_t NextMode = 0, RefineResolution = 0;
        double RemainingDeficit = 0;
        bool ConfirmingHigh = false;
        bool DeepReduction = false;
        std::vector<Config> Modes, Alternatives;
        std::deque<std::pair<Config, std::string>> Full;
    };

    std::vector<Config> UpscalerModes(Config settings) const;
    const Result* SavedPerformance(bool upscaled) const;
    std::optional<Config> SavedCompetitor(const Result& saved) const;

    void BeginPerformanceLower();
    static double ShortFps(const Result& result) { return result.ThresholdFps; }

    const Result* QualityFull(const Config& settings) const;
    const Result* QualityShort(const Config& settings) const;
    const Result* QualityFinalist(const std::vector<Config>* alternatives = nullptr, const Result* reductionBase = nullptr) const;
    size_t SmallerQualityResolution(size_t current) const;
    void QueueQualityAlternative(Config settings, const std::string& source);
    std::optional<Trial> AdvanceQuality(BistroBenchmarkData::Csv& csv, bool canProbe);

    const Result* PerformanceMeasure(const Config& settings, bool reuseCalibration = false) const;
    size_t LargerPerformanceResolution(size_t current) const;
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
    Config Workload(Config config, size_t placement) const;

    void ObserveCosts(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv);
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
    bool _remote = false, _aoDepthNormals = false;
    unsigned _probeIndex = 0;
    size_t _nextId = 0;
    int _preferredProvider = 0, _preferredPreset = 0;
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
    std::vector<Size> _sizes;
    std::array<std::array<int, Epic + 1>, std::size(Techniques)> _values{};
    std::array<bool, std::size(Techniques)> _fixedQuality{};
    std::array<bool, std::size(Techniques)> _available{};
    std::array<std::array<bool, std::size(Techniques)>, 2> _qualityAvailable{};
    std::deque<Trial> _pending, _priority;
    std::set<Config> _scheduled;
    std::set<Config> _slowWork;
    std::map<Config, Result> _short;
    std::set<Config> _failedProbes;
    std::set<Config> _fullSettings, _retryFull;
    std::vector<Result> _confirmed;
    std::vector<Result> _references;
    std::filesystem::path _baselineFile;
    std::string _conditions;
    bool _incompatibleBaseline = false;
    std::array<bool, 2> _completedMode{};
    std::array<bool, 2> _cachedMode{};
    bool _triedClosest = false;
    std::array<std::optional<Result>, 2> _nearest, _detail, _nearMiss;
    std::array<unsigned, 2> _rejected{};
};
}
