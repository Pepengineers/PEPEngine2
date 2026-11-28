#include "BistroBenchmark.h"
#include "BistroCharacters.h"
#include "Components/FluidRenderComponent.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"
#include <iomanip>
#include <numeric>
#include <sstream>

void BistroBenchmark::StartPlacement(World& world, ConsoleModule& console, const RenderModule& render,
    BistroRenderPipeline& pipeline, const std::vector<int>& adapters, bool manual)
{
    _manualPlacement = manual;
    _manualGpuTimings = manual && !pipeline.BenchmarkSubmissions().BenchmarkAdapterIndices().empty();
    _placementPhase = 0;
    _calibrating = !manual;
    _placementPhysics = {};
    _placementSearch = BistroPlacement::Search(_placementProfile, _launch.Level->Techniques);
    _placementSetupReserve = 2;
    _trial = {};
    _placementWaitsOpen = false;
    if (!manual)
    {
        _csv.Config("Profile", BistroPlacement::ProfileName[_placementProfile]);
        _csv.Config("MinimumFps", Csv::Number(BistroPlacement::Threshold[_placementProfile]));
    }
    _initialScene = CaptureScene(world, pipeline);
    if (!manual)
    {
        ConsoleVariablePatch patch{{L"Render.FrameGeneration.Provider", "0"}, {L"Render.Sky.SunCycle.Enabled", "false"}};
        if (world.GetECS().GetPool<RigidBodyComponent>().Size() != _startupBodies)
            throw std::runtime_error("Cannot replay the Bistro startup scene after changing its physics bodies");
        std::set<std::wstring> shadowNames;
        world.GetECS().ForEach<NameComponent, LightComponent>([&](Entity, const NameComponent& name, const LightComponent& light)
        {
            if (!light.Enabled || !light.CastShadows || light.Type == LightType::Directional) return;
            const auto decoded = std::filesystem::u8path(name.value).wstring();
            shadowNames.insert(decoded);
        });
        bool fixedWaterSubsteps = true;
        world.GetECS().ForEach<FluidRenderComponent>([&](Entity, const FluidRenderComponent& fluid)
        {
            fixedWaterSubsteps &= fluid.GetSettings().MaxSubsteps != 0;
        });
        auto& submissions = pipeline.BenchmarkSubmissions();
        std::map<int, int> preferredProviders;
        const auto vendorProvider = [&](BistroRender::Id id)
        {
            using Provider = GDX12UpscalerProvider;
            switch (submissions.AdapterVendor(id))
            {
            case 0x10de: return int(Provider::DLSS);
            case 0x1002: return int(Provider::FSR);
            case 0x8086: return int(Provider::XeSS);
            default: return int(Provider::Native);
            }
        };
        if (_launch.AllowUpscalers)
        {
            preferredProviders.emplace(-1, vendorProvider(0));
            for (int index : adapters) preferredProviders.emplace(index, vendorProvider(index + 1));
        }
        _placementSearch.Start(console, _launch.Output, adapters, _multiGPU, fixedWaterSubsteps,
            render.UsesSkinningPoseRatePreset(), shadowNames, _csv, _launch.AllowUpscalers, std::move(preferredProviders));
        const auto local = LocalAdapters(console);
        patch.insert(patch.end(), local.begin(), local.end());
        const auto controlled = _placementSearch.ControlledSettings();
        wchar_t executable[32768]{};
        const auto exeLength = GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
        if (!exeLength || exeLength == std::size(executable))
            throw std::runtime_error("Cannot identify benchmark executable");
        std::ostringstream conditions;
        conditions << "fixed-output-named-reconstruction-v2 measurement-" << BistroBenchmarkData::MeasurementVersion << ' ' <<
            std::quoted(_launch.Level->Id) << ' ' << std::quoted(_launch.Level->Route) << ' ' <<
            _launch.Output.first << 'x' << _launch.Output.second << ' ' << _launch.AllowUpscalers << ' ' <<
            std::setprecision(17) << Duration() << ' ' << _paintingHold << ' ' << _warmup << ' ' <<
            std::filesystem::last_write_time(executable).time_since_epoch().count() << ' ' <<
            world.GetTimeScale() << ' ' << _characters->Seed() << ' ' << _startupLayout << ' ' <<
            fixedWaterSubsteps << ' ' << render.UsesSkinningPoseRatePreset();
        const auto identifyAdapter = [&](BistroRender::Id id)
        {
            const auto luid = submissions.AdapterLuid(id);
            conditions << ' ' << std::quoted(submissions.AdapterLabel(id)) << ' ' << luid.HighPart << ' ' << luid.LowPart;
        };
        identifyAdapter(0);
        for (const int index : adapters) identifyAdapter(index + 1);
        for (const auto& name : shadowNames) conditions << ' ' << std::quoted(std::filesystem::path(name).u8string());
        for (size_t i = 0; i < _placementPhysics.size(); ++i)
        {
            const auto window = PlacementWindow(i);
            conditions << ' ' << window.Begin << ' ' << window.End;
        }
        for (size_t technique = 0; technique < std::size(BistroPlacement::Techniques); ++technique)
            for (auto level : {BistroPlacement::Off, BistroPlacement::Low, BistroPlacement::Medium, BistroPlacement::High, BistroPlacement::Epic})
                conditions << ' ' << _placementSearch.QualityValue(technique, level);
        auto renderConditions = RenderConfig(console);
        std::sort(renderConditions.begin(), renderConditions.end());
        _measurementDefaults.clear();
        _measurementDefaults.insert(renderConditions.begin(), renderConditions.end());
        for (const auto& [name, value] : patch) _measurementDefaults[name] = value;
        for (const auto& [name, value] : controlled) _measurementDefaults.erase(name);
        const auto controls = [](const auto& entries, const auto& name) {
            return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == name; });
        };
        for (const auto& [name, value] : renderConditions)
            if (name.compare(0, 14, L"App.Benchmark.") != 0 && !controls(patch, name) &&
                (!controls(controlled, name) || name == L"Render.Wind.Algorithm"))
                conditions << '\n' << std::quoted(std::filesystem::path(name).u8string()) << ' ' << std::quoted(value);
        OpenPlacementMeasurements(conditions.str());
        _placementSearch.LinkBaseline(_path, conditions.str());
        _trial = _placementSearch.Calibration();
        for (const auto& [name, value] : _placementSearch.Patch(_trial.Settings))
        {
            const auto existing = std::find_if(patch.begin(), patch.end(), [&](const auto& entry) { return entry.first == name; });
            if (existing == patch.end()) patch.emplace_back(name, value);
            else existing->second = value;
        }
        const double singleBudget = PlacementBudget(0);
        const double budget = PlacementBudget(_multiGPU ? adapters.size() : 0);
        _placementDeadline = _started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(budget - FinishReserve));
        _singleDeadline = _started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(singleBudget - FinishReserve));
        _basePlacementBudget = budget;
        _phaseBaseBudget = singleBudget;
        _phaseDeadline = _singleDeadline;
        _addedPlacementBudget = {};
        _comparisonEstimate = _activeComparisonEstimate = 0;
        _plannedComparisonPasses = 0;
        WritePlacementBudget();
        _csv.Config("SingleGPUBudgetSeconds", Csv::Number(singleBudget));
        _csv.Config("PlacementSeed", "20261122");
        _csv.Config("SkinningOwnerRestriction", "Remote skinning selection is startup-only; search uses scene GPU");
        _csv.Config("SceneOwnerRestriction", "The scene adapter is fixed at application startup");
        if (!_trial.Settings.Provider && Seconds(Clock::now(), _singleDeadline) < _warmup + Duration())
        {
            Finish(world, console, pipeline, "full route does not fit in budget", {}, 0, Outcome::Failed);
            return;
        }
        if (const auto result = console.ApplyPatch(patch, &_previous); !result.Error.empty())
            throw std::runtime_error(result.Error);
    }
    else _trial = {1, {}, "current settings", true};
    BeginPlacementTrial(world, console, pipeline, _trial, true);
}

