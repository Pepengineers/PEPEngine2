#pragma once

#include "Graph/QueueSubmission.h"
#include <algorithm>
#include <set>
#include <string_view>

namespace BistroPlacement
{
    inline std::string_view PublicationRole(std::string_view packet)
    {
        const auto has = [&](std::string_view text) { return packet.find(text) != std::string_view::npos; };
        if (has("reflection probe") || packet.compare(0, 6, "probe ") == 0) return "ReflectionProbes";
        if (has("local shadow") || has("shadow owner")) return "Local shadows";
        if (has("CSM") || has("sun atlas")) return "SunShadowAdapter";
        if (has("XeGTAO") || has("CACAO") || has("HBAO") || has("SSAO")) return "AOAdapter";
        if (has("light grid") || has("LightGrid")) return "LightGridAdapter";
        if (has("particle") && !has("water")) return "ParticleAdapter";
        if (has("water")) return "WaterAdapter";
        if (has("wind") || has("ProjectedGrid")) return "GrassWindAdapter";
        if (has("cloud picture") || has("CloudPicture")) return "CloudPictureAdapter";
        if (has("SkyView")) return "SkyViewAdapter";
        if (has("aerial pair") || has("Aerial Perspective")) return "AerialPerspectiveAdapter";
        if (has("Atrium") || has("NEON DISTRICT")) return "AtriumCanvasAdapter";
        if (has("Terrace") || has("ASTRA")) return "TerraceCanvasAdapter";
        if (has("HUD")) return "GameHUDAdapter";
        if (has("WBOIT") || has("transparent RGB/transmittance")) return "TransparencyAdapter";
        if (has("post")) return "PostAdapter";
        return {};
    }

    struct PlacementCost
    {
        size_t TimedVersions = 0, SlowVersions = 0, ImportedVersions = 0, ReadyIntervals = 0;
        uint64_t SharedBufferBytes = 0, PrimaryTransferBytes = 0;
        double ComputeMs = 0, TransferMs = 0, PrimaryTransferMs = 0, PrimaryWaitMs = 0, ReadySpanMs = 0;
    };

    using PlacementCosts = std::map<std::pair<std::string, BistroRender::Id>, PlacementCost>;

