#include "BistroBenchmark.h"
#include <iomanip>
#include <sstream>

void BistroBenchmark::OpenPlacementMeasurements(const std::string& conditions)
{
    _measurementIndex.clear();
    _measurementFile = _path;
    _measurementFile.replace_extension(".measurements");
    for (const auto* mode : {"SingleGPU", "MultiGPU"})
    {
        const auto directory = _path.parent_path().parent_path() / mode;
        if (!std::filesystem::is_directory(directory)) continue;
        for (const auto& item : std::filesystem::directory_iterator(directory))
        {
            if (item.path().extension() != ".measurements" || !item.is_regular_file()) continue;
            std::ifstream input(item.path(), std::ios::binary);
            unsigned version = 0, profile = 0;
            std::string saved, line;
            if (!(input >> version >> profile >> std::quoted(saved)) || version != 1 ||
                profile > BistroPlacement::Quality || saved != conditions) continue;
            std::getline(input, line);
            for (auto offset = input.tellg(); std::getline(input, line); offset = input.tellg())
            {
                std::istringstream row(line);
                std::string key;
                if (!(row >> std::quoted(key))) continue;
                auto& latest = _measurementIndex[key];
                // The run filename begins with its creation timestamp.
                if (item.path().filename() >= latest.File.filename()) latest = {item.path(), offset};
            }
        }
    }
    std::ofstream output(_measurementFile, std::ios::binary);
    output << 1 << ' ' << unsigned(_placementProfile) << ' ' << std::quoted(conditions) << '\n';
}

std::string BistroBenchmark::PlacementMeasurementKey(const BistroRenderPipeline& pipeline) const
{
    // Compare the requested workload as well as the live image and consumer paths
    // after preparation. A fallback or different adapter assignment gets a new key.
    const auto& s = _trial.Settings;
    std::ostringstream key;
    key << std::setprecision(17) << _trial.Full << ' ' << _placementWindowIndex << ' ' << _window.Begin << ' ' <<
        _window.End << ' ' << s.Provider << ' ' << s.Preset << ' ' << int(s.ProbeFrequency)
        << ' ' << s.ProbeCapture << ' ' << s.ProbeDelivery << ' ' << s.RemoteAOFilter;
    for (auto level : s.Levels) key << ' ' << int(level);
    for (size_t i = 0; i < s.Owners.size(); ++i) key << ' ' << s.Owners[i] << '/' << s.Sync[i];
    for (const auto& [name, value] : _trialConfig) key << ' ' << std::quoted(name) << ' ' << std::quoted(value);
    // The file header covers startup conditions. Include actual controlled values,
    // newly registered variables and any user edits between trials in this run.
    std::map<std::wstring, std::string> changes;
    for (const auto& [name, value] : _trialRequested)
    {
        if (name.compare(0, 14, L"App.Benchmark.") == 0) continue;
        const auto initial = _measurementDefaults.find(name);
        if (initial == _measurementDefaults.end() || value != initial->second) changes.emplace(name, value);
    }
    for (const auto& [name, value] : changes)
        key << ' ' << std::quoted(std::filesystem::path(name).u8string()) << ' ' << std::quoted(value);
    for (const bool active : _legMeasurements.Activity) key << ' ' << active;
    for (const auto& [role, owners] : _legMeasurements.ActiveOwners)
        for (const auto owner : owners) key << " A " << std::quoted(role) << ' ' << owner;
    for (const auto& [role, owners] : _legMeasurements.UsefulOwners)
        for (const auto owner : owners) key << " U " << std::quoted(role) << ' ' << owner;
    for (const auto& [role, owner, consumer] : _legMeasurements.Publications)
        key << " P " << std::quoted(role) << ' ' << owner << ' ' << std::quoted(consumer);
    const auto& packets = pipeline.BenchmarkSchedule()->Packets;
    for (const auto& link : pipeline.BenchmarkLinks())
        if (link.Import < packets.size())
        {
            const auto& packet = packets[link.Import];
            key << " I " << link.Owner << ' ' << std::quoted(packet.Name) << ' ' << int(packet.Target)
                << ' ' << packet.TransferBytes.value_or(0);
        }
    return key.str();
}