void BistroBenchmark::BeginPlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline,
    BistroPlacement::Trial trial, bool first)
{
    _trial = std::move(trial);
    _trialStarted = Clock::now();
    if (first && !_manualPlacement && _trial.Settings.Provider)
    {
        _activeComparisonEstimate = PlacementFullCost();
        ExtendPlacementBudget(_activeComparisonEstimate);
    }
    else if (_trial.Comparison)
    {
        _activeComparisonEstimate = _comparisonEstimate;
        --_plannedComparisonPasses;
    }
    _trialPostDevice = 0;
    _trialResult = {};
    _trialResult.Id = _trial.Id;
    _trialResult.Settings = _trial.Settings;
    _trialResult.ConfirmationSettings = _trial.Settings;
    _trialResult.Valid = true;
    _placementWindowIndex = 0;
    _trialMeasurements = {};
    _scheduleEvidence.clear();
    if (_calibrating) _placementSeenOnRoute = {};
    _trialWindowCount = _windows.size();
    _trialConfig.clear();
    _trialFallback.clear();
    _trialReconstructionFailed = false;
    _preparationSeconds.reset();
    _trialReason.clear();
    _trialLastPass.clear();
    _trialCompletedPasses = 0;
    _trialPending = true;
    _trialPrefix = "Trial." + std::to_string(_trial.Id) + ".";
    _csv.Config(_trialPrefix + "Source", _trial.Source);
    _csv.Config(_trialPrefix + "Scope", _trial.InspectImage ? "image mode discovery" : _trial.Full ? "full route" : "common windows");
    _csv.Config(_trialPrefix + "PacketTimingRequested",
        ManualGpuTimings() || (!_trial.Full && _placementSearch.RemotePhase()) ? "true" : "false");
    const auto shadowCeiling = _manualPlacement ? console.FindConsoleVariable(L"Render.Shadows.Local.Resolution").GetInt() :
        _placementSearch.QualityValue(BistroPlacement::LocalShadowQuality, _trial.Settings.Levels[BistroPlacement::LocalShadowQuality]);
    _csv.Config(_trialPrefix + "RequestedLocalShadowCeiling", std::to_string(shadowCeiling));
    if (!_manualPlacement)
    {
        if (_trial.Comparison)
        {
            _csv.Config(_trialPrefix + "ReconstructionComparison", std::to_string(_trial.Comparison));
            _csv.Config(_trialPrefix + "ComparisonPass", std::to_string(_trial.Pass));
        }
        const auto owner = _trial.Settings.Owners[BistroPlacement::Post];
        _csv.Config(_trialPrefix + "RequestedPostDevice", pipeline.BenchmarkSubmissions().AdapterLabel(owner < 0 ? 0 : owner + 1));
        if (_activeComparisonEstimate > 0)
            _csv.Config(_trialPrefix + "AddedRouteEstimateSeconds", Csv::Number(_activeComparisonEstimate));
        if (!first) Apply(console, _placementSearch.Patch(_trial.Settings));
        if (_trial.InspectImage)
        {
            // Render one frame with this provider. Its SDK chooses the internal
            // grid; no route sample, physics reset or cache entry is produced.
            _legOpen = _legDone = _frameConfigPending = false;
            _csv.Config(_trialPrefix + "Render.Upscaler.Provider", std::to_string(_trial.Settings.Provider));
            _csv.Config(_trialPrefix + "Render.Upscaler.Quality", std::to_string(_trial.Settings.Preset));
            return;
        }
        pipeline.ResetBenchmarkProbeCaptures();
        const auto [width, height] = _placementSearch.Output();
        if (_placementSearch.LowFirst() || _placementSearch.HighFirst())
            Log::Info("BistroBenchmark", "SingleGPU ", BistroPlacement::ProfileName[_placementProfile], " trial ", _trial.Id, "; ", _trial.Source,
                "; requested output=", width, "x", height, "; provider=", _trial.Settings.Provider,
                "; preset=", _trial.Settings.Preset);
    }
    _trialRequested = RenderConfig(console);
    for (const auto& [name, value] : _trialRequested)
        _csv.Config(_trialPrefix + std::filesystem::path(name).u8string(), value);
    if (_manualPlacement)
        for (const auto& [name, value] : _launch.Settings)
            if (name.rfind(L"Render.", 0) != 0 && name.rfind(L"App.", 0) != 0)
                _csv.Config(_trialPrefix + std::filesystem::path(name).u8string(), console.FindConsoleVariable(name).GetString());
    BeginPlacementLeg(world, pipeline);
}

void BistroBenchmark::CapturePlacementWindows(World& world, BistroRenderPipeline& pipeline)
{
    for (size_t i = 0; i < _placementPhysics.size(); ++i)
    {
        const double begin = PlacementWindow(i).Begin;
        const double snapshot = std::max(0., begin - _warmup - 1.);
        if (_placementPhysics[i] || _routeTime < snapshot) continue;
        _placementPhysics[i] = snapshot == 0 ? _replayScene : CaptureScene(world, pipeline);
    }
}

BistroBenchmark::Window BistroBenchmark::PlacementWindow(size_t index) const
{
    return index < _windows.size() ? _windows[index] : _extraWindows[index - _windows.size()];
}

void BistroBenchmark::BeginPlacementLeg(World& world, BistroRenderPipeline& pipeline)
{
    _placementLegStarted = Clock::now();
    _measurementKey.clear();
    _reusedPass.clear();
    _legMeasurements = {};
    _scheduleEvidence.clear();
    BeginLeg(world, pipeline, _trial.Full ? Window{0, Duration(), 0} : PlacementWindow(_placementWindowIndex));
}

