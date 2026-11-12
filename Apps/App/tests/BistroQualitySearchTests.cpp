#include "../src/BistroQualitySearch.h"

#include <iostream>
#include <limits>
#include <map>

namespace
{
    using namespace BistroQualitySearch;

    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    template<class Measure>
    Selection Run(Search& search, Measure&& measure)
    {
        std::map<Candidate, int> visits;
        size_t requests = 0;
        while (auto request = search.Next())
        {
            Require(++requests < (request->Settings.MGPU.Enabled ? 2000u : 200u), "Synthetic search did not terminate");
            const int visit = ++visits[request->Settings];
            Require(visit == 1 || request->RequireFull, "Search duplicated a warmup-only candidate");
            search.Submit(measure(*request, visit));
        }
        Require(search.Winner().has_value(), "Search failed to confirm a selection");
        Require(visits[search.Winner()->Settings] >= 2, "Winner was not confirmed by a separate full run");
        return *search.Winner();
    }

    void TestQualityBeforeSpeed()
    {
        Search performance(Profile::Performance, {{}}, 1280, 800);
        const auto result = Run(performance, [](const Request& request, int)
        {
            constexpr double fps[] = {0, 110, 85, 65, 59};
            return Result{fps[request.Settings.Tiers[0]], std::nullopt};
        });
        Require(result.TargetReached && result.AverageFps == 65 && result.Settings.Tiers[0] == 3 &&
            result.Settings.Height == 800, "Performance preferred excess FPS over higher quality");

        Search quality(Profile::Quality, {{}, {OffGroup::Never, true}}, 1280, 800);
        const auto graphics = Run(quality, [](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            const double fps = tiers[0] < 4 ? 55 : tiers[1] < 3 ? 29 : 25;
            return Result{fps, std::nullopt};
        });
        Require(graphics.TargetReached && graphics.Settings.Tiers == std::vector<int>({4, 2}),
            "Quality traded graphics for CPU density");
    }

    void TestWarmupAndResolution()
    {
        Search search(Profile::Performance, {{OffGroup::Post}, {OffGroup::Simulation}}, 1280, 800);
        std::vector<int> lowHeights;
        const auto result = Run(search, [&](const Request& request, int visit)
        {
            const auto& settings = request.Settings;
            if (settings.Tiers == std::vector<int>({2, 2}) && settings.Height == 800)
                return Result{50.0, 80.0};
            if (settings.Tiers == std::vector<int>({1, 1}))
            {
                Require(request.RequireFull, "All Low must be measured before the resolution can fall");
                if (visit == 1) lowHeights.push_back(settings.Height);
            }
            if (settings.Tiers[0] == 0 || settings.Tiers[1] == 0)
            {
                Require(settings.Width == 768 && settings.Height == 480, "Off preceded the resolution floor");
                Require(settings.Tiers[1] != 0, "Search disabled simulation after meeting the target with post effects");
                return Result{settings.Tiers == std::vector<int>({0, 1}) ? 60.0 : 50.0, std::nullopt};
            }
            return request.RequireFull ? Result{50.0, std::nullopt} : Result{std::nullopt, 40.0};
        });
        Require(lowHeights == std::vector<int>({800, 680, 578, 491, 480}) && result.TargetReached &&
            result.Settings.Tiers == std::vector<int>({0, 1}), "Search skipped a resolution rung or broke Off priority");
    }

    void TestCrossedGroups()
    {
        Search search(Profile::Performance, std::vector<Axis>(4, {OffGroup::Never, false, 2, 3}), 1280, 800);
        const std::vector<int> crossed{3, 2, 3, 2};
        bool leftFailed = false, rightFailed = false;
        const auto result = Run(search, [&](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            leftFailed |= tiers == std::vector<int>({3, 3, 2, 2});
            rightFailed |= tiers == std::vector<int>({2, 2, 3, 3});
            const bool passed = tiers == crossed || tiers == std::vector<int>({2, 2, 2, 2});
            return Result{passed ? 62.0 : 50.0, std::nullopt};
        });
        Require(leftFailed && rightFailed && result.Settings.Tiers == crossed,
            "Failed halves hid a faster nonlinear combination");
    }