void BistroBenchmark::SavePlacementWindow() const
{
    const auto& w = _legMeasurements;
    if (_measurementKey.empty() || !w.Frames || w.Seconds <= 0) return;
    std::ofstream row(_measurementFile, std::ios::binary | std::ios::app);
    row << std::setprecision(17) << std::quoted(_measurementKey) << ' '
        << std::quoted(std::filesystem::u8path(_trialLastPass).filename().generic_u8string()) << ' '
        << w.Frames << ' ' << w.Seconds << ' ' << w.FullP95;
    const auto& f = w.Freshness;
    row << ' ' << f.Due << ' ' << f.Used << ' ' << f.Missed[0] << ' ' << f.Missed[1] << ' ' <<
        f.AgeSamples << ' ' << f.MaxLatenessMs << ' ' << f.MeanAgeSumMs << ' ' << f.MaxAgeMs;
    for (auto active : w.Activity) row << ' ' << active;
    for (auto bytes : w.TransferBytes) row << ' ' << bytes;
    for (size_t i = 0; i < w.SectionShadowSamples.size(); ++i)
        row << ' ' << w.SectionShadowTexels[i] << ' ' << w.SectionShadowSamples[i];
    // The short windows need intervals to compute the joint p95. A full route already has its p95.
    for (auto ms : w.PrimaryIntervals) row << " T " << ms;
    for (const auto& pass : w.Passes) row << " N " << std::quoted(pass);
    for (const auto& [role, owners] : w.ActiveOwners)
        for (auto owner : owners) row << " A " << std::quoted(role) << ' ' << owner;
    for (const auto& [role, owners] : w.UsefulOwners)
        for (auto owner : owners) row << " U " << std::quoted(role) << ' ' << owner;
    for (const auto& [role, owner, consumer] : w.Publications)
        row << " P " << std::quoted(role) << ' ' << owner << ' ' << std::quoted(consumer);
    for (const auto& [source, sim] : w.Simulations)
        row << " S " << std::quoted(std::get<0>(source)) << ' ' << std::get<1>(source) << ' ' << std::get<2>(source)
            << ' ' << sim.Frames << ' ' << sim.UsedVersions << ' ' << sim.SimulatedSeconds;
    for (const auto& [source, c] : w.Costs)
        row << " C " << std::quoted(source.first) << ' ' << source.second << ' ' << c.ComputeVersions << ' ' << c.TimedVersions << ' '
            << c.ImportedVersions << ' ' << c.ReadyIntervals << ' ' << c.SharedBufferBytes << ' ' << c.PrimaryTransferBytes
            << ' ' << c.ComputeMs << ' ' << c.TransferMs << ' ' << c.PrimaryTransferMs << ' ' << c.PrimaryWaitMs << ' ' << c.ReadySpanMs;
    row << " E\n";
}