BistroBenchmark::ScheduleEvidence& BistroBenchmark::ObserveSchedule(const BistroRenderPipeline& pipeline)
{
    const auto slot = pipeline.BenchmarkSlot();
    if (_scheduleEvidence.size() <= slot) _scheduleEvidence.resize(slot + 1);
    auto& cached = _scheduleEvidence[slot];
    const auto& plan = pipeline.BenchmarkSchedule();
    if (cached.Plan == plan) return cached;
    cached = {};
    cached.Plan = plan;
    auto& activity = cached.Activity;
    const auto match = [&](std::string_view name)
    {
        const auto starts = [name](std::string_view prefix) { return name.substr(0, prefix.size()) == prefix; };
        auto& roles = cached.Roles;
        roles.AO |= starts("AO resolve") || starts("SSAO/HBAO visibility") || starts("CACAO dispatch") ||
            starts("XeGTAO GPU0 import terms") || starts("XeGTAO denoise");
        roles.LightGrid |= starts("GPU light grid masks") || starts("GPU0 import Exact light grid masks");
        roles.Particles |= starts("particle simulation") || starts("opaque particles G-buffer") || starts("transparent particles cull");
        roles.Water |= starts("water seed") || starts("water draw");
        roles.Wind |= starts("grass wind stage") || starts("linden wind stage");
        activity[0] |= name == "AO resolve" || name == "SSAO/HBAO visibility" || starts("CACAO dispatch") ||
            name == "XeGTAO GPU0 import terms" || name == "XeGTAO denoise";
        activity[1] |= name == "grass opaque depth HiZ" || starts("grass G-buffer");
        activity[2] |= starts("grass wind stage ") || starts("linden wind stage ") || name == "GPU0 import ProjectedGrid publication";
        activity[3] |= name == "opaque particles G-buffer (read-only depth)" ||
            name == "transparent particles cull and global sort" || name == "GPU0 import particle snapshot";
        activity[4] |= name == "water draw unlit opaque particles" || name == "GPU0 import water display";
        activity[7] |= name == "SSR specular composition";
        activity[10] |= name == "GPU0 import visible SkyView" || name == "GPU0 import aerial pair" ||
            starts("camera environment / atmosphere/source kernel");
        activity[12] |= name == "skin deformation";
        activity[13] |= name == "Game HUD raster" || name == "GPU0 import game HUD layer";
        activity[14] |= name == "world-space canvases over G-buffer";
        activity[15] |= starts("linden vertex fetch view ");
        activity[16] |= name == "cloud picture generation" || name == "cloud picture GPU0 import";
        activity[17] |= starts("sun atlas ") || name == "publish dirty remote CSM tiles on GPU0";
        activity[18] |= name == "local shadow CPU lists ExecuteIndirect" || name == "publish local shadow tiles";
    };
    for (const auto& packet : plan->Packets)
    {
        if (_stage == Stage::Placement)
        {
            _legMeasurements.Passes.insert(packet.Name);
            match(packet.Name);
        }
        else if (const int group = QueueGroup(packet.Name); group >= 0)
            cached.Groups |= 1u << group;
    }
    return cached;
}

void BistroBenchmark::ObservePlacement(BistroRenderPipeline& pipeline)
{
    auto& measurements = _legMeasurements;
    using BistroRender::Invalid;
    using BistroRender::Mode;
    auto& schedule = ObserveSchedule(pipeline);
    const auto& activity = schedule.Activity;
    const auto& packets = schedule.Plan->Packets;
    const auto observe = [&](std::string_view role, BistroRender::Id owner, std::string_view consumer)
    {
        if (owner && owner != Invalid) measurements.Publications.emplace(role, owner, consumer);
    };
    pipeline.BenchmarkPlacement(schedule.Roles, _framePlacement);
    auto& owners = _framePlacement.ActiveOwners;
    if (owners != measurements.PreviousOwners)
    {
        measurements.HasPublishedProbes = false;
        for (const auto& [role, owner] : owners)
        {
            const bool probe = role.compare(0, 6, "Probe ") == 0;
            measurements.HasPublishedProbes |= probe;
            if (!measurements.ActiveOwners[role].insert(owner).second) continue;
            if (role == "PostAdapter" || role == "GameHUDAdapter" || role == "TransparencyAdapter" || role == "SkyViewAdapter" ||
                role == "AerialPerspectiveAdapter" || role == "SunShadowAdapter" || role.compare(0, 18, "Local shadow page ") == 0 || probe)
                measurements.UsefulOwners[role].insert(owner);
            if (probe) observe(role, owner, "clustered deferred lighting");
        }
        measurements.PreviousOwners.swap(owners);
    }
    for (const auto& link : pipeline.BenchmarkLinks())
    {
        if (link.Import >= packets.size() || !schedule.Links.insert({link.Import, link.Owner}).second) continue;
        const auto& source = packets[link.Import];
        if (source.Name == "GPU0 import texture inspector atlas" || source.Name == "GPU0 publish remote reflection probe faces")
            continue;
        for (size_t i = link.Import + 1; i < packets.size(); ++i)
        {
            const auto& consumer = packets[i];
            const bool reads = std::any_of(consumer.Accesses.begin(), consumer.Accesses.end(), [&](const auto& read)
            {
                return read.Usage != Mode::Write && std::any_of(source.Accesses.begin(), source.Accesses.end(),
                    [&](const auto& write) { return write.Usage != Mode::Read && read.Resource == write.Resource; });
            });
            if (!reads) continue;
            observe(source.Name, link.Owner, consumer.Name);
            const auto role = BistroPlacement::PublicationRole(source.Name);
            if (!role.empty()) measurements.UsefulOwners[std::string(role)].insert(link.Owner);
            break;
        }
    }
    const auto [post, hud] = _framePlacement.PostPublication;
    if (measurements.PostPublication != std::make_pair(post, hud))
    {
        measurements.PostPublication = {post, hud};
        observe("PostAdapter", post, "presented post stack");
        observe("GameHUDAdapter", hud, "presented HUD composite");
    }
    for (const auto& snapshot : pipeline.BenchmarkSimulations())
    {
        if (!snapshot.Version) continue;
        const auto key = std::make_tuple(std::string(snapshot.Role), snapshot.Owner, snapshot.Lane);
        auto [it, first] = measurements.Simulations.try_emplace(key);
        if (first) measurements.UsefulOwners[std::string(snapshot.Role)].insert(snapshot.Owner);
        auto& stats = it->second;
        ++stats.Frames;
        if (stats.Generation != snapshot.Generation || stats.LastVersion != snapshot.Version)
            ++stats.UsedVersions;
        stats.Generation = snapshot.Generation;
        stats.LastVersion = snapshot.Version;
        stats.SimulatedSeconds = snapshot.SimulatedSeconds;
    }

    const auto [dof, blur] = pipeline.BenchmarkPostEffects();
    const auto shadowTexels = _framePlacement.ShadowTexels;
    const auto section = Section(_routeTime);
    measurements.SectionShadowTexels[section] += shadowTexels;
    ++measurements.SectionShadowSamples[section];
    const auto skySource = pipeline.BenchmarkSkySource();
    auto effectiveActivity = activity;
    effectiveActivity[5] = pipeline.BenchmarkCloudsActive();
    effectiveActivity[6] = measurements.HasPublishedProbes;
    effectiveActivity[8] = dof;
    effectiveActivity[9] = blur;
    effectiveActivity[10] |= skySource == SkySource::SkyAtmosphere;
    effectiveActivity[11] = skySource == SkySource::SkyBox && !effectiveActivity[5];
    effectiveActivity[13] |= hud != Invalid;
    effectiveActivity[BistroPlacement::LocalShadowQuality] = shadowTexels != 0;
    for (size_t i = 0; i < activity.size(); ++i)
    {
        measurements.Activity[i] |= effectiveActivity[i] && (_manualPlacement || _trial.Settings.Levels[i] != BistroPlacement::Off);
        if (_calibrating) _placementSeenOnRoute[i] |= measurements.Activity[i];
    }
}

