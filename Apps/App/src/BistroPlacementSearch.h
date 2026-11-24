#pragma once
#include "Engine.RendererDX12/GDX12LocalLights.h"

#include "BistroBenchmarkCsv.h"
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
    {L"Render.ReflectionProbes.Enabled", L"Render.ReflectionProbes.Quality", "environment"},
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
};

inline const Placement Placements[]{
    {L"Render.AO.Adapter", "AOAdapter"},
    {L"Render.LightGrid.Adapter", "LightGridAdapter"},
    {L"Render.Particles.Adapter", "ParticleAdapter"},
    {L"Render.Water.Adapter", "WaterAdapter"},
    {L"Render.Wind.Adapter", "GrassWindAdapter"},
    {L"App.UI.CloudPicture.Adapter", "CloudPictureAdapter"},
    {L"Render.Sky.SkyView.Adapter", "SkyViewAdapter"},
    {L"Render.Sky.AerialPerspective.Adapter", "AerialPerspectiveAdapter"},
    {L"Render.WBOIT.Adapter", "TransparencyAdapter"},
    {L"App.UI.WorldCanvas.Atrium.Adapter", "AtriumCanvasAdapter"},
    {L"App.UI.WorldCanvas.Terrace.Adapter", "TerraceCanvasAdapter"},
    {L"Render.Post.Adapter", "PostAdapter"},
    {L"App.UI.GameHUD.Adapter", "GameHUDAdapter"},
    {L"Render.Shadows.Sun.Adapter", "SunShadowAdapter"},
};
inline constexpr size_t AO = 0, Wind = 4, Post = 11, HUD = 12, SunShadow = 13;
inline constexpr size_t WaterQuality = 4, SkinningQuality = 12;
inline constexpr size_t SunShadowQuality = std::size(Techniques) - 2, LocalShadowQuality = std::size(Techniques) - 1;

enum Level { Off, Low, Medium, High, Epic };
enum Profile { Performance, Quality };
inline constexpr double Threshold[]{60, 28};
inline constexpr const char* ProfileName[]{"Performance", "Quality"};

struct Config
{
    size_t Resolution = 0;
    int Provider = 0, Preset = 0;
    std::array<Level, std::size(Techniques)> Levels{};
    std::vector<int> Owners;
    bool RemoteAOFilter = false;