    inline void AccumulatePlacementCosts(
        const std::vector<BistroRender::QueueSubmission::BenchmarkPacket>& packets, double targetMs,
        const std::map<std::string, std::set<BistroRender::Id>>& owners, PlacementCosts& costs)
    {
        using namespace BistroRender;
        using TokenKey = std::tuple<Id, Queue, uint64_t>;
        const auto key = [](CompletionToken t) { return TokenKey(t.Device, t.Target, t.Value); };
        using Interval = std::pair<double, double>;
        const auto coveredTime = [](std::vector<Interval>& intervals)
        {
            std::sort(intervals.begin(), intervals.end());
            double last = -1, total = 0;
            for (const auto& [begin, end] : intervals)
            {
                total += (std::max)(0., end - (std::max)(begin, last));
                last = (std::max)(last, end);
            }
            return total;
        };
        std::map<TokenKey, Interval> completed;
        std::map<TokenKey, std::set<std::string_view>> roles;
        for (const auto& packet : packets)
        {
            if (packet.Calibrated && packet.EndMs >= packet.BeginMs)
            {
                auto [it, inserted] = completed.try_emplace(key(packet.Token), packet.BeginMs, packet.EndMs);
                if (!inserted)
                {
                    it->second.first = (std::min)(it->second.first, packet.BeginMs);
                    it->second.second = (std::max)(it->second.second, packet.EndMs);
                }
            }
            if (packet.Token.Device) continue;
            const auto role = PublicationRole(packet.Name);
            if (role.empty()) continue;
            for (const auto& source : packet.Dependencies)
                if (source.Device) roles[key(source)].insert(role);
        }
        struct Version
        {
            std::array<std::vector<Interval>, 3> Work;
            std::vector<Interval> CalibratedWork;
            bool Uncalibrated = false;
        };
        std::map<std::pair<TokenKey, std::string>, Version> versions;
        std::set<std::pair<TokenKey, std::string_view>> imports;
        for (const auto& packet : packets)
        {
            const auto owner = packet.Token.Device;
            auto role = PublicationRole(packet.Name);
            if (owner)
            {
                const auto group = roles.find(key(packet.Terminal));
                if (group != roles.end() && group->second.size() == 1) role = *group->second.begin();
                if (role.empty() || packet.EndMs <= packet.BeginMs) continue;
                auto& cost = costs[{std::string(role), owner}];
                if (packet.SharedBytes)
                {
                    cost.TransferMs += packet.EndMs - packet.BeginMs;
                    cost.SharedBufferBytes += packet.SharedBytes;
                }
                else
                {
                    auto& version = versions[{key(packet.Terminal), std::string(role)}];
                    version.Work[size_t(packet.Token.Target)].emplace_back(packet.BeginMs, packet.EndMs);
                    if (packet.Calibrated) version.CalibratedWork.emplace_back(packet.BeginMs, packet.EndMs);
                    else version.Uncalibrated = true;
                }
                continue;
            }
            if (role.empty()) continue;
            if (packet.SharedBytes && packet.EndMs > packet.BeginMs)
            {
                std::set<Id> peers;
                for (const auto& dependency : packet.Dependencies)
                    if (dependency.Device) peers.insert(dependency.Device);
                if (peers.empty())
                    if (const auto found = owners.find(std::string(role)); found != owners.end())
                        for (const auto device : found->second)
                            if (device) peers.insert(device);
                if (peers.size() == 1)
                {
                    auto& cost = costs[{std::string(role), *peers.begin()}];
                    cost.PrimaryTransferMs += packet.EndMs - packet.BeginMs;
                    cost.PrimaryTransferBytes += packet.SharedBytes;
                }
            }
            if (!packet.SubmitMs) continue;
            const auto previous = completed.find(TokenKey{0, packet.Token.Target, packet.Token.Value - 1});
            auto ownReady = previous == completed.end() ? packet.SubmitMs :
                (std::max)(packet.SubmitMs, previous->second.second);
            bool knownLocal = previous != completed.end();
            for (const auto& source : packet.Dependencies)
                if (!source.Device)
                {
                    const auto it = completed.find(key(source));
                    if (it == completed.end()) knownLocal = false;
                    else ownReady = (std::max)(ownReady, it->second.second);
                }
            const CompletionToken* last = nullptr;
            double lastEnd = ownReady;
            for (const auto& source : packet.Dependencies)
            {
                if (!source.Device) continue;
                auto& cost = costs[{std::string(role), source.Device}];
                if (imports.emplace(key(source), role).second) ++cost.ImportedVersions;
                const auto it = completed.find(key(source));
                if (it != completed.end() && it->second.second > lastEnd)
                {
                    last = &source;
                    lastEnd = it->second.second;
                }
            }
            if (last && packet.Calibrated && knownLocal)
                costs[{std::string(role), last->Device}].PrimaryWaitMs +=
                    (std::max)(0., (std::min)(lastEnd, packet.BeginMs) - ownReady);
        }
        std::map<std::pair<std::string, Id>, std::vector<double>> readyTimes;
        for (auto& [source, version] : versions)
        {
            const auto role = std::pair{source.second, std::get<0>(source.first)};
            auto& cost = costs[role];
            double work = 0;
            if (version.Uncalibrated)
            {
                for (auto& queue : version.Work) work = (std::max)(work, coveredTime(queue));
            }
            else work = coveredTime(version.CalibratedWork);
            cost.ComputeMs += work;
            if (!version.Uncalibrated)
            {
                ++cost.TimedVersions;
                cost.SlowVersions += work > targetMs;
                if (const auto terminal = completed.find(source.first); terminal != completed.end())
                    readyTimes[role].push_back(terminal->second.second);
            }
        }
        for (const auto& [role, ready] : readyTimes)
            if (ready.size() > 1)
            {
                const auto [first, last] = std::minmax_element(ready.begin(), ready.end());
                costs[role].ReadyIntervals += ready.size() - 1;
                costs[role].ReadySpanMs += *last - *first;
            }
    }
}