void BistroBenchmark::ObservePlacementImage(RenderModule& render, BistroRenderPipeline& pipeline)
{
    _trialConfig = EffectiveConfig(render);
    const auto& common = *render.GetRenderPipelineCommonData();
    _trialResult.Output = {common.WindowWidth, common.WindowHeight};
    _trialResult.Render = {common.DownscaledWidth, common.DownscaledHeight};
    const auto& status = render.GetReconstructionStatus();
    _trialProvider = status.Effective;
    _trialFrameGeneration = render.GetFrameGenerationStatus().Effective;
    if (!_trialReconstructionFailed)
    {
        const auto& postFallback = pipeline.BenchmarkPostFallback();
        _trialFallback = postFallback.empty() ? status.FallbackReason : postFallback;
    }
    _trialPostDevice = pipeline.BenchmarkPostDevice();
    if (_trialResult.Output != _launch.Output)
        _trialReason = "requested output size unavailable";
    else if (!_manualPlacement)
    {
        const auto owner = _trial.Settings.Owners[BistroPlacement::Post];
        const auto requestedPost = owner < 0 ? 0 : BistroRender::Id(owner + 1);
        if (_trialPostDevice != requestedPost)
        {
            _trialReason = "requested post adapter unavailable";
            if (_trialFallback.empty()) _trialFallback = "post processing executed on another adapter";
        }
        else if (int(status.Requested.Provider) != _trial.Settings.Provider ||
            int(status.Requested.Quality) != _trial.Settings.Preset ||
            int(status.Effective) != _trial.Settings.Provider ||
            (status.Effective == GDX12UpscalerProvider::Native ? _trialResult.Render != _launch.Output :
                !_trialResult.Render.first || !_trialResult.Render.second ||
                _trialResult.Render.first >= _launch.Output.first || _trialResult.Render.second >= _launch.Output.second))
            _trialReason = "requested image mode unavailable";
        else if (!_trial.InspectImage && pipeline.CloudPictureSizeUnavailable())
            _trialReason = "requested cloud picture texture unavailable";
        else if (_trialFrameGeneration != GDX12FrameGenerationProvider::Off)
            _trialReason = "frame generation is active";
    }
    CheckPlacementReconstruction(pipeline, _trialProvider);
}

void BistroBenchmark::CheckPlacementReconstruction(const BistroRenderPipeline& pipeline, GDX12UpscalerProvider provider)
{
    if (_trialReconstructionFailed || provider == GDX12UpscalerProvider::Native || pipeline.BenchmarkReconstructionRecorded()) return;
    _trialReconstructionFailed = true;
    const auto& failure = pipeline.BenchmarkReconstructionFailure();
    if (!_trialFallback.empty() && !failure.empty() && _trialFallback != failure)
        _trialFallback += " / " + failure;
    else if (!failure.empty())
        _trialFallback = failure;
    if (_trialReason.empty())
        _trialReason = failure.empty() ? "no successful reconstruction frame" : "reconstruction failed";
}

bool BistroBenchmark::RecordPlacementFrame(RenderModule& render, BistroRenderPipeline& pipeline, Clock::time_point now)
{
    if (_legDone && !_calibrating) return false;
    if (!_legConfigured)
    {
        _legConfigured = true;
        if (_trialConfig.empty())
        {
            ObservePlacementImage(render, pipeline);
            _frameConfigPending = true;
        }
        else if (_trialReason.empty() && _trialConfig != EffectiveConfig(render))
            _trialReason = "effective display or reconstruction changed";
        if (!_trialReason.empty() && !_calibrating)
        {
            _elapsed = Seconds(_legStarted, now);
            _legDone = true;
            return false;
        }
    }
    const auto& common = *render.GetRenderPipelineCommonData();
    const auto provider = render.GetReconstructionStatus().Effective;
    if (provider != _trialProvider ||
        render.GetFrameGenerationStatus().Effective != _trialFrameGeneration ||
        _trialResult.Output != BistroPlacement::Size{common.WindowWidth, common.WindowHeight} ||
        _trialResult.Render != BistroPlacement::Size{common.DownscaledWidth, common.DownscaledHeight})
        _trialReason = "effective display, reconstruction or frame generation changed";
    if (pipeline.BenchmarkPostDevice() != _trialPostDevice)
        _trialReason = "post adapter changed during measurement";
    CheckPlacementReconstruction(pipeline, provider);
    if (!_trialReason.empty() && !_calibrating)
    {
        _legDone = true;
        return false;
    }
    ObservePlacement(pipeline);
    if (!_manualPlacement)
    {
        if (!_trial.Full && _frames.empty() && _trialReason.empty())
        {
            _measurementKey = PlacementMeasurementKey(pipeline);
            if (!_calibrating && !_trial.RequireLive && RestorePlacementWindow())
            {
                _trialResult.Reused = true;
                _legDone = true;
                return false;
            }
        }
        if (_placementSearch.RemotePhase() && _placementWindowIndex == 0 && _frames.empty())
            _placementSetupReserve = std::max(_placementSetupReserve, Seconds(_trialStarted, now) - _warmup);
        if (_placementSearch.HighFirst() && _placementWindowIndex == 0 && _frames.empty())
            _preparationSeconds = Seconds(_trialStarted, now);
        _legMeasurements.Freshness.Observe(pipeline.BenchmarkProbeFrame(), now);
        const auto bytes = pipeline.BenchmarkTransferBytes();
        for (size_t i = 0; i < bytes.size(); ++i) _legMeasurements.TransferBytes[i] += bytes[i];
    }
    _frames.push_back({_routeTime, Seconds(_frameStart, now) * 1000,
        Seconds(*_lastPresent, now) * 1000, Section(_routeTime)});
    return true;
}

