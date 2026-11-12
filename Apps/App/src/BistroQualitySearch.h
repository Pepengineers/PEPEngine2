#pragma once

#include "BistroMGPUQuality.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace BistroQualitySearch
{
    enum class OffGroup { Post, Simulation, Characters, Shadows, Never };

    struct Axis
    {
        OffGroup Group = OffGroup::Never;
        bool Cpu = false;
        int MinTier = 1;
        int MaxTier = 4;
        std::optional<size_t> Prerequisite = std::nullopt;
    };

    struct Candidate
    {
        std::vector<int> Tiers;
        int Width = 0;
        int Height = 0;
        MGPUSettings MGPU;
        bool operator==(const Candidate& other) const
        {
            return std::tie(Width, Height, Tiers, MGPU) == std::tie(other.Width, other.Height, other.Tiers, other.MGPU);
        }
        bool operator<(const Candidate& other) const
        {
            return std::tie(Width, Height, Tiers, MGPU) < std::tie(other.Width, other.Height, other.Tiers, other.MGPU);
        }
    };

    struct Request { Candidate Settings; bool RequireFull; };
    struct Result { std::optional<double> AverageFps; std::optional<double> WarmupFps; bool Valid = true; };
    struct Selection { Candidate Settings; double AverageFps; bool TargetReached; bool Stable; };

    class Search
    {
    public:
        Search(Profile profile, std::vector<Axis> axes, int width, int height, MGPUSettings mgpu = {},
            std::optional<size_t> waterAxis = {}, std::optional<size_t> grassAxis = {},
            std::array<std::optional<size_t>, 3> scheduledAxes = {})
            : _profile(profile), _axes(std::move(axes)), _width(width), _height(height),
              _waterAxis(waterAxis), _grassAxis(grassAxis), _scheduledAxes(scheduledAxes)
        {
            if (width <= 0 || height <= 0) throw std::invalid_argument("Invalid quality search dimensions");
            if (mgpu.Enabled && (!mgpu.Primary || !mgpu.Secondary || mgpu.Primary == mgpu.Secondary))
                throw std::invalid_argument("MGPU quality search requires two distinct fixed adapters");
            for (const auto& axis : _axes)
            {
                if (axis.MinTier < 1 || axis.MaxTier > 4 || axis.MinTier > axis.MaxTier)
                    throw std::invalid_argument("Invalid quality search tier bounds");
                size_t depth = 0;
                for (auto parent = axis.Prerequisite; parent; parent = _axes[*parent].Prerequisite)
                    if (*parent >= _axes.size() || ++depth >= _axes.size())
                        throw std::invalid_argument("Invalid or cyclic quality search prerequisite");
            }
            _coarse.Width = width;
            _coarse.Height = height;
            _coarse.MGPU = mgpu;
            RestartCoarseTiers();
        }

        static double Target(Profile profile) { return profile == Profile::Performance ? 60.0 : 28.0; }

        std::optional<Request> Next()
        {
            if (_placementRequest) return _placementRequest;
            while (!_request)
            {
                switch (_phase)
                {
                case Phase::Coarse:
                    if (_coarseFirst) _request = Request{_coarseFirst->Settings, true};
                    else RequestOnce(_coarse, true);
                    break;
                case Phase::Graphics:
                case Phase::Cpu:
                case Phase::Restore:
                    if (_neighborFirst) _request = Request{_neighborFirst->Settings, true};
                    else if (!RequestNeighbor(true)) FinishNeighborhood();
                    break;
                case Phase::Disable:
                    if (_offRepeat) _request = Request{_offRepeat->Settings, true};
                    else if (!RequestNeighbor(true)) ChooseDisableRepeat();
                    break;
                case Phase::Confirm:
                    _request = Request{_confirm->Settings, true};
                    break;
                case Phase::Recover:
                    if (!RequestDeferred()) StartConfirmation();
                    break;
                case Phase::Done:
                    return std::nullopt;
                }
            }
            if (_explorePlacement)
            {
                _explorePlacement = false;
                _placements.clear();
                _placementVisited.clear();
                _placementCursor = 0;
                _placementBest.reset();
                _placementAnchor.reset();
                AddPlacementNeighbors(_request->Settings);
                if (!_placements.empty())
                    _placementRequest = Request{_placements[_placementCursor++], true};
            }
            return _placementRequest ? _placementRequest : _request;
        }

        void Submit(Result result)
        {
            if (!_request) throw std::logic_error("Quality search has no outstanding request");
            if (_placementRequest)
            {
                if (result.Valid && result.AverageFps && std::isfinite(*result.AverageFps) && *result.AverageFps > 0)
                {
                    const Selection measured{_placementRequest->Settings, *result.AverageFps,
                        *result.AverageFps >= Target(_profile), true};
                    if (measured.TargetReached) _finalists.push_back(measured);
                    else RememberFallback(measured);
                    if (!_placementBest || Better(measured, *_placementBest)) _placementBest = measured;
                }
                _placementRequest.reset();
                if (_placementCursor < _placements.size())
                {
                    _placementRequest = Request{_placements[_placementCursor++], true};
                    return;
                }
                if (_placementBest && (!_placementAnchor || Better(*_placementBest, *_placementAnchor)))
                {
                    _placementAnchor = _placementBest;
                    AddPlacementNeighbors(_placementBest->Settings);
                    if (_placementCursor < _placements.size())
                    {
                        _placementRequest = Request{_placements[_placementCursor++], true};
                        return;
                    }
                }
                _placements.clear();
                if (_placementBest)
                {
                    _request->Settings = _placementBest->Settings;
                    result = {_placementBest->AverageFps, std::nullopt, true};
                }
                else result = {std::nullopt, std::nullopt, false};
            }
            const auto request = std::exchange(_request, std::nullopt);
            std::optional<Selection> measured;
            if (result.Valid && result.AverageFps && std::isfinite(*result.AverageFps) && *result.AverageFps > 0)
                measured = Selection{request->Settings, *result.AverageFps, *result.AverageFps >= Target(_profile), true};

            switch (_phase)
            {
            case Phase::Coarse:
                SubmitCoarse(std::move(measured));
                break;
            case Phase::Graphics:
            case Phase::Cpu:
            case Phase::Restore:
            {
                const auto first = std::exchange(_neighborFirst, std::nullopt);
                if (!first && measured && _phase != Phase::Restore &&
                    ComfortablyAboveTarget(*measured) && Better(*measured, *_base))
                {
                    // Confirm the speed margin before it can prune lower-ranked alternatives.
                    _neighborFirst = measured;
                    break;
                }
                if (first && measured)
                {
                    measured->Stable = std::abs(first->AverageFps - measured->AverageFps) <= Noise(first->AverageFps);
                    measured->AverageFps = (std::min)(first->AverageFps, measured->AverageFps);
                    measured->TargetReached = measured->AverageFps >= Target(_profile);
                }
                if (measured) ConsiderNeighbor(*measured);
                break;
            }
            case Phase::Disable:
                SubmitDisable(measured);
                break;
            case Phase::Confirm:
                SubmitConfirmation(measured);
                break;
            case Phase::Recover:
                if (measured)
                {
                    if (measured->TargetReached) _finalists.push_back(*measured);
                    else RememberFallback(*measured);
                }
                break;
            case Phase::Done:
                throw std::logic_error("Quality search is already complete");
            }
        }

        void Cancel()
        {
            _request.reset();
            _placementRequest.reset();
            _placements.clear();
            _phase = Phase::Done;
        }

        const std::optional<Selection>& Winner() const { return _winner; }

    private:
        enum class Phase { Coarse, Graphics, Cpu, Disable, Restore, Confirm, Recover, Done };

        void AddPlacementNeighbors(const Candidate& base)
        {
            const auto append = [&](Candidate candidate)
            {
                if (_waterAxis && (*_waterAxis >= candidate.Tiers.size() || !candidate.Tiers[*_waterAxis])) candidate.MGPU.Water = {};
                if (_grassAxis && (*_grassAxis >= candidate.Tiers.size() || !candidate.Tiers[*_grassAxis])) candidate.MGPU.Grass = {};
                const auto off = [&](const auto& axis) { return axis && (*axis >= candidate.Tiers.size() || !candidate.Tiers[*axis]); };
                if (off(_scheduledAxes[0])) candidate.MGPU.Sun = {};
                if (off(_scheduledAxes[1])) for (auto& light : candidate.MGPU.LocalShadows) light.Route = {};
                if (off(_scheduledAxes[2])) candidate.MGPU.Particles = {};
                if (!candidate.MGPU.Water.Secondary) candidate.MGPU.Water.Exact = false;
                if (!candidate.MGPU.Grass.Secondary) candidate.MGPU.Grass.Exact = false;
                candidate.MGPU.Reflections.Canonicalize();
                // Every route is compared with primary at exactly the same spatial and temporal quality.
                auto primary = candidate;
                primary.MGPU.Water.Secondary = primary.MGPU.Grass.Secondary = false;
                primary.MGPU.Sun.Secondary = primary.MGPU.Particles.Secondary = false;
                for (auto& light : primary.MGPU.LocalShadows) light.Route.Secondary = false;
                primary.MGPU.Reflections.Placement = 0;
                primary.MGPU.Reflections.SecondaryFaces = {};
                primary.MGPU.Water.Exact = primary.MGPU.Grass.Exact = false;
                if (_placementVisited.insert(primary).second) _placements.push_back(std::move(primary));
                if (_placementVisited.insert(candidate).second) _placements.push_back(std::move(candidate));
            };
            append(base);
            auto together = base;
            together.MGPU.Water.Secondary = together.MGPU.Grass.Secondary = true;
            together.MGPU.Sun.Secondary = together.MGPU.Particles.Secondary = true;
            for (auto& light : together.MGPU.LocalShadows) light.Route.Secondary = true;
            together.MGPU.Reflections.SecondaryFaces.fill(63);
            append(together);
            for (int unit = 0; unit != 2; ++unit)
                for (int route = 0; route != 3; ++route)
                    for (int hz : {0, 60, 30})
                    {
                        auto candidate = base;
                        (unit ? candidate.MGPU.Grass : candidate.MGPU.Water) = {route != 0, route == 1, hz};
                        append(std::move(candidate));
                    }
            for (size_t unit = 0; unit < base.MGPU.LocalShadows.size() + 2; ++unit)
                for (int route = 0; route != 3; ++route)
                    for (int interval : {1, 2})
                    {
                        auto candidate = base;
                        auto& policy = unit == 0 ? candidate.MGPU.Sun : unit == 1 ? candidate.MGPU.Particles :
                            candidate.MGPU.LocalShadows[unit - 2].Route;
                        policy = {route != 0, route != 2, interval};
                        append(std::move(candidate));
                    }
            // Whole-probe and face placement, then both distance partitions. Improvements seed the next neighborhood.
            for (int probe = 0; probe < base.MGPU.Reflections.Count; ++probe)
                for (unsigned mask : {0u, 63u, 21u, 42u, 1u, 2u, 4u, 8u, 16u, 32u})
                {
                    auto candidate = base;
                    candidate.MGPU.Reflections.Placement = 0;
                    candidate.MGPU.Reflections.SecondaryFaces[probe] = mask;
                    append(std::move(candidate));
                }
            for (int probe = 0; probe < base.MGPU.Reflections.Count; ++probe)
                for (unsigned face = 0; face < 6; ++face)
                {
                    auto candidate = base;
                    candidate.MGPU.Reflections.Placement = 0;
                    candidate.MGPU.Reflections.SecondaryFaces[probe] ^= 1u << face;
                    append(std::move(candidate));
                }
            for (int placement = 0; placement != 3; ++placement)
            {
                auto candidate = base;
                candidate.MGPU.Reflections.Placement = placement;
                append(std::move(candidate));
            }
            const auto vary = [&](auto member, const auto& values)
            {
                for (int value : values)
                {
                    auto candidate = base;
                    candidate.MGPU.Reflections.*member = value;
                    append(std::move(candidate));
                }
            };
            vary(&ReflectionSearchSettings::Count, std::array{0, 1, 2, 3, 4});
            vary(&ReflectionSearchSettings::Resolution, std::array{64, 128, 256, 512});
            vary(&ReflectionSearchSettings::FilterSamples, std::array{32, 64, 128, 256});
            vary(&ReflectionSearchSettings::DistanceMetres, std::array{5, 10, 20, 40, -1});
            vary(&ReflectionSearchSettings::Schedule, std::array{0, 1, 2, 3});
            vary(&ReflectionSearchSettings::Implementation, std::array{0, 1});
        }

        // A fixed minimum and relative tolerance avoid optimizing small timing fluctuations.
        static double Noise(double fps) { return (std::max)(0.25, 0.01 * fps); }
        bool AtMinimumHeight() const { return _coarse.Height <= 480; }

        bool AtMinimumTiers() const
        {
            for (size_t i = 0; i < _axes.size(); ++i)
                if (_coarse.Tiers[i] != _axes[i].MinTier) return false;
            return true;
        }

        void RestartCoarseTiers()
        {
            _coarse.Tiers.clear();
            for (const auto& axis : _axes)
                _coarse.Tiers.push_back(_profile == Profile::Quality && axis.Cpu ? axis.MinTier :
                    (std::clamp)(2, axis.MinTier, axis.MaxTier));
        }

        void AdvanceCoarse(const std::optional<Selection>& measured)
        {
            if (!AtMinimumTiers())
            {
                for (size_t i = 0; i < _axes.size(); ++i)
                    _coarse.Tiers[i] = (std::max)(_axes[i].MinTier, _coarse.Tiers[i] - 1);
            }
            else if (!AtMinimumHeight())
            {
                _coarse.Height = (std::max)(480, static_cast<int>(std::lround(0.85 * _coarse.Height)));
                _coarse.Width = (std::max)(1, static_cast<int>(std::lround(double(_width) * _coarse.Height / _height)));
                RestartCoarseTiers();
            }
            else
            {
                _phase = Phase::Disable;
                _offBase = measured.value_or(Selection{_coarse, 0, false, false});
                if (!measured)
                    for (auto it = _fallbackOptions.rbegin(); it != _fallbackOptions.rend(); ++it)
                        if (it->Settings == _coarse) { _offBase = *it; break; }
                _offGroup = OffGroup::Post;
                BuildDisableBatch();
            }
        }

        void SubmitCoarse(std::optional<Selection> measured)
        {
            const auto first = std::exchange(_coarseFirst, std::nullopt);
            if (!first && measured && measured->TargetReached)
            {
                _coarseFirst = measured;
                return;
            }
            if (first && measured)
            {
                measured->Stable = std::abs(first->AverageFps - measured->AverageFps) <= Noise(first->AverageFps);
                measured->AverageFps = (std::min)(first->AverageFps, measured->AverageFps);
                measured->TargetReached = measured->AverageFps >= Target(_profile);
                if (measured->TargetReached)
                {
                    StartOptimization(*measured, false);
                    return;
                }
            }
            if (measured) RememberFallback(*measured);
            AdvanceCoarse(measured);
        }

        void RequestOnce(const Candidate& candidate, bool requireFull)
        {
            auto key = candidate;
            if (key.MGPU.Enabled)
            {
                key.MGPU.Water = {}; key.MGPU.Grass = {}; key.MGPU.Sun = {}; key.MGPU.Particles = {};
                for (auto& light : key.MGPU.LocalShadows) light.Route = {};
                key.MGPU.Reflections = {};
            }
            if (_visited.insert(key).second)
            {
                _request = Request{candidate, requireFull};
                _explorePlacement = candidate.MGPU.Enabled;
            }
            else if (_phase == Phase::Coarse) AdvanceCoarse(std::nullopt);
        }

        bool RequestNeighbor(bool requireFull)
        {
            const bool optimize = _phase == Phase::Graphics || _phase == Phase::Cpu;
            while (_cursor < _batch.size())
            {
                const bool canPrune = _batchBest && (!optimize || ComfortablyAboveTarget(*_batchBest));
                if (optimize && canPrune && Rank(_batch[_cursor]) < Rank(_batchBest->Settings))
                {
                    _deferred.insert(_batch.begin() + _cursor, _batch.end());
                    return false;
                }
                RequestOnce(_batch[_cursor++], requireFull);
                if (_request) return true;
            }
            return false;
        }

        bool RequestDeferred()
        {
            while (!_deferred.empty())
            {
                const auto best = std::max_element(_deferred.begin(), _deferred.end(),
                    [&](const Candidate& a, const Candidate& b) { return Rank(a) < Rank(b); });
                // Keep lower-ranked probes deferred until the remaining finalist also fails.
                if (std::any_of(_finalists.begin(), _finalists.end(),
                    [&](const Selection& finalist) { return Rank(finalist.Settings) > Rank(*best); })) return false;
                const auto candidate = *best;
                _deferred.erase(best);
                RequestOnce(candidate, true);
                if (_request) return true;
            }
            return false;
        }

        std::array<int, 9> Rank(const Candidate& candidate) const
        {
            std::array<int, 9> rank{};
            for (size_t i = 0; i < _axes.size(); ++i)
            {
                const auto tier = candidate.Tiers[i];
                if (!tier) continue;
                ++rank[0];
                const bool cpu = _profile == Profile::Quality && _axes[i].Cpu;
                ++rank[(cpu ? 9 : 5) - tier];
            }
            return rank;
        }

        bool Better(const Selection& a, const Selection& b) const
        {
            if (a.TargetReached != b.TargetReached) return a.TargetReached;
            if (!a.TargetReached && std::abs(a.AverageFps - b.AverageFps) > Noise(b.AverageFps))
                return a.AverageFps > b.AverageFps;
            const auto aRank = Rank(a.Settings), bRank = Rank(b.Settings);
            if (aRank != bRank) return aRank > bRank;
            const auto aPixels = static_cast<long long>(a.Settings.Width) * a.Settings.Height;
            const auto bPixels = static_cast<long long>(b.Settings.Width) * b.Settings.Height;
            if (aPixels != bPixels) return aPixels > bPixels;
            if (a.Settings.MGPU.Enabled)
            {
                if (a.Settings.MGPU.Reflections.Quality() != b.Settings.MGPU.Reflections.Quality())
                    return a.Settings.MGPU.Reflections.Quality() > b.Settings.MGPU.Reflections.Quality();
                const auto cadence = [](const MGPUSettings& settings)
                {
                    const auto water = settings.Water.UpdateHz ? settings.Water.UpdateHz : 241;
                    const auto grass = settings.Grass.UpdateHz ? settings.Grass.UpdateHz : 241;
                    int interval = settings.Sun.Interval + settings.Particles.Interval;
                    for (const auto& light : settings.LocalShadows) interval += light.Route.Interval;
                    return std::make_tuple((std::min)(water, grass), water + grass, -interval);
                };
                if (cadence(a.Settings.MGPU) != cadence(b.Settings.MGPU))
                    return cadence(a.Settings.MGPU) > cadence(b.Settings.MGPU);
            }
            return a.AverageFps > b.AverageFps + Noise(b.AverageFps);
        }

        bool ComfortablyAboveTarget(const Selection& selection) const
        {
            return selection.AverageFps >= Target(_profile) + Noise(selection.AverageFps);
        }

        void RememberFallback(const Selection& measured)
        {
            if (measured.TargetReached) return;
            _fallbackOptions.push_back(measured);
            if (!_fallback || Better(measured, *_fallback)) _fallback = measured;
        }

        void StartOptimization(const Selection& start, bool fromOff)
        {
            _fromOff = fromOff;
            _finalists.push_back(start);
            _base = start;
            _phase = Phase::Graphics;
            BuildNeighborhood();
        }

        int RaisedTier(const Candidate& candidate, size_t axis) const
        {
            return (std::max)(_axes[axis].MinTier, candidate.Tiers[axis] + 1);
        }

        bool PrerequisitesActive(const Candidate& candidate, size_t axis) const
        {
            for (auto parent = _axes[axis].Prerequisite; parent; parent = _axes[*parent].Prerequisite)
                if (!candidate.Tiers[*parent]) return false;
            return true;
        }

        void QueueGroup(const std::vector<size_t>& axes, const Candidate& base, bool disable)
        {
            if (axes.empty()) return;
            auto candidate = base;
            for (const size_t axis : axes) candidate.Tiers[axis] = disable ? 0 : RaisedTier(base, axis);
            if (disable)
                for (size_t i = 0; i < _axes.size(); ++i)
                    if (!PrerequisitesActive(candidate, i)) candidate.Tiers[i] = 0;
            _batch.push_back(std::move(candidate));
        }

        void QueueHalves(const std::vector<size_t>& axes, const Candidate& base)
        {
            if (axes.size() < 2) return;
            const auto middle = axes.begin() + axes.size() / 2;
            const std::vector<size_t> left(axes.begin(), middle), right(middle, axes.end());
            QueueGroup(left, base, false);
            QueueGroup(right, base, false);
            QueueHalves(left, base);
            QueueHalves(right, base);
        }

        void BuildNeighborhood()
        {
            // shortcut: bounded groups can miss multi-axis optima; exhaustive search needs an explicit offline budget.
            _batch.clear();
            _cursor = 0;
            _batchBest.reset();
            const auto& candidate = _base->Settings;
            std::vector<size_t> raise, lower;
            for (size_t i = 0; i < _axes.size(); ++i)
            {
                if (_profile == Profile::Quality && _phase != Phase::Restore && _axes[i].Cpu != (_phase == Phase::Cpu)) continue;
                if (!PrerequisitesActive(candidate, i)) continue;
                if (candidate.Tiers[i] < _axes[i].MaxTier) raise.push_back(i);
                if (candidate.Tiers[i] > _axes[i].MinTier) lower.push_back(i);
            }
            QueueGroup(raise, candidate, false);
            QueueHalves(raise, candidate);
            std::vector<size_t> alternating[2];
            for (size_t i = 0; i < raise.size(); ++i) alternating[i % 2].push_back(raise[i]);
            for (const auto& group : alternating) QueueGroup(group, candidate, false);
            for (size_t i = 0; i < raise.size(); ++i)
            {
                QueueGroup({raise[i]}, candidate, false);
                if (raise.size() > 1)
                {
                    QueueGroup({raise[i], raise[(i + 1) % raise.size()]}, candidate, false);
                    QueueGroup({raise[i], raise[(i + raise.size() / 2) % raise.size()]}, candidate, false);
                }
                if (lower.empty()) continue;
                auto down = lower[i % lower.size()];
                if (down == raise[i]) down = lower[(i + 1) % lower.size()];
                if (down == raise[i]) continue;
                auto swap = candidate;
                swap.Tiers[raise[i]] = RaisedTier(candidate, raise[i]);
                --swap.Tiers[down];
                _batch.push_back(std::move(swap));
            }
            // Probe beyond a failed intermediate tier without assuming monotonic GPU/CPU costs.
            for (int tier = 2; tier <= 4; ++tier)
            {
                auto group = candidate;
                bool raised = false;
                for (const auto axis : raise)
                {
                    const auto targetTier = (std::clamp)(tier, _axes[axis].MinTier, _axes[axis].MaxTier);
                    if (targetTier <= candidate.Tiers[axis]) continue;
                    if (targetTier > RaisedTier(candidate, axis))
                    {
                        auto single = candidate;
                        single.Tiers[axis] = targetTier;
                        _batch.push_back(std::move(single));
                    }
                    group.Tiers[axis] = targetTier;
                    raised = true;
                }
                if (raised) _batch.push_back(std::move(group));
            }
            if (_phase == Phase::Graphics || _phase == Phase::Cpu)
            {
                const auto higherRank = [&](const Candidate& a, const Candidate& b) { return Rank(a) > Rank(b); };
                const auto baseRank = Rank(candidate);
                _batch.erase(std::remove_if(_batch.begin(), _batch.end(),
                    [&](const Candidate& item) { return Rank(item) < baseRank; }), _batch.end());
                std::stable_sort(_batch.begin(), _batch.end(), higherRank);
            }
        }

        void ConsiderNeighbor(const Selection& measured)
        {
            if (measured.TargetReached) _finalists.push_back(measured);
            else
            {
                RememberFallback(measured);
                if (_phase != Phase::Restore) return;
            }
            if (_phase == Phase::Restore && !measured.TargetReached &&
                measured.AverageFps < _base->AverageFps - Noise(_base->AverageFps)) return;
            if (Better(measured, *_base) && (!_batchBest || Better(measured, *_batchBest)))
            {
                _batchBest = measured;
            }
        }

        void FinishNeighborhood()
        {
            if (_batchBest)
            {
                if (_phase == Phase::Restore && _batchBest->TargetReached)
                {
                    StartOptimization(*_batchBest, true);
                    return;
                }
                if (_phase == Phase::Cpu) _phase = Phase::Graphics;
                _base = _batchBest;
                if (_phase == Phase::Restore) RememberFallback(*_base);
                BuildNeighborhood();
            }
            else if (_phase == Phase::Graphics && _profile == Profile::Quality)
            {
                _phase = Phase::Cpu;
                BuildNeighborhood();
            }
            else
            {
                _confirmingFallback = _phase == Phase::Restore;
                StartConfirmation();
            }
        }

        void BuildDisableBatch()
        {
            _batch.clear();
            _cursor = 0;
            _offOptions.clear();
            std::vector<size_t> disable;
            for (size_t i = 0; i < _axes.size(); ++i)
                if (_axes[i].Group == _offGroup && _offBase.Settings.Tiers[i] > 0) disable.push_back(i);
            for (size_t i = 0; i < disable.size(); ++i)
            {
                QueueGroup({disable[i]}, _offBase.Settings, true);
                for (size_t j = i + 1; j < disable.size(); ++j)
                    QueueGroup({disable[i], disable[j]}, _offBase.Settings, true);
            }
            if (disable.size() > 2) QueueGroup(disable, _offBase.Settings, true);
        }

        void ChooseDisableRepeat()
        {
            if (!_offOptions.empty())
            {
                auto best = _offOptions.begin();
                for (auto it = best + 1; it != _offOptions.end(); ++it)
                    if (Better(*it, *best)) best = it;
                _offRepeat = *best;
                _offOptions.erase(best);
                return;
            }
            _offGroup = static_cast<OffGroup>(static_cast<int>(_offGroup) + 1);
            if (_offGroup != OffGroup::Never) BuildDisableBatch();
            else if (_fallback)
            {
                _base = _fallback;
                _phase = Phase::Restore;
                BuildNeighborhood();
            }
            else _phase = Phase::Done;
        }

        void SubmitDisable(const std::optional<Selection>& measured)
        {
            const bool faster = measured && measured->AverageFps > _offBase.AverageFps + Noise(_offBase.AverageFps);
            if (_offRepeat)
            {
                const auto first = std::exchange(_offRepeat, std::nullopt);
                if (!faster) return;
                _offBase = *measured;
                _offBase.Stable = std::abs(first->AverageFps - measured->AverageFps) <= Noise(first->AverageFps);
                _offBase.AverageFps = (std::min)(first->AverageFps, measured->AverageFps);
                _offBase.TargetReached = _offBase.AverageFps >= Target(_profile);
                if (_offBase.TargetReached) StartOptimization(_offBase, true);
                else
                {
                    RememberFallback(_offBase);
                    BuildDisableBatch();
                }
            }
            else if (faster)
            {
                if (measured->TargetReached) _offRepeat = measured;
                else _offOptions.push_back(*measured);
            }
        }

        void StartConfirmation()
        {
            auto& options = _confirmingFallback ? _fallbackOptions : _finalists;
            if (!options.empty())
            {
                auto best = options.begin();
                for (auto it = best + 1; it != options.end(); ++it)
                    if (Better(*it, *best)) best = it;
                _confirm = *best;
                const auto candidate = best->Settings;
                options.erase(std::remove_if(options.begin(), options.end(),
                    [&](const Selection& entry) { return entry.Settings == candidate; }), options.end());
                if (_confirmingFallback && _confirmedFallback && !Better(*_confirm, *_confirmedFallback))
                {
                    _winner = _confirmedFallback;
                    _phase = Phase::Done;
                    return;
                }
            }
            else if (_confirmingFallback)
            {
                _winner = _confirmedFallback;
                _phase = Phase::Done;
                return;
            }
            else
            {
                _phase = _fromOff ? Phase::Disable : Phase::Coarse;
                if (_fromOff && _offGroup != OffGroup::Never) BuildDisableBatch();
                else if (_fromOff)
                {
                    _confirmingFallback = true;
                    StartConfirmation();
                }
                else AdvanceCoarse(std::nullopt);
                return;
            }
            _phase = Phase::Confirm;
        }

        void SubmitConfirmation(const std::optional<Selection>& measured)
        {
            const auto first = std::exchange(_confirm, std::nullopt);
            if (measured)
            {
                auto confirmed = *measured;
                confirmed.Stable = first->Stable &&
                    std::abs(first->AverageFps - measured->AverageFps) <= Noise(first->AverageFps);
                confirmed.AverageFps = (std::min)(first->AverageFps, measured->AverageFps);
                confirmed.TargetReached = confirmed.AverageFps >= Target(_profile) &&
                    (!confirmed.Settings.MGPU.Enabled || confirmed.Stable);
                if (confirmed.TargetReached)
                {
                    _winner = confirmed;
                    _phase = Phase::Done;
                    return;
                }
                if (_confirmingFallback)
                {
                    if (!_confirmedFallback || Better(confirmed, *_confirmedFallback)) _confirmedFallback = confirmed;
                }
                else
                {
                    if (_fromOff && _offBase.Settings == confirmed.Settings) _offBase = confirmed;
                    RememberFallback(confirmed);
                }
            }
            if (!_confirmingFallback && !_deferred.empty()) _phase = Phase::Recover;
            else StartConfirmation();
        }

        Profile _profile;
        std::vector<Axis> _axes;
        int _width, _height;
        Candidate _coarse;
        Phase _phase = Phase::Coarse;
        OffGroup _offGroup = OffGroup::Post;
        Selection _offBase{};
        bool _fromOff = false;
        bool _confirmingFallback = false;
        size_t _cursor = 0;
        std::set<Candidate> _visited, _deferred;
        std::vector<Candidate> _batch;
        std::vector<Selection> _finalists, _offOptions, _fallbackOptions;
        std::optional<Request> _request;
        std::optional<size_t> _waterAxis, _grassAxis;
        std::array<std::optional<size_t>, 3> _scheduledAxes;
        bool _explorePlacement = false;
        size_t _placementCursor = 0;
        std::vector<Candidate> _placements;
        std::set<Candidate> _placementVisited;
        std::optional<Request> _placementRequest;
        std::optional<Selection> _placementBest, _placementAnchor;
        std::optional<Selection> _coarseFirst, _neighborFirst, _base, _batchBest, _fallback, _offRepeat, _confirm, _confirmedFallback, _winner;
    };
}
