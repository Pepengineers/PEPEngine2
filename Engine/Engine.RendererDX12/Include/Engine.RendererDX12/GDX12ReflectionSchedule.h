#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace GDX12ReflectionSchedule
{
    enum class Schedule { All, WholeProbe, FaceEach, OneFace };
    struct Policy
    {
        Schedule Update = Schedule::All;
        float FullUpdateDistanceMetres = 0; // 0 disables distance scheduling; -1 means always full.
    };
    struct Probe
    {
        std::uint64_t Id;
        float Distance;
    };
    struct Work
    {
        unsigned Faces = 0;
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

    class Scheduler
    {
    public:
        void Reset() { _states.clear(); _cursor = 0; }

        std::vector<Work> Select(const std::vector<Probe>& probes, const Policy& policy)
        {
            if (!std::isfinite(policy.FullUpdateDistanceMetres) || policy.FullUpdateDistanceMetres < -1 ||
                static_cast<unsigned>(policy.Update) > 3)
                throw std::invalid_argument("Invalid reflection update policy");
            for (size_t i = 0; i < probes.size(); ++i)
            {
                if (!std::isfinite(probes[i].Distance) || probes[i].Distance < 0)
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
            return work;
        }

    private:
        struct State { std::uint64_t Id; unsigned NextFace = 0; bool Near = false; };
        std::vector<State> _states;
        std::uint64_t _cursor = 0;
    };
}