void BistroBenchmark::CompletePlacementLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
{
    if (_trial.InspectImage)
    {
        _placementSearch.RecordImageMode(_trial, _trialReason.empty());
        const auto owner = _trial.Settings.Owners[BistroPlacement::Post];
        const auto requested = owner < 0 ? 0 : BistroRender::Id(owner + 1);
        const auto& submissions = pipeline.BenchmarkSubmissions();
        _csv.Config(_trialPrefix + "EffectivePostDevice", submissions.AdapterLabel(_trialPostDevice));
        _csv.Config(_trialPrefix + "SdkAcceptedOnRequestedDevice", _trialPostDevice == requested &&
            int(_trialProvider) == _trial.Settings.Provider ? "true" : "false");
        _csv.Config(_trialPrefix + "ReconstructionFrameVerified", _trialReason.empty() ? "true" : "false");
        for (const auto& [name, value] : _trialConfig) _csv.Config(_trialPrefix + name, value);
        if (_trialReconstructionFailed) _csv.Config(_trialPrefix + "ReconstructionApplied", "Display transform fallback");
        if (!_trialFallback.empty()) _csv.Config(_trialPrefix + "ReconstructionFallback", _trialFallback);
        _csv.Config(_trialPrefix + "Status", _trialReason.empty() ? "available" : _trialReason);
        _trialPending = false;
        NextPlacementTrial(world, console, pipeline);
        return;
    }
    auto& measurements = _trialMeasurements;
    const bool reachedEnd = _elapsed >= LegWarmup() + _window.End - _preroll;
    const bool covered = BistroBenchmarkData::CoversWindow(_frames, _window.Begin, _window.End);
    if (_trialReason.empty() && RenderConfig(console) != _trialRequested)
        _trialReason = "render settings changed during measurement";
    if (_trialReason.empty() && _reusedPass.empty() && (!reachedEnd || !covered))
        _trialReason = "incomplete route coverage";
    WritePlacementWaits(pipeline);
    if (_reusedPass.empty())
    {
        WriteLeg(_trialReason.empty() ? "completed" : _trialReason.c_str());
        for (const auto& frame : _frames)
        {
            ++_legMeasurements.Frames;
            _legMeasurements.Seconds += frame.IntervalMs / 1000;
            _legMeasurements.PrimaryIntervals.push_back(frame.IntervalMs);
        }
        if (_trial.Full && !_legMeasurements.PrimaryIntervals.empty())
        {
            auto& intervals = _legMeasurements.PrimaryIntervals;
            const auto rank = intervals.begin() + (intervals.size() * 95 - 1) / 100;
            std::nth_element(intervals.begin(), rank, intervals.end());
            _legMeasurements.FullP95 = *rank;
            intervals.clear();
        }
        if (_trialReason.empty() && !_manualPlacement && !_trial.Full) SavePlacementWindow();
    }
    else
    {
        _legOpen = _frameConfigPending = false;
        _trialLastPass = _reusedPass;
        for (const auto& [name, value] : _trialConfig) _csv.Config(_trialPrefix + name, value);
        _csv.Config(_trialPrefix + "Window." + std::to_string(_placementWindowIndex) + ".ReusedPassCSV", _reusedPass);
    }
    if (_trialReason.empty()) ++_trialCompletedPasses;
    AccumulatePlacementWindow(_trial.Full || _placementWindowIndex < _windows.size());
    if (_trialReason.empty() && !_trial.Full && ++_placementWindowIndex == _windows.size() && measurements.Seconds > 0)
    {
        if (_placementSearch.RemotePhase())
        {
            _trialResult.CommonCosts = _trialResult.Costs;
            const auto& roles = _placementSearch.Candidates();
            _trialResult.CommonPlacementsConsumed = true;
            for (size_t i = 0; i < roles.size(); ++i)
            {
                const int peer = _trial.Settings.Owners[i];
                if (peer < 0) continue;
                const auto active = measurements.ActiveOwners.find(roles[i].Owner);
                const auto useful = measurements.UsefulOwners.find(roles[i].Owner);
                const auto device = BistroRender::Id(peer + 1);
                if (active == measurements.ActiveOwners.end() || !active->second.count(device) ||
                    useful == measurements.UsefulOwners.end() || !useful->second.count(device))
                {
                    _trialResult.CommonPlacementsConsumed = false;
                    break;
                }
            }
        }
        const double fps = measurements.Frames / measurements.Seconds;
        const auto threshold = BistroPlacement::Threshold[_placementProfile];
        bool missing = _placementSearch.RemotePhase() && !_trialResult.CommonPlacementsConsumed;
        for (size_t i = 0; i < _placementSeenOnRoute.size(); ++i)
            missing |= _placementSeenOnRoute[i] && _trial.Settings.Levels[i] != BistroPlacement::Off &&
                !_trialResult.Activity[i];
        if (missing || std::abs(fps - threshold) <= threshold * .04) _trialWindowCount = _placementPhysics.size();
    }
    if (_trialReason.empty() && !_trial.Full && _placementWindowIndex < _trialWindowCount)
    {
        if (Clock::now() >= _placementDeadline)
            Finish(world, console, pipeline, "placement budget exhausted");
        else
            BeginPlacementLeg(world, pipeline);
        return;
    }
    CompletePlacementTrial(world, console, pipeline);
}

void BistroBenchmark::WritePlacementWaits(BistroRenderPipeline& pipeline)
{
    if (!_placementWaitsOpen) return;
    _placementWaitsOpen = false;
    const auto& submissions = pipeline.BenchmarkSubmissions();
    const auto waits = pipeline.BenchmarkSubmissions().EndBenchmarkWaits();
    auto packets = pipeline.BenchmarkSubmissions().TakeBenchmarkPackets();
    if (_manualPlacement || !_reusedPass.empty()) return;
    for (const auto& wait : waits)
    {
        const auto queues = std::array{"DIRECT", "COMPUTE", "COPY"};
        const auto& from = submissions.AdapterLabel(wait.From);
        const auto& to = submissions.AdapterLabel(wait.To);
        _csv.Config(_trialPrefix + "Window." + std::to_string(_placementWindowIndex) + ".Wait",
            from + "/" + queues[size_t(wait.SourceQueue)] + " -> " + to + "/" + queues[size_t(wait.TargetQueue)] +
            "; first fence " + std::to_string(wait.FirstFence) + "; count " + std::to_string(wait.Count));
    }
    const double targetMs = 1000. / BistroPlacement::Threshold[_placementProfile];
    // The first windows are identical for all short trials. Supplemental threshold
    // checks may differ between candidates and are not evidence for extrapolation.
    auto* window = !_trial.Full && _placementWindowIndex < _windows.size()
        ? &_trialResult.ProducerWindows[_placementWindowIndex] : nullptr;
    BistroPlacement::AccumulatePlacementCosts(packets, targetMs, _legMeasurements.UsefulOwners, _legMeasurements.Costs, window);
    if (window)
    {
        const auto ao = GetAmbientOcclusionSettings();
        _trialResult.AOImplementation = ao.Implementation;
        _trialResult.AOResolution = int(ao.Resolution);
    }
}