    bool operator<(const Config& rhs) const
    {
        return std::tie(Resolution, Provider, Preset, Levels, Owners, RemoteAOFilter) <
            std::tie(rhs.Resolution, rhs.Provider, rhs.Preset, rhs.Levels, rhs.Owners, rhs.RemoteAOFilter);
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
                    result.Settings = EffectiveSettings(std::move(result.Settings));
                    const auto source = result.SourceCsv.empty() ? std::filesystem::path(item.path()).replace_extension(".csv") :
                        (directory / std::filesystem::u8path(result.SourceCsv)).lexically_normal();
                    result.SourceCsv = source.lexically_relative(output.parent_path()).generic_u8string();
                    if (_remote && completed[bool(result.Settings.Provider)]) _cachedMode[bool(result.Settings.Provider)] = true;
                    _fullSettings.insert(result.Settings);
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
        _windAlgorithm = console.FindConsoleVariable(L"Render.Wind.Algorithm").GetString();
        _preferredProvider = console.FindConsoleVariable(L"Render.Upscaler.Provider").GetInt();
        _preferredPreset = console.FindConsoleVariable(L"Render.Upscaler.Quality").GetInt();
        _placements.assign(std::begin(Placements), std::end(Placements));
        if (console.FindConsoleVariable(L"Render.ReflectionProbes.Count").GetInt() > 0)
            _placements.push_back({L"Render.ReflectionProbes.First.Adapter", "Probe 0"});
        else
            csv.Config("Render.ReflectionProbes.First.Adapter.Reason", "reflection probe count is zero");
        _firstLocalShadow = _placements.size();
        if (!shadowFixtures.empty())
        {
            const UINT pages = console.FindConsoleVariable(L"Render.Shadows.Local.AtlasCount").GetUInt();
            for (UINT page = 0; page < pages; ++page)
            {
                RegisterLocalShadowPageSettings(page);
                _placements.push_back({LocalShadowPageAdapterName(page), "Local shadow page " + std::to_string(page)});
                auto name = LocalShadowPageCrossSyncName(page);
                auto mode = console.FindConsoleVariable(name).GetString();
                _localShadowSync.emplace_back(std::move(name), std::move(mode));
            }
        }
        for (unsigned height : {480u, 720u, 900u, 1080u, 1440u, 2160u, screen.second})
            if (height <= screen.second)
                _sizes.emplace_back(unsigned((uint64_t(height) * screen.first + screen.second / 2) / screen.second), height);
        std::sort(_sizes.begin(), _sizes.end());
        _sizes.erase(std::unique(_sizes.begin(), _sizes.end()), _sizes.end());
        _high.Owners.assign(_placements.size(), -1);
        _high.Levels.fill(High);
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            const auto& technique = Techniques[i];
            if (!technique.Quality) continue;
            const auto& variable = console.FindConsoleVariable(technique.Quality);
            auto& levels = _values[i];
            levels.fill(-1);
            for (int v = variable.GetEnumFirstValue(); v < variable.GetEnumFirstValue() + variable.GetEnumCount(); ++v)
            {
                const auto name = variable.GetEnumValueName(v);
                if (name == "Off") levels[Off] = v;
                if (name == "Low") levels[Low] = v;
                if (name == "Mid" || name == "Medium") levels[Medium] = v;
                if (name == "High") levels[High] = v;
                if (name == "Epic" || name == "Ultra") levels[Epic] = v;
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
        if (_adapters.empty() && _remote)
            csv.Config("MultiGPU.Reason", "no secondary GPU available");
    }

    Size Resolution(const Config& config) const { return _sizes.at(config.Resolution); }
    const auto& Resolutions() const { return _sizes; }
    const auto& Candidates() const { return _placements; }
    std::optional<std::string_view> ShadowPageSync(size_t candidate) const
    {
        if (candidate < _firstLocalShadow) return {};
        return _localShadowSync.at(candidate - _firstLocalShadow).second;
    }
    int QualityValue(size_t technique, Level level) const { return _values[technique][level]; }

    ConsoleVariablePatch Patch(const Config& config) const
    {
        ConsoleVariablePatch patch{
            {L"Render.Upscaler.Provider", std::to_string(config.Provider)},
            {L"Render.Upscaler.Quality", std::to_string(config.Preset)},
            {L"Render.AO.BlurOnSecondary", config.RemoteAOFilter ? "1" : "0"},
            {L"Render.Wind.Algorithm", config.Owners[Wind] >= 0 ? "1" : _windAlgorithm},
            {L"Render.AO.CrossSync", "1"}, {L"Render.Water.CrossSync", "1"}, {L"Render.Particles.CrossSync", "1"},
            {L"App.UI.CloudPicture.CrossSync", "1"}, {L"Render.Wind.CrossSync", "1"},
            {L"Render.Sky.SkyView.CrossSync", "1"}, {L"Render.Sky.AerialPerspective.CrossSync", "1"},
            {L"Render.Shadows.Sun.CrossSync", "1"},
            {L"App.UI.WorldCanvas.Atrium.CrossSync", "1"}, {L"App.UI.WorldCanvas.Terrace.CrossSync", "1"},
        };
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            if (Techniques[i].Enable)
                patch.emplace_back(Techniques[i].Enable, config.Levels[i] == Off ? "false" : "true");
            if (Techniques[i].Quality && QualityValue(i, config.Levels[i]) >= 0)
                patch.emplace_back(Techniques[i].Quality, std::to_string(QualityValue(i, config.Levels[i])));
        }
        for (size_t i = 0; i < _placements.size(); ++i)
            patch.emplace_back(_placements[i].Variable, std::to_string(config.Owners[i]));
        patch.insert(patch.end(), _localShadowSync.begin(), _localShadowSync.end());
        return patch;
    }

    ConsoleVariablePatch ControlledSettings() const { return Patch(_high); }

