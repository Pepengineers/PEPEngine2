#pragma once
#include "Engine.RendererDX12/GDX12LocalLights.h"

#include "BistroBenchmarkCsv.h"
#include "BistroPlacementMetrics.h"
#include "BistroProbeSettings.h"
#include "Techniques/SceneAO.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include <array>
#include <deque>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <tuple>

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
    {L"Render.Sky.Clouds.Enabled", L"Render.Sky.Clouds.Quality", "cloud"},
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
    {L"App.UI.CloudPicture.Enabled", nullptr, "cloud picture"},
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
};
inline constexpr size_t AO = 0, Water = 3, Wind = 4, Post = 11, HUD = 12, SunShadow = 13;
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

struct Preset
{
    std::filesystem::path Path;
    Size Output{};
};

class Search
{
public:
    explicit Search(Profile profile = Performance) : _profile(profile) {}

    void LinkBaseline(const std::filesystem::path& output, std::string conditions)
    {
        _conditions = std::move(conditions);
        _baselineFile = output;
        _baselineFile.replace_extension(".baseline");
        for (const auto* folder : {"SingleGPU", "MultiGPU"})
        {
            const auto directory = output.parent_path().parent_path() / folder;
            if (!std::filesystem::is_directory(directory)) continue;
            for (const auto& item : std::filesystem::directory_iterator(directory))
            {
                if (item.path().extension() != ".baseline" || !item.is_regular_file()) continue;
                std::ifstream input(item.path());
                std::string key;
                std::array<bool, 2> completed{};
                if (!(input >> std::quoted(key) >> completed[0] >> completed[1]) || key != _conditions)
                {
                    _incompatibleBaseline = true;
                    continue;
                }
                Result result;
                while (ReadReference(input, result))
                {
                    const auto size = std::find(_sizes.begin(), _sizes.end(), result.Output);
                    if (size == _sizes.end()) continue;
                    result.Settings.Resolution = size - _sizes.begin();
                    result.Settings.Owners = _high.Owners;
                    result.Settings.Sync = _high.Sync;
                    result.Settings = Canonical(std::move(result.Settings));
                    const auto source = result.SourceCsv.empty() ? std::filesystem::path(item.path()).replace_extension(".csv") :
                        (directory / std::filesystem::u8path(result.SourceCsv)).lexically_normal();
                    result.SourceCsv = source.lexically_relative(output.parent_path()).generic_u8string();
                    if (_remote && completed[bool(result.Settings.Provider)]) _cachedMode[bool(result.Settings.Provider)] = true;
                    if (!LowFirst()) _fullSettings.insert(result.Settings);
                    ObserveComposition(result);
                    _references.push_back(result);
                }
            }
        }
        std::sort(_references.begin(), _references.end(), [](const auto& a, const auto& b) {
            return std::tie(a.SourceCsv, a.Id) < std::tie(b.SourceCsv, b.Id);
        });
    }