void BistroBenchmark::ReportPlacementEvidence(BistroRenderPipeline& pipeline)
{
    const auto& measurements = _trialMeasurements;
    const auto reflections = _trialPrefix + "Reflection.";
    _csv.Config(reflections + "DueFaces", std::to_string(measurements.Freshness.Due));
    _csv.Config(reflections + "FirstUsedVersions", std::to_string(measurements.Freshness.Used));
    _csv.Config(reflections + "MissedDeadlines", std::to_string(measurements.Freshness.Missed[_placementProfile]));
    _csv.Config(reflections + "MaxFirstUseLatenessMs", Csv::Number(measurements.Freshness.MaxLatenessMs));
    if (measurements.Freshness.AgeSamples)
    {
        _csv.Config(reflections + "MeanDataAgeMs", Csv::Number(measurements.Freshness.MeanAgeSumMs / measurements.Freshness.AgeSamples));
        _csv.Config(reflections + "MaxDataAgeMs", Csv::Number(measurements.Freshness.MaxAgeMs));
    }
    for (const auto& [source, cost] : _trialResult.Costs)
    {
        const auto prefix = _trialPrefix + "Measured." + source.first + "." +
            pipeline.BenchmarkSubmissions().AdapterLabel(source.second) + ".";
        _csv.Config(prefix + "ComputeVersions", std::to_string(cost.ComputeVersions));
        _csv.Config(prefix + "TimedVersions", std::to_string(cost.TimedVersions));
        _csv.Config(prefix + "ImportedVersions", std::to_string(cost.ImportedVersions));
        _csv.Config(prefix + "RemoteComputeMs", Csv::Number(cost.ComputeMs));
        _csv.Config(prefix + "RemoteTransferMs", Csv::Number(cost.TransferMs));
        _csv.Config(prefix + "SharedBufferBytes", std::to_string(cost.SharedBufferBytes));
        _csv.Config(prefix + "PrimaryTransferMs", Csv::Number(cost.PrimaryTransferMs));
        _csv.Config(prefix + "PrimaryTransferBytes", std::to_string(cost.PrimaryTransferBytes));
        _csv.Config(prefix + "PrimaryQueueWaitMs", Csv::Number(cost.PrimaryWaitMs));
        if (cost.ReadyIntervals && cost.ReadySpanMs > 0)
            _csv.Config(prefix + "ReadyVersionsPerSecond", Csv::Number(1000. * cost.ReadyIntervals / cost.ReadySpanMs));
    }
    for (const auto& [source, sim] : measurements.Simulations)
    {
        const auto& [role, adapter, lane] = source;
        const auto prefix = _trialPrefix + "Simulation." + role + "." + pipeline.BenchmarkSubmissions().AdapterLabel(adapter) + "." + std::to_string(lane) + ".";
        _csv.Config(prefix + "ConsumedFrames", std::to_string(sim.Frames));
        _csv.Config(prefix + "ConsumedVersions", std::to_string(sim.UsedVersions));
        _csv.Config(prefix + "LastSnapshotSimulationSeconds", Csv::Number(sim.SimulatedSeconds));
    }
}

void BistroBenchmark::CompletePlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
{
    auto& measurements = _trialMeasurements;
    _trialResult.Remote = std::any_of(measurements.UsefulOwners.begin(), measurements.UsefulOwners.end(), [](const auto& entry) {
            return std::any_of(entry.second.begin(), entry.second.end(), [](auto owner) { return owner != 0; });
        });
    const auto& samples = measurements.SectionShadowSamples;
    const auto& texels = measurements.SectionShadowTexels;
    const uint64_t totalSamples = std::accumulate(samples.begin(), samples.end(), uint64_t{0});
    if (totalSamples)
        _trialResult.ShadowTexels = double(std::accumulate(texels.begin(), texels.end(), uint64_t{0})) / totalSamples;
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i]) _trialResult.SectionShadowTexels[i] = double(texels[i]) / samples[i];
    const bool remote = _placementSearch.RemotePhase();
    if (!_manualPlacement && !remote && _trialResult.Remote && _trialReason.empty())
        _trialReason = "unexpected cross-GPU work";
    if (!_manualPlacement)
    {
        const auto& placements = _placementSearch.Candidates();
        for (size_t i = 0; i < placements.size(); ++i)
        {
            const auto& placement = placements[i];
            const auto requested = _trial.Settings.Owners[i];
            const auto evidence = measurements.ActiveOwners.find(placement.Owner);
            const auto useful = measurements.UsefulOwners.find(placement.Owner);
            auto effective = -1;
            if (evidence != measurements.ActiveOwners.end())
            {
                for (const auto device : evidence->second)
                {
                    if (device && useful != measurements.UsefulOwners.end() && useful->second.count(device) &&
                        (effective < 0 || int(device - 1) == requested))
                        effective = int(device - 1);
                }
            }
            _trialResult.Settings.Owners[i] = effective;
            if (effective != requested && evidence != measurements.ActiveOwners.end())
                _trialResult.EquivalentInactiveOwners = false;
            const auto key = _trialPrefix + std::filesystem::path(placement.Variable).u8string();
            _csv.Config(key + ".RequestedOwner", std::to_string(requested));
            _csv.Config(key + ".EffectiveOwner", std::to_string(effective));
            if (!placement.Modes.empty())
            {
                const auto sync = _trial.Settings.Sync[i];
                _csv.Config(key + ".RequestedSync", sync < 0 ? "local" : std::to_string(sync));
                _csv.Config(key + ".EffectiveSync", effective < 0 ? "local" : std::to_string(sync));
            }
            if (evidence == measurements.ActiveOwners.end())
                _csv.Config(key + ".PlacementStatus", placement.Owner.compare(0, 18, "Local shadow page ") == 0 ?
                    "empty page" : "inactive");
            else
            {
                for (const auto device : evidence->second)
                    _csv.Config(key + ".ObservedOwner", pipeline.BenchmarkSubmissions().AdapterLabel(device));
                _csv.Config(key + ".PlacementStatus", requested >= 0 && effective < 0 ?
                    (evidence->second.count(0) ? "GPU0 fallback" : "remote result had no observed consumer") :
                    evidence->second.size() > 1 ? "mixed owners" : "confirmed");
            }
        }
        _trialResult.Settings = _placementSearch.EffectiveConfig(std::move(_trialResult.Settings));
    }
    _trialResult.CoveredExtraWindows = !_trial.Full && _trialCompletedPasses == _placementPhysics.size();
    _trialResult.Fps = measurements.PrimarySeconds > 0 ? measurements.PrimaryFrames / measurements.PrimarySeconds : 0;
    _trialResult.P95 = measurements.FullP95;
    _trialResult.ThresholdFps = measurements.Seconds > 0 ? measurements.Frames / measurements.Seconds : 0;
    if (!measurements.PrimaryIntervals.empty())
    {
        const auto rank = measurements.PrimaryIntervals.begin() + (measurements.PrimaryIntervals.size() * 95 - 1) / 100;
        std::nth_element(measurements.PrimaryIntervals.begin(), rank, measurements.PrimaryIntervals.end());
        _trialResult.P95 = *rank;
    }
    if (_trialReason.empty() && _trialResult.Fps <= 0) _trialReason = "no rendered samples";
    if (_trialReason.empty() && !_manualPlacement && measurements.Freshness.Missed[_placementProfile])
        _trialReason = "reflection face deadline missed";
    if (!_manualPlacement) ReportPlacementEvidence(pipeline);
    if (_trial.Comparison && _trialReason.empty() &&
        (_trial.Settings.Owners != _trialResult.Settings.Owners || _trial.Settings.Sync != _trialResult.Settings.Sync))
        _csv.Config(_trialPrefix + "ComparisonEligibility", "effective placement differs from requested layout");
    _trialResult.Valid = _trialReason.empty();
    SettleComparisonPass();
    if (!_manualPlacement || _trialResult.Valid)
    {
        _csv.Config(_trialPrefix + "FPS", Csv::Number(_trialResult.Fps));
        if (_trialWindowCount > _windows.size())
            _csv.Config(_trialPrefix + "FPSWithThresholdRecheck", Csv::Number(_trialResult.ThresholdFps));
        _csv.Config(_trialPrefix + "P95ms", Csv::Number(_trialResult.P95));
    }
    _csv.Config(_trialPrefix + "WallSeconds", Csv::Number(SecondsSince(_trialStarted)));
    WritePlacementTrialReport(pipeline, _trialResult.Valid ? "measured" : _trialReason);
    if (!_manualPlacement && (_placementSearch.LowFirst() || _placementSearch.HighFirst()))
        Log::Info("BistroBenchmark", "SingleGPU ", BistroPlacement::ProfileName[_placementProfile], " trial ", _trial.Id,
            "; effective output=", _trialResult.Output.first, "x", _trialResult.Output.second,
            "; render=", _trialResult.Render.first, "x", _trialResult.Render.second,
            "; measured FPS=", _trialResult.ThresholdFps, "; status=", _trialResult.Valid ? "measured" : _trialReason);
    if (_manualPlacement)
    {
        Finish(world, console, pipeline, _trialResult.Valid ? "completed" : _trialReason.c_str(), {}, 0,
            _trialResult.Valid ? Outcome::Completed : Outcome::Failed);
        return;
    }
    _placementSearch.Record(_trial, _trialResult, _csv, _trialReason);
    if (_calibrating)
    {
        _calibrating = false;
        if (std::any_of(_placementPhysics.begin(), _placementPhysics.end(), [](const auto& snapshot) { return !snapshot; }))
        {
            Finish(world, console, pipeline, "incomplete route checkpoints", {}, 0, Outcome::Failed);
            return;
        }
        BeginNextPlacementPhase(world, console, pipeline);
        return;
    }
    NextPlacementTrial(world, console, pipeline);
}

