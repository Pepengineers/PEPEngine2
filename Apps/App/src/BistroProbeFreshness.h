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
        struct Deadline { uint64_t Version; Time Due; std::array<bool, 2> Missed{}; };
        struct Face { std::deque<Deadline> Pending, Overdue; };
        std::map<uint64_t, std::array<Face, 6>> _faces;

        static double Milliseconds(Time begin, Time end)
        {
            return (std::max)(0.0, std::chrono::duration<double, std::milli>(end - begin).count());
        }

    public:
        // Both profiles judge the same observation, including versions superseded before first use.
        static constexpr std::array<double, 2> ToleranceMs{2000. / 60, 1000. / 30};
        uint64_t Due = 0, Used = 0, AgeSamples = 0;
        std::array<uint64_t, 2> Missed{};
        double MaxLatenessMs = 0, MeanAgeSumMs = 0, MaxAgeMs = 0;

        void Observe(const BistroRender::ReflectionFrame& frame, Time now)
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
                        auto& deadline = pending.front();
                        const auto late = Milliseconds(deadline.Due, now);
                        for (size_t profile = 0; profile < Missed.size(); ++profile)
                            if (!deadline.Missed[profile] && (used.Version > deadline.Version || late > ToleranceMs[profile]))
                            {
                                ++Missed[profile];
                                deadline.Missed[profile] = true;
                            }
                        if (used.Version == deadline.Version)
                        {
                            ++Used;
                            MaxLatenessMs = (std::max)(MaxLatenessMs, late);
                        }
                        else if (used.Version < deadline.Version)
                        {
                            if (!deadline.Missed[0] || !deadline.Missed[1]) break;
                            overdue.push_back(deadline);
                        }
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