    void TestPassingGroupPrunesItsSubsets()
    {
        Search search(Profile::Quality, std::vector<Axis>(4, {OffGroup::Never, false, 2, 3}), 768, 480);
        const std::vector<int> left{3, 3, 2, 2};
        const std::vector<int> right{2, 2, 3, 3};
        bool leftTried = false, rightTried = false;
        const auto result = Run(search, [&](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            leftTried |= tiers == left;
            rightTried |= tiers == right;
            Require(std::count(tiers.begin(), tiers.end(), 3) != 1,
                "A proven higher-ranked group did not prune smaller subgroups");
            return Result{tiers == left || tiers == std::vector<int>({2, 2, 2, 2}) ? 30.0 : 26.0, std::nullopt};
        });
        Require(leftTried && rightTried && result.Settings.Tiers == left,
            "Pruning must still measure both equally ranked halves");
    }

    void TestBorderlineGroupRetainsAlternatives()
    {
        Search search(Profile::Performance, std::vector<Axis>(2, {OffGroup::Never, false, 2, 3}), 768, 480);
        const auto result = Run(search, [](const Request& request, int visit)
        {
            const auto& tiers = request.Settings.Tiers;
            if (tiers == std::vector<int>({3, 3})) return Result{visit == 1 ? 60.1 : 59.9, std::nullopt};
            if (tiers == std::vector<int>({3, 2})) return Result{70.0, std::nullopt};
            return Result{62.0, std::nullopt};
        });
        Require(result.TargetReached && result.Settings.Tiers == std::vector<int>({3, 2}),
            "A failed borderline confirmation hid a stable half-group improvement");

        Search spike(Profile::Performance, {{}}, 768, 480);
        const auto stable = Run(spike, [](const Request& request, int visit)
        {
            const auto tier = request.Settings.Tiers[0];
            return Result{tier == 4 ? (visit == 1 ? 75.0 : 50.0) : tier == 3 ? 65.0 : 70.0,
                std::nullopt};
        });
        Require(stable.Settings.Tiers[0] == 3, "A single fast outlier pruned the stable High candidate");
    }

    void TestFallbackAndRollback()
    {
        const std::vector<Axis> axes{{OffGroup::Post, false, 1, 1}, {OffGroup::Simulation, false, 1, 1},
            {OffGroup::Characters, true, 1, 1}};
        Search tied(Profile::Quality, axes, 768, 480);
        const auto unchanged = Run(tied, [](const Request&, int) { return Result{20.0, std::nullopt}; });
        Require(!unchanged.TargetReached && unchanged.Settings.Tiers == std::vector<int>({1, 1, 1}),
            "Tied fallback lost active effects");

        bool declined = false;
        const auto measure = [&](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            if (tiers == std::vector<int>({1, 1, 0})) { declined = true; return Result{21.0, std::nullopt}; }
            if (tiers == std::vector<int>({0, 1, 1})) return Result{22.0, std::nullopt};
            if (tiers == std::vector<int>({0, 0, 1})) return Result{23.0, std::nullopt};
            if (tiers == std::vector<int>({0, 0, 0}) || tiers == std::vector<int>({1, 0, 0}))
                return Result{24.0, std::nullopt};
            return Result{20.0, std::nullopt};
        };
        Search search(Profile::Quality, axes, 768, 480);
        const auto restored = Run(search, measure);
        Require(declined && !restored.TargetReached && restored.AverageFps == 24 &&
            restored.Settings.Tiers == std::vector<int>({1, 0, 0}),
            "Fallback failed to restore the effect at equal speed or rolled back too far after a slowdown");

        Search noisy(Profile::Quality, axes, 768, 480);
        const auto rolledBack = Run(noisy, [&](const Request& request, int visit)
        {
            if (request.Settings.Tiers == std::vector<int>({1, 0, 0}) && visit > 1) return Result{21.0, std::nullopt};
            return measure(request, visit);
        });
        Require(!rolledBack.TargetReached && rolledBack.AverageFps == 24 &&
            rolledBack.Settings.Tiers == std::vector<int>({0, 0, 0}), "An unstable restoration replaced the faster confirmed fallback");
    }