void BistroBenchmark::WritePlacementTrialReport(BistroRenderPipeline& pipeline, std::string_view status)
{
    auto& measurements = _trialMeasurements;
    const auto& submissions = pipeline.BenchmarkSubmissions();
    if (!_trialConfig.empty())
    {
        _csv.Config(_trialPrefix + "EffectivePostDevice", submissions.AdapterLabel(_trialPostDevice));
        if (_trialResult.Output.first && _trialResult.Output.second)
            _csv.Config(_trialPrefix + "InternalScale", Csv::Number(double(_trialResult.Render.first) / _trialResult.Output.first) +
                " x " + Csv::Number(double(_trialResult.Render.second) / _trialResult.Output.second));
    }
    if (_manualPlacement && _trialCompletedPasses)
    {
        _csv.Config(_trialPrefix + "PlacementEvidenceScope", "measured route frames");
    }
    for (const auto& [name, owners] : measurements.ActiveOwners)
        for (const auto owner : owners) _csv.Config(_trialPrefix + name, submissions.AdapterLabel(owner));
    if (_trialResult.ShadowTexels)
        _csv.Config(_trialPrefix + "MeanLocalShadowTexels", Csv::Number(*_trialResult.ShadowTexels));
    constexpr std::array transferNames{"PostSceneInputBytes", "PostHUDBytes", "PostInspectorBytes", "PostReturnBytes"};
    if (!_manualPlacement)
        for (size_t i = 0; i < transferNames.size(); ++i)
            _csv.Config(_trialPrefix + transferNames[i], std::to_string(measurements.TransferBytes[i]));
    for (size_t i = 0; i < _trialResult.Activity.size(); ++i)
        if (_trialResult.Activity[i])
        {
            _csv.Config(_trialPrefix + "ActiveTechnique", _launch.Level->Techniques[i].Pass);
            if (_manualPlacement)
            {
                const auto* quality = _launch.Level->Techniques[i].Quality;
                const auto found = std::find_if(_trialRequested.begin(), _trialRequested.end(), [&](const auto& entry) {
                    return quality && entry.first == quality;
                });
                _csv.Config(_trialPrefix + "ActiveTechniqueLevel", std::string(_launch.Level->Techniques[i].Pass) + "=" +
                    (found == _trialRequested.end() ? "On" : found->second));
            }
            else
                _csv.Config(_trialPrefix + "ActiveTechniqueLevel", std::string(_launch.Level->Techniques[i].Pass) + "=" +
                    (_launch.Level->Techniques[i].Quality ?
                        std::to_string(_placementSearch.QualityValue(i, _trialResult.Settings.Levels[i])) : "On"));
        }
    for (const auto& pass : measurements.Passes)
        _csv.Config(_trialPrefix + "ActivePass", pass);
    for (const auto& [role, adapter, consumer] : measurements.Publications)
        _csv.Config(_trialPrefix + "ConsumedRemotePublication", role + " @ " + submissions.AdapterLabel(adapter) + " -> " + consumer);
    if (!_trialLastPass.empty()) _csv.Config(_trialPrefix + "LastPassCSV", _trialLastPass);
    _csv.Config(_trialPrefix + "CompletedPasses", std::to_string(_trialCompletedPasses));
    const auto requiredPasses = _trial.Full ? 1u : _trialWindowCount;
    _csv.Config(_trialPrefix + "Partial", _trialCompletedPasses < requiredPasses ? "true" : "false");
    _csv.Config(_trialPrefix + "Status", status);
    _trialPending = false;
}

bool BistroBenchmark::CheckPlacementDeadline(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, Clock::time_point now)
{
    if (_stage != Stage::Placement || _manualPlacement) return false;
    if (_activeComparisonEstimate > 0 && !_calibrating)
    {
        if (!_placementInitialProbesReady && Seconds(_placementLegStarted, now) >= _activeComparisonEstimate)
        {
            _trialReason = "initial publications did not become ready";
            CompletePlacementLeg(world, console, pipeline);
            return true;
        }
        return false;
    }
    if (now >= _placementDeadline)
    {
        Finish(world, console, pipeline, "placement budget exhausted");
        return true;
    }
    if (_placementSearch.RemotePhase() && !_trial.InspectImage)
    {
        // Bound a stalled publication independently of the time spent measuring valid windows.
        const bool stalled = !_placementInitialProbesReady && Seconds(_placementLegStarted, now) >= PlacementProbeCost();
        const bool reserveFullRoute = !_trial.Full && Seconds(now, _phaseDeadline) <=
            (_placementSearch.Upscaled() ? FinishReserve : PlacementFullCost() + FinishReserve);
        if (stalled || reserveFullRoute)
        {
            _trialReason = stalled ? "remote publication did not become ready" : "time reserved for full route";
            CompletePlacementLeg(world, console, pipeline);
            return true;
        }
    }
    if (!_multiGPU || _placementPhase >= 2 || now < _singleDeadline) return false;
    if (_calibrating)
    {
        Finish(world, console, pipeline, "single-GPU budget exhausted during checkpoint capture");
        return true;
    }
    WritePlacementWaits(pipeline);
    WriteLeg("single-GPU budget exhausted");
    WritePlacementTrialReport(pipeline, "single-GPU budget exhausted");
    _placementPhase = 2;
    BeginNextPlacementPhase(world, console, pipeline);
    return true;
}

