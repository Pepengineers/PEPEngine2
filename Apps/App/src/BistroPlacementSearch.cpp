#include "BistroPlacementSearch.h"
#include "BistroBenchmarkCsv.h"
#include "BistroProbeSettings.h"
#include "Techniques/SceneAO.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include <iomanip>
#include <sstream>

namespace BistroPlacement
{
namespace
{
using Provider = GDX12UpscalerProvider;
using Preset = GDX12UpscalerQuality;
constexpr Preset ImageOrder[]{Preset::UltraPerformance, Preset::Performance, Preset::Balanced,
    Preset::Quality, Preset::UltraQuality, Preset::UltraQualityPlus};

const char* ModeName(int preset)
{
    switch (Preset(preset))
    {
    case Preset::UltraPerformance: return "Ultra Performance";
    case Preset::Performance: return "Performance";
    case Preset::Balanced: return "Balanced";
    case Preset::Quality: return "Quality";
    case Preset::UltraQuality: return "Ultra Quality";
    case Preset::UltraQualityPlus: return "Ultra Quality Plus";
    default: return "Native";
    }
}
}

void Search::LinkBaseline(const std::filesystem::path& output, std::string conditions)
{
    _conditions = std::string(ProfileName[_profile]) + ' ' + conditions;
    _baselineFile = output;
    _baselineFile.replace_extension(".baseline");
    std::filesystem::path latestSelection;
    for (const auto* folder : {"SingleGPU", "MultiGPU"})
    {
        const auto directory = output.parent_path().parent_path() / folder;
        if (!std::filesystem::is_directory(directory)) continue;
        for (const auto& item : std::filesystem::directory_iterator(directory))
        {
            if (item.path().extension() != ".baseline" || !item.is_regular_file()) continue;
            std::ifstream input(item.path());
            std::string key;
            if (!(input >> std::quoted(key)) || key != _conditions) continue;
            Result result;
            std::string role;
            while (input >> role && ReadReference(input, result))
            {
                if (result.Output != _output || (!_allowUpscalers && result.Settings.Provider)) continue;
                result.Settings.Owners = _high.Owners;
                result.Settings.Sync = _high.Sync;
                result.Settings = Canonical(std::move(result.Settings));
                const auto source = result.SourceCsv.empty() ? std::filesystem::path(item.path()).replace_extension(".csv") :
                    (directory / std::filesystem::u8path(result.SourceCsv)).lexically_normal();
                result.SourceCsv = source.lexically_relative(output.parent_path()).generic_u8string();
                if (role == "selected" && item.path().filename() >= latestSelection)
                {
                    _savedSelection = result;
                    latestSelection = item.path().filename();
                }
                if (role == "local") _references.push_back(result);
            }
        }
    }
    std::sort(_references.begin(), _references.end(), [](const auto& a, const auto& b) {
        return std::tie(a.SourceCsv, a.Id) < std::tie(b.SourceCsv, b.Id);
    });
}

void Search::Start(ConsoleModule& console, Size output, std::vector<int> adapters, bool remote,
    bool fixedWaterSubsteps, bool usesSkinningPreset, const std::set<std::wstring>& shadowFixtures, BistroBenchmarkData::Csv& csv,
    bool allowUpscalers, std::map<int, int> preferredProviders)
{
    _adapters = std::move(adapters);
    _remote = remote;
    _aoDepthNormals = console.FindConsoleVariable(L"Render.DepthNormals.PrepassEnabled").GetBool();
    _windAlgorithm = console.FindConsoleVariable(L"Render.Wind.Algorithm").GetString();
    _output = output;
    _allowUpscalers = allowUpscalers;
    _preferredProviders = std::move(preferredProviders);
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
    _high.Owners.assign(_placements.size(), -1);
    _high.Sync.assign(_placements.size(), -1);
    _high.Levels.fill(High);
    _minimum = _high;
    for (size_t i = 0; i < std::size(Techniques); ++i)
    {
        const auto& technique = _techniques[i];
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

ConsoleVariablePatch Search::Patch(const Config& config) const
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
    };
    for (size_t i = 0; i < std::size(Techniques); ++i)
    {
        if (_techniques[i].Enable)
            patch.emplace_back(_techniques[i].Enable, config.Levels[i] == Off ? "false" : "true");
        if (_techniques[i].Quality && QualityValue(i, config.Levels[i]) >= 0)
            patch.emplace_back(_techniques[i].Quality, std::to_string(QualityValue(i, config.Levels[i])));
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

Trial Search::Calibration()
{
    const auto* saved = _savedSelection ? &*_savedSelection : SavedPreset(false);
    auto trial = Make(saved ? saved->Settings : LowFirst() ? _minimum : _high,
        saved ? "remeasure saved preset and capture route checkpoints" :
        LowFirst() ? "Native minimum active levels and route checkpoints" : "Native High and route checkpoints", true);
    _calibrationId = trial.Id;
    return trial;
}

bool Search::NeedsPhase(unsigned phase) const
{
    if ((phase & 1) && !_allowUpscalers) return false;
    if (phase < 2) return true;
    return phase < 4 && _remote && !_adapters.empty() &&
        (HasReference(false) || (phase == 3 && HasReference(true)));
}

bool Search::BeginPhase(unsigned phase)
{
    _phase = phase;
    _triedClosest = false;
    _estimated.clear();
    _pending.clear();
    _priority.clear();
    _initialPlacements.clear();
    _dispatched.clear();
    _proposals.clear();
    _probeIndex = 0;
    _scheduled.clear();
    _short.clear();
    _failedProbes.clear();
    _inspect.clear();
    if (!NeedsPhase(phase)) return false;
    if (phase == 1) QueueDiscovery(-1);
    if (phase == 3)
    {
        for (int adapter : _adapters) QueueDiscovery(adapter);
        _seedRemoteAfterDiscovery = true;
        return true;
    }
    if (HighFirst())
    {
        _quality = {};
        return true;
    }
    if (LowFirst())
    {
        _performance = {};
        if (const auto* saved = SavedPreset(Upscaled()))
        {
            _performance.Full.emplace_back(saved->Settings, "remeasure saved preset");
            if (auto other = SavedCompetitor(*saved))
                Schedule(*other, "probe nearest saved competitor");
        }
        return true;
    }
    SeedRemotePhase();
    return !_initialPlacements.empty() || !_priority.empty() || !_pending.empty();
}

void Search::SeedRemotePhase()
{
    const auto* reference = BestReference(Upscaled());
    if (!reference && Upscaled()) reference = BestReference(false);
    if (!reference) return;
    if (!Upscaled() || reference->Settings.Provider)
    {
        SeedAdapters(*reference);
        Seed(reference->Settings, false, true);
        auto next = FullComposition(reference->Settings);
        if (next.Levels != reference->Settings.Levels) Seed(next, false, true);
        const auto closest = ClosestReference(Upscaled());
        if (closest && closest != reference) Seed(FullComposition(closest->Settings), true);
        const auto detail = DetailedReference(Upscaled());
        if (detail && detail != reference && detail != closest) Seed(FullComposition(detail->Settings), true);
    }
    if (!Upscaled())
    {
        Seed(_high);
        return;
    }
    const auto seedImages = [&](Config base)
    {
        for (size_t i = 0; i < StepCount(base.Owners[Post]); ++i)
        {
            const size_t step = _profile == Quality ? StepCount(base.Owners[Post]) - 1 - i : i;
            for (const auto& mode : AtStep(base, step))
                if (base.Owners[Post] < 0) Seed(mode);
                else Schedule(mode, "post placement / named reconstruction mode");
        }
    };
    seedImages(_high);
    for (int adapter : _adapters)
    {
        auto base = FullComposition(reference->Settings);
        base.Owners[Post] = adapter;
        seedImages(base);
        base.Levels = _high.Levels;
        seedImages(base);
    }
}

std::optional<Trial> Search::NextProbe(BistroBenchmarkData::Csv& csv)
{
    if (auto inspect = InspectNextImage()) return inspect;
    if (std::exchange(_seedRemoteAfterDiscovery, false)) SeedRemotePhase();
    while (!_initialPlacements.empty())
    {
        auto [adapter, settings] = std::move(_initialPlacements.front());
        _initialPlacements.pop_front();
        if (!_peerCoverage[Upscaled()][adapter].Consumed && _dispatched.insert(settings).second)
        {
            auto trial = Make(settings, "initial DXGI-order adapter probe", false);
            trial.RequireLive = true;
            return trial;
        }
    }
    while (!_priority.empty() || !_pending.empty() || !_proposals.empty())
    {
        ++_probeIndex;
        Trial trial;
        if (!_proposals.empty() && (_probeIndex % 3 != 0 || (_priority.empty() && _pending.empty())))
        {
            // Alternate combinations of consumed work and refinements of measured leaders.
            const bool preferMix = _probeIndex % 3 == 1;
            const auto order = [preferMix](const MeasuredProposal& proposal) {
                return std::tuple{(proposal.Kind == ProposalKind::Mixed) != preferMix,
                    proposal.Kind == ProposalKind::Refinement, proposal.Cost, proposal.Kind};
            };
            const auto best = std::min_element(_proposals.begin(), _proposals.end(), [&](const auto& a, const auto& b) {
                return order(a.second) < order(b.second);
            });
            trial = Make(best->first, std::move(best->second.Source), false);
            trial.RequireLive = best->second.Kind == ProposalKind::CompareLight;
            _proposals.erase(best);
        }
        else
        {
            const bool preferPriority = RemotePhase() ? _probeIndex % 2 != 0 : _probeIndex % 3 != 0;
            auto& queue = !_priority.empty() && (_pending.empty() || preferPriority) ? _priority : _pending;
            trial = std::move(queue.front());
            queue.pop_front();
            const auto measured = _proposals.find(trial.Settings);
            if (measured != _proposals.end() && measured->second.Kind >= ProposalKind::CompareLight) continue;
            _proposals.erase(trial.Settings);
        }
        const auto previous = _short.find(trial.Settings);
        const bool needsLiveTiming = trial.RequireLive && previous != _short.end() && previous->second.Reused;
        if (!needsLiveTiming && (previous != _short.end() || (RemotePhase() && !_dispatched.insert(trial.Settings).second))) continue;
        if (!RemotePhase() || trial.RequireLive || !SkipEstimated(trial, csv)) return trial;
    }
    return {};
}

std::optional<Trial> Search::PerformanceConfirmation(BistroBenchmarkData::Csv& csv)
{
    if (!LowFirst() || (Upscaled() && !_inspect.empty())) return {};
    for (;;)
    {
        if (_performance.Full.empty() && _pending.empty() && _priority.empty()) AdvancePerformance(csv);
        if (_performance.Full.empty()) return {};
        auto [settings, source] = std::move(_performance.Full.front());
        _performance.Full.pop_front();
        if (!_fullSettings.count(settings))
        {
            auto trial = Make(settings, std::move(source), true);
            return trial;
        }
    }
}

std::optional<Trial> Search::NextQualityTrial(BistroBenchmarkData::Csv& csv, bool canProbe)
{
    if (Upscaled() && !_inspect.empty()) return canProbe ? InspectNextImage() : std::nullopt;
    if (_quality.RecheckSaved)
    {
        _quality.RecheckSaved = false;
        if (const auto* saved = SavedPreset(Upscaled()); saved && !_fullSettings.count(saved->Settings))
            return Make(saved->Settings, "remeasure saved preset", true);
    }
    if (_pending.empty())
        if (auto full = AdvanceQuality(csv, canProbe)) return full;
    if (canProbe)
        if (auto probe = NextProbe(csv)) return probe;
    if (const auto* finalist = QualityFinalist()) return Confirm(*finalist);
    if (const auto* reserve = Finalist()) return Confirm(*reserve);
    return {};
}

const Result* Search::Finalist() const
{
    const auto* incumbent = RemotePhase() ? Best(Upscaled(), true) : BestReference(Upscaled());
    const Result* best = nullptr;
    const Result* detail = nullptr;
    for (const auto& entry : _short)
    {
        const auto& result = entry.second;
        if (_fullSettings.count(result.ConfirmationSettings) || (RemotePhase() && !result.Remote)) continue;
        if (ShortFps(result) < Threshold[_profile] && (!detail || Better(result, *detail, true, true))) detail = &result;
        const bool dominated = std::any_of(_short.begin(), _short.end(), [&](const auto& other)
            { return (!RemotePhase() || other.second.Remote) &&
                !_fullSettings.count(other.second.ConfirmationSettings) && !_retryFull.count(other.second.ConfirmationSettings) &&
                ShortFps(other.second) >= Threshold[_profile] &&
                Dominates(other.second, result); });
        if (dominated && ShortFps(result) >= Threshold[_profile]) continue;
        if (!best || Better(result, *best, false, true)) best = &result;
    }
    if ((!incumbent || incumbent->Fps < Threshold[_profile]) && _triedClosest &&
        (!best || ShortFps(*best) < Threshold[_profile]))
    {
        if (detail) return detail;
    }
    return best && (!incumbent || ShortFps(*best) > incumbent->Fps || ShortFps(*best) >= Threshold[_profile] * .96)
        ? best : nullptr;
}

Trial Search::Confirm(const Result& result)
{
    if (ShortFps(result) < Threshold[_profile]) _triedClosest = true;
    const auto source = _retryFull.count(result.ConfirmationSettings) ? "retry finalist of trial " : "finalist of trial ";
    return Make(result.ConfirmationSettings, source + std::to_string(result.Id), true);
}

void Search::Record(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv, std::string_view rejection)
{
    if (RemotePhase())
        for (size_t i = 0; i < trial.Settings.Owners.size(); ++i)
        {
            const int peer = trial.Settings.Owners[i];
            if (peer < 0) continue;
            auto& seen = _peerCoverage[Upscaled()][peer];
            seen.Attempt = trial.Id;
            if (!result.Valid) continue;
            seen.Completed = trial.Id;
            if (result.Settings.Owners[i] == peer) seen.Consumed = trial.Id;
        }
    const bool outputUnavailable = rejection == "requested output size unavailable";
    if (trial.Full)
    {
        if (trial.Comparison)
        {
            RecordComparison(trial, result, csv);
            return;
        }
        if (trial.Id == _calibrationId && trial.Settings.Provider)
        {
            if (result.Valid && trial.Settings.Owners == result.Settings.Owners) _savedCalibration = result;
            return;
        }
        if (LowFirst() && _performance.ConfirmingHigh && trial.Settings == _performance.Quality)
        {
            _performance.ConfirmingHigh = false;
            if (!result.Valid || result.Fps < Threshold[Performance])
                ReducePerformanceQuality(result.Valid ? &result : nullptr);
        }
        if (HighFirst() || (LowFirst() && trial.Id == _calibrationId))
        {
            if (LowFirst() && result.Valid) _calibration = result;
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
            SaveReferences();
        }
        else if (outputUnavailable || rejection == "requested image mode unavailable" ||
            rejection == "reconstruction failed" || rejection == "requested post adapter unavailable" ||
            rejection == "frame generation is active" || !_retryFull.insert(trial.Settings).second)
            _fullSettings.insert(trial.Settings);
        return;
    }
    ObserveCosts(trial, result, csv);
    if (!result.Valid)
    {
        _failedProbes.insert(trial.Settings);
        ++_rejected[Upscaled()];
        if (!RemotePhase()) return;
        if (rejection == "reflection face deadline missed")
        {
            ExpandPolicies(trial, result);
            ScheduleReflectionVariants(trial.Settings, "different reflection workload after missed deadline", true);
        }
        return;
    }
    const auto [stored, inserted] = _short.emplace(trial.Settings, result);
    if (!inserted)
    {
        if (!trial.RequireLive || !stored->second.Reused || result.Reused) return;
        stored->second = result;
    }
    auto& probe = _nearest[Upscaled()];
    if (!probe || Better(result, *probe, false, true)) probe = result;
    auto& detail = _detail[Upscaled()];
    if (ShortFps(result) < Threshold[_profile] && (!detail || Better(result, *detail, true, true))) detail = result;
    auto& nearMiss = _nearMiss[Upscaled()];
    if (ShortFps(result) < Threshold[_profile] && (!nearMiss || ShortFps(result) > ShortFps(*nearMiss))) nearMiss = result;
    if (HighFirst())
    {
        if (_quality.Stage == QualityStage::Reduce)
            if (const auto* base = QualityShort(_quality.Candidate))
            {
                const auto* full = QualityFull(_quality.Candidate);
                const double deficit = 1000. / (full ? full->Fps : ShortFps(*base)) - 1000. / Threshold[Quality];
                const double saved = 1000. / base->Fps - 1000. / result.Fps;
                const auto key = "Trial." + std::to_string(result.Id) + ".QualityTrade.";
                csv.Config(key + "ComparedTrial", std::to_string(base->Id));
                csv.Config(key + "DeficitMs", BistroBenchmarkData::Csv::Number((std::max)(0., deficit)));
                csv.Config(key + "SavedMeanMs", BistroBenchmarkData::Csv::Number(saved));
            }
        return;
    }
    if (!LowFirst()) Expand(trial, result, csv);
    else if (_performance.Stage == PerformanceStage::RemovalResults && ShortFps(result) >= Threshold[Performance])
    {
        const auto& base = *PerformanceMeasure(_performance.Anchor, true);
        if (std::count(result.Activity.begin(), result.Activity.end(), true) + 1 >=
            std::count(base.Activity.begin(), base.Activity.end(), true)) _pending.clear();
    }
}

void Search::Report(BistroBenchmarkData::Csv& csv, std::string_view reason,
    ConsoleModule& console, const std::filesystem::path& output) const
{
    if (const auto* selected = Winner()) csv.Config("SelectedImage", selected->Settings.Provider ? "Upscaled" : "Native");
    const Result* native = nullptr;
    const auto changes = [&](const std::string& key, const Result& before, const Result& after, bool improvementsOnly)
    {
        static constexpr const char* names[] = {"Off", "Low", "Medium", "High", "Epic"};
        for (size_t i = 0; i < std::size(Techniques); ++i)
        {
            const auto from = before.Activity[i] ? before.Settings.Levels[i] : Off;
            const auto to = after.Activity[i] ? after.Settings.Levels[i] : Off;
            if (from != to && (!improvementsOnly || to > from))
                csv.Config(key, std::string(_techniques[i].Pass) + " " + names[from] + " -> " + names[to]);
        }
        if (after.Activity[ProbeQuality] && before.Activity[ProbeQuality] && after.Settings.ProbeFrequency != before.Settings.ProbeFrequency &&
            (!improvementsOnly || after.Settings.ProbeFrequency > before.Settings.ProbeFrequency))
            csv.Config(key, "reflection probe update frequency " + std::to_string(int(before.Settings.ProbeFrequency)) +
                " -> " + std::to_string(int(after.Settings.ProbeFrequency)));
    };
    for (bool upscaled : {false, true})
    {
        if (upscaled && !_allowUpscalers) continue;
        const auto prefix = std::string(upscaled ? "Upscaled." : "Native.") + ProfileName[_profile] + ".";
        const auto* remote = _remote ? Best(upscaled, true) : nullptr;
        const auto* reference = BestReference(upscaled);
        const auto* best = remote && (!reference || Better(*remote, *reference)) ? remote : reference;
        ReportResult(csv, prefix, best);
        if (!upscaled) native = best;
        else if (native && best) changes(prefix + "ChangeFromNative", *native, *best, false);
        if (best)
        {
            auto path = output.parent_path() / (output.stem().wstring() + (upscaled ? L"-upscaled.ini" : L"-native.ini"));
            auto patch = Patch(best->Settings);
            const auto extraBegin = patch.size();
            constexpr auto atlasCount = L"Render.Shadows.Local.AtlasCount";
            patch.emplace_back(atlasCount, console.FindConsoleVariable(atlasCount).GetString());
            patch.emplace_back(L"Render.FrameGeneration.Provider", "0");
            const auto saved = console.SavePatchIni(path, patch);
            if (saved.Error.empty())
            {
                csv.Config(prefix + "PresetFile", path.filename().u8string());
                for (size_t i = extraBegin; i < patch.size(); ++i)
                    csv.Config(prefix + "Preset." + std::filesystem::path(patch[i].first).u8string(), patch[i].second);
            }
            else csv.Config(prefix + "PresetError", saved.Error);
        }
        if (_remote)
        {
            for (const int peer : _adapters)
            {
                const auto it = _peerCoverage[upscaled].find(peer);
                const PeerCoverage seen = it == _peerCoverage[upscaled].end() ? PeerCoverage{} : it->second;
                const auto key = prefix + "Peer." + std::to_string(peer) + ".";
                if (seen.Consumed)
                    csv.Config(key + "ConsumedTrial", std::to_string(seen.Consumed));
                else if (seen.Completed)
                    csv.Config(key + "NoConsumedWork", "placement fell back or no consumer observed / trial " + std::to_string(seen.Completed));
                else
                    csv.Config(key + "NoCompletedMeasurement", seen.Attempt ? "last unsuccessful trial " +
                        std::to_string(seen.Attempt) : "no trial completed before search ended");
            }
            ReportResult(csv, prefix + "SingleGPU.", reference);
            ReportResult(csv, prefix + "MultiGPU.", remote);
            if (reference && !remote)
                csv.Config(prefix + "Winner", "Single GPU");
            if (!reference || !remote)
                csv.Config(prefix + "Comparison", !reference ?
                    "no confirmed Single GPU route in this run for this image mode" :
                    "no verified route with a consumed remote publication");
            else
            {
                csv.Config(prefix + "Winner", best == reference ? "Single GPU" : "Multi-GPU");
                const auto* comparable = SameImage(*remote, *reference) ? reference : nullptr;
                if (upscaled && !comparable)
                    for (const auto& measured : _comparedResults)
                        if (!measured.Remote && SameImage(*remote, measured) && (!comparable || Better(measured, *comparable)))
                            comparable = &measured;
                if (comparable)
                {
                    csv.Config(prefix + "SameImageBaselineTrial", std::to_string(comparable->Id));
                    csv.Config(prefix + "SameImageFpsGainPct",
                        BistroBenchmarkData::Csv::Number(100 * (remote->Fps / comparable->Fps - 1)));
                }
                else
                {
                    csv.Config(prefix + "Comparison", "compare measured render size, technique levels and local-shadow coverage");
                    changes(prefix + "ImprovedSetting", *reference, *remote, true);
                }
            }
        }
        if (const auto& probe = _nearest[upscaled]; probe)
        {
            csv.Config(prefix + "ProbeTrial", std::to_string(probe->Id));
            csv.Config(prefix + "ProbeFPS", BistroBenchmarkData::Csv::Number(ShortFps(*probe)));
        }
        if (const auto& detail = _detail[upscaled]; detail)
            csv.Config(prefix + "DetailedBelowTargetTrial", std::to_string(detail->Id));
        if (const auto& closest = _nearMiss[upscaled]; closest)
            csv.Config(prefix + "ClosestBelowTargetTrial", std::to_string(closest->Id));
        csv.Config(prefix + "RejectedProbes", std::to_string(_rejected[upscaled]));
        csv.Config(prefix + "StopReason", reason);
    }
}

void Search::QueueDiscovery(int owner)
{
    std::vector<Provider> providers{Provider::DLSS, Provider::FSR, Provider::XeSS, Provider::NIS, Provider::TAA};
    if (const auto first = std::find(providers.begin(), providers.end(), Provider(_preferredProviders.at(owner)));
        first != providers.end()) std::rotate(providers.begin(), first, std::next(first));
    for (const auto preset : ImageOrder)
        for (const auto provider : providers)
        {
            if ((provider == Provider::DLSS && preset == Preset::UltraQualityPlus) ||
                (provider == Provider::FSR && preset >= Preset::UltraQuality) ||
                ((provider == Provider::NIS || provider == Provider::TAA) && preset >= Preset::UltraPerformance)) continue;
            _inspect.emplace_back(owner, int(provider), int(preset));
        }
}

std::optional<Trial> Search::InspectNextImage()
{
    if (_inspect.empty()) return {};
    auto config = _profile == Performance ? _minimum : _high;
    std::tie(config.Owners[Post], config.Provider, config.Preset) = _inspect.front();
    _inspect.pop_front();
    auto trial = Make(config, "verify reconstruction on post adapter", false);
    trial.RequireLive = trial.InspectImage = true;
    return trial;
}

void Search::RecordImageMode(const Trial& trial, bool available)
{
    if (!available) return;
    auto& steps = _images[trial.Settings.Owners[Post]];
    if (steps.empty() || steps.back().first != trial.Settings.Preset)
        steps.emplace_back(trial.Settings.Preset, std::vector<int>{});
    steps.back().second.push_back(trial.Settings.Provider);
}

const Search::ImageSteps& Search::Steps(int owner) const
{
    static const ImageSteps empty;
    const auto it = _images.find(owner);
    return it == _images.end() ? empty : it->second;
}

size_t Search::Step(const Config& settings) const
{
    if (!settings.Provider) return 0;
    const auto& steps = Steps(settings.Owners[Post]);
    const auto it = std::find_if(steps.begin(), steps.end(), [&](const auto& step) { return step.first == settings.Preset; });
    return std::distance(steps.begin(), it);
}

bool Search::SupportsImage(const Config& config) const
{
    if (!config.Provider) return true;
    const auto index = Step(config);
    const auto& steps = Steps(config.Owners[Post]);
    if (index == steps.size()) return false;
    const auto& providers = steps[index].second;
    return std::find(providers.begin(), providers.end(), config.Provider) != providers.end();
}

std::vector<Config> Search::AtStep(const Config& base, size_t step) const
{
    if (!Upscaled()) return {base};
    std::vector<Config> candidates;
    const auto& steps = Steps(base.Owners[Post]);
    if (step >= steps.size()) return candidates;
    const auto& [preset, providers] = steps[step];
    for (int provider : providers)
    {
        auto candidate = base;
        candidate.Provider = provider;
        candidate.Preset = preset;
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

Config Search::ComparisonKey(Config config)
{
    if (config.Provider) config.Provider = -1;
    return config;
}

Trial Search::CompareImageMode(Trial trial, BistroBenchmarkData::Csv& csv)
{
    if (!trial.Settings.Provider || !trial.Full || trial.Comparison) return trial;
    const auto& settings = trial.Settings;
    _fullSettings.insert(settings);
    const auto key = ComparisonKey(settings);
    if (!_compared.insert(key).second) return {};
    auto candidates = AtStep(settings, Step(settings));
    if (candidates.empty())
    {
        if (_performance.ConfirmingHigh && settings == _performance.Quality)
        {
            _performance.ConfirmingHigh = false;
            ReducePerformanceQuality(nullptr);
        }
        return {};
    }
    _comparisonOpen = true;
    const auto id = ++_nextComparison;
    const auto prefix = "ReconstructionComparison." + std::to_string(id) + ".";
    csv.Config(prefix + "Mode", ModeName(settings.Preset));
    csv.Config(prefix + "ProviderCount", std::to_string(candidates.size()));
    csv.Config(prefix + "Status", "in progress");
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& candidate = candidates[i];
        auto pass = candidate == settings ? trial : Make(candidate, trial.Source, true);
        pass.Comparison = id;
        pass.Pass = i + 1;
        pass.PassCount = candidates.size();
        csv.Config(prefix + "Participant", std::to_string(pass.Id) + " " + std::to_string(candidate.Provider));
        _comparisonTrials.push_back(std::move(pass));
    }
    return {};
}

std::optional<Trial> Search::SavedComparison()
{
    if (_phase != 1 || !_inspect.empty() || !_savedCalibration || std::exchange(_comparedSavedCalibration, true)) return {};
    // Checkpoint capture adds CPU work to calibration. The cohort uses equal route work.
    return Make(_savedCalibration->Settings, "compare remeasured saved preset", true);
}

std::optional<Trial> Search::NextComparisonTrial()
{
    if (_comparisonTrials.empty()) return {};
    auto trial = std::move(_comparisonTrials.front());
    _comparisonTrials.pop_front();
    return trial;
}

void Search::InterruptComparison(BistroBenchmarkData::Csv& csv) const
{
    if (_comparisonOpen)
        csv.Config("ReconstructionComparison." + std::to_string(_nextComparison) + ".Status", "interrupted");
}

void Search::RecordComparison(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
{
    _fullSettings.insert(trial.Settings);
    if (result.Valid && trial.Settings.Owners == result.Settings.Owners && trial.Settings.Sync == result.Settings.Sync)
    {
        ObserveComposition(result);
        _comparedResults.push_back(result);
        if (!_comparisonWinner || Better(result, *_comparisonWinner)) _comparisonWinner = result;
    }
    else if (result.Valid && result.Remote)
    {
        auto effective = result;
        effective.ConfirmationSettings = effective.Settings;
        if (!_fullSettings.count(effective.Settings)) _short.insert_or_assign(effective.Settings, std::move(effective));
    }
    if (trial.Pass != trial.PassCount) return;

    const auto prefix = "ReconstructionComparison." + std::to_string(trial.Comparison) + ".";
    const auto* winner = _comparisonWinner ? &*_comparisonWinner : nullptr;
    if (_performance.ConfirmingHigh && ComparisonKey(trial.Settings) == ComparisonKey(_performance.Quality))
    {
        _performance.ConfirmingHigh = false;
        if (!winner || winner->Fps < Threshold[Performance]) ReducePerformanceQuality(winner);
        else _performance.Quality = winner->Settings;
    }
    if (winner)
    {
        csv.Config(prefix + "WinningTrial", std::to_string(winner->Id));
        _confirmed.push_back(*winner);
        SaveReferences();
    }
    csv.Config(prefix + "Status", !winner ? "no confirmed implementation" :
        trial.PassCount == 1 ? "single supported implementation" : "completed");
    _comparisonWinner.reset();
    _comparisonOpen = false;
}

const Result* Search::SavedPreset(bool upscaled) const
{
    if (_savedSelection && bool(_savedSelection->Settings.Provider) == upscaled) return &*_savedSelection;
    const Result* best = nullptr;
    for (const auto& result : _references)
        if (bool(result.Settings.Provider) == upscaled && (!best || Better(result, *best))) best = &result;
    return best;
}

std::optional<Config> Search::SavedCompetitor(const Result& saved) const
{
    const auto& base = saved.Settings;
    const Result* nearest = nullptr;
    const auto distance = [&](const Result& result)
    {
        const auto& other = result.Settings;
        return std::pair{other.Provider != base.Provider,
            std::abs(int64_t(saved.Render.second) - result.Render.second)};
    };
    for (const auto& result : _references)
        if (bool(result.Settings.Provider) == bool(base.Provider) && !(result.Settings == base) &&
            (!nearest || distance(result) < distance(*nearest) || (distance(result) == distance(*nearest) &&
                Better(result, *nearest)))) nearest = &result;
    return nearest ? std::optional<Config>(nearest->Settings) : std::nullopt;
}

const Result* Search::QualityFull(const Config& settings) const
{
    const auto key = ComparisonKey(settings);
    for (const auto& result : _confirmed)
        if (ComparisonKey(result.Settings) == key) return &result;
    return nullptr;
}

const Result* Search::QualityShort(const Config& settings) const
{
    const auto found = _short.find(settings);
    return found == _short.end() ? nullptr : &found->second;
}

const Result* Search::QualityFinalist(const std::vector<Config>* alternatives, const Result* reductionBase) const
{
    const Result* best = nullptr;
    bool bestCovers = false;
    const auto* fullBase = reductionBase ? QualityFull(reductionBase->Settings) : nullptr;
    const double deficit = reductionBase ? (std::max)(0., 1000. / (fullBase ? fullBase->Fps : ShortFps(*reductionBase)) -
        1000. / Threshold[Quality]) : 0.;
    for (const auto& [settings, result] : _short)
    {
        if ((alternatives && std::find(alternatives->begin(), alternatives->end(), settings) == alternatives->end()) ||
            ShortFps(result) < Threshold[Quality] * .96 || _fullSettings.count(settings) || QualityFull(settings)) continue;
        if (std::any_of(_confirmed.begin(), _confirmed.end(), [&](const Result& full) {
                return full.Fps >= Threshold[Quality] && QualityOutranks(full, result);
            })) continue;
        const bool covers = reductionBase && ShortFps(result) >= Threshold[Quality] &&
            1000. / reductionBase->Fps - 1000. / result.Fps >= deficit;
        if (!best || (covers != bestCovers ? covers : Better(result, *best, false, true)))
        {
            best = &result;
            bestCovers = covers;
        }
    }
    return best;
}

void Search::QueueQualityAlternative(Config settings, const std::string& source)
{
    settings = Canonical(std::move(settings));
    if (std::find(_quality.Alternatives.begin(), _quality.Alternatives.end(), settings) != _quality.Alternatives.end()) return;
    _quality.Alternatives.push_back(settings);
    if (!QualityFull(settings) && !_fullSettings.count(settings)) Schedule(settings, source);
}

std::optional<Trial> Search::AdvanceQuality(BistroBenchmarkData::Csv& csv, bool canProbe)
{
    auto& progress = _quality;
    const double threshold = Threshold[Quality];
    const auto passes = [&](const Result* result) { return result && result->Fps >= threshold; };
    const auto bestFull = [&]()
    {
        const Result* best = nullptr;
        for (const auto& config : progress.Alternatives)
            if (const auto* result = QualityFull(config); passes(result) && (!best || Better(*result, *best))) best = result;
        return best;
    };
    while (StepCount() && progress.Stage != QualityStage::Done && _pending.empty())
    {
        switch (progress.Stage)
        {
        case QualityStage::High:
        case QualityStage::RaiseDetail:
        {
            const bool raising = progress.Stage == QualityStage::RaiseDetail;
            const auto& base = raising ? progress.Anchor : _high;
            size_t step = StepCount() - 1, upper = StepCount();
            std::optional<size_t> lower;
            for (;;)
            {
                const Result* passing = nullptr;
                const Result* reserve = nullptr;
                for (auto candidate : AtStep(base, step))
                {
                    for (;;)
                    {
                        const auto* full = QualityFull(candidate);
                        const auto* sample = QualityShort(candidate);
                        if (!full && !sample && !_fullSettings.count(candidate) && !_failedProbes.count(candidate))
                        {
                            if (canProbe) Schedule(candidate, "find largest internal size for the full composition");
                            break;
                        }
                        const auto* measured = full ? full : sample;
                        if (measured && (!reserve || measured->Fps > reserve->Fps)) reserve = measured;
                        const auto* anchor = raising ? QualityFull(progress.Anchor) : nullptr;
                        const bool retainsEffects = !full ? false : anchor ?
                            std::equal(anchor->Activity.begin(), anchor->Activity.end(), full->Activity.begin(),
                                [](bool required, bool active) { return !required || active; }) : QualityComplete(*full);
                        const bool passed = full ? passes(full) && retainsEffects :
                            sample && !_fullSettings.count(candidate) && ShortFps(*sample) >= threshold * .96;
                        if (passed)
                        {
                            if (!passing || Better(*measured, *passing, false, true)) passing = measured;
                            break;
                        }
                        if (raising || candidate.ProbeFrequency == Frequency::Off) break;
                        candidate.ProbeFrequency = Frequency::Off;
                    }
                }
                if (!_pending.empty()) return {};
                if (passing)
                {
                    lower = step;
                    if (upper == step + 1)
                    {
                        if (!QualityFull(passing->Settings)) return Confirm(*passing);
                        progress.Anchor = passing->Settings;
                        progress.Stage = raising ? QualityStage::Reflections : QualityStage::Epic;
                        break;
                    }
                }
                else upper = step;
                if (lower) step = *lower + (upper - *lower) / 2;
                else if (step) step /= 2;
                else
                {
                    if (!reserve) { progress.Stage = QualityStage::Done; break; }
                    progress.Anchor = reserve->Settings;
                    progress.Stage = QualityStage::Fallback;
                    break;
                }
            }
            break;
        }
        case QualityStage::Done: return {};
        case QualityStage::Epic:
        {
            auto epic = progress.Anchor;
            const auto& high = *QualityFull(progress.Anchor);
            for (size_t i = 0; i < epic.Levels.size(); ++i)
                if (high.Activity[i] && PerformanceScale(i) && QualityValue(i, Epic) != QualityValue(i, High))
                    epic.Levels[i] = Epic;
            if (epic.Levels != progress.Anchor.Levels)
            {
                if (const auto* full = QualityFull(epic); passes(full) && QualityComplete(*full))
                    progress.Anchor = full->Settings;
                else if (!_failedProbes.count(epic))
                {
                    if (const auto* sample = QualityShort(epic))
                    {
                        if (!_fullSettings.count(epic) && !QualityFull(epic) && ShortFps(*sample) >= threshold * .96)
                            return Confirm(*sample);
                    }
                    else
                    {
                        if (canProbe) Schedule(epic, "all Epic at the confirmed High internal size");
                        return {};
                    }
                }
                if (epic.Levels != progress.Anchor.Levels)
                {
                    progress.Candidate = epic;
                    progress.Stage = QualityStage::Reduce;
                    break;
                }
            }
            progress.Stage = QualityStage::Reflections;
            break;
        }
        case QualityStage::Reduce:
        {
            if (!canProbe && progress.Alternatives.empty()) return {};
            if (progress.Alternatives.empty())
                for (size_t i = 0; i < progress.Candidate.Levels.size(); ++i)
                    if (progress.Candidate.Levels[i] == Epic)
                    {
                        auto next = progress.Candidate;
                        next.Levels[i] = High;
                        QueueQualityAlternative(next, "measure Epic to High frame-time change for " + std::string(_techniques[i].Pass));
                    }
            if (!_pending.empty()) break;
            const auto* base = QualityShort(progress.Candidate);
            if (const auto* finalist = QualityFinalist(&progress.Alternatives, base)) return Confirm(*finalist);
            if (const auto* full = bestFull())
            {
                if (Better(*full, *QualityFull(progress.Anchor))) progress.Anchor = full->Settings;
                progress.Alternatives.clear();
                progress.Stage = QualityStage::Reflections;
                break;
            }
            const Result* saving = nullptr;
            for (const auto& config : progress.Alternatives)
                if (const auto* sample = QualityShort(config); sample && (!base || sample->Fps > base->Fps) &&
                    (!saving || sample->Fps > saving->Fps || (sample->Fps == saving->Fps && Better(*sample, *saving, true, true))))
                    saving = sample;
            progress.Alternatives.clear();
            if (saving && !(saving->Settings == progress.Anchor))
                progress.Candidate = saving->Settings;
            else progress.Stage = QualityStage::Reflections;
            break;
        }
        case QualityStage::Reflections:
        {
            if (!canProbe && progress.Alternatives.empty()) return {};
            if (progress.Alternatives.empty() && progress.Anchor.Levels[ProbeQuality] != Off)
                for (int frequency = int(Frequency::Epic); frequency >= int(progress.Anchor.ProbeFrequency); --frequency)
                    for (int capture : {progress.Anchor.ProbeCapture, 1 - progress.Anchor.ProbeCapture})
                    {
                        auto next = progress.Anchor;
                        next.ProbeFrequency = Frequency(frequency);
                        next.ProbeCapture = capture;
                        QueueQualityAlternative(next, "probe cadence and capture at confirmed spatial quality");
                    }
            if (!_pending.empty()) break;
            if (const auto* finalist = QualityFinalist(&progress.Alternatives)) return Confirm(*finalist);
            progress.Stage = QualityStage::Done;
            break;
        }
        case QualityStage::Fallback:
        {
            if (!canProbe && progress.Alternatives.empty()) return {};
            auto minimum = progress.Anchor;
            minimum.Levels = _minimum.Levels;
            minimum = Canonical(std::move(minimum));
            if (progress.Alternatives.empty())
            {
                auto medium = progress.Anchor;
                for (size_t i = 0; i < medium.Levels.size(); ++i)
                    if (PerformanceScale(i) && QualityValue(i, Medium) != QualityValue(i, High))
                    {
                        auto next = progress.Anchor;
                        next.Levels[i] = medium.Levels[i] = Medium;
                        QueueQualityAlternative(next, "reduce one technique after High failed at minimum internal size");
                    }
                QueueQualityAlternative(medium, "full Medium after High failed at minimum internal size");
                QueueQualityAlternative(minimum, "minimum active levels at minimum internal size");
            }
            if (!_pending.empty()) break;
            if (const auto* finalist = QualityFinalist(&progress.Alternatives)) return Confirm(*finalist);
            const auto* full = bestFull();
            progress.Alternatives.clear();
            if (full && QualityComplete(*full))
            {
                progress.Anchor = full->Settings;
                progress.Stage = QualityStage::RaiseDetail;
            }
            else if (const auto* measure = QualityFull(minimum); measure && measure->Fps < threshold)
            {
                progress.Anchor = measure->Settings;
                progress.Stage = QualityStage::Remove;
            }
            else if (!_fullSettings.count(minimum) && QualityShort(minimum) && ShortFps(*QualityShort(minimum)) < threshold)
            {
                progress.Anchor = minimum;
                progress.Stage = QualityStage::Remove;
            }
            else progress.Stage = QualityStage::Done;
            break;
        }
        case QualityStage::Remove:
        {
            if (!canProbe && progress.Alternatives.empty()) return {};
            if (progress.Alternatives.empty())
            {
                const auto* base = QualityFull(progress.Anchor);
                if (!base) base = QualityShort(progress.Anchor);
                WriteRemovalEvidence(*base, csv);
                for (size_t i = 0; i < base->Activity.size(); ++i)
                    if (base->Activity[i] && (_techniques[i].Enable || _techniques[i].ScaledOff))
                    {
                        auto next = progress.Anchor;
                        next.Levels[i] = Off;
                        QueueQualityAlternative(next, "remove " + std::string(_techniques[i].Pass) +
                            " at minimum active levels after trial " + std::to_string(base->Id));
                    }
            }
            if (!_pending.empty()) break;
            if (const auto* finalist = QualityFinalist(&progress.Alternatives)) return Confirm(*finalist);
            if (const auto* full = bestFull())
            {
                progress.Anchor = full->Settings;
                progress.Alternatives.clear();
                progress.Stage = QualityStage::RaiseDetail;
                break;
            }
            const Result* best = nullptr;
            double bestFps = 0;
            for (const auto& config : progress.Alternatives)
            {
                const auto* measured = QualityFull(config);
                double fps = measured ? measured->Fps : 0;
                if (!measured && !_fullSettings.count(config)) measured = QualityShort(config);
                if (measured && !fps) fps = ShortFps(*measured);
                if (fps > bestFps && fps < threshold)
                {
                    best = measured;
                    bestFps = fps;
                }
            }
            progress.Alternatives.clear();
            if (best) progress.Anchor = best->Settings;
            else progress.Stage = QualityStage::Done;
            break;
        }
        }
    }
    return {};
}

const Result* Search::PerformanceMeasure(const Config& settings, bool reuseCalibration) const
{
    if (const auto it = _short.find(settings); it != _short.end()) return &it->second;
    if (reuseCalibration)
    {
        if (const auto* full = QualityFull(settings); full && full->Settings == settings) return full;
        if (_calibration && _calibration->Settings == settings) return &*_calibration;
    }
    return nullptr;
}

Config Search::PerformanceHigh(Config settings) const
{
    const auto* measured = PerformanceMeasure(settings, true);
    for (size_t i = 0; i < settings.Levels.size(); ++i)
        if (settings.Levels[i] != Off && !_fixedQuality[i] && (_available[i] || (measured && measured->Activity[i])))
            settings.Levels[i] = High;
    return Canonical(std::move(settings));
}

void Search::ReducePerformanceQuality(const Result* full)
{
    _performance.Stage = PerformanceStage::Reduce;
    _performance.RemainingDeficit = full ? 1000. / full->Fps - 1000. / Threshold[Performance] : 0;
}

void Search::WriteRemovalEvidence(const Result& base, BistroBenchmarkData::Csv& csv) const
{
    const auto key = "Trial." + std::to_string(base.Id) + ".Removal.";
    csv.Config(key + "ImageMode", base.Settings.Provider ? "upscaled provider " +
        std::to_string(base.Settings.Provider) + " preset " + std::to_string(base.Settings.Preset) : "Native");
    csv.Config(key + "Output", std::to_string(base.Output.first) + "x" + std::to_string(base.Output.second));
    csv.Config(key + "FPS", BistroBenchmarkData::Csv::Number(ShortFps(base)));
    for (size_t i = 0; i < base.Activity.size(); ++i)
        if (base.Activity[i])
            csv.Config(key + "ActiveTechnique", std::string(_techniques[i].Pass) + "=" +
                (_techniques[i].Quality ? std::to_string(QualityValue(i, base.Settings.Levels[i])) : "On"));
}

void Search::AdvancePerformance(BistroBenchmarkData::Csv& csv)
{
    auto& progress = _performance;
    const auto passes = [&](const Result* result) { return result && ShortFps(*result) >= Threshold[Performance]; };
    const auto measure = [&](const Config& settings, const std::string& source)
    {
        if (!_short.count(settings) && !_failedProbes.count(settings)) Schedule(settings, source);
        return !_pending.empty();
    };
    const auto measureStep = [&](const Config& base, size_t step, const char* source)
    {
        const Result* best = nullptr;
        for (const auto& candidate : AtStep(base, step))
        {
            if (const auto* result = PerformanceMeasure(candidate, true))
            {
                if (!best || Better(*result, *best, false, true)) best = result;
            }
            else measure(candidate, source);
        }
        return best;
    };
    if (!StepCount()) return;
    while (_pending.empty() && progress.Full.empty())
    {
        auto next = progress.Anchor;
        switch (progress.Stage)
        {
        case PerformanceStage::Lower:
        {
            const auto* result = measureStep(_minimum, 0, "minimum internal size with all effects at minimum active levels");
            if (!_pending.empty()) return;
            if (!result) { progress.Stage = PerformanceStage::Done; break; }
            progress.Anchor = result->Settings;
            progress.Stage = passes(result) ? PerformanceStage::Raise : PerformanceStage::Remove;
            break;
        }
        case PerformanceStage::Remove:
        {
            const auto& base = *PerformanceMeasure(progress.Anchor, true);
            WriteRemovalEvidence(base, csv);
            progress.Alternatives.clear();
            for (size_t i = 0; i < base.Activity.size(); ++i)
                if (base.Activity[i] && (_techniques[i].Enable || _techniques[i].ScaledOff))
                {
                    next = progress.Anchor;
                    next.Levels[i] = Off;
                    next = Canonical(std::move(next));
                    progress.Alternatives.push_back(next);
                    Schedule(next, "remove " + std::string(_techniques[i].Pass) + " at lower anchor / trial " +
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
        {
            const size_t current = Step(next);
            if (current + 1 >= StepCount()) { progress.Stage = PerformanceStage::High; break; }
            const size_t step = (std::min)(StepCount() - 1, current + (StepCount() - current) / 2);
            const auto* result = measureStep(next, step, "increase internal size with the same active levels");
            if (!_pending.empty()) return;
            if (passes(result)) progress.Anchor = result->Settings;
            else
            {
                progress.RefineStep = step - 1;
                progress.Stage = PerformanceStage::Refine;
            }
            break;
        }
        case PerformanceStage::Refine:
        {
            if (progress.RefineStep <= Step(next)) { progress.Stage = PerformanceStage::High; break; }
            const auto* result = measureStep(next, progress.RefineStep, "refine internal size after a missed target");
            if (!_pending.empty()) return;
            if (passes(result)) progress.Anchor = result->Settings;
            --progress.RefineStep;
            break;
        }
        case PerformanceStage::High:
        {
            const auto* result = measureStep(PerformanceHigh(progress.Anchor), Step(progress.Anchor), "raise all active techniques to High");
            if (!_pending.empty()) return;
            next = result ? result->Settings : progress.Anchor;
            progress.Quality = next;
            if (passes(result))
            {
                progress.Stage = PerformanceStage::NeighborHigh;
                if (!_fullSettings.count(next))
                {
                    progress.Full.emplace_back(next, "confirm High / trial " + std::to_string(result->Id));
                    progress.ConfirmingHigh = true;
                }
                else
                {
                    const auto* full = QualityFull(next);
                    if (!full || full->Fps < Threshold[Performance]) ReducePerformanceQuality(full);
                    else progress.Quality = full->Settings;
                }
            }
            else
            {
                progress.Stage = result ? PerformanceStage::Reduce : PerformanceStage::NeighborHigh;
                if (progress.Stage == PerformanceStage::NeighborHigh) progress.Quality = progress.Anchor;
            }
            break;
        }
        case PerformanceStage::Reduce:
        {
            const auto& base = *PerformanceMeasure(progress.Quality, true);
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
                    Schedule(next, "lower " + std::string(_techniques[i].Pass) + " / trial " +
                        std::to_string(base.Id));
                    if (!progress.DeepReduction) break;
                }
            }
            progress.Stage = PerformanceStage::ReductionResults;
            break;
        }
        case PerformanceStage::ReductionResults:
        {
            const auto& base = *PerformanceMeasure(progress.Quality, true);
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
            const size_t step = Step(progress.Quality);
            if (quality && step && step < StepCount() && !HighMajority(progress.Quality, quality->Activity))
            {
                const auto* neighbor = measureStep(PerformanceHigh(progress.Anchor), step - 1,
                    "compare the adjacent smaller internal size with all active techniques at High");
                if (!_pending.empty()) return;
                if (passes(neighbor) && Better(*neighbor, *quality, false, true)) progress.Quality = neighbor->Settings;
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

void Search::ImprovePerformance(const Config& settings)
{
    const auto* measured = PerformanceMeasure(settings, true);
    if (!measured || ShortFps(*measured) < Threshold[Performance]) return;
    for (size_t i = 0; i < settings.Levels.size(); ++i)
        if (measured->Activity[i] && settings.Levels[i] == High && QualityValue(i, Epic) != QualityValue(i, High))
        {
            auto next = settings;
            next.Levels[i] = Epic;
            Schedule(next, "raise " + std::string(_techniques[i].Pass) + " to Epic at the chosen internal size");
        }
    ScheduleReflectionVariants(settings, "reflection options at the chosen internal size");
}

Config Search::EffectiveSettings(Config config) const
{
    for (size_t i = 0; i < config.Levels.size(); ++i)
    {
        auto& level = config.Levels[i];
        if (level != Off && (_values[i][level] < 0 || _values[i][level] == _values[i][High])) level = High;
    }
    return config;
}

Config Search::Canonical(Config config) const
{
    config = EffectiveSettings(std::move(config));
    for (const auto [technique, owner] : {std::pair<size_t, size_t>{0, AO}, {2, Wind}, {3, 2},
            {4, 3}, {5, Clouds}, {10, 6}, {10, 7}, {13, HUD}, {16, 5}, {17, SunShadow}})
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

Config Search::FullComposition(Config config) const
{
    for (size_t i = 0; i < config.Levels.size(); ++i)
        if (config.Levels[i] == Off) config.Levels[i] = _high.Levels[i];
    return config;
}

Config Search::WithMediumShadows(Config config)
{
    for (const auto shadow : {SunShadowQuality, LocalShadowQuality})
        if (config.Levels[shadow] == High) config.Levels[shadow] = Medium;
    return config;
}

bool Search::KeepsComposition(const Config& config) const
{
    for (size_t i = 0; i < _available.size(); ++i)
        if (_available[i] && config.Levels[i] == Off) return false;
    return true;
}

void Search::ObserveComposition(const Result& result)
{
    if (std::find(result.Settings.Levels.begin(), result.Settings.Levels.end(), Off) != result.Settings.Levels.end()) return;
    auto& available = _qualityAvailable[bool(result.Settings.Provider)];
    for (size_t i = 0; i < _available.size(); ++i)
    {
        _available[i] |= result.Activity[i];
        available[i] |= result.Activity[i];
    }
}

bool Search::ReadReference(std::istream& file, Result& r)
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

void Search::SaveReferences() const
{
    auto pending = _baselineFile;
    pending += ".tmp";
    std::ofstream output(pending, std::ios::trunc);
    output << std::quoted(_conditions) << '\n' << std::setprecision(17);
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
    const auto* winner = BestReference(false);
    if (const auto* upscaled = BestReference(true); upscaled && (!winner || Better(*upscaled, *winner))) winner = upscaled;
    if (winner)
    {
        auto seed = *winner;
        seed.Settings.Owners = _high.Owners;
        seed.Settings.Sync = _high.Sync;
        seed.Settings = Canonical(std::move(seed.Settings));
        output << "selected ";
        write(seed);
    }
    for (const auto& r : _confirmed)
        if (!r.Remote) { output << "local "; write(r); }
    output.close();
    if (!output || !MoveFileExW(pending.c_str(), _baselineFile.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Cannot save Single GPU reference");
}

bool Search::SameImage(const Result& a, const Result& b)
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

const Result* Search::BestReference(bool upscaled) const
{
    return Best(upscaled, false);
}

const Result* Search::ClosestReference(bool upscaled) const
{
    const Result* best = nullptr;
    for (const auto& r : _confirmed)
        if (!r.Remote && bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] && (!best || r.Fps > best->Fps))
            best = &r;
    return best;
}

const Result* Search::DetailedReference(bool upscaled) const
{
    const Result* best = nullptr;
    for (const auto& r : _confirmed)
        if (!r.Remote && bool(r.Settings.Provider) == upscaled && r.Fps < Threshold[_profile] &&
            (!best || Better(r, *best, true))) best = &r;
    return best;
}

Trial Search::Make(const Config& config, std::string source, bool full)
{
    return {++_nextId, config, std::move(source), full, full};
}

void Search::Schedule(Config config, const std::string& source, bool priority)
{
    config = Canonical(std::move(config));
    if (RemotePhase() && std::all_of(config.Owners.begin(), config.Owners.end(), [](int owner) { return owner < 0; })) return;
    if (!SupportsImage(config) || !_scheduled.insert(config).second) return;
    auto trial = Make(config, source, false);
    if (priority) _priority.push_back(std::move(trial));
    else _pending.push_back(std::move(trial));
}

void Search::ScheduleReflectionVariants(const Config& config, const std::string& source, bool priority)
{
    auto next = config;
    for (const auto quality : {Low, Medium, High, Epic})
    {
        if (config.Levels[SSRQuality] == Off) break;
        next.Levels[SSRQuality] = quality;
        Schedule(next, source, priority);
    }
    for (const auto quality : {Low, Medium, High, Epic})
    {
        if (config.Levels[ProbeQuality] == Off) break;
        next = config;
        next.Levels[ProbeQuality] = quality;
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

void Search::Seed(const Config& config, bool postOnly, bool priority)
{
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

void Search::SeedAdapters(const Result& reference)
{
    // The first round covers every physical peer before a failed placement gets a second try.
    // Different active producers on the same image can immediately supply a measured combination.
    std::vector<size_t> roles;
    if (reference.Activity[AO]) roles.push_back(AO);
    if (reference.Activity[ProbeQuality]) roles.push_back(FirstProbe);
    roles.push_back(Post);
    if (reference.Activity[WaterQuality]) roles.push_back(Water);
    roles.push_back(1); // Light grid
    for (size_t round = 0; round < 2; ++round)
        for (size_t i = 0; i < _adapters.size(); ++i)
        {
            for (size_t offset = 0; offset < roles.size(); ++offset)
            {
                const auto role = roles[(i + round + offset) % roles.size()];
                auto settings = reference.Settings;
                settings.Owners[role] = _adapters[i];
                settings.RemoteAOFilter = role == AO;
                settings = Canonical(std::move(settings));
                if (settings.Owners[role] != _adapters[i] || !SupportsImage(settings)) continue;
                if (std::any_of(_initialPlacements.begin(), _initialPlacements.end(),
                    [&](const auto& probe) { return settings == probe.second; })) continue;
                _initialPlacements.emplace_back(_adapters[i], std::move(settings));
                break;
            }
        }
}

void Search::ScheduleMeasured(Config config, std::string source, double cost, ProposalKind kind)
{
    config = Canonical(std::move(config));
    const auto previous = _short.find(config);
    const bool needsLiveTiming = kind == ProposalKind::CompareLight && previous != _short.end() && previous->second.Reused;
    if (!SupportsImage(config) || (!needsLiveTiming && (_dispatched.count(config) || previous != _short.end())) || _failedProbes.count(config) ||
        std::none_of(config.Owners.begin(), config.Owners.end(), [](int owner) { return owner >= 0; })) return;
    auto [it, inserted] = _proposals.try_emplace(config, MeasuredProposal{cost, kind, source});
    if (!inserted && ((it->second.Kind == ProposalKind::Refinement && kind != ProposalKind::Refinement) ||
        (it->second.Kind == ProposalKind::CompareHeavy && kind == ProposalKind::CompareLight) ||
        (it->second.Kind == kind && cost < it->second.Cost)))
        it->second = {cost, kind, std::move(source)};
    _scheduled.insert(std::move(config));
}

double Search::PlacementRank(const Result& result) const
{
    // Rank follow-up probes using the common windows. Charge primary transfers and exposed waits,
    // and penalize producers that need longer than a target frame even before the consumer can run.
    double primaryMs = 0;
    std::map<BistroRender::Id, double> producerMs;
    for (const auto& [key, cost] : result.CommonCosts)
    {
        if (!cost.ImportedVersions) continue;
        if (cost.ComputeVersions) producerMs[key.second] += cost.ComputeMs / cost.ComputeVersions;
        producerMs[key.second] += cost.TransferMs / cost.ImportedVersions;
        primaryMs += (cost.PrimaryTransferMs + cost.PrimaryWaitMs) / cost.ImportedVersions;
    }
    double longestProducer = 0;
    for (const auto& entry : producerMs) longestProducer = (std::max)(longestProducer, entry.second);
    return 1000. / result.Fps + primaryMs + (std::max)(0., longestProducer - 1000. / Threshold[_profile]);
}

void Search::CombinePlacements(const Result& result, double cost)
{
    if (_adapters.size() < 2) return;
    const auto& settings = result.Settings;
    const auto hasRemoteProbes = [&](const Config& c) {
        return std::any_of(c.Owners.begin() + FirstProbe, c.Owners.begin() + _firstLocalShadow,
            [](int peer) { return peer >= 0; });
    };
    for (const auto& [requested, sample] : _short)
    {
        const auto& other = sample.Settings;
        const bool sameCoverage = (result.CommonPlacementsConsumed && sample.CommonPlacementsConsumed) ||
            (result.CoveredExtraWindows && sample.CoveredExtraWindows);
        if (sample.Id == result.Id || !sample.Remote || !sameCoverage || !(requested == other) ||
            result.Output != sample.Output || result.Render != sample.Render ||
            settings.Levels != other.Levels || settings.Provider != other.Provider || settings.Preset != other.Preset ||
            settings.ProbeFrequency != other.ProbeFrequency || settings.ProbeCapture != other.ProbeCapture ||
            (hasRemoteProbes(settings) && hasRemoteProbes(other) && settings.ProbeDelivery != other.ProbeDelivery)) continue;
        // Both trials must have supplied different work to the frame from disjoint sets of GPUs.
        auto combined = settings;
        bool disjoint = true;
        for (size_t i = 0; i < other.Owners.size(); ++i)
        {
            const int peer = other.Owners[i];
            if (peer < 0) continue;
            if (settings.Owners[i] >= 0 ||
                std::find(settings.Owners.begin(), settings.Owners.end(), peer) != settings.Owners.end())
            {
                disjoint = false;
                break;
            }
            combined.Owners[i] = peer;
            combined.Sync[i] = other.Sync[i];
        }
        if (!disjoint) continue;
        if (hasRemoteProbes(other)) combined.ProbeDelivery = other.ProbeDelivery;
        combined.RemoteAOFilter |= other.RemoteAOFilter;
        ScheduleMeasured(std::move(combined), "combine consumed placements / trials " +
            std::to_string(sample.Id) + " and " + std::to_string(result.Id),
            (std::max)(cost, PlacementRank(sample)), ProposalKind::Mixed);
    }
}

void Search::ObserveCosts(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
{
    if (result.Reused) return;
    const double targetMs = 1000. / Threshold[_profile];
    // These exact modes require a new version for each presented frame.
    for (const auto [i, technique] : {std::pair{AO, size_t{0}}, {Water, WaterQuality}, {Wind, size_t{2}}})
    {
        const int owner = trial.Settings.Owners[i];
        if (owner < 0 || owner != result.Settings.Owners[i] || trial.Settings.Sync[i] != 1) continue;
        const auto found = result.Costs.find({_placements[i].Owner, BistroRender::Id(owner + 1)});
        if (found == result.Costs.end()) continue;
        const auto& cost = found->second;
        if (cost.TimedVersions < 8 || cost.ImportedVersions < 4 || cost.ReadyIntervals < 7 ||
            cost.SlowVersions < .8 * cost.TimedVersions || cost.ReadySpanMs <= cost.ReadyIntervals * targetMs)
            continue;
        csv.Config("Trial." + std::to_string(trial.Id) + ".OverBudgetProducer", _placements[i].Owner);
        if (_adapters.size() < 2 || !result.Valid || !(result.Settings == trial.Settings)) continue;
        // Obtain the matched lighter observations before spending another expensive
        // probe. Keep all other work, owners and publication settings identical.
        for (int level = int(trial.Settings.Levels[technique]) - 1; level >= Low; --level)
        {
            auto light = trial.Settings;
            light.Levels[technique] = Level(level);
            light = Canonical(std::move(light));
            if (light.Levels[technique] >= trial.Settings.Levels[technique] || light.Owners[i] != owner) continue;
            const auto rank = PlacementRank(result);
            for (const auto adapter : _adapters)
            {
                if (std::find(trial.Settings.Owners.begin(), trial.Settings.Owners.end(), adapter) != trial.Settings.Owners.end())
                    continue;
                auto expensive = trial.Settings;
                expensive.Owners[i] = adapter;
                if (_short.count(expensive) || _dispatched.count(expensive)) continue;
                auto lightB = light;
                lightB.Owners[i] = adapter;
                const auto source = "compare lighter producer / trial " + std::to_string(trial.Id);
                ScheduleMeasured(light, source, rank, ProposalKind::CompareLight);
                ScheduleMeasured(std::move(lightB), source, rank, ProposalKind::CompareLight);
                ScheduleMeasured(expensive, "compare expensive producer / trial " + std::to_string(trial.Id),
                    rank, ProposalKind::CompareHeavy);
            }
            break;
        }
    }
}

namespace
{
using TimingKey = ProducerTimings::key_type;

bool ComparableProducerWindows(const Result& highA, const Result& lowA, const Result& lowB,
    const TimingKey& a, const TimingKey& b, double deadline)
{
    if (highA.ProducerWindows.empty() || highA.ProducerWindows.size() != lowA.ProducerWindows.size() ||
        highA.ProducerWindows.size() != lowB.ProducerWindows.size()) return false;
    bool active = false;
    for (const auto& [index, window] : highA.ProducerWindows)
    {
        const auto aw = lowA.ProducerWindows.find(index), bw = lowB.ProducerWindows.find(index);
        if (aw == lowA.ProducerWindows.end() || bw == lowB.ProducerWindows.end()) return false;
        const auto high = window.find(a), first = aw->second.find(a), second = bw->second.find(b);
        if (high == window.end() && first == aw->second.end() && second == bw->second.end()) continue;
        if (high == window.end() || first == aw->second.end() || second == bw->second.end()) return false;
        const auto& h = high->second;
        const auto& x = first->second;
        const auto& y = second->second;
        // Require repeated *consumed* versions in every active common window.
        // The middle 60% of the distributions must be separated beyond the noise margin.
        if (h.Samples < 8 || x.Samples < 8 || y.Samples < 8 ||
            h.TransferPath.empty() || h.TransferPath != x.TransferPath || x.TransferPath != y.TransferPath ||
            h.OtherWork != x.OtherWork || x.OtherWork != y.OtherWork ||
            h.P20Ms <= deadline || h.MedianMs <= x.MedianMs ||
            y.P20Ms <= x.P80Ms + (std::max)(0.1, .05 * x.P80Ms)) return false;
        active = true;
    }
    return active;
}
}

bool Search::SkipEstimated(const Trial& trial, BistroBenchmarkData::Csv& csv)
{
    if (!RemotePhase() || _adapters.size() < 2) return false;
    const auto& settings = trial.Settings;
    const auto deadline = 1000. / Threshold[_profile];
    const auto measured = [&](const Config& config, size_t placement) -> const Result*
    {
        const auto found = _short.find(config);
        if (found == _short.end()) return nullptr;
        const auto& r = found->second;
        if (r.Reused || !r.Valid || r.ProducerWindows.empty() || !(r.Settings == config)) return nullptr;
        if (placement == AO)
        {
            const auto ao = GetAmbientOcclusionSettings(QualityValue(AO, config.Levels[AO]));
            if (r.AOImplementation != ao.Implementation || r.AOResolution != int(ao.Resolution)) return nullptr;
        }
        return &r;
    };
    for (const auto [i, technique] : {std::pair{AO, size_t{0}}, {Water, WaterQuality}, {Wind, size_t{2}}})
    {
        const int target = settings.Owners[i];
        if (target < 0 || settings.Sync[i] != 1 || settings.Levels[technique] <= Low) continue;
        // If B has another assignment, its high-level path and competition on B are unknown.
        if (std::count(settings.Owners.begin(), settings.Owners.end(), target) != 1) continue;
        const TimingKey b{_placements[i].Owner, BistroRender::Id(target + 1)};
        for (const int peer : _adapters)
        {
            if (std::find(settings.Owners.begin(), settings.Owners.end(), peer) != settings.Owners.end()) continue;
            auto high = settings;
            high.Owners[i] = peer;
            const auto* highA = measured(high, i);
            if (!highA) continue;
            const TimingKey a{_placements[i].Owner, BistroRender::Id(peer + 1)};
            for (int level = int(settings.Levels[technique]) - 1; level >= Low; --level)
            {
                auto light = high;
                light.Levels[technique] = Level(level);
                const auto* lowA = measured(light, i);
                light.Owners[i] = target;
                const auto* lowB = measured(light, i);
                if (!lowA || !lowB || lowA->Output != highA->Output || lowB->Output != highA->Output ||
                    lowA->Render != highA->Render || lowB->Render != highA->Render ||
                    (i == AO && (highA->AOImplementation != lowA->AOImplementation ||
                        highA->AOResolution != lowA->AOResolution)) ||
                    !ComparableProducerWindows(*highA, *lowA, *lowB, a, b, deadline)) continue;
                const auto prefix = "Trial." + std::to_string(trial.Id) + ".";
                csv.Config(prefix + "Source", trial.Source);
                csv.Config(prefix + "Profile", ProfileName[_profile]);
                csv.Config(prefix + "EstimatedProducer", _placements[i].Owner);
                csv.Config(prefix + "EstimatedAdapter", std::to_string(target));
                csv.Config(prefix + "EstimatedLevel", std::to_string(settings.Levels[technique]));
                csv.Config(prefix + "RequiredPublicationIntervalMs", BistroBenchmarkData::Csv::Number(deadline));
                const auto write = [&](const char* name, const Result& r, const TimingKey& key)
                {
                    const auto label = prefix + name + ".";
                    csv.Config(label + "TrialCSV", "trial-" + std::to_string(r.Id) + ".csv");
                    for (const auto& [index, window] : r.ProducerWindows)
                        if (const auto it = window.find(key); it != window.end())
                        {
                            const auto field = label + "Window." + std::to_string(index) + ".";
                            csv.Config(field + "ComputeP20Ms", BistroBenchmarkData::Csv::Number(it->second.P20Ms));
                            csv.Config(field + "ComputeMedianMs", BistroBenchmarkData::Csv::Number(it->second.MedianMs));
                            csv.Config(field + "ComputeP80Ms", BistroBenchmarkData::Csv::Number(it->second.P80Ms));
                            csv.Config(field + "ConsumedTimedVersions", std::to_string(it->second.Samples));
                        }
                };
                write("ExpensiveOnA", *highA, a);
                write("LighterOnA", *lowA, a);
                write("LighterOnB", *lowB, b);
                csv.Config(prefix + "Status", u8"пропущен по оценке");
                _estimated.insert(settings);
                return true;
            }
        }
    }
    return false;
}

void Search::RetryChangedEstimates()
{
    // External changes between trials can alter the producer, its transfer path or contention.
    // Previously observed timings no longer establish a bound in the changed environment.
    for (auto& entry : _short) entry.second.ProducerWindows.clear();
    for (const auto& settings : _estimated)
    {
        _dispatched.erase(settings);
        ScheduleMeasured(settings, "recheck estimate after render settings changed", 0, ProposalKind::CompareHeavy);
    }
    _estimated.clear();
}

void Search::ExpandPolicies(const Trial& trial, const Result& result)
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

void Search::Expand(const Trial& trial, const Result& result, BistroBenchmarkData::Csv& csv)
{
    const auto& config = trial.Settings;
    const auto source = std::to_string(trial.Id);
    const auto threshold = Threshold[_profile];
    const bool consumed = result.Remote && result.Settings == config;
    const double rank = consumed ? PlacementRank(result) : 0;
    if (consumed) CombinePlacements(result, rank);
    const auto enqueue = [&](const Config& next, const char* reason, bool priority = false)
    {
        const auto origin = std::string(reason) + " / trial " + source;
        if (consumed && priority) ScheduleMeasured(next, origin, rank, ProposalKind::Refinement);
        else Schedule(next, origin, priority);
    };
    auto next = config;
    if (ShortFps(result) >= threshold * .75)
        ScheduleReflectionVariants(config, "reflection options / trial " + source);
    ExpandPolicies(trial, result);
    if (result.ThresholdFps < threshold)
    {
        const size_t step = Step(config);
        if (step && step < StepCount())
        {
            for (const auto& smaller : AtStep(config, step - 1)) enqueue(smaller, "smaller internal size, same effects", true);
        }
        else
        {
            auto minimum = config;
            for (size_t i = 0; i < minimum.Levels.size(); ++i)
                if (minimum.Levels[i] != Off) minimum.Levels[i] = _minimum.Levels[i];
            minimum.ProbeFrequency = (std::min)(minimum.ProbeFrequency, _minimum.ProbeFrequency);
            minimum = Canonical(std::move(minimum));
            if (!(minimum == config))
                enqueue(minimum, "measure minimum active levels at minimum internal size", true);
            else if (result.Settings == config)
            {
                WriteRemovalEvidence(result, csv);
                for (size_t i = 0; i < config.Levels.size(); ++i)
                    if ((_techniques[i].Enable || _techniques[i].ScaledOff) && result.Activity[i])
                    {
                        next = config;
                        next.Levels[i] = Off;
                        enqueue(next, "effect removal at measured minimum", true);
                    }
            }
        }
    }
    if (ShortFps(result) >= threshold * .96)
    {
        if (Step(config) + 1 < StepCount(config.Owners[Post]))
            for (const auto& higher : AtStep(config, Step(config) + 1))
            {
                enqueue(higher, "larger internal size", consumed);
                if (_profile != Performance || !KeepsComposition(config)) continue;
                const auto mediumShadows = WithMediumShadows(higher);
                if (HighMajority(mediumShadows, result.Activity))
                    enqueue(mediumShadows, "larger internal size with Medium shadows", true);
                for (size_t i = 0; i < std::size(Techniques); ++i)
                    if (config.Levels[i] == High && _values[i][Medium] != _values[i][High])
                    {
                        next = higher;
                        next.Levels[i] = Medium;
                        if (HighMajority(next, result.Activity))
                            enqueue(next, "larger internal size with Medium", _techniques[i].ScaledOff);
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
    if (RemotePhase() && ShortFps(result) >= threshold * .75)
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

bool Search::HasQualityScale(size_t i) const
{
    return _values[i][Medium] != _values[i][High] || _values[i][High] != _values[i][Epic];
}

bool Search::PerformanceScale(size_t i) const
{
    return !_fixedQuality[i] && (HasQualityScale(i) || (_values[i][Low] >= 0 && _values[i][Low] != _values[i][High]));
}

bool Search::HighMajority(const Config& config, const std::array<bool, std::size(Techniques)>& active) const
{
    size_t high = 0, medium = 0;
    for (size_t i = 0; i < active.size(); ++i)
        if (i != LocalShadowQuality && active[i] && PerformanceScale(i))
        {
            high += config.Levels[i] >= High;
            medium += config.Levels[i] == Medium || config.Levels[i] == Low;
        }
    return high > medium;
}

bool Search::QualityComplete(const Result& result) const
{
    const auto& available = _qualityAvailable[bool(result.Settings.Provider)];
    for (size_t i = 0; i < available.size(); ++i)
        if (available[i] && !result.Activity[i]) return false;
    return true;
}

auto Search::QualityImageRank(const Result& result) const
{
    bool allHigh = true;
    unsigned active = 0, high = 0, medium = 0, epic = 0;
    for (size_t i = 0; i < result.Activity.size(); ++i)
    {
        if (!result.Activity[i]) continue;
        ++active;
        if (!PerformanceScale(i)) continue;
        const auto level = result.Settings.Levels[i];
        // Adaptive local shadows are compared using rendered texel coverage, not the ceiling alone.
        if (i == LocalShadowQuality) continue;
        allHigh &= level >= High;
        high += level >= High;
        medium += level >= Medium;
        epic += level == Epic;
    }
    const uint64_t pixels = uint64_t(result.Render.first) * result.Render.second;
    const auto probe = result.Activity[ProbeQuality] ? result.Settings.Levels[ProbeQuality] : Off;
    return std::tuple{QualityComplete(result), active, allHigh, allHigh ? pixels : 0, high, medium, pixels, epic,
        probe, probe == Off ? Frequency::Off : result.Settings.ProbeFrequency};
}

bool Search::QualityOutranks(const Result& full, const Result& sample) const
{
    const auto& a = full.Settings;
    const auto& b = sample.Settings;
    if (a.Provider != b.Provider || a.Preset != b.Preset || full.Activity != sample.Activity) return false;
    if (full.Render != sample.Render && (a.ProbeFrequency != b.ProbeFrequency || a.ProbeCapture != b.ProbeCapture)) return false;
    return QualityImageRank(full) > QualityImageRank(sample);
}

bool Search::Better(const Result& a, const Result& b, bool preferDetail, bool shortWindow) const
{
    const auto threshold = Threshold[_profile];
    const auto fps = [shortWindow](const Result& r) { return shortWindow ? ShortFps(r) : r.Fps; };
    const double fpsA = fps(a), fpsB = fps(b);
    const bool meetsA = fpsA >= threshold, meetsB = fpsB >= threshold;
    if (meetsA != meetsB) return meetsA;
    if (meetsA && (a.Settings.Provider || b.Settings.Provider))
    {
        const auto* native = Best(false, false);
        const auto* remote = Best(false, true);
        if (remote && (!native || Better(*remote, *native))) native = remote;
        if (native)
        {
            const auto preserves = [native](const Result& r)
            {
                if (!r.Settings.Provider) return true;
                for (size_t i = 0; i < r.Activity.size(); ++i)
                    if (native->Activity[i] && (!r.Activity[i] || r.Settings.Levels[i] < native->Settings.Levels[i]))
                        return false;
                return !native->Activity[ProbeQuality] || r.Settings.ProbeFrequency >= native->Settings.ProbeFrequency;
            };
            if (preserves(a) != preserves(b)) return preserves(a);
        }
    }
    if (!meetsA && !preferDetail && fpsA != fpsB) return fpsA > fpsB;
    if (_profile == Quality)
    {
        const auto rank = [&](const Result& result) {
            return std::tuple{QualityImageRank(result), result.ShadowTexels, fps(result), -result.P95};
        };
        return rank(a) > rank(b);
    }
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
                return std::pair{cadence, quality};
            };
            if (rankProbe(a) != rankProbe(b)) return rankProbe(a) > rankProbe(b);
            if (fpsA != fpsB) return fpsA > fpsB;
            if (a.P95 != b.P95) return a.P95 < b.P95;
        }
    }
    const auto count = [this](const Result& r, Level level)
    {
        size_t n = 0;
        for (size_t i = 0; i < r.Activity.size(); ++i)
        {
            if (level > Low && !PerformanceScale(i)) continue;
            n += r.Activity[i] && (i == LocalShadowQuality ? level == Low : r.Settings.Levels[i] >= level);
        }
        return n;
    };
    const auto activeA = count(a, Low), activeB = count(b, Low);
    if (activeA != activeB) return activeA > activeB;
    const auto rank = [&](const Result& r)
    {
        const auto high = count(r, High);
        const auto epic = count(r, Epic);
        const bool resolutionFirst = _profile == Performance && KeepsComposition(r.Settings) &&
            HighMajority(r.Settings, r.Activity);
        const auto pixels = uint64_t(r.Render.first) * r.Render.second;
        return std::tuple{resolutionFirst, resolutionFirst ? pixels : 0, high, count(r, Medium),
            pixels, epic, r.ShadowTexels, fps(r), -r.P95};
    };
    return rank(a) > rank(b);
}

bool Search::Dominates(const Result& a, const Result& b)
{
    if (a.Settings.Provider != b.Settings.Provider || a.Settings.Preset != b.Settings.Preset ||
        ShortFps(a) < ShortFps(b) || a.Output.first < b.Output.first || a.Output.second < b.Output.second ||
        a.Render.first < b.Render.first || a.Render.second < b.Render.second ||
        a.Settings.ProbeFrequency < b.Settings.ProbeFrequency ||
        ((a.Activity[LocalShadowQuality] || b.Activity[LocalShadowQuality]) &&
         (!a.ShadowTexels || !b.ShadowTexels || a.ShadowTexels < b.ShadowTexels)))
        return false;
    // Keep layouts whose short-window results are indistinguishable within measurement noise.
    if (ShortFps(a) <= ShortFps(b) * 1.04) return false;
    for (size_t i = 0; i < a.Activity.size(); ++i)
    {
        if (i == LocalShadowQuality) continue;
        const auto left = a.Activity[i] ? a.Settings.Levels[i] : Off;
        const auto right = b.Activity[i] ? b.Settings.Levels[i] : Off;
        if (left < right) return false;
    }
    return true;
}

const Result* Search::Best(bool upscaled, bool remote) const
{
    const Result* best = nullptr;
    for (const auto& result : _confirmed)
        if (bool(result.Settings.Provider) == upscaled && result.Remote == remote &&
            (!best || Better(result, *best)))
            best = &result;
    return best;
}

const Result* Search::Winner() const
{
    const Result* best = nullptr;
    for (const auto& result : _confirmed)
        if (!best || Better(result, *best)) best = &result;
    return best;
}

void Search::ReportResult(BistroBenchmarkData::Csv& csv, const std::string& prefix, const Result* r) const
{
    csv.Config(prefix + "Status", !r ? "no confirmed result" : r->Fps < Threshold[_profile] ? "reserve below target" :
        _profile != Quality ? "confirmed" : r->Fps < 30 ? "confirmed within tolerance" : "confirmed at or above target");
    if (!r) return;
    csv.Config(prefix + "Trial", std::to_string(r->Id));
    const auto trialPath = _baselineFile.stem() / ("trial-" + std::to_string(r->Id) + ".csv");
    std::error_code error;
    if (std::filesystem::is_regular_file(_baselineFile.parent_path() / trialPath, error))
        csv.Config(prefix + "TrialCSV", trialPath.generic_u8string());
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
        if (r->Activity[i]) csv.Config(prefix + "Technique", std::string(_techniques[i].Pass) + "=" +
            (_techniques[i].Quality ? std::to_string(QualityValue(i, r->Settings.Levels[i])) : "enabled"));
    csv.Config(prefix + "Output", std::to_string(r->Output.first) + "x" + std::to_string(r->Output.second));
    csv.Config(prefix + "Render", std::to_string(r->Render.first) + "x" + std::to_string(r->Render.second));
    csv.Config(prefix + "ScaleX", BistroBenchmarkData::Csv::Number(double(r->Render.first) / r->Output.first));
    csv.Config(prefix + "ScaleY", BistroBenchmarkData::Csv::Number(double(r->Render.second) / r->Output.second));
    csv.Config(prefix + "FPS", BistroBenchmarkData::Csv::Number(r->Fps));
    csv.Config(prefix + "P95ms", BistroBenchmarkData::Csv::Number(r->P95));
}
} // namespace BistroPlacement
