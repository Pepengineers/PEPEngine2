#pragma once

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
    enum class Profile { Performance, Quality };
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
        bool operator==(const Candidate& other) const
        {
            return std::tie(Width, Height, Tiers) == std::tie(other.Width, other.Height, other.Tiers);
        }
        bool operator<(const Candidate& other) const
        {
            return std::tie(Width, Height, Tiers) < std::tie(other.Width, other.Height, other.Tiers);
        }
    };

    struct Request { Candidate Settings; bool RequireFull; };
    struct Result { std::optional<double> AverageFps; std::optional<double> WarmupFps; bool Valid = true; };
    struct Selection { Candidate Settings; double AverageFps; bool TargetReached; bool Stable; };

    class Search
    {
    public:
        Search(Profile profile, std::vector<Axis> axes, int width, int height)
            : _profile(profile), _axes(std::move(axes)), _width(width), _height(height)
        {
            if (width <= 0 || height <= 0) throw std::invalid_argument("Invalid quality search dimensions");
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
            RestartCoarseTiers();
        }

        static double Target(Profile profile) { return profile == Profile::Performance ? 60.0 : 28.0; }

        std::optional<Request> Next()
        {
            while (!_request)
            {
                switch (_phase)
                {
                case Phase::Coarse:
                    if (_coarseFirst) _request = Request{_coarseFirst->Settings, true};
                    else RequestOnce(_coarse, AtMinimumTiers());
                    break;
                case Phase::Graphics:
                case Phase::Cpu:
                case Phase::Restore:
                    if (!RequestNeighbor(_phase == Phase::Restore)) FinishNeighborhood();
                    break;
                case Phase::Disable:
                    if (_offRepeat) _request = Request{_offRepeat->Settings, true};
                    else if (!RequestNeighbor(true)) ChooseDisableRepeat();
                    break;
                case Phase::Confirm:
                    _request = Request{_confirm->Settings, true};
                    break;
                case Phase::Done:
                    return std::nullopt;
                }
            }
            return _request;
        }

        void Submit(Result result)
        {
            if (!_request) throw std::logic_error("Quality search has no outstanding request");
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
                if (measured) ConsiderNeighbor(*measured);
                break;
            case Phase::Disable:
                SubmitDisable(measured);
                break;
            case Phase::Confirm:
                SubmitConfirmation(measured);
                break;
            case Phase::Done:
                throw std::logic_error("Quality search is already complete");
            }
        }

        void Cancel()
        {
            _request.reset();
            _phase = Phase::Done;
        }

        const std::optional<Selection>& Winner() const { return _winner; }

    private:
        enum class Phase { Coarse, Graphics, Cpu, Disable, Restore, Confirm, Done };

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
            if (_visited.insert(candidate).second) _request = Request{candidate, requireFull};
            else if (_phase == Phase::Coarse) AdvanceCoarse(std::nullopt);
        }

        bool RequestNeighbor(bool requireFull)
        {
            const bool optimize = _phase == Phase::Graphics || _phase == Phase::Cpu;
            while (_cursor < _batch.size())
            {
                const bool canPrune = _batchBest && (!optimize || ComfortablyAboveTarget(*_batchBest));
                if (_phase != Phase::Disable && _cursor == _groupCount && canPrune) return false;
                if (optimize && canPrune && Rank(_batch[_cursor]) < Rank(_batchBest->Settings)) return false;
                RequestOnce(_batch[_cursor++], requireFull);
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
            return aRank == bRank ? a.AverageFps > b.AverageFps + Noise(b.AverageFps) : aRank > bRank;
        }

        bool ComfortablyAboveTarget(const Selection& selection) const
        {
            return selection.AverageFps >= Target(_profile) + Noise(selection.AverageFps);
        }

        void RememberFallback(const Selection& measured)
        {
            if (_profile != Profile::Quality || !AtMinimumHeight() || measured.TargetReached) return;
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
            _batch.clear();
            _cursor = 0;
            _batchBest.reset();
            const auto& candidate = _base->Settings;
            std::vector<size_t> raise, lower;
            for (size_t i = 0; i < _axes.size(); ++i)
            {
                if (_phase == Phase::Restore && candidate.Tiers[i] != 0) continue;
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
            _groupCount = _batch.size();
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
            if (_phase == Phase::Graphics || _phase == Phase::Cpu)
            {
                const auto higherRank = [&](const Candidate& a, const Candidate& b) { return Rank(a) > Rank(b); };
                const auto endOfGroups = _batch.begin() + _groupCount;
                std::stable_sort(_batch.begin(), endOfGroups, higherRank);
                std::stable_sort(endOfGroups, _batch.end(), higherRank);
                const auto baseRank = Rank(candidate);
                _batch.erase(std::remove_if(endOfGroups, _batch.end(),
                    [&](const Candidate& item) { return Rank(item) < baseRank; }), _batch.end());
            }
        }

        void ConsiderNeighbor(const Selection& measured)
        {
            if (measured.TargetReached) _finalists.push_back(measured);
            else if (_phase != Phase::Restore) return;
            if (_phase == Phase::Restore && !measured.TargetReached &&
                measured.AverageFps < _base->AverageFps - Noise(_base->AverageFps)) return;
            if (Better(measured, *_base) && (!_batchBest || Better(measured, *_batchBest)))
            {
                _batchBest = measured;
                // When raising every eligible axis works, partial raises and swaps cannot improve its rank.
                if (_phase != Phase::Restore && ComfortablyAboveTarget(measured) && measured.Settings == _batch.front())
                    _cursor = _batch.size();
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
                if (_phase == Phase::Cpu)
                    for (const auto& axis : _axes)
                        if (axis.Prerequisite && !_base->Settings.Tiers[*axis.Prerequisite] &&
                            _batchBest->Settings.Tiers[*axis.Prerequisite]) _phase = Phase::Graphics;
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
                confirmed.TargetReached = confirmed.AverageFps >= Target(_profile);
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
            StartConfirmation();
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
        size_t _cursor = 0, _groupCount = 0;
        std::set<Candidate> _visited;
        std::vector<Candidate> _batch;
        std::vector<Selection> _finalists, _offOptions, _fallbackOptions;
        std::optional<Request> _request;
        std::optional<Selection> _coarseFirst, _base, _batchBest, _fallback, _offRepeat, _confirm, _confirmedFallback, _winner;
    };
}