void BistroBenchmark::BeginNextPlacementPhase(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
{
    const double probeAndFull = PlacementProbeCost() + PlacementFullCost();
    for (; _placementPhase < 4 && Clock::now() < _placementDeadline; ++_placementPhase)
    {
        const bool local = _placementPhase < 2;
        const auto deadline = local ? _singleDeadline : _placementDeadline;
        if (Clock::now() >= deadline) continue;
        if (!_placementSearch.BeginPhase(_placementPhase)) continue;
        if (_placementPhase == 2 || (_placementPhase == 3 && !_placementSearch.HasReference(false)))
            _csv.Config("MultiGPUStartSeconds", Csv::Number(SecondsSince(_started)));
        unsigned phases = 1;
        for (unsigned next = _placementPhase + 1; next < (local ? 2u : 4u); ++next)
            phases += _placementSearch.NeedsPhase(next);
        const auto now = Clock::now();
        const double budget = std::max(Seconds(now, deadline) / phases, probeAndFull);
        _phaseBaseBudget = std::min(Seconds(now, deadline), budget);
        if (_placementPhase == 0)
            _phaseBaseBudget += std::max(0., Seconds(_started, now) - _addedPlacementBudget[0]);
        _csv.Config("SearchPhase." + std::to_string(_placementPhase) + ".BaseSeconds", Csv::Number(_phaseBaseBudget));
        _phaseDeadline = std::min(deadline,
            now + std::chrono::ceil<Clock::duration>(std::chrono::duration<double>(budget)));
        WritePlacementBudget();
        NextPlacementTrial(world, console, pipeline, now);
        return;
    }
    Finish(world, console, pipeline, Seconds(Clock::now(), _placementDeadline) < probeAndFull ?
        "placement budget cannot fit another probe and full route" : "placement search finished");
}

void BistroBenchmark::ExtendPlacementBudget(double seconds)
{
    const auto delta = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    _addedPlacementBudget[_placementPhase] += seconds;
    _placementDeadline += delta;
    _phaseDeadline += delta;
    if (_placementPhase < 2) _singleDeadline += delta;
    WritePlacementBudget();
}

void BistroBenchmark::WritePlacementBudget()
{
    const double added = std::accumulate(_addedPlacementBudget.begin(), _addedPlacementBudget.end(), 0.);
    _csv.Config("BaseBudgetSeconds", Csv::Number(_basePlacementBudget));
    _csv.Config("AddedFullRouteSeconds", Csv::Number(added));
    _csv.Config("BudgetSeconds", Csv::Number(_basePlacementBudget + added));
    if (_placementPhase < _addedPlacementBudget.size())
    {
        const auto prefix = "SearchPhase." + std::to_string(_placementPhase) + ".";
        _csv.Config(prefix + "AddedFullRouteSeconds", Csv::Number(_addedPlacementBudget[_placementPhase]));
        _csv.Config(prefix + "BudgetSeconds", Csv::Number(_phaseBaseBudget + _addedPlacementBudget[_placementPhase]));
    }
}

void BistroBenchmark::SettleComparisonPass(bool interrupted)
{
    if (_activeComparisonEstimate > 0)
    {
        const double elapsed = SecondsSince(_trialStarted);
        ExtendPlacementBudget(elapsed - std::exchange(_activeComparisonEstimate, 0.));
        _csv.Config(_trialPrefix + "AddedRouteActualSeconds", Csv::Number(elapsed));
    }
    if (interrupted && _plannedComparisonPasses)
    {
        ExtendPlacementBudget(-_comparisonEstimate * _plannedComparisonPasses);
        _plannedComparisonPasses = 0;
    }
    if (interrupted) _placementSearch.InterruptComparison(_csv);
}

bool BistroBenchmark::StartPlacementConfirmation(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline,
    BistroPlacement::Trial trial)
{
    trial = _placementSearch.CompareImageMode(std::move(trial), _csv);
    if (!trial.Id)
    {
        _plannedComparisonPasses = _placementSearch.PendingComparisonPasses();
        if (_plannedComparisonPasses)
        {
            _comparisonEstimate = PlacementFullCost();
            ExtendPlacementBudget(_comparisonEstimate * _plannedComparisonPasses);
        }
        const auto next = _placementSearch.NextComparisonTrial();
        if (!next) return false;
        trial = *next;
    }
    BeginPlacementTrial(world, console, pipeline, std::move(trial));
    return true;
}

void BistroBenchmark::NextPlacementTrial(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline,
    Clock::time_point now)
{
    if (auto trial = _placementSearch.NextComparisonTrial())
    {
        BeginPlacementTrial(world, console, pipeline, std::move(*trial));
        return;
    }
    if (auto saved = _placementSearch.SavedComparison())
        if (StartPlacementConfirmation(world, console, pipeline, std::move(*saved))) return;
    if (_placementSearch.RemotePhase() && RenderConfig(console) != _trialRequested)
        _placementSearch.RetryChangedEstimates();
    const double fullCost = _placementSearch.Upscaled() ? 0. : PlacementFullCost();
    const double remaining = Seconds(now, _phaseDeadline);
    const bool canProbe = remaining >= PlacementProbeCost() + fullCost;
    const auto start = [&](BistroPlacement::Trial trial)
    {
        if (trial.Full) return StartPlacementConfirmation(world, console, pipeline, std::move(trial));
        BeginPlacementTrial(world, console, pipeline, std::move(trial));
        return true;
    };
    if (_placementSearch.HighFirst())
    {
        if (remaining >= fullCost)
            while (auto trial = _placementSearch.NextQualityTrial(_csv, canProbe))
                if (start(std::move(*trial))) return;
    }
    else
    {
        if (remaining >= fullCost)
            while (auto confirmation = _placementSearch.PerformanceConfirmation(_csv))
                if (start(std::move(*confirmation))) return;
        if (canProbe)
            if (auto probe = _placementSearch.NextProbe(_csv))
                if (start(std::move(*probe))) return;
        if (remaining >= fullCost)
            while (const auto* finalist = _placementSearch.Finalist())
                if (start(_placementSearch.Confirm(*finalist))) return;
    }
    ++_placementPhase;
    BeginNextPlacementPhase(world, console, pipeline);
}