    void TestConfirmationAndCancel()
    {
        Search search(Profile::Performance, {{OffGroup::Never, false, 1, 2}}, 640, 400);
        const auto winner = Run(search, [](const Request& request, int visit)
        {
            Require(request.Settings.Height == 400, "A smaller original height was changed");
            return Result{request.Settings.Tiers[0] == 2 && visit > 1 ? 59.0 : 60.0, std::nullopt};
        });
        Require(winner.TargetReached && winner.Settings.Tiers[0] == 1, "A failed confirmation stayed selected");

        Search invalid(Profile::Quality, {}, 640, 400);
        Require(invalid.Next().has_value(), "An empty axis list must still measure the base candidate");
        invalid.Submit({std::nullopt, std::nullopt, false});
        Require(!invalid.Next() && !invalid.Winner(), "An invalid run was returned as a winner");

        Search cancelled(Profile::Quality, {}, 640, 400);
        Require(cancelled.Next().has_value(), "Missing first candidate");
        cancelled.Cancel();
        Require(!cancelled.Next() && !cancelled.Winner(), "Cancellation left a pending candidate");
    }

    void TestPrerequisites()
    {
        const std::vector<Axis> axes{{OffGroup::Never, false, 1, 2, 1}, {OffGroup::Simulation, false, 1, 1}};
        Search search(Profile::Performance, axes, 768, 480);
        const auto result = Run(search, [](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            Require(tiers[1] != 0 || tiers[0] == 0, "An orphaned dependent remained active");
            return Result{tiers[1] == 0 ? 60.0 : 50.0, std::nullopt};
        });
        Require(result.TargetReached && result.Settings.Tiers == std::vector<int>({0, 0}),
            "A dependent prevented disabling its prerequisite");

        bool rejected = false;
        try { Search invalid(Profile::Quality, {{OffGroup::Never, false, 1, 1, 0}}, 768, 480); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Search accepted a cyclic prerequisite");
    }

    void TestSwapAfterPassingGroup()
    {
        Search search(Profile::Performance, {{OffGroup::Never, false, 3, 4},
            {OffGroup::Never, false, 1, 3}, {OffGroup::Never, false, 1, 3}}, 768, 480);
        const auto winner = Run(search, [](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            if (tiers == std::vector<int>({4, 1, 2})) return Result{70.0, std::nullopt};
            return Result{tiers == std::vector<int>({3, 2, 2}) || tiers == std::vector<int>({3, 3, 3}) ?
                65.0 : 50.0, std::nullopt};
        });
        Require(winner.Settings.Tiers == std::vector<int>({4, 1, 2}),
            "Passing group pruned a higher-ranked swap");
    }

    void TestMeasuredReserves()
    {
        Search performance(Profile::Performance, {{OffGroup::Post, false, 1, 1}}, 768, 480);
        const auto reserve = Run(performance, [](const Request&, int) { return Result{50.0, std::nullopt}; });
        Require(!reserve.TargetReached && reserve.Settings.Tiers[0] == 1,
            "Unreachable Performance lost its measured reserve or disabled an effect without a gain");

        Search quality(Profile::Quality, {{OffGroup::Never}}, 1280, 800);
        const auto graphics = Run(quality, [](const Request& request, int)
        {
            return Result{request.Settings.Height == 800 ? 25.0 : 20.0, std::nullopt};
        });
        Require(!graphics.TargetReached && graphics.AverageFps == 25 && graphics.Settings.Height == 800 &&
            graphics.Settings.Tiers[0] == 4, "Reserve discarded resolution or quality without a speed gain");
    }

    void TestLateConfirmationRevisitsPrunedCandidates()
    {
        for (const auto profile : {Profile::Performance, Profile::Quality})
            for (const bool invalid : {false, true})
            for (const bool highPasses : {false, true})
            {
                Search search(profile, {{}}, 768, 480);
                int epicVisits = 0, highVisits = 0, requests = 0;
                const auto winner = Run(search, [&](const Request& request, int visit)
                {
                    Require(++requests < 20, "Late confirmation recovery kept retrying candidates");
                    const auto tier = request.Settings.Tiers[0];
                    const auto target = Search::Target(profile);
                    if (tier == 4)
                    {
                        ++epicVisits;
                        if (visit >= 3)
                            return invalid ? Result{std::nullopt, std::nullopt, false} :
                                Result{target - 10, std::nullopt};
                        return Result{target + 15, std::nullopt};
                    }
                    if (tier == 3)
                    {
                        ++highVisits;
                        return Result{target + (highPasses ? 5 : -5), std::nullopt};
                    }
                    return Result{target + 10, std::nullopt};
                });
                Require(winner.TargetReached && winner.Settings.Tiers[0] == (highPasses ? 3 : 2) &&
                    epicVisits == 3 && highVisits == (highPasses ? 2 : 1),
                    "Late failed or invalid Epic confirmation hid the stable High candidate");
            }
    }

    void TestNonmonotonicTiersAndCpuShift()
    {
        Search performance(Profile::Performance, {{}}, 768, 480);
        const auto skippedTier = Run(performance, [](const Request& request, int)
        {
            Require(request.RequireFull, "Stationary warmup cannot prove a candidate fails its full route");
            return Result{request.Settings.Tiers[0] == 3 ? 50.0 : 65.0, 45.0};
        });
        Require(skippedTier.Settings.Tiers[0] == 4, "A failed intermediate tier hid a passing Epic tier");

        for (const int secondMax : {3, 4})
        {
            Search mixed(Profile::Performance, {{OffGroup::Never, false, 3, 4},
                {OffGroup::Never, false, 1, secondMax}}, 768, 480);
            const auto combined = Run(mixed, [secondMax](const Request& request, int)
            {
                return Result{request.Settings.Tiers == std::vector<int>({3, 1}) ||
                    request.Settings.Tiers == std::vector<int>({4, secondMax}) ? 65.0 : 50.0, std::nullopt};
            });
            Require(combined.Settings.Tiers == std::vector<int>({4, secondMax}),
                "Direct group probe skipped an axis or its tier bound");
        }

        Search quality(Profile::Quality, {{}, {OffGroup::Never, true}}, 768, 480);
        const auto shifted = Run(quality, [](const Request& request, int)
        {
            const auto& tiers = request.Settings.Tiers;
            return Result{tiers[1] > 2 ? 25.0 : tiers[0] <= 2 ? 40.0 : tiers[1] == 2 ? 29.0 : 25.0,
                std::nullopt};
        });
        Require(shifted.Settings.Tiers == std::vector<int>({4, 2}),
            "CPU change did not recheck graphics under a changed bottleneck");
    }

    void TestInvalidRecoveryTerminates()
    {
        for (const auto profile : {Profile::Performance, Profile::Quality})
        {
            Search search(profile, {{}}, 768, 480);
            std::map<int, int> visits;
            int requests = 0;
            while (auto request = search.Next())
            {
                Require(++requests < 12, "Failed recovery restarted the same candidates");
                const auto tier = request->Settings.Tiers[0];
                const int visit = ++visits[tier];
                search.Submit((tier == 2 || tier == 4) && visit <= 2 ?
                    Result{Search::Target(profile) + 15, std::nullopt} :
                    Result{std::nullopt, std::nullopt, false});
            }
            Require(!search.Winner() && visits[4] == 3 && visits[3] == 1 && visits[2] == 3,
                "Failed deferred measurements or finalists produced a winner or repeated a failed probe");
        }
    }

    void TestInvalidMeasurementsTerminate()
    {
        Search search(Profile::Performance, {{OffGroup::Post}, {OffGroup::Simulation}}, 1280, 800);
        size_t count = 0;
        while (auto request = search.Next())
        {
            Require(++count < 100, "Invalid candidates kept restarting the search");
            Require(search.Next()->Settings == request->Settings, "Next changed an outstanding request");
            search.Submit({count % 2 ? std::numeric_limits<double>::quiet_NaN() :
                std::numeric_limits<double>::infinity(), std::nullopt});
        }
        Require(!search.Winner(), "Invalid measurements produced a winner");
    }

    void TestMGPUPlacementAndFreshness()
    {
        MGPUSettings pair;
        pair.Enabled = true;
        pair.Primary = 11;
        pair.Secondary = 22;
        Search search(Profile::Performance, {{OffGroup::Never, false, 2, 2}}, 768, 480, pair);
        std::set<Candidate> measured;
        const auto result = Run(search, [&](const Request& request, int)
        {
            measured.insert(request.Settings);
            const auto& policy = request.Settings.MGPU;
            const bool water = policy.Water.Secondary;
            const bool grass = policy.Grass.Secondary;
            return Result{water && !grass ? 70.0 : 63.0, std::nullopt};
        });
        Require(result.TargetReached && result.Settings.MGPU.Water.Secondary &&
            !result.Settings.MGPU.Grass.Secondary, "MGPU search missed the mixed placement at equal quality");
        Require(measured.size() > 10, "MGPU placement exploration did not measure alternatives");
        Require(result.Settings.MGPU.Reflections.FilterSamples == 256,
            "MGPU search lost the existing Epic reflection filtering quality");
        Require(result.Settings.MGPU.Water.UpdateHz == 0 && result.Settings.MGPU.Grass.UpdateHz == 0,
            "MGPU selected a lower temporal quality without a performance need");
        auto different = result.Settings;
        ++different.MGPU.Secondary;
        Require(!(different == result.Settings), "Candidates for different adapter pairs compare equal");

        FreshnessMeasurements samples;
        TechniqueMeasurement sample;
        sample.Active = sample.RequestedSecondary = sample.EffectiveSecondary = sample.Ready = true;
        sample.SourceAgeMs = 30;
        samples.Observe(sample, 0, Profile::Performance);
        Require(samples.Valid(), "A permitted snapshot was rejected");
        sample.SourceAgeMs = 34;
        samples.Observe(sample, 1, Profile::Performance);
        Require(!samples.Valid(), "Freshness used current FPS instead of the fixed target budget");
        samples = {};
        sample.SourceAgeMs = 1;
        sample.EffectiveSecondary = false;
        samples.Observe(sample, 0, Profile::Quality);
        Require(!samples.Valid(), "A hidden fallback was accepted as MGPU");
        samples = {};
        sample.EffectiveSecondary = true;
        sample.MissingSeconds = 0.01;
        samples.Observe(sample, 0, Profile::Quality);
        Require(!samples.Valid(), "A lost simulation input was accepted");
        samples = {};
        Require(!samples.Valid(true), "A required route passed without any measurement");
        sample.MissingSeconds = 0;
        sample.SimulationDebtSeconds = 0.001;
        samples.Observe(sample, 2, Profile::Quality);
        sample.SimulationDebtSeconds = 0.005;
        samples.Observe(sample, 4, Profile::Quality);
        Require(!samples.Valid(), "Growing debt below the age cap was accepted or late first sample hid it");
        Require(FreshnessBudgetMs(Profile::Quality) > 66.66 && FreshnessBudgetMs(Profile::Quality) < 66.67,
            "Quality freshness budget used the 28 FPS acceptance threshold");
        samples = {};
        for (int second = 0; second < 100; ++second)
        {
            sample.SimulationDebtSeconds = 0.001 + second * 0.00001;
            samples.Observe(sample, second, Profile::Quality);
        }
        Require(!samples.Valid(), "A slow persistent debt trend passed the gate");
        samples = {};
        for (int second = 0; second < 100; ++second)
        {
            sample.SimulationDebtSeconds = (second % 3) * 0.001;
            samples.Observe(sample, second, Profile::Quality);
        }
        Require(samples.Valid(), "Bounded cadence sawtooth was mistaken for growing debt");

        ReflectionSearchSettings canonical;
        canonical.Count = 1; canonical.DistanceMetres = 10; canonical.Schedule = 3;
        canonical.SecondaryFaces = {3, 63, 63, 63};
        canonical.Canonicalize();
        Require(canonical.Schedule == 2 && canonical.SecondaryFaces[1] == 0,
            "Equivalent single-probe schedules or inactive faces multiplied candidates");
        canonical.DistanceMetres = -1; canonical.Placement = 2;
        canonical.Canonicalize();
        Require(canonical.Schedule == 0 && canonical.Placement == 0 && canonical.SecondaryFaces[0] == 63,
            "Always-near placement was not canonicalized");
        Search unstable(Profile::Performance, {{OffGroup::Never, false, 2, 2}}, 768, 480, pair);
        unstable.SetTransferCostEstimate([](const Candidate& candidate) -> std::optional<double>
            { return candidate.MGPU.Water.Secondary ? 10.0 : 0.0; });
        const auto stable = Run(unstable, [&](const Request& request, int visit)
        {
            const auto& p = request.Settings.MGPU;
            const bool secondary = p.Water.Secondary || p.Grass.Secondary || p.Sun.Secondary || p.Particles.Secondary ||
                p.Reflections.Placement || std::any_of(p.Reflections.SecondaryFaces.begin(), p.Reflections.SecondaryFaces.end(),
                    [](unsigned mask) { return mask != 0; });
            return Result{secondary ? (visit == 1 ? 70.0 : 61.0) : 65.0, std::nullopt};
        });
        Require(stable.TargetReached && stable.Stable && stable.AverageFps == 65,
            "An unstable MGPU finalist discarded the measured stable primary alternative");

        pair.LocalShadows = {{17, {}}, {42, {}}};
        Search units(Profile::Performance, {{OffGroup::Never, false, 2, 2}}, 768, 480, pair);
        const auto mixed = Run(units, [&](const Request& request, int)
        {
            const auto& p = request.Settings.MGPU;
            return Result{63.0 + (p.Sun.Secondary ? 3 : 0) + (p.Particles.Secondary ? 3 : 0) +
                (p.LocalShadows[0].Route.Secondary ? 3 : 0) - (p.LocalShadows[1].Route.Secondary ? 8 : 0), std::nullopt};
        });
        Require(mixed.Settings.MGPU.Sun.Secondary && mixed.Settings.MGPU.Particles.Secondary &&
            mixed.Settings.MGPU.LocalShadows[0].Route.Secondary && !mixed.Settings.MGPU.LocalShadows[1].Route.Secondary,
            "Joint search missed the beneficial local-light count and particle placement");

        pair.LocalShadows.clear();
        for (unsigned id = 135; id; --id) pair.LocalShadows.push_back({id, {}});
        Search counts(Profile::Performance, {{OffGroup::Never, false, 2, 2}}, 768, 480, pair);
        std::set<size_t> visitedCounts;
        const auto counted = Run(counts, [&](const Request& request, int)
        {
            const auto& lights = request.Settings.MGPU.LocalShadows;
            size_t count = 0;
            bool primarySeen = false;
            for (size_t i = 0; i < lights.size(); ++i)
            {
                Require(lights[i].Entity == i + 1, "Count search did not canonicalize light order");
                if (!lights[i].Route.Secondary) primarySeen = true;
                else { Require(!primarySeen, "Count search selected a non-nested light set"); ++count; }
            }
            visitedCounts.insert(count);
            return Result{80.0 - std::abs(static_cast<double>(count) - 50.0), std::nullopt};
        });
        Require(std::count_if(counted.Settings.MGPU.LocalShadows.begin(), counted.Settings.MGPU.LocalShadows.end(),
            [](const auto& light) { return light.Route.Secondary; }) == 50,
            "Count refinement missed the measured optimum between coarse counts");
        for (const size_t count : {size_t{0}, size_t{33}, size_t{67}, size_t{101}, size_t{135}})
            Require(visitedCounts.count(count) != 0, "Count search omitted a representative fraction");

        const auto invalidTrace = [&](bool ordered)
        {
            Search search(Profile::Quality, {{OffGroup::Never, false, 2, 2}}, 768, 480, pair);
            if (ordered) search.SetTransferCostEstimate([](const Candidate& candidate) -> std::optional<double>
            {
                if (candidate.MGPU.Particles.Secondary) return std::nullopt;
                return candidate.MGPU.Water.Secondary ? 1.0 : 0.0;
            });
            std::multiset<Candidate> trace;
            for (int step = 0; step < 2000; ++step)
            {
                const auto request = search.Next();
                if (!request) return trace;
                Require(request->RequireFull, "Cost hint replaced a full-flight request");
                trace.insert(request->Settings);
                search.Submit({30.0, std::nullopt, false});
            }
            throw std::runtime_error("Cost-ordered invalid search did not terminate");
        };
        Require(invalidTrace(false) == invalidTrace(true),
            "Transfer cost ordering discarded unknown candidates or changed the measured candidate multiset");
    }
}

int main()
{
    try
    {
        TestQualityBeforeSpeed();
        TestWarmupAndResolution();
        TestCrossedGroups();
        TestPassingGroupPrunesItsSubsets();
        TestBorderlineGroupRetainsAlternatives();
        TestFallbackAndRollback();
        TestConfirmationAndCancel();
        TestPrerequisites();
        TestSwapAfterPassingGroup();
        TestMeasuredReserves();
        TestLateConfirmationRevisitsPrunedCandidates();
        TestNonmonotonicTiersAndCpuShift();
        TestInvalidRecoveryTerminates();
        TestInvalidMeasurementsTerminate();
        TestMGPUPlacementAndFreshness();
        std::cout << "Bistro quality search checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