    void Start(ConsoleModule& console, Size screen, std::vector<int> adapters, bool remote,
        bool fixedWaterSubsteps, bool usesSkinningPreset, const std::set<std::wstring>& shadowFixtures, BistroBenchmarkData::Csv& csv)
    {
        _adapters = std::move(adapters);
        _remote = remote;
        _aoDepthNormals = console.FindConsoleVariable(L"Render.DepthNormals.PrepassEnabled").GetBool();
        _windAlgorithm = console.FindConsoleVariable(L"Render.Wind.Algorithm").GetString();
        _preferredProvider = console.FindConsoleVariable(L"Render.Upscaler.Provider").GetInt();
        _preferredPreset = console.FindConsoleVariable(L"Render.Upscaler.Quality").GetInt();
        _placements.assign(std::begin(Placements), std::end(Placements));
        for (size_t i = 0; i < BistroReflections::AdapterNames.size(); ++i)
            _placements.push_back({BistroReflections::AdapterNames[i], "Probe " + std::to_string(i)});
        _firstLocalShadow = _placements.size();
        if (!shadowFixtures.empty())
        {
            const UINT pages = console.FindConsoleVariable(L"Render.Shadows.Local.AtlasCount").GetUInt();
            for (UINT page = 0; page < pages; ++page)
            {
                RegisterLocalShadowPageSettings(page);
                _placements.push_back({LocalShadowPageAdapterName(page), "Local shadow page " + std::to_string(page),
                    LocalShadowPageCrossSyncName(page), {int(ShadowCrossSync::Exact), int(ShadowCrossSync::Latest)}});
            }
        }
        for (unsigned height : {480u, 720u, 900u, 1080u, 1440u, 2160u, screen.second})
            if (height <= screen.second)
                _sizes.emplace_back(unsigned((uint64_t(height) * screen.first + screen.second / 2) / screen.second), height);
        std::sort(_sizes.begin(), _sizes.end());
        _sizes.erase(std::unique(_sizes.begin(), _sizes.end()), _sizes.end());
        _high.Owners.assign(_placements.size(), -1);
        _high.Sync.assign(_placements.size(), -1);
        _high.Levels.fill(High);
        _minimum = _high;
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            const auto& technique = Techniques[i];
            if (!technique.Quality) continue;
            const auto& variable = console.FindConsoleVariable(technique.Quality);
            auto& levels = _values[i];
            levels.fill(-1);
            int firstActive = -1;
            for (int v = variable.GetEnumFirstValue(); v < variable.GetEnumFirstValue() + variable.GetEnumCount(); ++v)
            {
                const auto name = variable.GetEnumValueName(v);
                if (firstActive == -1 && name != "Off" && variable.IsValidString(std::to_string(v))) firstActive = v;
                if (name == "Off") levels[Off] = v;
                if (name == "Low") levels[Low] = v;
                if (name == "Mid" || name == "Medium") levels[Medium] = v;
                if (name == "High") levels[High] = v;
                if (name == "Epic" || name == "Ultra") levels[Epic] = v;
            }
            if (levels[Low] == -1 && firstActive >= 0 &&
                std::find(levels.begin() + Low, levels.end(), firstActive) == levels.end())
                levels[Low] = firstActive;
            for (const auto level : {Low, Medium, High, Epic})
                if (levels[level] >= 0)
                {
                    _minimum.Levels[i] = level;
                    break;
                }
            if (levels[High] == -1) levels[High] = variable.GetInt();
            if (levels[Medium] == -1) levels[Medium] = levels[High];
            if (levels[Epic] == -1) levels[Epic] = levels[High];
            if (levels[Off] == -1) levels[Off] = levels[High];
        }
        // The renderer exposes shadow quality as a discrete resolution.
        _values[SunShadowQuality] = {0, 512, 1024, 2048, 4096};
        using ShadowLevel = GDX12LocalShadowLevel;
        _values[LocalShadowQuality] = {int(GetLocalShadowProfile(ShadowLevel::Off).Face),
            int(GetLocalShadowProfile(ShadowLevel::Low).Face), int(GetLocalShadowProfile(ShadowLevel::Medium).Face),
            int(GetLocalShadowProfile(ShadowLevel::High).Face), int(GetLocalShadowProfile(ShadowLevel::Epic).Face)};
        _values[SkinningQuality][Epic] = _values[SkinningQuality][High];
        if (!usesSkinningPreset)
            _values[SkinningQuality][Medium] = _values[SkinningQuality][High];
        _values[WaterQuality][Epic] = _values[WaterQuality][High];
        if (fixedWaterSubsteps)
            _values[WaterQuality][Medium] = _values[WaterQuality][High];
        _fixedQuality[SkinningQuality] = !usesSkinningPreset;
        _fixedQuality[WaterQuality] = fixedWaterSubsteps;
        _minimum.Levels[SunShadowQuality] = _minimum.Levels[LocalShadowQuality] = Low;
        _minimum = Canonical(std::move(_minimum));
        if (_adapters.empty() && _remote)
            csv.Config("MultiGPU.Reason", "no secondary GPU available");
    }

    Size Resolution(const Config& config) const { return _sizes.at(config.Resolution); }
    const auto& Resolutions() const { return _sizes; }
    const auto& Candidates() const { return _placements; }
    Config EffectiveConfig(Config config) const { return Canonical(std::move(config)); }
    int QualityValue(size_t technique, Level level) const { return _values[technique][level]; }

    ConsoleVariablePatch Patch(const Config& config) const
    {
        ConsoleVariablePatch patch{
            {L"Render.Upscaler.Provider", std::to_string(config.Provider)},
            {L"Render.Upscaler.Quality", std::to_string(config.Preset)},
            {L"Render.AO.BlurOnSecondary", config.RemoteAOFilter ? "1" : "0"},
            {L"Render.Wind.Algorithm", config.Owners[Wind] >= 0 ? "1" : _windAlgorithm},
            {L"Render.ReflectionProbes.UseQualityPreset", "true"},
            {L"Render.ReflectionProbes.Count", std::to_string(config.Levels[ProbeQuality])},
            {L"Render.ReflectionProbes.Resolution", std::to_string(BistroReflections::Side(config.Levels[ProbeQuality] ?
                config.Levels[ProbeQuality] : Medium))},
            {L"Render.ReflectionProbes.PrefilterSamples", std::to_string(BistroReflections::Samples(config.Levels[ProbeQuality] ?
                config.Levels[ProbeQuality] : Medium))},
            {L"Render.ReflectionProbes.UpdateFrequency", std::to_string(int(config.ProbeFrequency))},
            {L"Render.ReflectionProbes.Implementation", std::to_string(config.ProbeCapture)},
            {L"Render.ReflectionProbes.First.Implementation", "-1"},
            {L"Render.ReflectionProbes.Schedule.Enabled", "false"},
            {L"Render.ReflectionProbes.TransferMode", std::to_string(config.ProbeDelivery)},
            {L"Render.ReflectionProbes.ShowSpheres", "false"},
        };
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            if (Techniques[i].Enable)
                patch.emplace_back(Techniques[i].Enable, config.Levels[i] == Off ? "false" : "true");
            if (Techniques[i].Quality && QualityValue(i, config.Levels[i]) >= 0)
                patch.emplace_back(Techniques[i].Quality, std::to_string(QualityValue(i, config.Levels[i])));
        }
        for (size_t i = 0; i < _placements.size(); ++i)
        {
            patch.emplace_back(_placements[i].Variable, std::to_string(config.Owners[i]));
            if (!_placements[i].Modes.empty())
                patch.emplace_back(_placements[i].Sync, std::to_string(config.Sync[i] < 0 ?
                    _placements[i].Modes.front() : config.Sync[i]));
        }
        return patch;
    }

    ConsoleVariablePatch ControlledSettings() const { return Patch(_high); }

    bool LowFirst() const { return _profile == Performance && !_remote; }

    Trial Calibration()
    {
        if (LowFirst())
        {
            const auto* saved = SavedPerformance(false);
            auto trial = Make(saved ? saved->Settings : _minimum, saved ?
                "recheck saved Native preset and capture route checkpoints" : "Native lower anchor and route checkpoints", true);
            _calibrationId = trial.Id;
            return trial;
        }
        auto settings = _high;
        if (const auto* reference = BestReference(false)) settings.Resolution = reference->Settings.Resolution;
        return Make(settings, "native route checkpoints", true);
    }

    bool NeedsPhase(unsigned phase) const
    {
        if (phase < 2) return !_cachedMode[phase];
        return phase < 4 && _remote && !_adapters.empty();
    }

    bool HasReference(bool upscaled) const { return BestReference(upscaled) != nullptr; }

    bool BeginPhase(unsigned phase)
    {
        _phase = phase;
        _confirmedBeforePhase = _confirmed.size();
        _triedClosest = false;
        _pending.clear();
        _priority.clear();
        _scheduled.clear();
        _short.clear();
        _failedProbes.clear();
        if (!NeedsPhase(phase)) return false;
        if (LowFirst())
        {
            _performance = {};
            if (const auto* saved = SavedPerformance(Upscaled()))
            {
                _performance.Stage = PerformanceStage::Saved;
                _performance.Full.emplace_back(saved->Settings, "recheck saved preset");
                if (auto other = SavedCompetitor(*saved))
                    _performance.Full.emplace_back(std::move(*other), "recheck nearest saved competitor");
            }
            else BeginPerformanceLower();
            return true;
        }
        if (RemotePhase())
        {
            if (const auto* reference = BestReference(Upscaled()))
            {
                Seed(reference->Settings, false, true);
                auto next = FullComposition(reference->Settings);
                if (next.Levels != reference->Settings.Levels) Seed(next, false, true);
                if (next.Resolution + 1 < _sizes.size())
                {
                    ++next.Resolution;
                    Seed(next, true);
                }
                const auto closest = ClosestReference(Upscaled());
                if (closest != reference && closest) Seed(FullComposition(closest->Settings), true);
                const auto detail = DetailedReference(Upscaled());
                if (detail && detail != reference && detail != closest) Seed(FullComposition(detail->Settings), true);
            }
            else return false;
        }
        Config seed = _high;
        seed.Resolution = _sizes.size() - 1;
        if (!Upscaled())
        {
            Seed(seed);
            if (!RemotePhase()) ScheduleReflectionVariants(seed, "native reflection settings");
            return true;
        }
        const auto modes = UpscalerModes(seed);
        if (RemotePhase())
            for (const auto& mode : modes) Seed(mode, true);
        for (const auto& mode : modes) Seed(mode);
        if (!RemotePhase() && !modes.empty()) ScheduleReflectionVariants(modes.front(), "upscaled reflection settings", true);
        return !_pending.empty();
    }

    void EndPhase()
    {
        if (RemotePhase() || !HasReference(Upscaled())) return;
        if (LowFirst())
        {
            if (BestReference(Upscaled())->Fps < Threshold[Performance]) return;
        }
        else if (_confirmed.size() == _confirmedBeforePhase && (_short.empty() || Finalist())) return;
        _completedMode[Upscaled()] = true;
        SaveReferences();
    }

    std::optional<Trial> NextProbe(BistroBenchmarkData::Csv& csv)
    {
        while (!_priority.empty() || !_pending.empty())
        {
            auto& queue = !_priority.empty() && (_pending.empty() || (++_probeIndex % 3 != 0)) ? _priority : _pending;
            auto trial = std::move(queue.front());
            queue.pop_front();
            if (_short.count(trial.Settings)) continue;
            bool tooSlow = false;
            for (size_t i = 0; i < _placements.size(); ++i)
            {
                if (trial.Settings.Sync[i] != 1 || !_slowWork.count(Workload(trial.Settings, i))) continue;
                csv.Config("Trial." + std::to_string(trial.Id) + ".Pruned", _placements[i].Owner +
                    ": measured producer work and publication cadence exceeded one target frame");
                tooSlow = true;
                break;
            }
            if (!tooSlow) return trial;
        }
        return {};
    }

    std::optional<Trial> PerformanceConfirmation(BistroBenchmarkData::Csv& csv)
    {
        if (!LowFirst()) return {};
        for (;;)
        {
            if (_performance.Full.empty() && _pending.empty() && _priority.empty()) AdvancePerformance(csv);
            if (_performance.Full.empty()) return {};
            auto [settings, source] = std::move(_performance.Full.front());
            _performance.Full.pop_front();
            if (!_fullSettings.count(settings)) return Make(settings, std::move(source), true);
        }
    }

    const Result* Finalist() const
    {
        const auto* incumbent = RemotePhase() ? Best(Upscaled(), true) : BestReference(Upscaled());
        const Result* best = nullptr;
        const Result* detail = nullptr;
        for (const auto& entry : _short)
        {
            const auto& result = entry.second;
            if (_fullSettings.count(result.ConfirmationSettings)) continue;
            if (ShortFps(result) < Threshold[_profile] && (!detail || Better(result, *detail, true, true))) detail = &result;
            const bool dominated = std::any_of(_short.begin(), _short.end(), [&](const auto& other)
                { return !_fullSettings.count(other.second.ConfirmationSettings) && !_retryFull.count(other.second.ConfirmationSettings) &&
                    (!LowFirst() || ShortFps(other.second) >= Threshold[Performance]) &&
                    Dominates(other.second, result); });
            if (dominated && ShortFps(result) >= Threshold[_profile]) continue;
            if (!best || Better(result, *best, false, true)) best = &result;
        }
        if ((!incumbent || incumbent->Fps < Threshold[_profile]) && _triedClosest &&
            (!best || ShortFps(*best) < Threshold[_profile]))
        {
            if (detail) return detail;
        }
        return best && (!incumbent || Better(*best, *incumbent) || ShortFps(*best) >= Threshold[_profile] * .96)
            ? best : nullptr;
    }

    Trial Confirm(const Result& result)
    {
        if (ShortFps(result) < Threshold[_profile]) _triedClosest = true;
        const auto source = _retryFull.count(result.ConfirmationSettings) ? "retry finalist of trial " : "finalist of trial ";
        return Make(result.ConfirmationSettings, source + std::to_string(result.Id), true);
    }

    void ScheduleWithinVideoMemory(const Trial& trial, ConsoleModule& console, Size output)
    {
        if (LowFirst()) return;
        auto next = trial.Settings;
        while (next.Resolution && (_sizes[next.Resolution].first > output.first || _sizes[next.Resolution].second > output.second))
            --next.Resolution;
        for (size_t i = 0; i < next.Levels.size(); ++i)
        {
            const auto& t = Techniques[i];
            if (t.Enable && !console.FindConsoleVariable(t.Enable).GetBool()) next.Levels[i] = Off;
            if (!t.Quality) continue;
            const int admitted = console.FindConsoleVariable(t.Quality).GetInt();
            auto& level = next.Levels[i];
            while (level > Off && (QualityValue(i, level) < 0 || QualityValue(i, level) > admitted))
                level = Level(int(level) - 1);
        }
        for (size_t i = 0; i < next.Owners.size(); ++i)
            next.Owners[i] = console.FindConsoleVariable(_placements[i].Variable).GetInt();
        next.Levels[ProbeQuality] = Level((std::min)(int(next.Levels[ProbeQuality]),
            console.FindConsoleVariable(L"Render.ReflectionProbes.Count").GetInt()));
        if (next.Provider == console.FindConsoleVariable(L"Render.Upscaler.Provider").GetInt())
            Schedule(next, "within VRAM budget", true);
        if (next.Resolution)
        {
            --next.Resolution;
            Schedule(next, "lower output after VRAM rejection");
        }
    }

    void Record(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv, std::string_view rejection)
    {
        const bool outputUnavailable = rejection == "requested output size unavailable";
        if (trial.Full)
        {
            if (LowFirst() && _performance.ConfirmingHigh && trial.Settings == _performance.Quality)
            {
                _performance.ConfirmingHigh = false;
                if (!result.Valid || result.Fps < Threshold[Performance])
                    ReducePerformanceQuality(result.Valid ? &result : nullptr);
            }
            if (LowFirst() && trial.Id == _calibrationId)
            {
                if (result.Valid) _calibration = result;
                _fullSettings.insert(trial.Settings);
                if (result.Valid && trial.Settings.Owners == result.Settings.Owners)
                {
                    ObserveComposition(result);
                    _confirmed.push_back(result);
                    SaveReferences();
                }
                return;
            }
            if (result.Valid)
            {
                ObserveComposition(result);
                if (!_fullSettings.insert(trial.Settings).second) return;
                if (trial.Settings.Owners != result.Settings.Owners)
                {
                    // The effective configuration still needs its own full-route measurement.
                    if (result.Remote && !_fullSettings.count(result.Settings))
                    {
                        auto effective = result;
                        effective.ConfirmationSettings = effective.Settings;
                        _short.insert_or_assign(result.Settings, std::move(effective));
                        if (result.EquivalentInactiveOwners) _scheduled.insert(result.Settings);
                    }
                    return;
                }
                _confirmed.push_back(result);
                if (!result.Remote) SaveReferences();
            }
            else if (outputUnavailable || rejection == "requested image mode unavailable" ||
                rejection == "VRAM budget exceeded" || rejection == "NIS reconstruction failed" ||
                rejection == "frame generation is active" || !_retryFull.insert(trial.Settings).second)
                _fullSettings.insert(trial.Settings);
            return;
        }
        ObserveCosts(trial, result, csv);
        if (!result.Valid)
        {
            _failedProbes.insert(trial.Settings);
            ++_rejected[Upscaled()];
            if (LowFirst()) return;
            if (rejection == "reflection face deadline missed")
            {
                ExpandPolicies(trial, result);
                ScheduleReflectionVariants(trial.Settings, "different reflection workload after missed deadline", true);
            }
            if (outputUnavailable && trial.Settings.Resolution)
            {
                auto smaller = trial.Settings;
                --smaller.Resolution;
                Schedule(smaller, "lower output after window size rejection", true);
            }
            if (trial.Settings.Resolution == 0)
            {
                const auto high = WithHighInsteadOfMedium(trial.Settings);
                if (const auto measured = _short.find(high); measured != _short.end() && measured->second.ThresholdFps < Threshold[_profile])
                    Expand({measured->second.Id, high, {}, false}, measured->second, csv);
            }
            return;
        }
        if (!_short.emplace(trial.Settings, result).second) return;
        auto& probe = _nearest[Upscaled()];
        if (!probe || Better(result, *probe, false, true)) probe = result;
        auto& detail = _detail[Upscaled()];
        if (result.Fps < Threshold[_profile] && (!detail || Better(result, *detail, true))) detail = result;
        auto& nearMiss = _nearMiss[Upscaled()];
        if (result.Fps < Threshold[_profile] && (!nearMiss || result.Fps > nearMiss->Fps)) nearMiss = result;
        if (!LowFirst()) Expand(trial, result, csv);
        else if (_performance.Stage == PerformanceStage::RemovalResults && ShortFps(result) >= Threshold[Performance])
        {
            const auto& base = *PerformanceMeasure(_performance.Anchor, true);
            if (std::count(result.Activity.begin(), result.Activity.end(), true) + 1 >=
                std::count(base.Activity.begin(), base.Activity.end(), true)) _pending.clear();
        }
    }

    std::array<Preset, 2> Report(BistroBenchmarkData::Csv& csv, std::string_view reason,
        ConsoleModule& console, const std::filesystem::path& output) const
    {
        std::array<Preset, 2> presets;
        for (bool upscaled : {false, true})
        {
            const auto prefix = std::string(upscaled ? "Upscaled." : "Native.") + ProfileName[_profile] + ".";
            const auto* remote = _remote ? Best(upscaled, true) : nullptr;
            const auto* reference = BestReference(upscaled);
            const auto* best = remote && (!reference || Better(*remote, *reference)) ? remote : reference;
            ReportResult(csv, prefix, best);
            if (best && best->Fps >= Threshold[_profile])
            {
                auto path = output.parent_path() / (output.stem().wstring() + (upscaled ? L"-upscaled.ini" : L"-native.ini"));
                auto patch = Patch(best->Settings);
                for (const auto* name : {L"Render.ReflectionProbes.Seed", L"Render.ReflectionProbes.Reset",
                    L"Render.Shadows.Local.AtlasCount"})
                    patch.emplace_back(name, console.FindConsoleVariable(name).GetString());
                patch.emplace_back(L"Render.FrameGeneration.Provider", "0");
                const auto saved = console.SavePatchIni(path, patch);
                if (saved.Error.empty())
                {
                    csv.Config(prefix + "PresetFile", path.filename().u8string());
                    presets[upscaled] = {std::move(path), best->Output};
                }
                else csv.Config(prefix + "PresetError", saved.Error);
            }
            if (_remote)
            {
                ReportResult(csv, prefix + "SingleGPU.", reference);
                ReportResult(csv, prefix + "MultiGPU.", remote);
                if (reference && !remote && reference->Fps >= Threshold[_profile])
                    csv.Config(prefix + "Winner", "Single GPU");
                if (!reference || !remote)
                    csv.Config(prefix + "Comparison", !reference ?
                        (_incompatibleBaseline ? "no baseline matching build, profile, route and scene replay state" :
                            "no saved Single GPU route for this profile and image mode") :
                        "no verified route with a consumed remote publication");
                else
                {
                    csv.Config(prefix + "Winner", best->Fps < Threshold[_profile] ? "neither met the target" :
                        best == reference ? "Single GPU" : "Multi-GPU");
                    if (SameImage(*remote, *reference))
                        csv.Config(prefix + "SameImageFpsGainPct",
                            BistroBenchmarkData::Csv::Number(100 * (remote->Fps / reference->Fps - 1)));
                    else
                    {
                        csv.Config(prefix + "Comparison", "image comparability not verified; compare output, render, techniques and local-shadow texels");
                        csv.Config(prefix + "OutputPixelChangePct", BistroBenchmarkData::Csv::Number(100 *
                            (double(remote->Output.first) * remote->Output.second /
                                (double(reference->Output.first) * reference->Output.second) - 1)));
                    }
                }
            }
            if (const auto& probe = _nearest[upscaled]; probe)
            {
                csv.Config(prefix + "ProbeTrial", std::to_string(probe->Id));
                csv.Config(prefix + "ProbeFPS", BistroBenchmarkData::Csv::Number(probe->Fps));
            }
            if (const auto& detail = _detail[upscaled]; detail)
                csv.Config(prefix + "DetailedBelowTargetTrial", std::to_string(detail->Id));
            if (const auto& closest = _nearMiss[upscaled]; closest)
                csv.Config(prefix + "ClosestBelowTargetTrial", std::to_string(closest->Id));
            csv.Config(prefix + "RejectedProbes", std::to_string(_rejected[upscaled]));
            csv.Config(prefix + "StopReason", reason);
        }
        return presets;
    }

    bool Upscaled() const { return (_phase & 1u) != 0; }
    bool RemotePhase() const { return _phase >= 2; }