    Trial Calibration()
    {
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
            return true;
        }
        using U = GDX12UpscalerProvider;
        using P = GDX12UpscalerQuality;
        std::vector<Config> modes;
        const auto addMode = [&](int provider, int preset)
        {
            if (provider && preset != int(P::NativeAA) &&
                (provider != int(U::NIS) || preset <= int(P::Performance)))
            {
                seed.Provider = provider;
                seed.Preset = preset;
                modes.push_back(seed);
            }
        };
        addMode(_preferredProvider, _preferredPreset);
        for (int provider = int(U::TAA); provider > 0; --provider)
            addMode(provider, int(P::Balanced));
        for (int preset : {int(P::Quality), int(P::Performance), int(P::UltraQuality),
                           int(P::UltraPerformance), int(P::UltraQualityPlus), int(P::Default)})
            for (int provider = int(U::TAA); provider > 0; --provider)
                addMode(provider, preset);
        if (RemotePhase())
            for (const auto& mode : modes) Seed(mode, true);
        for (const auto& mode : modes) Seed(mode);
        return !_pending.empty();
    }

    void EndPhase()
    {
        if (RemotePhase() || !HasReference(Upscaled())) return;
        if (_confirmed.size() == _confirmedBeforePhase && (_short.empty() || Finalist())) return;
        _completedMode[Upscaled()] = true;
        SaveReferences();
    }

    std::optional<Trial> NextProbe()
    {
        while (!_priority.empty() || !_pending.empty())
        {
            auto& queue = !_priority.empty() && (_pending.empty() || (++_probeIndex % 3 != 0)) ? _priority : _pending;
            auto trial = std::move(queue.front());
            queue.pop_front();
            if (!_short.count(trial.Settings)) return trial;
        }
        return {};
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
            if (result.Fps < Threshold[_profile] && (!detail || Better(result, *detail, true))) detail = &result;
            const bool dominated = std::any_of(_short.begin(), _short.end(), [&](const auto& other)
                { return !_fullSettings.count(other.second.ConfirmationSettings) && !_retryFull.count(other.second.ConfirmationSettings) &&
                    Dominates(other.second, result); });
            if (dominated && result.Fps >= Threshold[_profile]) continue;
            if (!best || Better(result, *best)) best = &result;
        }
        if ((!incumbent || incumbent->Fps < Threshold[_profile]) && _triedClosest &&
            (!best || best->Fps < Threshold[_profile]))
        {
            if (detail) return detail;
        }
        return best && (!incumbent || Better(*best, *incumbent) || best->Fps >= Threshold[_profile] * .96)
            ? best : nullptr;
    }

    Trial Confirm(const Result& result)
    {
        if (result.Fps < Threshold[_profile]) _triedClosest = true;
        const auto source = _retryFull.count(result.ConfirmationSettings) ? "retry finalist of trial " : "finalist of trial ";
        return Make(result.ConfirmationSettings, source + std::to_string(result.Id), true);
    }

    void ScheduleWithinVideoMemory(const Trial& trial, ConsoleModule& console, Size output)
    {
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
        if (!result.Valid)
        {
            _failedProbes.insert(trial.Settings);
            ++_rejected[Upscaled()];
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
        if (!probe || Better(result, *probe)) probe = result;
        auto& detail = _detail[Upscaled()];
        if (result.Fps < Threshold[_profile] && (!detail || Better(result, *detail, true))) detail = result;
        auto& nearMiss = _nearMiss[Upscaled()];
        if (result.Fps < Threshold[_profile] && (!nearMiss || result.Fps > nearMiss->Fps)) nearMiss = result;
        Expand(trial, result, csv);
    }

    void Report(BistroBenchmarkData::Csv& csv, std::string_view reason) const
    {
        for (bool upscaled : {false, true})
        {
            const auto prefix = std::string(upscaled ? "Upscaled." : "Native.") + ProfileName[_profile] + ".";
            const auto* remote = _remote ? Best(upscaled, true) : nullptr;
            const auto* reference = BestReference(upscaled);
            const auto* best = remote && (!reference || Better(*remote, *reference)) ? remote : reference;
            ReportResult(csv, prefix, best);
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
    }

    bool Upscaled() const { return (_phase & 1u) != 0; }
    bool RemotePhase() const { return _phase >= 2; }

private:
    Config EffectiveSettings(Config config) const
    {
        for (size_t i = 0; i < config.Levels.size(); ++i)
        {
            auto& level = config.Levels[i];
            if (level != Off && (_values[i][level] < 0 || _values[i][level] == _values[i][High])) level = High;
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
                ' ' << r.Fps << ' ' << r.P95 << ' ' << std::quoted(r.SourceCsv);
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
            a.Settings.Owners[Wind] == b.Settings.Owners[Wind];
    }

    const Result* BestReference(bool upscaled) const
    {
        const Result* best = Best(upscaled, false);
        for (const auto& r : _references)
            if (bool(r.Settings.Provider) == upscaled && (!best || Better(r, *best))) best = &r;
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
        config = EffectiveSettings(std::move(config));
        // Owners of disabled techniques cannot change the publication or the schedule.
        for (const auto [technique, owner] : {std::pair<size_t, size_t>{0, AO}, {2, Wind}, {3, 2},
                {4, 3}, {10, 6}, {10, 7}, {13, HUD}, {16, 5}, {17, SunShadow}})
            if (config.Levels[technique] == Off) config.Owners[owner] = -1;
        if (config.Owners[AO] < 0) config.RemoteAOFilter = false;
        if (config.Levels[6] == Off && _firstLocalShadow > std::size(Placements))
            config.Owners[std::size(Placements)] = -1;
        if (config.Levels[18] == Off)
            std::fill(config.Owners.begin() + _firstLocalShadow, config.Owners.end(), -1);
        if (RemotePhase() && std::all_of(config.Owners.begin(), config.Owners.end(), [](int owner) { return owner < 0; })) return;
        if (!_scheduled.insert(config).second) return;
        auto trial = Make(config, source, false);
        if (priority) _priority.push_back(std::move(trial));
        else _pending.push_back(std::move(trial));
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

    bool HighMajority(const Config& config, const std::array<bool, std::size(Techniques)>& active) const
    {
        size_t high = 0, medium = 0;
        for (size_t i = 0; i < active.size(); ++i)
            if (i != LocalShadowQuality && active[i] && HasQualityScale(i))
            {
                high += config.Levels[i] >= High;
                medium += config.Levels[i] == Medium;
            }
        return high > medium;
    }

    bool Better(const Result& a, const Result& b, bool preferDetail = false) const
    {
        const auto threshold = Threshold[_profile];
        const bool meetsA = a.Fps >= threshold, meetsB = b.Fps >= threshold;
        if (meetsA != meetsB) return meetsA;
        if (!meetsA && !preferDetail && a.Fps != b.Fps) return a.Fps > b.Fps;
        const auto count = [](const Result& r, Level level)
        {
            size_t n = 0;
            for (size_t i = 0; i < r.Activity.size(); ++i)
                n += r.Activity[i] && (i == LocalShadowQuality ? level == Medium : r.Settings.Levels[i] >= level);
            return n;
        };
        const auto activeA = count(a, Medium), activeB = count(b, Medium);
        if (activeA != activeB) return activeA > activeB;
        const auto rank = [&](const Result& r)
        {
            const auto high = count(r, High);
            const auto epic = count(r, Epic);
            const bool resolutionFirst = _profile == Performance && KeepsComposition(r.Settings) &&
                HighMajority(r.Settings, r.Activity);
            const auto pixels = uint64_t(r.Output.first) * r.Output.second;
            return std::tuple{resolutionFirst, resolutionFirst ? pixels : 0, high, pixels, epic, r.ShadowTexels,
                uint64_t(r.Render.first) * r.Render.second, r.Fps, -r.P95};
        };
        return rank(a) > rank(b);
    }

    static bool Dominates(const Result& a, const Result& b)
    {
        if (a.Settings.Provider != b.Settings.Provider || a.Settings.Preset != b.Settings.Preset ||
            a.Fps < b.Fps || a.Output.first < b.Output.first || a.Output.second < b.Output.second ||
            a.Render.first < b.Render.first || a.Render.second < b.Render.second ||
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
            if (const auto sync = ShadowPageSync(i))
                csv.Config(prefix + std::filesystem::path(_localShadowSync[i - _firstLocalShadow].first).u8string(),
                    r->Settings.Owners[i] < 0 ? "local" : *sync);
        }
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
    bool _remote = false;
    unsigned _probeIndex = 0;
    size_t _nextId = 0;
    int _preferredProvider = 0, _preferredPreset = 0;
    std::string _windAlgorithm;
    std::mt19937 _random{20261122};
    Config _high;
    std::vector<int> _adapters;
    std::vector<Placement> _placements;
    size_t _firstLocalShadow = 0;
    ConsoleVariablePatch _localShadowSync;
    std::vector<Size> _sizes;
    std::array<std::array<int, Epic + 1>, std::size(Techniques)> _values{};
    std::array<bool, std::size(Techniques)> _available{};
    std::deque<Trial> _pending, _priority;
    std::set<Config> _scheduled;
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
