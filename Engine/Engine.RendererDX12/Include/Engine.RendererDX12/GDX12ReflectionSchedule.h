#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace GDX12ReflectionSchedule
{
    enum class Schedule { All, WholeProbe, FaceEach, OneFace };
    enum class Placement { Fixed, NearPrimary, NearSecondary };
    struct Policy
    {
        Schedule Update = Schedule::All;
        float FullUpdateDistanceMetres = 0; // 0 disables distance scheduling; -1 means always full.
        Placement Gpu = Placement::Fixed;
    };
    struct Probe
    {
        std::uint64_t Id;
        float Distance;
        unsigned SecondaryFaces;
    };
    struct Work
    {
        unsigned Faces = 0, SecondaryFaces = 0;
        bool Near = false;
    };

    inline float DistanceToBox(const std::array<float,3>& camera, const std::array<float,3>& minimum,
        const std::array<float,3>& maximum)
    {
        float squared = 0;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const auto delta = (std::max)({minimum[axis] - camera[axis], 0.0f, camera[axis] - maximum[axis]});
            squared += delta * delta;
        }
        return std::sqrt(squared);
    }

    inline Schedule Canonical(Schedule value, size_t count)
    {
        if (!count) return Schedule::All;
        if (count == 1 && value == Schedule::WholeProbe) return Schedule::All;
        if (count == 1 && value == Schedule::OneFace) return Schedule::FaceEach;
        return value;
    }

    // The schedule advances independently of completion. A slow producer cannot move its deadline.
    class PublicationClock
    {
    public:
        void Reset() { *this = {}; }
        void ScheduleFaces(unsigned mask, double sourceTime)
        {
            if (mask > 63 || !std::isfinite(sourceTime) || sourceTime < _lastTime)
                throw std::invalid_argument("Invalid reflection source time");
            _lastTime = sourceTime;
            for (unsigned face = 0; face < 6; ++face)
                if (mask & (1u << face)) _pending[face] = sourceTime;
            _mask |= mask;
            if (_mask == 63)
            {
                _due.push_back({_pending, sourceTime});
                _mask = 0;
            }
        }
        void Publish(const std::array<double, 6>& sources, double completedTime)
        {
            if (!std::isfinite(completedTime) || completedTime < _lastTime)
                throw std::invalid_argument("Invalid reflection completion time");
            for (double source : sources)
                if (!std::isfinite(source) || source < 0 || source > completedTime)
                    throw std::invalid_argument("Invalid reflection publication");
            _displayed = sources;
            _ready = true;
            bool completed = false;
            while (!_due.empty() && MatchesExpected(_due.front().Sources))
            {
                const auto lag = completedTime - _due.front().Deadline;
                _completedLag = completed ? (std::max)(_completedLag, lag) : lag;
                completed = true;
                _due.pop_front();
            }
        }
        bool Ready() const { return _ready; }
        double AdditionalLag(double now) const
        {
            return _due.empty() ? _completedLag : (std::max)(_completedLag, now - _due.front().Deadline);
        }
        double FullAge(double now, unsigned face) const
        {
            if (face >= 6) throw std::out_of_range("Reflection face");
            return _ready ? (std::max)(0.0, now - _displayed[face]) : 0;
        }
    private:
        bool MatchesExpected(const std::array<double, 6>& expected) const
        {
            if (!_ready) return false;
            for (unsigned face = 0; face < 6; ++face)
                if (_displayed[face] < expected[face]) return false;
            return true;
        }
        struct Publication { std::array<double, 6> Sources; double Deadline; };
        std::deque<Publication> _due;
        std::array<double, 6> _pending{}, _displayed{};
        unsigned _mask = 0;
        double _lastTime = 0, _completedLag = 0;
        bool _ready = false;
    };

    class Scheduler
    {
    public:
        void Reset() { _states.clear(); _cursor = 0; }

        std::vector<Work> Select(const std::vector<Probe>& probes, const Policy& policy)
        {
            if (!std::isfinite(policy.FullUpdateDistanceMetres) || policy.FullUpdateDistanceMetres < -1 ||
                static_cast<unsigned>(policy.Update) > 3 || static_cast<unsigned>(policy.Gpu) > 2)
                throw std::invalid_argument("Invalid reflection update policy");
            for (size_t i = 0; i < probes.size(); ++i)
            {
                if (!std::isfinite(probes[i].Distance) || probes[i].Distance < 0 || probes[i].SecondaryFaces > 63)
                    throw std::invalid_argument("Invalid reflection scheduling input");
                for (size_t j = 0; j < i; ++j)
                    if (probes[i].Id == probes[j].Id) throw std::invalid_argument("Duplicate reflection probe identity");
            }
            bool changed = probes.size() != _states.size();
            if (!changed)
                for (size_t i = 0; i < probes.size(); ++i) changed |= probes[i].Id != _states[i].Id;
            if (changed)
            {
                Reset();
                for (const auto& probe : probes) _states.push_back({probe.Id});
            }
            std::vector<Work> work(probes.size());
            std::vector<size_t> distant;
            const float radius = policy.FullUpdateDistanceMetres * 100;
            for (size_t i = 0; i < probes.size(); ++i)
            {
                auto& state = _states[i];
                state.Near = radius < 0 || (radius > 0 && probes[i].Distance <= (state.Near ? radius * 1.2f : radius));
                work[i].Near = state.Near;
                if (state.Near) work[i].Faces = 63;
                else distant.push_back(i);
            }
            const auto oneFace = [&](size_t i)
            {
                auto& face = _states[i].NextFace;
                work[i].Faces = 1u << face;
                face = (face + 1) % 6;
            };
            if (!distant.empty())
            {
                const auto update = Canonical(policy.Update, distant.size());
                switch (update)
                {
                case Schedule::All:
                    for (auto i : distant) work[i].Faces = 63;
                    break;
                case Schedule::WholeProbe:
                    work[distant[_cursor % distant.size()]].Faces = 63;
                    ++_cursor;
                    break;
                case Schedule::FaceEach:
                    for (auto i : distant) oneFace(i);
                    break;
                case Schedule::OneFace:
                    oneFace(distant[_cursor % distant.size()]);
                    ++_cursor;
                    break;
                }
            }
            for (size_t i = 0; i < probes.size(); ++i)
            {
                unsigned mask = probes[i].SecondaryFaces;
                if (policy.Gpu == Placement::NearPrimary) mask = work[i].Near ? 0u : 63u;
                if (policy.Gpu == Placement::NearSecondary) mask = work[i].Near ? 63u : 0u;
                work[i].SecondaryFaces = work[i].Faces & mask;
            }
            return work;
        }

    private:
        struct State { std::uint64_t Id; unsigned NextFace = 0; bool Near = false; };
        std::vector<State> _states;
        std::uint64_t _cursor = 0;
    };
}