private:
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

    std::vector<Config> UpscalerModes(Config settings) const
    {
        using U = GDX12UpscalerProvider;
        using P = GDX12UpscalerQuality;
        std::vector<Config> modes;
        const auto add = [&](int provider, int preset)
        {
            if (!provider || preset == int(P::NativeAA) ||
                (provider == int(U::NIS) && preset > int(P::Performance))) return;
            settings.Provider = provider;
            settings.Preset = preset;
            if (std::find(modes.begin(), modes.end(), settings) == modes.end()) modes.push_back(settings);
        };
        add(_preferredProvider, _preferredPreset);
        if (LowFirst())
        {
            add(int(U::TAA), int(P::Balanced));
            add(int(U::TAA), int(P::UltraPerformance));
            for (int provider = int(U::TAA); provider > 0; --provider)
                add(provider, int(P::Performance));
            for (int provider = int(U::TAA) - 1; provider > 0; --provider)
                add(provider, int(P::Balanced));
            return modes;
        }
        for (int provider = int(U::TAA); provider > 0; --provider)
            add(provider, int(P::Balanced));
        for (const int preset : {int(P::Quality), int(P::Performance), int(P::UltraQuality), int(P::UltraPerformance),
                                int(P::UltraQualityPlus), int(P::Default)})
            for (int provider = int(U::TAA); provider > 0; --provider)
                add(provider, preset);
        return modes;
    }

    const Result* SavedPerformance(bool upscaled) const
    {
        const Result* best = nullptr;
        for (const auto& result : _references)
            if (bool(result.Settings.Provider) == upscaled && result.Fps >= Threshold[Performance] &&
                (!best || Better(result, *best))) best = &result;
        return best;
    }

    std::optional<Config> SavedCompetitor(const Result& saved) const
    {
        const auto& base = saved.Settings;
        const Result* nearest = nullptr;
        const auto distance = [&](const Result& result)
        {
            const auto& other = result.Settings;
            const auto size = (std::max)(base.Resolution, other.Resolution) - (std::min)(base.Resolution, other.Resolution);
            return std::pair{other.Provider != base.Provider || other.Preset != base.Preset, size};
        };
        for (const auto& result : _references)
            if (bool(result.Settings.Provider) == bool(base.Provider) && !(result.Settings == base) &&
                (!nearest || distance(result) < distance(*nearest) || (distance(result) == distance(*nearest) &&
                    Better(result, *nearest)))) nearest = &result;
        if (nearest) return nearest->Settings;
        auto neighbor = base;
        if (neighbor.Resolution) --neighbor.Resolution;
        else if (_sizes.size() > 1) ++neighbor.Resolution;
        else
        {
            for (size_t i = 0; i < neighbor.Levels.size(); ++i)
                if (PerformanceScale(i) && neighbor.Levels[i] > _minimum.Levels[i])
                {
                    neighbor.Levels[i] = _minimum.Levels[i];
                    return Canonical(std::move(neighbor));
                }
            for (size_t i = 0; i < neighbor.Levels.size(); ++i)
                if (PerformanceScale(i) && neighbor.Levels[i] != Off && neighbor.Levels[i] < High)
                {
                    neighbor.Levels[i] = High;
                    return Canonical(std::move(neighbor));
                }
            if (neighbor.Levels[ProbeQuality] != Off)
            {
                neighbor.ProbeCapture = 1 - neighbor.ProbeCapture;
                return neighbor;
            }
            return {};
        }
        return neighbor;
    }

    void BeginPerformanceLower()
    {
        _performance.Stage = PerformanceStage::Lower;
        _performance.Modes = Upscaled() ? UpscalerModes(_minimum) : std::vector<Config>{_minimum};
    }

    double ShortFps(const Result& result) const { return LowFirst() ? result.ThresholdFps : result.Fps; }

    const Result* PerformanceMeasure(const Config& settings, bool reuseCalibration = false) const
    {
        if (const auto it = _short.find(settings); it != _short.end()) return &it->second;
        if (reuseCalibration && _calibration && _calibration->Settings == settings) return &*_calibration;
        return nullptr;
    }

    size_t LargerPerformanceResolution(size_t current) const
    {
        const uint64_t target = uint64_t(_sizes[current].second) * 2;
        size_t best = current + 1;
        const auto distance = [&](size_t index) -> uint64_t
        {
            const uint64_t height = _sizes[index].second;
            return height > target ? height - target : target - height;
        };
        for (size_t i = best + 1; i < _sizes.size(); ++i)
            if (distance(i) <= distance(best)) best = i;
        return best;
    }

    Config PerformanceHigh(Config settings) const
    {
        const auto* measured = PerformanceMeasure(settings, true);
        for (size_t i = 0; i < settings.Levels.size(); ++i)
            if (settings.Levels[i] != Off && !_fixedQuality[i] && (_available[i] || (measured && measured->Activity[i])))
                settings.Levels[i] = High;
        return Canonical(std::move(settings));
    }

    void ReducePerformanceQuality(const Result* full)
    {
        _performance.Stage = PerformanceStage::Reduce;
        _performance.RemainingDeficit = full ? 1000. / full->Fps - 1000. / Threshold[Performance] : 0;
    }

    void WriteRemovalEvidence(const Result& base, BistroBenchmarkData::Csv& csv) const
    {
        const auto key = "Trial." + std::to_string(base.Id) + ".Removal.";
        csv.Config(key + "ImageMode", base.Settings.Provider ? "upscaled provider " +
            std::to_string(base.Settings.Provider) + " preset " + std::to_string(base.Settings.Preset) : "Native");
        csv.Config(key + "Output", std::to_string(base.Output.first) + "x" + std::to_string(base.Output.second));
        csv.Config(key + "FPS", BistroBenchmarkData::Csv::Number(ShortFps(base)));
        for (size_t i = 0; i < base.Activity.size(); ++i)
            if (base.Activity[i])
                csv.Config(key + "ActiveTechnique", std::string(Techniques[i].Pass) + "=" +
                    (Techniques[i].Quality ? std::to_string(QualityValue(i, base.Settings.Levels[i])) : "On"));
    }

    void AdvancePerformance(BistroBenchmarkData::Csv& csv)
    {
        auto& progress = _performance;
        const auto passes = [&](const Result* result) { return result && ShortFps(*result) >= Threshold[Performance]; };
        const auto measure = [&](const Config& settings, const std::string& source)
        {
            if (!_short.count(settings) && !_failedProbes.count(settings)) Schedule(settings, source);
            return !_pending.empty();
        };
        while (_pending.empty() && progress.Full.empty())
        {
            auto next = progress.Anchor;
            switch (progress.Stage)
            {
            case PerformanceStage::Saved:
                if (const auto* best = Best(Upscaled(), false); best && best->Fps >= Threshold[Performance])
                    progress.Stage = PerformanceStage::Done;
                else BeginPerformanceLower();
                break;
            case PerformanceStage::Lower:
            {
                if (progress.NextMode == progress.Modes.size())
                {
                    const Result* fastest = nullptr;
                    for (const auto& mode : progress.Modes)
                        if (const auto* result = PerformanceMeasure(mode, true); result &&
                            (!fastest || ShortFps(*result) > ShortFps(*fastest))) fastest = result;
                    if (!fastest) { progress.Stage = PerformanceStage::Done; break; }
                    progress.Anchor = fastest->Settings;
                    progress.Stage = PerformanceStage::Remove;
                    break;
                }
                const auto& lower = progress.Modes[progress.NextMode];
                const auto* result = PerformanceMeasure(lower, true);
                if (!result && measure(lower, Upscaled() ? "upscaled lower anchor" : "Native lower anchor")) return;
                ++progress.NextMode;
                if (passes(result))
                {
                    progress.Anchor = lower;
                    progress.Stage = PerformanceStage::Raise;
                }
                break;
            }
            case PerformanceStage::Remove:
            {
                const auto& base = *PerformanceMeasure(progress.Anchor, true);
                WriteRemovalEvidence(base, csv);
                progress.Alternatives.clear();
                for (size_t i = 0; i < base.Activity.size(); ++i)
                    if (base.Activity[i] && (Techniques[i].Enable || Techniques[i].ScaledOff))
                    {
                        next = progress.Anchor;
                        next.Levels[i] = Off;
                        next = Canonical(std::move(next));
                        progress.Alternatives.push_back(next);
                        Schedule(next, "remove " + std::string(Techniques[i].Pass) + " at lower anchor / trial " +
                            std::to_string(base.Id));
                    }
                progress.Stage = PerformanceStage::RemovalResults;
                break;
            }
            case PerformanceStage::RemovalResults:
            {
                const Result* best = nullptr;
                for (const auto& candidate : progress.Alternatives)
                    if (const auto* result = PerformanceMeasure(candidate); result &&
                        (!best || Better(*result, *best, false, true))) best = result;
                if (!best) { progress.Stage = PerformanceStage::Done; break; }
                progress.Anchor = best->Settings;
                progress.Stage = passes(best) ? PerformanceStage::Raise : PerformanceStage::Remove;
                break;
            }
            case PerformanceStage::Raise:
                if (next.Resolution + 1 == _sizes.size())
                {
                    progress.Stage = PerformanceStage::Neighbor;
                    break;
                }
                next.Resolution = LargerPerformanceResolution(next.Resolution);
                if (measure(next, "raise output at minimum active quality")) return;
                if (passes(PerformanceMeasure(next))) progress.Anchor = next;
                else
                {
                    progress.RefineResolution = next.Resolution - 1;
                    progress.Stage = PerformanceStage::Refine;
                }
                break;
            case PerformanceStage::Refine:
                if (progress.RefineResolution <= next.Resolution)
                {
                    progress.Stage = PerformanceStage::Neighbor;
                    break;
                }
                next.Resolution = progress.RefineResolution;
                if (measure(next, "refine output between passing and failing sizes")) return;
                if (passes(PerformanceMeasure(next))) progress.Anchor = next;
                --progress.RefineResolution;
                break;
            case PerformanceStage::Neighbor:
                if (next.Resolution)
                {
                    --next.Resolution;
                    if (measure(next, "compare adjacent lower output at minimum quality")) return;
                }
                progress.Stage = PerformanceStage::High;
                break;
            case PerformanceStage::High:
                next = PerformanceHigh(progress.Anchor);
                if (measure(next, "raise all active techniques to High")) return;
                progress.Quality = next;
                if (const auto* result = PerformanceMeasure(next); passes(result))
                {
                    progress.Stage = PerformanceStage::NeighborHigh;
                    if (!_fullSettings.count(next))
                    {
                        progress.Full.emplace_back(next, "confirm High / trial " + std::to_string(result->Id));
                        progress.ConfirmingHigh = true;
                    }
                    else
                    {
                        const auto full = std::find_if(_confirmed.begin(), _confirmed.end(), [&](const Result& confirmed) {
                            return confirmed.Settings == next;
                        });
                        if (full == _confirmed.end() || full->Fps < Threshold[Performance])
                            ReducePerformanceQuality(full == _confirmed.end() ? nullptr : &*full);
                    }
                }
                else
                {
                    progress.Stage = PerformanceMeasure(next) ? PerformanceStage::Reduce : PerformanceStage::NeighborHigh;
                    if (progress.Stage == PerformanceStage::NeighborHigh) progress.Quality = progress.Anchor;
                }
                break;
            case PerformanceStage::Reduce:
            {
                const auto& base = *PerformanceMeasure(progress.Quality);
                progress.Alternatives.clear();
                for (size_t i = 0; i < base.Activity.size(); ++i)
                {
                    if (!PerformanceScale(i) || (!base.Activity[i] && !_available[i])) continue;
                    const int level = base.Settings.Levels[i];
                    for (int lower = level - 1; lower >= _minimum.Levels[i]; --lower)
                    {
                        if (QualityValue(i, Level(lower)) < 0 ||
                            QualityValue(i, Level(lower)) == QualityValue(i, Level(level))) continue;
                        next = progress.Quality;
                        next.Levels[i] = Level(lower);
                        next = Canonical(std::move(next));
                        progress.Alternatives.push_back(next);
                        Schedule(next, "lower " + std::string(Techniques[i].Pass) + " / trial " +
                            std::to_string(base.Id));
                        if (!progress.DeepReduction) break;
                    }
                }
                progress.Stage = PerformanceStage::ReductionResults;
                break;
            }
            case PerformanceStage::ReductionResults:
            {
                const auto& base = *PerformanceMeasure(progress.Quality);
                const double deficit = (std::max)(progress.RemainingDeficit, 1000. / ShortFps(base) - 1000. / Threshold[Performance]);
                const Result* best = nullptr;
                double bestSaving = 0;
                bool covered = false;
                for (const auto& candidate : progress.Alternatives)
                    if (const auto* result = PerformanceMeasure(candidate))
                    {
                        const auto gain = 1000. / base.Fps - 1000. / result->Fps;
                        const auto key = "Trial." + std::to_string(result->Id) + ".QualityTrade.";
                        csv.Config(key + "ComparedTrial", std::to_string(base.Id));
                        csv.Config(key + "DeficitMs", BistroBenchmarkData::Csv::Number(deficit));
                        csv.Config(key + "SavedMeanMs", BistroBenchmarkData::Csv::Number(gain));
                        const bool covers = passes(result) && gain >= progress.RemainingDeficit;
                        if (covers ? (!covered || Better(*result, *best, false, true)) :
                            (!covered && (!best || gain > bestSaving)))
                        {
                            best = result;
                            bestSaving = gain;
                            covered = covers;
                        }
                    }
                if (best && !covered && !progress.DeepReduction)
                {
                    progress.DeepReduction = true;
                    progress.Stage = PerformanceStage::Reduce;
                }
                else if (best && !covered)
                {
                    progress.Quality = best->Settings;
                    progress.DeepReduction = false;
                    if (progress.RemainingDeficit > 0)
                        progress.RemainingDeficit = (std::max)(0., progress.RemainingDeficit - bestSaving);
                    progress.Stage = PerformanceStage::Reduce;
                }
                else
                {
                    progress.Quality = best ? best->Settings : progress.Anchor;
                    progress.Stage = PerformanceStage::NeighborHigh;
                }
                break;
            }
            case PerformanceStage::NeighborHigh:
            {
                const auto* quality = PerformanceMeasure(progress.Quality, true);
                if (quality && progress.Quality.Resolution && !HighMajority(progress.Quality, quality->Activity, true))
                {
                    next = progress.Anchor;
                    --next.Resolution;
                    next = PerformanceHigh(next);
                    if (measure(next, "compare adjacent lower output with all active techniques at High")) return;
                    if (const auto* neighbor = PerformanceMeasure(next); passes(neighbor) && Better(*neighbor, *quality, false, true))
                        progress.Quality = next;
                }
                progress.Stage = PerformanceStage::Improve;
                break;
            }
            case PerformanceStage::Improve:
                ImprovePerformance(progress.Quality);
                progress.Stage = PerformanceStage::Done;
                break;
            case PerformanceStage::Done:
                return;
            }
        }
    }

    void ImprovePerformance(const Config& settings)
    {
        const auto* measured = PerformanceMeasure(settings, true);
        if (!measured || ShortFps(*measured) < Threshold[Performance]) return;
        for (size_t i = 0; i < settings.Levels.size(); ++i)
            if (measured->Activity[i] && settings.Levels[i] == High && QualityValue(i, Epic) != QualityValue(i, High))
            {
                auto next = settings;
                next.Levels[i] = Epic;
                Schedule(next, "raise " + std::string(Techniques[i].Pass) + " to Epic at chosen output");
            }
        ScheduleReflectionVariants(settings, "reflection options at chosen output", false, true);
    }

    Config EffectiveSettings(Config config) const
    {
        for (size_t i = 0; i < config.Levels.size(); ++i)
        {
            auto& level = config.Levels[i];
            if (level != Off && (_values[i][level] < 0 || _values[i][level] == _values[i][High])) level = High;
        }
        return config;
    }

    Config Canonical(Config config) const
    {
        config = EffectiveSettings(std::move(config));
        for (const auto [technique, owner] : {std::pair<size_t, size_t>{0, AO}, {2, Wind}, {3, 2},
                {4, 3}, {10, 6}, {10, 7}, {13, HUD}, {16, 5}, {17, SunShadow}})
            if (config.Levels[technique] == Off) config.Owners[owner] = -1;
        if (config.Owners[AO] >= 0)
        {
            const auto settings = GetAmbientOcclusionSettings(QualityValue(AO, config.Levels[AO]));
            if (!BistroRender::SceneAO::SupportsRemote(settings, _aoDepthNormals)) config.Owners[AO] = -1;
            if (settings.Implementation == 3) config.RemoteAOFilter = false;
        }
        if (config.Owners[AO] < 0) config.RemoteAOFilter = false;
        const auto probes = config.Levels[ProbeQuality];
        std::fill(config.Owners.begin() + FirstProbe + probes, config.Owners.begin() + _firstLocalShadow, -1);
        const auto remoteProbes = std::count_if(config.Owners.begin() + FirstProbe,
            config.Owners.begin() + _firstLocalShadow, [](int owner) { return owner >= 0; });
        config.ProbeFrequency = GDX12ReflectionSchedule::CanonicalFrequency(config.ProbeFrequency, probes);
        if (!probes) config.ProbeCapture = 0;
        if (!remoteProbes || config.ProbeFrequency <= Frequency::Low || (remoteProbes == 1 && config.ProbeDelivery == 1))
            config.ProbeDelivery = 0;
        if (config.Levels[LocalShadowQuality] == Off)
            std::fill(config.Owners.begin() + _firstLocalShadow, config.Owners.end(), -1);
        for (size_t i = 0; i < _placements.size(); ++i)
        {
            const auto& modes = _placements[i].Modes;
            if (config.Owners[i] < 0 || modes.empty()) config.Sync[i] = -1;
            else if (std::find(modes.begin(), modes.end(), config.Sync[i]) == modes.end()) config.Sync[i] = modes.front();
        }
        return config;
    }

    Config FullComposition(Config config) const
    {
        for (size_t i = 0; i < config.Levels.size(); ++i)
            if (config.Levels[i] == Off) config.Levels[i] = _high.Levels[i];
        return config;
    }

    static Config WithMediumShadows(Config config)
    {
        for (const auto shadow : {SunShadowQuality, LocalShadowQuality})
            if (config.Levels[shadow] == High) config.Levels[shadow] = Medium;
        return config;
    }

    static Config WithHighInsteadOfMedium(Config config)
    {
        for (auto& level : config.Levels)
            if (level == Low || level == Medium) level = High;
        return config;
    }

    bool KeepsComposition(const Config& config) const
    {
        for (size_t i = 0; i < _available.size(); ++i)
            if (_available[i] && config.Levels[i] == Off) return false;
        return true;
    }

    void ObserveComposition(const Result& result)
    {
        if (std::find(result.Settings.Levels.begin(), result.Settings.Levels.end(), Off) != result.Settings.Levels.end()) return;
        for (size_t i = 0; i < _available.size(); ++i) _available[i] |= result.Activity[i];
    }

    static bool ReadReference(std::istream& file, Result& r)
    {
        std::string line;
        if (!std::getline(file >> std::ws, line)) return false;
        std::istringstream input(line);
        r.ShadowTexels.reset();
        r.SectionShadowTexels.fill(-1);
        if (!(input >> r.Id >> r.Settings.Provider >> r.Settings.Preset >> r.Output.first >> r.Output.second >>
            r.Render.first >> r.Render.second >> r.Fps >> r.P95 >> std::quoted(r.SourceCsv))) return false;
        int frequency;
        if (!(input >> frequency >> r.Settings.ProbeCapture >> r.Settings.ProbeDelivery) ||
            frequency < 0 || frequency > int(Frequency::Epic) ||
            r.Settings.ProbeCapture < 0 || r.Settings.ProbeCapture > 1 || r.Settings.ProbeDelivery != 0) return false;
        r.Settings.ProbeFrequency = Frequency(frequency);
        if (!std::isfinite(r.Fps) || r.Fps <= 0 || !std::isfinite(r.P95) || r.P95 <= 0 ||
            r.Settings.Provider < 0 || r.Settings.Provider >= int(GDX12UpscalerProvider::Count) ||
            r.Settings.Preset < 0 || r.Settings.Preset >= int(GDX12UpscalerQuality::Count)) return false;
        for (size_t i = 0; i < r.Activity.size(); ++i)
        {
            unsigned level;
            if (!(input >> level >> r.Activity[i]) || level > Epic) return false;
            r.Settings.Levels[i] = Level(level);
        }
        if (input >> std::ws && !input.eof())
        {
            double texels;
            if (!(input >> texels) || !std::isfinite(texels) || texels < 0) return false;
            r.ShadowTexels = texels;
            for (double& section : r.SectionShadowTexels)
                if (!(input >> section) || !std::isfinite(section) || (section < 0 && section != -1)) return false;
            if (!(input >> std::ws).eof()) return false;
        }
        r.Valid = true;
        return true;
    }

    void SaveReferences() const
    {
        auto pending = _baselineFile;
        pending += ".tmp";
        std::ofstream output(pending, std::ios::trunc);
        output << std::quoted(_conditions) << ' ' << _completedMode[0] << ' ' << _completedMode[1] << '\n' << std::setprecision(17);
        const auto write = [&output](const Result& r)
        {
            output << r.Id << ' ' << r.Settings.Provider << ' ' << r.Settings.Preset << ' ' <<
                r.Output.first << ' ' << r.Output.second << ' ' << r.Render.first << ' ' << r.Render.second <<
                ' ' << r.Fps << ' ' << r.P95 << ' ' << std::quoted(r.SourceCsv) << ' ' <<
                int(r.Settings.ProbeFrequency) << ' ' << r.Settings.ProbeCapture << ' ' << r.Settings.ProbeDelivery;
            for (size_t i = 0; i < r.Activity.size(); ++i)
                output << ' ' << unsigned(r.Settings.Levels[i]) << ' ' << r.Activity[i];
            if (r.ShadowTexels)
            {
                output << ' ' << *r.ShadowTexels;
                for (const double section : r.SectionShadowTexels) output << ' ' << section;
            }
            output << '\n';
        };
        for (const auto& r : _confirmed)
            if (!r.Remote) write(r);
        for (bool upscaled : {false, true})
            if (_completedMode[upscaled])
                if (const auto* reference = BestReference(upscaled); reference && !reference->SourceCsv.empty())
                    write(*reference);
        output.close();
        if (!output || !MoveFileExW(pending.c_str(), _baselineFile.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("Cannot save Single GPU reference");
    }

    static bool SameImage(const Result& a, const Result& b)
    {
        // Route totals cannot establish that the same lights received the same maps.
        if (a.Activity[LocalShadowQuality] || b.Activity[LocalShadowQuality] ||
            a.Settings.Levels[LocalShadowQuality] != Off || b.Settings.Levels[LocalShadowQuality] != Off)
            return false;
        return a.Output == b.Output && a.Render == b.Render && a.Settings.Provider == b.Settings.Provider &&
            a.Settings.Preset == b.Settings.Preset && a.Activity == b.Activity && a.Settings.Levels == b.Settings.Levels &&
            a.Settings.Owners[Wind] == b.Settings.Owners[Wind] &&
            a.Settings.ProbeFrequency == b.Settings.ProbeFrequency && a.Settings.ProbeCapture == b.Settings.ProbeCapture &&
            a.Settings.ProbeDelivery == b.Settings.ProbeDelivery && a.Settings.Sync == b.Settings.Sync;
    }

    const Result* BestReference(bool upscaled) const
    {
        const Result* best = Best(upscaled, false);
        if (LowFirst()) return best;
        for (const auto& r : _references)
            if (bool(r.Settings.Provider) == upscaled && (!best || Better(r, *best, false, false, true))) best = &r;
        return best;
    }

    const Result* ClosestReference(bool upscaled) const
    {
        const Result* best = nullptr;
        for (const auto& r : _references)
            if (bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] && (!best || r.Fps > best->Fps))
                best = &r;
        for (const auto& r : _confirmed)
            if (!r.Remote && bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] && (!best || r.Fps > best->Fps))
                best = &r;
        return best;
    }

    const Result* DetailedReference(bool upscaled) const
    {
        const Result* best = nullptr;
        for (const auto& r : _references)
            if (bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] &&
                (!best || Better(r, *best, true))) best = &r;
        for (const auto& r : _confirmed)
            if (!r.Remote && bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] &&
                (!best || Better(r, *best, true))) best = &r;
        return best;
    }
    Trial Make(const Config& config, std::string source, bool full)
    {
        return {++_nextId, config, std::move(source), full};
    }

    void Schedule(Config config, const std::string& source, bool priority = false)
    {
        config = Canonical(std::move(config));
        if (RemotePhase() && std::all_of(config.Owners.begin(), config.Owners.end(), [](int owner) { return owner < 0; })) return;
        if (!_scheduled.insert(config).second) return;
        auto trial = Make(config, source, false);
        if (priority) _priority.push_back(std::move(trial));
        else _pending.push_back(std::move(trial));
    }

    void ScheduleReflectionVariants(const Config& config, const std::string& source, bool priority = false,
        bool keepActive = false)
    {
        auto next = config;
        for (const auto quality : {Off, Low, Medium, High, Epic})
        {
            if (keepActive && (quality == Off || config.Levels[SSRQuality] == Off)) continue;
            next.Levels[SSRQuality] = quality;
            Schedule(next, source, priority);
        }
        for (const auto quality : {Low, Medium, High, Epic, Off})
        {
            if (keepActive && (quality == Off || config.Levels[ProbeQuality] == Off)) continue;
            next = config;
            next.Levels[ProbeQuality] = quality;
            if (config.Levels[ProbeQuality] == Off) next.ProbeFrequency = Frequency::Low;
            Schedule(next, source, priority);
        }
        if (!config.Levels[ProbeQuality]) return;
        next = config;
        for (const auto frequency : {Frequency::Off, Frequency::Low, Frequency::Medium, Frequency::High, Frequency::Epic})
        {
            next.ProbeFrequency = frequency;
            Schedule(next, source, priority);
        }
        next = config;
        next.ProbeCapture = 1 - next.ProbeCapture;
        Schedule(next, source, priority);
    }

    void Seed(const Config& config, bool postOnly = false, bool priority = false)
    {
        if (!RemotePhase())
        {
            Schedule(config, "full-quality ladder");
            return;
        }
        for (auto adapter : _adapters)
        {
            auto next = config;
            next.Owners[Post] = adapter;
            Schedule(next, "post placement", priority);
            next.Owners[HUD] = adapter;
            Schedule(next, "post and HUD placement", priority);
            if (!postOnly)
            {
                next = config;
                if (next.Levels[ProbeQuality])
                {
                    next.Owners[FirstProbe] = adapter;
                    Schedule(next, "reflection probe placement", priority);
                    std::fill_n(next.Owners.begin() + FirstProbe, next.Levels[ProbeQuality], adapter);
                    Schedule(next, "all probes on one adapter", priority);
                }
                next = config;
                next.Owners[AO] = adapter;
                next.RemoteAOFilter = true;
                Schedule(next, "AO and filter on one adapter", priority);
            }
        }
        if (postOnly) return;
        std::vector<std::vector<int>> seeds;
        for (const auto adapter : _adapters)
        {
            for (size_t i = 0; i < _placements.size(); ++i)
            {
                if (i >= FirstProbe && i < _firstLocalShadow && i - FirstProbe >= config.Levels[ProbeQuality]) continue;
                auto owners = config.Owners;
                owners[i] = adapter;
                seeds.push_back(owners);
            }
            if (_firstLocalShadow < _placements.size())
            {
                auto owners = config.Owners;
                std::fill(owners.begin() + _firstLocalShadow, owners.end(), adapter);
                seeds.push_back(std::move(owners));
            }
            for (const auto& [a, b] : {std::pair{Post, HUD}, {6u, 7u}, {9u, 10u}, {2u, 8u}})
            {
                auto owners = config.Owners;
                owners[a] = owners[b] = adapter;
                seeds.push_back(owners);
            }
        }
        std::shuffle(seeds.begin(), seeds.end(), _random);
        for (const auto& owners : seeds)
        {
            auto next = config;
            next.Owners = owners;
            Schedule(next, "single or coupled placement");
        }
    }

    Config Workload(Config config, size_t placement) const
    {
        const int owner = config.Owners[placement];
        for (size_t i = 0; i < config.Owners.size(); ++i)
            if (i != placement && config.Owners[i] != owner)
            {
                config.Owners[i] = -1;
                config.Sync[i] = -1;
            }
        return config;
    }

    void ObserveCosts(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
    {
        const double targetMs = 1000. / (_profile == Performance ? 60 : 30);
        for (size_t i = 0; i < FirstProbe; ++i)
        {
            // These exact modes request a version per frame even when their inputs stay unchanged.
            if (i != AO && i != Water && i != Wind) continue;
            const int owner = trial.Settings.Owners[i];
            if (owner < 0 || owner != result.Settings.Owners[i] || trial.Settings.Sync[i] != 1) continue;
            const auto found = result.Costs.find({_placements[i].Owner, BistroRender::Id(owner + 1)});
            if (found == result.Costs.end()) continue;
            const auto& cost = found->second;
            if (cost.TimedVersions < 8 || cost.ImportedVersions < 4 || cost.ReadyIntervals < 7 ||
                cost.SlowVersions < .8 * cost.TimedVersions || cost.ReadySpanMs <= cost.ReadyIntervals * targetMs)
                continue;
            _slowWork.insert(Workload(trial.Settings, i));
            csv.Config("Trial." + std::to_string(trial.Id) + ".OverBudgetProducer", _placements[i].Owner);
        }
    }

    void ExpandPolicies(const Trial& trial, const Result& result)
    {
        const auto& config = trial.Settings;
        auto next = config;
        const auto source = " / trial " + std::to_string(trial.Id);
        for (size_t i = 0; i < _placements.size(); ++i)
        {
            if (config.Owners[i] < 0 || result.Settings.Owners[i] != config.Owners[i]) continue;
            const auto cost = result.Costs.find({_placements[i].Owner, BistroRender::Id(config.Owners[i] + 1)});
            const bool waits = cost != result.Costs.end() && cost->second.PrimaryWaitMs > 0;
            for (const auto mode : _placements[i].Modes)
            {
                next = config;
                next.Sync[i] = mode;
                Schedule(next, "publication policy" + source, waits);
            }
        }
        if (config.ProbeFrequency > Frequency::Low)
            for (const auto mode : {0, 1, 2})
            {
                next = config;
                next.ProbeDelivery = mode;
                Schedule(next, "reflection transfer policy" + source);
            }
    }

    void Expand(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
    {
        const auto& config = trial.Settings;
        const auto source = std::to_string(trial.Id);
        const auto threshold = Threshold[_profile];
        const auto enqueue = [&](const Config& next, const char* reason, bool priority = false)
        {
            Schedule(next, std::string(reason) + " / trial " + source, priority);
        };
        auto next = config;
        if (result.Fps >= threshold * .75)
            ScheduleReflectionVariants(config, "reflection options / trial " + source);
        ExpandPolicies(trial, result);
        if (result.ThresholdFps < threshold)
        {
            if (config.Resolution)
            {
                --next.Resolution;
                enqueue(next, "lower output, same effects", true);
            }
            else
            {
                const auto high = WithHighInsteadOfMedium(config);
                auto reduced = high;
                bool pending = false, passes = false;
                const auto lowerQuality = [&](const Config& candidate)
                {
                    if (const auto measured = _short.find(candidate); measured != _short.end())
                        passes |= measured->second.ThresholdFps >= threshold;
                    else if (!_failedProbes.count(candidate))
                    {
                        enqueue(candidate, "minimum output with reduced quality", true);
                        pending = true;
                    }
                };
                if (result.Activity[SunShadowQuality] && result.Activity[LocalShadowQuality] &&
                    high.Levels[SunShadowQuality] == High && high.Levels[LocalShadowQuality] == High)
                    lowerQuality(WithMediumShadows(high));
                for (size_t i = 0; i < high.Levels.size(); ++i)
                {
                    if (!result.Activity[i] || high.Levels[i] != High || _values[i][Medium] == _values[i][High]) continue;
                    next = high;
                    next.Levels[i] = reduced.Levels[i] = Medium;
                    lowerQuality(next);
                }
                if (reduced.Levels != high.Levels) lowerQuality(reduced);
                if (!pending && !passes)
                {
                    auto both = reduced;
                    for (const auto shadow : {SunShadowQuality, LocalShadowQuality})
                        if (result.Activity[shadow] && reduced.Levels[shadow] > Low)
                        {
                            next = reduced;
                            next.Levels[shadow] = both.Levels[shadow] = Low;
                            lowerQuality(next);
                        }
                    if (both.Levels[SunShadowQuality] != reduced.Levels[SunShadowQuality] &&
                        both.Levels[LocalShadowQuality] != reduced.Levels[LocalShadowQuality]) lowerQuality(both);
                }
                if (!pending && !passes)
                {
                    for (size_t i = 0; i < std::size(Techniques); ++i)
                        if ((Techniques[i].Enable || Techniques[i].ScaledOff) && result.Activity[i] && config.Levels[i] != Off)
                        {
                            next = config;
                            next.Levels[i] = Off;
                            next.Resolution = _sizes.size() - 1;
                            enqueue(next, "effect removal after minimum resolution", true);
                        }
                    csv.Config("Trial." + source + "." + ProfileName[_profile] + ".RemovalEvidence",
                        BistroBenchmarkData::Csv::Number(result.ThresholdFps) + " FPS at " + std::to_string(result.Output.second) + "p");
                }
            }
        }
        if (result.Fps >= threshold * .96)
        {
            if (config.Resolution + 1 < _sizes.size())
            {
                next = config;
                ++next.Resolution;
                enqueue(next, "higher output");
                if (_profile == Performance && KeepsComposition(config))
                {
                    if (next.Levels[SunShadowQuality] == High && next.Levels[LocalShadowQuality] == High)
                    {
                        const auto mediumShadows = WithMediumShadows(next);
                        if (HighMajority(mediumShadows, result.Activity))
                            enqueue(mediumShadows, "higher output with Medium shadows", true);
                    }
                    for (size_t i = 0; i < std::size(Techniques); ++i)
                    {
                        if (config.Levels[i] != High || _values[i][Medium] == _values[i][High]) continue;
                        next = config;
                        ++next.Resolution;
                        next.Levels[i] = Medium;
                        if (HighMajority(next, result.Activity))
                            enqueue(next, "higher output with Medium", Techniques[i].ScaledOff);
                    }
                }
            }
            const bool allHigh = std::none_of(config.Levels.begin(), config.Levels.end(), [](Level l) { return l == Low || l == Medium; });
            for (size_t i = 0; i < std::size(Techniques); ++i)
            {
                if (config.Levels[i] == Off || config.Levels[i] == Epic) continue;
                next = config;
                next.Levels[i] = Level(config.Levels[i] + 1);
                if ((!allHigh && next.Levels[i] == Epic) || _values[i][next.Levels[i]] == _values[i][config.Levels[i]])
                    continue;
                enqueue(next, "raise effect quality");
            }
        }
        if (RemotePhase() && result.Fps >= threshold * .75)
        {
            for (auto adapter : _adapters)
                for (size_t i = 0; i < _placements.size(); ++i)
                {
                    if (config.Owners[i] == adapter) continue;
                    next = FullComposition(config);
                    next.Owners[i] = adapter;
                    enqueue(next, "neighbor placement");
                }
            if (config.Owners[AO] >= 0 && !config.RemoteAOFilter)
            {
                next = FullComposition(config);
                next.RemoteAOFilter = true;
                enqueue(next, "AO and filter on one adapter", true);
                if (config.Levels[AO] == High && _values[AO][Epic] != _values[AO][High])
                {
                    next.Levels[AO] = Epic;
                    enqueue(next, "higher AO with remote filter");
                }
            }
        }
    }

    bool HasQualityScale(size_t i) const
    {
        return _values[i][Medium] != _values[i][High] || _values[i][High] != _values[i][Epic];
    }

    bool PerformanceScale(size_t i) const
    {
        return !_fixedQuality[i] && (HasQualityScale(i) || (_values[i][Low] >= 0 && _values[i][Low] != _values[i][High]));
    }

    bool HighMajority(const Config& config, const std::array<bool, std::size(Techniques)>& active,
        bool countLow = false) const
    {
        size_t high = 0, medium = 0;
        for (size_t i = 0; i < active.size(); ++i)
            if (i != LocalShadowQuality && active[i] && (countLow ? PerformanceScale(i) : HasQualityScale(i)))
            {
                high += config.Levels[i] >= High;
                medium += config.Levels[i] == Medium || (countLow && config.Levels[i] == Low);
            }
        return high > medium;
    }

    bool Better(const Result& a, const Result& b, bool preferDetail = false, bool shortWindow = false,
        bool compareReferences = false) const
    {
        const auto threshold = Threshold[_profile];
        const bool lowFirst = LowFirst() || (_profile == Performance && compareReferences);
        const double fpsA = shortWindow ? ShortFps(a) : a.Fps, fpsB = shortWindow ? ShortFps(b) : b.Fps;
        const bool meetsA = fpsA >= threshold, meetsB = fpsB >= threshold;
        if (meetsA != meetsB) return meetsA;
        if (!meetsA && !preferDetail && fpsA != fpsB) return fpsA > fpsB;
        if (meetsA && a.Output == b.Output && a.Render == b.Render && a.Settings.Provider == b.Settings.Provider &&
            a.Settings.Preset == b.Settings.Preset && a.Settings.Levels[LocalShadowQuality] == b.Settings.Levels[LocalShadowQuality])
        {
            const auto comparable = [&](const Result& result) {
                auto levels = result.Settings.Levels;
                for (size_t i = 0; i < levels.size(); ++i)
                    if (!result.Activity[i] || i == ProbeQuality) levels[i] = Off;
                return levels;
            };
            if (comparable(a) == comparable(b))
            {
                const auto rankProbe = [&](const Result& result) {
                    const int quality = result.Activity[ProbeQuality] ? result.Settings.Levels[ProbeQuality] : Off;
                    const int cadence = quality ? int(result.Settings.ProbeFrequency) : 0;
                    return _profile == Quality ? std::pair{quality, cadence} : std::pair{cadence, quality};
                };
                if (rankProbe(a) != rankProbe(b)) return rankProbe(a) > rankProbe(b);
                if (a.Fps != b.Fps) return a.Fps > b.Fps;
                if (a.P95 != b.P95) return a.P95 < b.P95;
            }
        }
        const auto count = [this, lowFirst](const Result& r, Level level)
        {
            size_t n = 0;
            for (size_t i = 0; i < r.Activity.size(); ++i)
            {
                if (lowFirst && level > Low && !PerformanceScale(i)) continue;
                n += r.Activity[i] && (i == LocalShadowQuality ? level == (lowFirst ? Low : Medium) : r.Settings.Levels[i] >= level);
            }
            return n;
        };
        const auto activeA = count(a, lowFirst ? Low : Medium), activeB = count(b, lowFirst ? Low : Medium);
        if (activeA != activeB) return activeA > activeB;
        const auto rank = [&](const Result& r)
        {
            const auto high = count(r, High);
            const auto epic = count(r, Epic);
            const bool resolutionFirst = _profile == Performance && KeepsComposition(r.Settings) &&
                HighMajority(r.Settings, r.Activity, lowFirst);
            const auto pixels = uint64_t(r.Output.first) * r.Output.second;
            return std::tuple{resolutionFirst, resolutionFirst ? pixels : 0, high, lowFirst ? count(r, Medium) : 0,
                pixels, epic, r.ShadowTexels,
                uint64_t(r.Render.first) * r.Render.second, r.Fps, -r.P95};
        };
        return rank(a) > rank(b);
    }

    static bool Dominates(const Result& a, const Result& b)
    {
        if (a.Settings.Provider != b.Settings.Provider || a.Settings.Preset != b.Settings.Preset ||
            a.Fps < b.Fps || a.Output.first < b.Output.first || a.Output.second < b.Output.second ||
            a.Render.first < b.Render.first || a.Render.second < b.Render.second ||
            a.Settings.ProbeFrequency < b.Settings.ProbeFrequency ||
            ((a.Activity[LocalShadowQuality] || b.Activity[LocalShadowQuality]) &&
             (!a.ShadowTexels || !b.ShadowTexels || a.ShadowTexels < b.ShadowTexels)))
            return false;
        // Keep layouts whose short-window results are indistinguishable within measurement noise.
        if (a.Fps <= b.Fps * 1.04) return false;
        for (size_t i = 0; i < a.Activity.size(); ++i)
        {
            if (i == LocalShadowQuality) continue;
            const auto left = a.Activity[i] ? a.Settings.Levels[i] : Off;
            const auto right = b.Activity[i] ? b.Settings.Levels[i] : Off;
            if (left < right) return false;
        }
        return true;
    }

    const Result* Best(bool upscaled, bool remote) const
    {
        const Result* best = nullptr;
        for (const auto& result : _confirmed)
            if (bool(result.Settings.Provider) == upscaled && result.Remote == remote &&
                (!best || Better(result, *best)))
                best = &result;
        return best;
    }

    void ReportResult(BistroBenchmarkData::Csv& csv, const std::string& prefix, const Result* r) const
    {
        csv.Config(prefix + "Status", !r || r->Fps < Threshold[_profile] ? "not found during search" :
            _profile == Quality && r->Fps < 30 ? "confirmed within tolerance" : "confirmed");
        if (!r) return;
        csv.Config(prefix + "Trial", std::to_string(r->Id));
        if (!r->SourceCsv.empty())
        {
            csv.Config(prefix + "CSV", r->SourceCsv);
            auto trial = std::filesystem::u8path(r->SourceCsv);
            trial.replace_extension();
            trial /= "trial-" + std::to_string(r->Id) + ".csv";
            std::error_code error;
            if (std::filesystem::is_regular_file(_baselineFile.parent_path() / trial, error))
                csv.Config(prefix + "TrialCSV", trial.generic_u8string());
        }
        csv.Config(prefix + "Provider", std::to_string(r->Settings.Provider));
        csv.Config(prefix + "Preset", std::to_string(r->Settings.Preset));
        for (size_t i = 0; i < r->Settings.Owners.size(); ++i)
        {
            csv.Config(prefix + std::filesystem::path(_placements[i].Variable).u8string() + ".EffectiveOwner",
                std::to_string(r->Settings.Owners[i]));
            if (!_placements[i].Modes.empty())
                csv.Config(prefix + std::filesystem::path(_placements[i].Sync).u8string() + ".Effective",
                    r->Settings.Sync[i] < 0 ? "local" : std::to_string(r->Settings.Sync[i]));
        }
        for (const auto& [name, value] : Patch(r->Settings))
            csv.Config(prefix + "Preset." + std::filesystem::path(name).u8string(), value);
        csv.Config(prefix + "ReflectionPreference", _profile == Quality ?
            "spatial quality, update cadence, FPS/P95" : "update cadence, spatial quality, FPS/P95");
        for (size_t i = 0; i < r->Activity.size(); ++i)
            if (r->Activity[i]) csv.Config(prefix + "Technique", std::string(Techniques[i].Pass) + "=" +
                (HasQualityScale(i) ? std::to_string(QualityValue(i, r->Settings.Levels[i])) : "enabled"));
        csv.Config(prefix + "Output", std::to_string(r->Output.first) + "x" + std::to_string(r->Output.second));
        csv.Config(prefix + "Render", std::to_string(r->Render.first) + "x" + std::to_string(r->Render.second));
        csv.Config(prefix + "FPS", BistroBenchmarkData::Csv::Number(r->Fps));
        csv.Config(prefix + "P95ms", BistroBenchmarkData::Csv::Number(r->P95));
    }

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
    size_t _calibrationId = 0;
    std::optional<Result> _calibration;
    std::vector<int> _adapters;
    std::vector<Placement> _placements;
    size_t _firstLocalShadow = 0;
    std::vector<Size> _sizes;
    std::array<std::array<int, Epic + 1>, std::size(Techniques)> _values{};
    std::array<bool, std::size(Techniques)> _fixedQuality{};
    std::array<bool, std::size(Techniques)> _available{};
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
    std::array<bool, 2> _cachedMode{};
    std::array<bool, 2> _completedMode{};
    size_t _confirmedBeforePhase = 0;
    bool _triedClosest = false;
    std::array<std::optional<Result>, 2> _nearest, _detail, _nearMiss;
    std::array<unsigned, 2> _rejected{};
};
}
