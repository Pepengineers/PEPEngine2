#pragma once

#include "BistroRoutes.h"
#include "BistroArch.h"
#include "BistroWaterLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <vector>

namespace BistroBenchmarkFlight
{
    inline constexpr char RouteVersion[] = "bistro-people-exterior-v1";
    inline constexpr double WarmupSeconds = 10;
    inline constexpr double FlightSeconds = 100;

    struct Pose
    {
        Vector3 Position;
        Vector3 Target;
        size_t Segment;
    };

    namespace Detail
    {
        struct LookKey
        {
            Vector3 Approach;
            Vector3 Target;
            double Radius;
        };

        inline const std::array<LookKey, 5> LookKeys{{
            {{-1396.653f, 0, -815.225f}, BistroWater::Center + Vector3(0, 40, 0), 400},
            {{-288.780f, 0, -802.709f}, {-480, 95, -430}, 250}, // Terrace embers
            {{-453.336f, 0, -416.980f}, {-170, 145, -170}, 300}, // Entrance fountain
            {{-262.637f, 0, 235.045f}, {-40.9755f, 107.503f, 356.486f}, 320}, // Terrace probe/table
            {{1144.871f, 0, 1207.097f}, BistroArch::GrassPosition + Vector3(0, 100, 0), 330},
        }};

        struct Route
        {
            const BistroRoutes::Network& People = BistroRoutes::People();
            std::vector<double> Distances{0};
            std::array<double, LookKeys.size()> KeyDistances{};

            Route()
            {
                for (const auto& edge : People.Edges)
                    Distances.push_back(Distances.back() + edge.Length());
                for (size_t i = 0; i < LookKeys.size(); ++i)
                {
                    const auto approach = LookKeys[i].Approach;
                    const auto closest = std::min_element(People.Nodes.begin(), People.Nodes.end(), [&](Vector3 a, Vector3 b)
                    {
                        return BistroRoutes::HorizontalDistanceSquared(a, approach) <
                            BistroRoutes::HorizontalDistanceSquared(b, approach);
                    });
                    KeyDistances[i] = Distances[closest - People.Nodes.begin()];
                }
            }

            double Length() const { return Distances.back(); }

            BistroRoutes::Cursor CursorAt(double distance) const
            {
                distance = std::fmod(distance, Length());
                if (distance < 0) distance += Length();
                const auto edge = static_cast<size_t>(
                    std::upper_bound(Distances.begin(), Distances.end() - 1, distance) - Distances.begin() - 1);
                return {edge, static_cast<float>(distance - Distances[edge])};
            }
        };
    }

    inline Pose SampleRoute(double progress)
    {
        static const Detail::Route route;
        const auto u = std::clamp(progress, 0.0, 1.0);
        const double distance = route.KeyDistances[1] + (u < 1 ? u * route.Length() : 0); // Terrace approach
        const auto cursor = route.CursorAt(distance);
        auto position = route.People.Position(cursor);
        const auto before = route.People.Position(route.CursorAt(distance - 120));
        const auto after = route.People.Position(route.CursorAt(distance + 120));
        position.y = (before.y + 2 * position.y + after.y) * 0.25f + 200;

        auto direction = after - before;
        direction.y = -32;
        direction.Normalize();
        for (size_t i = 0; i < Detail::LookKeys.size(); ++i)
        {
            const auto& key = Detail::LookKeys[i];
            const double offset = std::remainder(distance - route.KeyDistances[i], route.Length());
            if (std::abs(offset) >= key.Radius) continue;
            const float blend = static_cast<float>(1 - std::abs(offset) / key.Radius);
            auto aim = key.Target - route.People.Position(route.CursorAt(route.KeyDistances[i]));
            aim.y -= 200;
            aim.Normalize();
            direction = Vector3::Lerp(direction, aim, blend * blend * (3 - 2 * blend));
        }
        return {position, position + direction * 500, cursor.Edge};
    }

    enum class Phase { Warmup, WarmupReady, Flight, Complete };

    struct FrameSample
    {
        std::uint64_t FrameId;
        double Elapsed;
        double Progress;
        size_t Segment;
        double Duration;
        double Fps;
    };

    struct Metrics
    {
        size_t FrameCount;
        double Duration;
        double AverageFps;
        double MinFrameTime;
        double MaxFrameTime;
        double P50FrameTime;
        double P95FrameTime;
        double P99FrameTime;
        double OnePercentLow;
        double PointOnePercentLow;
    };

    class Sampler
    {
    public:
        Phase State() const { return _phase; }
        const std::vector<FrameSample>& Samples() const { return _samples; }