bool BistroBenchmark::RestorePlacementWindow()
{
    const auto found = _measurementIndex.find(_measurementKey);
    if (found == _measurementIndex.end()) return false;
    const auto& record = found->second;
    std::ifstream input(record.File, std::ios::binary);
    std::string line, key, pass;
    if (!std::getline(input.seekg(record.Offset), line)) return false;
    std::istringstream row(line);
    TrialMeasurements w;
    auto& f = w.Freshness;
    if (!(row >> std::quoted(key) >> std::quoted(pass) >> w.Frames >> w.Seconds >> w.FullP95 >>
        f.Due >> f.Used >> f.Missed[0] >> f.Missed[1] >> f.AgeSamples >> f.MaxLatenessMs >>
        f.MeanAgeSumMs >> f.MaxAgeMs) || key != _measurementKey || !w.Frames || !(w.Seconds > 0)) return false;
    const auto file = std::filesystem::u8path(pass);
    if (file != file.filename() || file.extension() != ".csv") return false;
    const auto source = record.File.parent_path() / record.File.stem() / file;
    if (!std::filesystem::is_regular_file(source)) return false;
    for (auto& active : w.Activity) row >> active;
    for (auto& bytes : w.TransferBytes) row >> bytes;
    for (size_t i = 0; i < w.SectionShadowSamples.size(); ++i)
        row >> w.SectionShadowTexels[i] >> w.SectionShadowSamples[i];
    char tag;
    std::string role, consumer;
    BistroRender::Id owner;
    while (row >> tag)
    {
        if (tag == 'E')
        {
            if (!(_trial.Full ? w.FullP95 > 0 : w.PrimaryIntervals.size() == w.Frames)) return false;
            if (!CompatibleRemotePaths(w)) return false;
            _reusedPass = source.lexically_relative(_path.parent_path()).generic_u8string();
            _legMeasurements = std::move(w);
            return true;
        }
        if (tag == 'T')
        {
            double ms;
            if (!(row >> ms) || ms < 0) return false;
            w.PrimaryIntervals.push_back(ms);
            continue;
        }
        if (!(row >> std::quoted(role))) return false;
        if (tag == 'N') { w.Passes.insert(role); continue; }
        if (!(row >> owner)) return false;
        switch (tag)
        {
        case 'A': w.ActiveOwners[role].insert(owner); break;
        case 'U': w.UsefulOwners[role].insert(owner); break;
        case 'P':
            if (!(row >> std::quoted(consumer))) return false;
            w.Publications.emplace(role, owner, consumer);
            break;
        case 'S':
        {
            size_t lane;
            if (!(row >> lane)) return false;
            auto& sim = w.Simulations[{role, owner, lane}];
            row >> sim.Frames >> sim.UsedVersions >> sim.SimulatedSeconds;
            break;
        }
        case 'C':
        {
            auto& c = w.Costs[{role, owner}];
            row >> c.ComputeVersions >> c.TimedVersions >> c.ImportedVersions >> c.ReadyIntervals >> c.SharedBufferBytes >>
                c.PrimaryTransferBytes >> c.ComputeMs >> c.TransferMs >> c.PrimaryTransferMs >> c.PrimaryWaitMs >> c.ReadySpanMs;
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool BistroBenchmark::CompatibleRemotePaths(const TrialMeasurements& saved) const
{
    // Reuse only if every remote consumer seen in the source window is visible
    // after preparation. A different route phase or fallback requires live work.
    if (saved.Publications != _legMeasurements.Publications) return false;
    const auto& placements = _placementSearch.Candidates();
    for (size_t i = 0; i < placements.size(); ++i)
    {
        const int requested = _trial.Settings.Owners[i];
        if (requested < 0) continue;
        const auto& role = placements[i].Owner;
        const auto owner = BistroRender::Id(requested + 1);
        const auto active = saved.ActiveOwners.find(role);
        const auto current = _legMeasurements.ActiveOwners.find(role);
        if (!_trial.Full && active == saved.ActiveOwners.end() && current == _legMeasurements.ActiveOwners.end()) continue;
        if (active == saved.ActiveOwners.end() || current == _legMeasurements.ActiveOwners.end() ||
            active->second != current->second || active->second.size() != 1 || !active->second.count(owner)) return false;
        const auto used = saved.UsefulOwners.find(role);
        const auto liveUsed = _legMeasurements.UsefulOwners.find(role);
        if (used == saved.UsefulOwners.end() || liveUsed == _legMeasurements.UsefulOwners.end() ||
            !used->second.count(owner) || used->second != liveUsed->second) return false;
    }
    return true;
}

void BistroBenchmark::AccumulatePlacementWindow(bool primary)
{
    const auto& w = _legMeasurements;
    auto& t = _trialMeasurements;
    t.Frames += w.Frames;
    t.Seconds += w.Seconds;
    if (primary)
    {
        t.PrimaryFrames += w.Frames;
        t.PrimarySeconds += w.Seconds;
        t.FullP95 = w.FullP95;
        t.PrimaryIntervals.insert(t.PrimaryIntervals.end(), w.PrimaryIntervals.begin(), w.PrimaryIntervals.end());
    }
    for (size_t i = 0; i < w.TransferBytes.size(); ++i) t.TransferBytes[i] += w.TransferBytes[i];
    for (size_t i = 0; i < w.SectionShadowSamples.size(); ++i)
    {
        t.SectionShadowTexels[i] += w.SectionShadowTexels[i];
        t.SectionShadowSamples[i] += w.SectionShadowSamples[i];
    }
    for (size_t i = 0; i < w.Activity.size(); ++i) _trialResult.Activity[i] |= w.Activity[i];
    t.Passes.insert(w.Passes.begin(), w.Passes.end());
    t.Publications.insert(w.Publications.begin(), w.Publications.end());
    for (const auto& [role, owners] : w.ActiveOwners) t.ActiveOwners[role].insert(owners.begin(), owners.end());
    for (const auto& [role, owners] : w.UsefulOwners) t.UsefulOwners[role].insert(owners.begin(), owners.end());
    for (const auto& [source, s] : w.Simulations)
    {
        auto& sim = t.Simulations[source];
        sim.Frames += s.Frames;
        sim.UsedVersions += s.UsedVersions;
        sim.SimulatedSeconds = s.SimulatedSeconds;
    }
    const auto& f = w.Freshness;
    t.Freshness.Due += f.Due;
    t.Freshness.Used += f.Used;
    for (size_t i = 0; i < f.Missed.size(); ++i) t.Freshness.Missed[i] += f.Missed[i];
    t.Freshness.AgeSamples += f.AgeSamples;
    t.Freshness.MeanAgeSumMs += f.MeanAgeSumMs;
    t.Freshness.MaxAgeMs = std::max(t.Freshness.MaxAgeMs, f.MaxAgeMs);
    t.Freshness.MaxLatenessMs = std::max(t.Freshness.MaxLatenessMs, f.MaxLatenessMs);
    for (const auto& [source, c] : w.Costs)
    {
        auto& cost = _trialResult.Costs[source];
        cost.ComputeVersions += c.ComputeVersions;
        cost.TimedVersions += c.TimedVersions;
        cost.SlowVersions += c.SlowVersions;
        cost.ImportedVersions += c.ImportedVersions;
        cost.ReadyIntervals += c.ReadyIntervals;
        cost.SharedBufferBytes += c.SharedBufferBytes;
        cost.PrimaryTransferBytes += c.PrimaryTransferBytes;
        cost.ComputeMs += c.ComputeMs;
        cost.TransferMs += c.TransferMs;
        cost.PrimaryTransferMs += c.PrimaryTransferMs;
        cost.PrimaryWaitMs += c.PrimaryWaitMs;
        cost.ReadySpanMs += c.ReadySpanMs;
    }
}
