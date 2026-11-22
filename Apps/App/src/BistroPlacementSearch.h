#pragma once

#include "BistroBenchmarkCsv.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
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
    {L"Render.Fluid.Enabled", L"Render.Fluid.Quality", "water"},
    {L"Render.Clouds.Enabled", L"Render.Clouds.Quality", "cloud"},
    {L"Render.Reflection.Enabled", L"Render.Reflection.Quality", "environment"},
    {L"Render.SSR.Enabled", L"Render.SSR.Quality", "SSR"},
    {L"Render.DOF.Enabled", L"Render.DOF.Quality", "DoF"},
    {L"Render.MotionBlur.Enabled", L"Render.MotionBlur.Quality", "motion blur"},
    {L"Render.SkyAtmosphere.Enabled", L"Render.SkyAtmosphere.Quality", "atmosphere"},
    {L"Render.SkyBox.Enabled", L"Render.SkyBox.Quality", "sky"},
    {L"Render.Skinning.Enabled", L"Render.Skinning.Quality", "skin deformation"},
    {L"Render.UI.GameHUD.Enabled", L"Render.UI.GameHUD.Quality", "HUD"},
    {nullptr, L"Render.UI.WorldCanvas.Quality", "canvas"},
    {nullptr, L"Render.Tree.Quality", "Linden"},
    {L"App.CloudPicture.Enabled", nullptr, "cloud picture"},
    {nullptr, L"Render.Shadows.Sun.Resolution", "sun shadow", true},
    {nullptr, L"Render.Shadows.Local.Resolution", "local shadow", true},
};

struct Placement
{
    const wchar_t* Variable;
    const char* Owner;
};

inline constexpr Placement Placements[]{
    {L"App.AO.Adapter", "AOAdapter"},
    {L"App.LightGrid.Adapter", "LightGridAdapter"},
    {L"App.Particles.Adapter", "ParticleAdapter"},
    {L"App.Water.Adapter", "WaterAdapter"},
    {L"App.Grass.Wind.Adapter", "GrassWindAdapter"},
    {L"App.CloudPicture.Adapter", "CloudPictureAdapter"},
    {L"App.Sky.SkyView.Adapter", "SkyViewAdapter"},
    {L"App.Sky.AerialPerspective.Adapter", "AerialPerspectiveAdapter"},
    {L"App.WBOIT.Adapter", "TransparencyAdapter"},
    {L"App.UI.WorldCanvas.Adapter.Atrium", "AtriumCanvasAdapter"},
    {L"App.UI.WorldCanvas.Adapter.Terrace", "TerraceCanvasAdapter"},
    {L"App.UI.WorldCanvas.Adapter.Clouds", "CloudCanvasAdapter"},
    {L"App.Post.Adapter", "PostAdapter"},
    {L"App.UI.GameHUD.Adapter", "GameHUDAdapter"},
    {L"App.Shadows.Sun.Adapter", "SunShadowAdapter"},
};
inline constexpr size_t AO = 0, Wind = 4, Post = 12, HUD = 13;

enum Level { Off, Medium, High, Epic };
enum Profile { Performance, Quality };
inline constexpr double Threshold[]{60, 28};
inline constexpr const char* ProfileName[]{"Performance", "Quality"};

struct Config
{
    size_t Resolution = 0;
    int Provider = 0, Preset = 0;
    std::array<Level, std::size(Techniques)> Levels{};
    std::array<int, std::size(Placements)> Owners{};
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
    Size Output{}, Render{};
    double Fps = 0, P95 = 0, ThresholdFps = 0;
    bool Valid = false, Remote = false;
    std::array<bool, std::size(Techniques)> Activity{};
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

