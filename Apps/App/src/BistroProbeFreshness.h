#pragma once

#include "Techniques/ReflectionFrame.h"
#include <algorithm>
#include <deque>
#include <map>

namespace BistroPlacement
{
    class ProbeFreshness
    {
        using Time = std::chrono::steady_clock::time_point;
        struct Deadline { uint64_t Version; Time Due; };
        struct Face { std::deque<Deadline> Pending, Overdue; };
        std::map<uint64_t, std::array<Face, 6>> _faces;

        static double Milliseconds(Time begin, Time end)
        {
            return (std::max)(0.0, std::chrono::duration<double, std::milli>(end - begin).count());
        }

    public:
        uint64_t Due = 0, Used = 0, Missed = 0, AgeSamples = 0;
        double MaxLatenessMs = 0, MeanAgeSumMs = 0, MaxAgeMs = 0;

        void BeginWindow() { _faces.clear(); }

        void Observe(const BistroRender::ReflectionFrame& frame, Time now, double toleranceMs)
        {
            for (const auto& probe : frame.Probes)
            {
                auto& faces = _faces[probe.Identity];
                for (size_t face = 0; face < faces.size(); ++face)
                {
                    auto& [pending, overdue] = faces[face];
                    const auto& used = probe.Used[face];
                    if (probe.DueFaces & (1u << face))
                    {
                        pending.push_back({frame.Version, now});
                        ++Due;
                    }
                    while (!overdue.empty() && overdue.front().Version <= used.Version)
                    {
                        if (overdue.front().Version == used.Version)
                        {
                            ++Used;
                            MaxLatenessMs = (std::max)(MaxLatenessMs, Milliseconds(overdue.front().Due, now));
                        }
                        overdue.pop_front();
                    }
                    while (!pending.empty())
                    {
                        const auto [version, due] = pending.front();
                        const auto late = Milliseconds(due, now);
                        if (used.Version == version)
                        {
                            ++Used;
                            Missed += late > toleranceMs;
                            MaxLatenessMs = (std::max)(MaxLatenessMs, late);
                        }
                        else if (used.Version > version)
                            ++Missed;
                        else if (late > toleranceMs)
                        {
                            ++Missed;
                            overdue.push_back(pending.front());
                        }
                        else
                            break;
                        pending.pop_front();
                    }
                    if (used.Version)
                    {
                        const auto age = Milliseconds(used.Captured, now);
                        ++AgeSamples;
                        MeanAgeSumMs += age;
                        MaxAgeMs = (std::max)(MaxAgeMs, age);
                    }
                }
            }
        }
    };
}
