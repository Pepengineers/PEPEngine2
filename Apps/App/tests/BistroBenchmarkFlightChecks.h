#pragma once

#include "../src/BistroBenchmarkFlight.h"

#include <iostream>

namespace
{
    using namespace BistroBenchmarkFlight;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    bool Near(double a, double b)
    {
        return std::abs(a - b) < 1e-9;
    }

    void TestRoute()
    {
        const auto start = SampleRoute(0);
        const auto finish = SampleRoute(1);
        Require(start.Segment == finish.Segment && start.Position == finish.Position && start.Target == finish.Target,
            "Exterior flight must close at the warmup pose");
        auto previous = start;
        for (int i = 1; i <= 1000; ++i)
        {
            const auto current = SampleRoute(i / 1000.0);
            const auto travel = std::sqrt(BistroRoutes::HorizontalDistanceSquared(previous.Position, current.Position));
            auto before = previous.Target - previous.Position;
            auto after = current.Target - current.Position;
            before.Normalize();
            after.Normalize();
            Require(travel > 20 && travel < 40, "Flight lost arc-length speed along the exterior route");
            Require(before.Dot(after) > 0.8f, "Flight camera aim is discontinuous");
            previous = current;
        }
    }

    void TestWarmupAndFlight()
    {
        Sampler sampler;
        const auto stationary = SampleRoute(0);
        double now = 20;
        for (std::uint64_t i = 0; i < 39; ++i)
        {
            const auto pose = sampler.BeginFrame(i, now);
            Require(pose.Position == stationary.Position && pose.Target == stationary.Target, "Warmup camera moved");
            now += 0.25;
            sampler.EndFrame(now);
        }
        Require(sampler.State() == Phase::Warmup && !sampler.StableWarmupFps(), "Warmup ended before 10 seconds");
        sampler.BeginFrame(39, now);
        now += 0.5;
        sampler.EndFrame(now);
        Require(sampler.State() == Phase::WarmupReady && sampler.Samples().empty(), "Warmup frame leaked into flight");
        const auto warmup = sampler.StableWarmupFps();
        Require(warmup && Near(*warmup, 4), "Stationary warmup did not report stable cadence");

        sampler.StartFlight();
        for (std::uint64_t i = 0; i < 102; ++i)
        {
            const double frameStart = i == 0 ? now : now + 0.125;
            const auto pose = sampler.BeginFrame(1000 + i, frameStart);
            if (i == 0)
                Require(pose.Position == stationary.Position && pose.Target == stationary.Target, "Flight start broke warmup continuity");
            now += i < 100 ? 0.5 : i == 100 ? 4 : 47;
            sampler.EndFrame(now);
        }
        Require(sampler.State() == Phase::Complete, "Crossing frame did not complete the flight");
        sampler.BeginFrame(1102, now);
        sampler.EndFrame(now + 1);
        const auto& samples = sampler.Samples();
        Require(samples.size() == 102 && samples.front().FrameId == 1000 && samples.front().Elapsed == 0,
            "Wrong initial flight sample or sample count");
        const auto& last = samples.back();
        Require(last.FrameId == 1101 && Near(last.Elapsed, 54.125) && Near(last.Progress, 0.54125) &&
            Near(last.Duration, 47) && Near(last.Fps, 1.0 / 47), "Flight lost or truncated the final frame");

        const auto metrics = sampler.Summarize();
        Require(metrics.FrameCount == 102 && Near(metrics.Duration, 101) && Near(metrics.AverageFps, 102.0 / 101),
            "Average FPS did not use all rendered frames and actual duration");
        Require(Near(metrics.MinFrameTime, 0.5) && Near(metrics.MaxFrameTime, 47) &&
            Near(metrics.P50FrameTime, 0.5) && Near(metrics.P95FrameTime, 0.5) && Near(metrics.P99FrameTime, 4),
            "Frametime extrema or nearest-rank percentiles are wrong");
        Require(Near(metrics.OnePercentLow, 2.0 / 51) && Near(metrics.PointOnePercentLow, 1.0 / 47),
            "FPS lows did not use the mean duration of the slowest ceil(N * fraction) frames");
    }

    void TestUnstableWarmup()
    {
        Sampler sampler;
        double now = 0;
        std::uint64_t frameId = 0;
        while (sampler.State() == Phase::Warmup)
        {
            sampler.BeginFrame(frameId++, now);
            now += now < 8 ? 0.125 : 0.25;
            sampler.EndFrame(now);
        }
        Require(!sampler.StableWarmupFps() && sampler.Samples().empty(), "Unstable warmup qualified for early rejection");
    }
}

inline void CheckBistroBenchmarkFlight()
{
    TestRoute();
    TestWarmupAndFlight();
    TestUnstableWarmup();
    std::cout << "Bistro benchmark flight checks passed\n";
}