    void Start(ConsoleModule& console, Size original, Size screen, std::vector<int> adapters, bool remote)
    {
        _adapters = std::move(adapters);
        _remote = remote;
        const auto limit = std::max(original.second,
            std::min(screen.second, unsigned(uint64_t(screen.first) * original.second / original.first)));
        for (unsigned height : {480u, 720u, 900u, 1080u, 1440u, 2160u, original.second})
            if (height >= std::min(480u, original.second) && height <= limit)
                _sizes.emplace_back(unsigned(uint64_t(height) * original.first / original.second), height);
        std::sort(_sizes.begin(), _sizes.end());
        _sizes.erase(std::unique(_sizes.begin(), _sizes.end()), _sizes.end());
        _high.Owners.fill(-1);
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
        _values[_values.size() - 2] = {0, 1024, 2048, 4096};
        _values.back() = {0, 256, 512, 1024};
    }

    Size Resolution(const Config& config) const { return _sizes.at(config.Resolution); }
    int QualityValue(size_t technique, Level level) const { return _values[technique][level]; }

    ConsoleVariablePatch Patch(const Config& config) const
    {
        ConsoleVariablePatch patch{
            {L"Render.Upscaler.Provider", std::to_string(config.Provider)},
            {L"Render.Upscaler.Quality", std::to_string(config.Preset)},
            {L"App.AO.BlurOnSecondary", config.RemoteAOFilter ? "1" : "0"},
            {L"App.Grass.Wind.Algorithm", config.Owners[Wind] >= 0 ? "1" : _windAlgorithm},
            {L"App.AO.CrossSync", "1"}, {L"App.Water.CrossSync", "1"}, {L"App.Particles.CrossSync", "1"},
            {L"App.CloudPicture.CrossSync", "1"}, {L"App.Grass.Wind.CrossSync", "1"},
            {L"App.Sky.SkyView.CrossSync", "1"}, {L"App.Sky.AerialPerspective.CrossSync", "1"},
            {L"App.Shadows.Sun.CrossSync", "1"},
            {L"App.UI.WorldCanvas.CrossSync.Atrium", "1"}, {L"App.UI.WorldCanvas.CrossSync.Terrace", "1"},
            {L"App.UI.WorldCanvas.CrossSync.Clouds", "1"},
        };
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            if (Techniques[i].Enable)
                patch.emplace_back(Techniques[i].Enable, config.Levels[i] == Off ? "false" : "true");
            if (Techniques[i].Quality)
                patch.emplace_back(Techniques[i].Quality, std::to_string(QualityValue(i, config.Levels[i])));
        }
        for (size_t i = 0; i < std::size(Placements); ++i)
            patch.emplace_back(Placements[i].Variable, std::to_string(config.Owners[i]));
        return patch;
    }

    Trial Calibration(ConsoleModule& console)
    {
        _windAlgorithm = console.FindConsoleVariable(L"App.Grass.Wind.Algorithm").GetString();
        const auto& provider = console.FindConsoleVariable(L"Render.Upscaler.Provider");
        const auto& preset = console.FindConsoleVariable(L"Render.Upscaler.Quality");
        _preferredProvider = provider.GetInt();
        _preferredPreset = preset.GetInt();
        auto settings = _high;
        if (_remote) settings.Owners[Post] = _adapters.front();
        return Make(settings, "native minimum / route checkpoints", true);
    }

    bool BeginPhase(unsigned phase)
    {
        _phase = phase;
        _pending.clear();
        _priority.clear();
        _scheduled.clear();
        _short.clear();
        if (phase >= 2) return false;
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
            if (provider && preset != int(P::NativeAA))
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

    std::optional<Trial> NextProbe()
    {
        auto& queue = !_priority.empty() && (_pending.empty() || (++_probeIndex % 3 != 0)) ? _priority : _pending;
        if (queue.empty()) return {};
        auto trial = std::move(queue.front());
        queue.pop_front();
        return trial;
    }

    std::optional<Trial> NextFinal()
    {
        const auto* incumbent = Best(Upscaled());
        const Result* best = nullptr;
        for (const auto& [candidate, result] : _short)
        {
            if ((RemotePhase() && !result.Remote) || _fullSettings.count(candidate)) continue;
            const bool dominated = std::any_of(_short.begin(), _short.end(), [&](const auto& other)
                { return Dominates(other.second, result); });
            if (dominated && result.Fps >= Threshold[_profile]) continue;
            if (!best || Better(result, *best)) best = &result;
        }
        if (best && (!incumbent || Better(*best, *incumbent) || best->Fps >= Threshold[_profile] * .96))
            return Make(best->Settings, "finalist of trial " + std::to_string(best->Id), true);
        return {};
    }

    void Record(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
    {
        if (trial.Full)
        {
            _fullSettings.insert(trial.Settings);
            if (result.Valid)
                _confirmed.push_back(result);
            return;
        }
        if (!result.Valid)
        {
            ++_rejected[Upscaled()];
            return;
        }
        _short.emplace(trial.Settings, result);
        auto& probe = _nearest[Upscaled()];
        if (!probe || Better(result, *probe)) probe = result;
        Expand(trial, result, csv);
    }

    void Report(BistroBenchmarkData::Csv& csv, std::string_view reason) const
    {
        for (bool upscaled : {false, true})
        {
            const auto prefix = std::string(upscaled ? "Upscaled." : "Native.") + ProfileName[_profile] + ".";
            ReportResult(csv, prefix, Best(upscaled));
            if (const auto& probe = _nearest[upscaled]; probe)
            {
                csv.Config(prefix + "ProbeTrial", std::to_string(probe->Id));
                csv.Config(prefix + "ProbeFPS", BistroBenchmarkData::Csv::Number(probe->Fps));
            }
            csv.Config(prefix + "RejectedProbes", std::to_string(_rejected[upscaled]));
            csv.Config(prefix + "StopReason", reason);
        }
    }

    bool Upscaled() const { return (_phase & 1u) != 0; }
    bool RemotePhase() const { return _remote; }

private:
    Trial Make(const Config& config, std::string source, bool full)
    {
        return {++_nextId, config, std::move(source), full};
    }

    void Schedule(const Config& config, const std::string& source, bool priority = false)
    {
        if (!_scheduled.insert(config).second) return;
        auto trial = Make(config, source, false);
        if (priority) _priority.push_back(std::move(trial));
        else _pending.push_back(std::move(trial));
    }

    void Seed(const Config& config, bool postOnly = false)
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
            Schedule(next, "post placement");
            next.Owners[HUD] = adapter;
            Schedule(next, "post and HUD placement");
        }
        if (postOnly) return;
        std::vector<std::array<int, std::size(Placements)>> seeds;
        for (const auto adapter : _adapters)
        {
            for (size_t i = 0; i < std::size(Placements); ++i)
            {
                auto owners = config.Owners;
                owners[i] = adapter;
                seeds.push_back(owners);
            }
            for (const auto& [a, b] : {std::pair{Post, HUD}, {6u, 7u}, {9u, 10u}, {10u, 11u}, {2u, 8u}})
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
        if (result.Fps >= threshold * .96)
        {
            if (config.Resolution + 1 < _sizes.size())
            {
                next = config;
                ++next.Resolution;
                enqueue(next, "higher output");
                if (_profile == Performance)
                    for (size_t i = 0; i < std::size(Techniques); ++i)
                    {
                        if (config.Levels[i] != High || _values[i][Medium] == _values[i][High]) continue;
                        next = config;
                        ++next.Resolution;
                        next.Levels[i] = Medium;
                        if (HighMajority(next, result.Activity)) enqueue(next, "higher output with Medium");
                    }
            }
            const bool allHigh = std::none_of(config.Levels.begin(), config.Levels.end(), [](Level l) { return l == Medium; });
            for (size_t i = 0; i < std::size(Techniques); ++i)
            {
                if (config.Levels[i] == Off || config.Levels[i] == Epic) continue;
                next = config;
                next.Levels[i] = config.Levels[i] == Medium ? High : Epic;
                if ((!allHigh && next.Levels[i] == Epic) || _values[i][next.Levels[i]] == _values[i][config.Levels[i]])
                    continue;
                enqueue(next, "raise effect quality");
            }
        }
        if (RemotePhase() && result.Fps >= threshold * .75)
        {
            for (auto adapter : _adapters)
                for (size_t i = 0; i < std::size(Placements); ++i)
                {
                    if (config.Owners[i] == adapter) continue;
                    next = config;
                    next.Owners[i] = adapter;
                    enqueue(next, "neighbor placement");
                }
            if (config.Owners[AO] >= 0 && !config.RemoteAOFilter)
            {
                next = config;
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

    static bool HighMajority(const Config& config, const std::array<bool, std::size(Techniques)>& active)
    {
        size_t high = 0, medium = 0;
        for (size_t i = 0; i < active.size(); ++i)
            if (active[i] && Techniques[i].Quality)
            {
                high += config.Levels[i] >= High;
                medium += config.Levels[i] == Medium;
            }
        return high > medium;
    }

    bool Better(const Result& a, const Result& b) const
    {
        const auto threshold = Threshold[_profile];
        const bool meetsA = a.Fps >= threshold, meetsB = b.Fps >= threshold;
        if (meetsA != meetsB) return meetsA;
        const auto count = [](const Result& r, Level level)
        {
            size_t n = 0;
            for (size_t i = 0; i < r.Activity.size(); ++i)
                n += r.Activity[i] && r.Settings.Levels[i] >= level;
            return n;
        };
        const auto activeA = count(a, Medium), activeB = count(b, Medium);
        if (activeA != activeB) return activeA > activeB;
        if (!meetsA && a.Fps != b.Fps) return a.Fps > b.Fps;
        const auto rank = [&](const Result& r)
        {
            const auto high = count(r, High);
            const auto epic = count(r, Epic);
            const bool resolutionFirst = _profile == Performance && HighMajority(r.Settings, r.Activity);
            const auto pixels = uint64_t(r.Output.first) * r.Output.second;
            return std::tuple{resolutionFirst, resolutionFirst ? pixels : 0, high, pixels, epic,
                uint64_t(r.Render.first) * r.Render.second, r.Fps, -r.P95};
        };
        return rank(a) > rank(b);
    }

    static bool Dominates(const Result& a, const Result& b)
    {
        if (a.Settings.Provider != b.Settings.Provider || a.Settings.Preset != b.Settings.Preset ||
            a.Fps < b.Fps || a.Output.first < b.Output.first || a.Output.second < b.Output.second ||
            a.Render.first < b.Render.first || a.Render.second < b.Render.second)
            return false;
        bool strict = a.Fps > b.Fps || a.Output != b.Output || a.Render != b.Render;
        for (size_t i = 0; i < a.Activity.size(); ++i)
        {
            const auto left = a.Activity[i] ? a.Settings.Levels[i] : Off;
            const auto right = b.Activity[i] ? b.Settings.Levels[i] : Off;
            if (left < right) return false;
            strict |= left > right;
        }
        return strict;
    }

    const Result* Best(bool upscaled) const
    {
        const Result* best = nullptr;
        for (const auto& result : _confirmed)
            if (bool(result.Settings.Provider) == upscaled && result.Remote == _remote &&
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
    std::vector<Size> _sizes;
    std::array<std::array<int, 4>, std::size(Techniques)> _values{};
    std::deque<Trial> _pending, _priority;
    std::set<Config> _scheduled;
    std::map<Config, Result> _short;
    std::set<Config> _fullSettings;
    std::vector<Result> _confirmed;
    std::array<std::optional<Result>, 2> _nearest;
    std::array<unsigned, 2> _rejected{};
};
}