        Pose BeginFrame(std::uint64_t frameId, double nowSeconds)
        {
            if (_pending) throw std::logic_error("Previous Bistro flight frame has no completion");
            if (!std::isfinite(nowSeconds)) throw std::invalid_argument("Invalid Bistro frame timestamp");
            if (_phase == Phase::Complete) return SampleRoute(1);
            if (_phase == Phase::WarmupReady) return SampleRoute(0);
            if (!_phaseStart)
            {
                _phaseStart = nowSeconds;
                _lastCompletion = nowSeconds;
            }
            if (nowSeconds < _lastCompletion) throw std::invalid_argument("Bistro frame clock went backwards");
            const double elapsed = nowSeconds - *_phaseStart;
            const double progress = _phase == Phase::Flight ? (std::min)(elapsed / FlightSeconds, 1.0) : 0;
            const auto pose = SampleRoute(progress);
            _pending = FrameSample{frameId, elapsed, progress, pose.Segment, 0, 0};
            _frameStart = nowSeconds;
            return pose;
        }

        void EndFrame(double nowSeconds)
        {
            if (!_pending) return;
            if (!std::isfinite(nowSeconds) || nowSeconds < _frameStart || nowSeconds <= _lastCompletion)
                throw std::invalid_argument("Invalid Bistro frame completion timestamp");
            const double elapsed = nowSeconds - *_phaseStart;
            const double duration = nowSeconds - _lastCompletion;
            if (_phase == Phase::Flight)
            {
                _pending->Duration = duration;
                _pending->Fps = 1 / duration;
                _samples.push_back(*_pending);
                if (elapsed >= FlightSeconds) _phase = Phase::Complete;
            }
            else
            {
                // Use two late, complete windows for a conservative stationary warmup estimate.
                for (size_t i = 0; i < _warmup.size(); ++i)
                {
                    const double from = WarmupSeconds - 4 + 2 * i;
                    if (_lastCompletion - *_phaseStart >= from && elapsed <= from + 2)
                    {
                        auto& window = _warmup[i];
                        ++window.Count;
                        window.Duration += duration;
                        window.MaxDuration = (std::max)(window.MaxDuration, duration);
                    }
                }
                if (elapsed >= WarmupSeconds) _phase = Phase::WarmupReady;
            }
            _lastCompletion = nowSeconds;
            _pending.reset();
        }

        std::optional<double> StableWarmupFps() const
        {
            if (_phase == Phase::Warmup) return std::nullopt;
            for (const auto& window : _warmup)
                if (window.Count < 4 || window.Duration < 1.5 || window.MaxDuration > 2 * window.Duration / window.Count)
                    return std::nullopt;
            const double earlyFps = _warmup[0].Count / _warmup[0].Duration;
            const double lateFps = _warmup[1].Count / _warmup[1].Duration;
            if (std::abs(earlyFps - lateFps) > 0.1 * (std::max)(earlyFps, lateFps)) return std::nullopt;
            return (_warmup[0].Count + _warmup[1].Count) / (_warmup[0].Duration + _warmup[1].Duration);
        }

        void StartFlight()
        {
            if (_phase != Phase::WarmupReady) throw std::logic_error("Bistro flight requires a completed warmup");
            _samples.reserve(12000);
            _phaseStart.reset();
            _phase = Phase::Flight;
        }

        Metrics Summarize() const
        {
            if (_phase != Phase::Complete || _samples.empty()) throw std::logic_error("Bistro flight is incomplete");
            std::vector<double> durations;
            durations.reserve(_samples.size());
            for (const auto& sample : _samples) durations.push_back(sample.Duration);
            const double duration = std::accumulate(durations.begin(), durations.end(), 0.0);
            std::sort(durations.begin(), durations.end());
            const size_t count = durations.size();
            // Nearest-rank percentiles and the reciprocal of the mean of the slowest ceil(N * fraction) samples.
            const auto percentile = [&](size_t percent) { return durations[(percent * count - 1) / 100]; };
            const auto low = [&](size_t divisor)
            {
                const size_t tail = 1 + (count - 1) / divisor;
                return tail / std::accumulate(durations.end() - tail, durations.end(), 0.0);
            };
            return {count, duration, count / duration, durations.front(), durations.back(),
                percentile(50), percentile(95), percentile(99), low(100), low(1000)};
        }

    private:
        struct WarmupWindow
        {
            size_t Count = 0;
            double Duration = 0;
            double MaxDuration = 0;
        };

        Phase _phase = Phase::Warmup;
        std::optional<double> _phaseStart;
        double _frameStart = 0;
        double _lastCompletion = 0;
        std::optional<FrameSample> _pending;
        std::array<WarmupWindow, 2> _warmup{};
        std::vector<FrameSample> _samples;
    };
}
