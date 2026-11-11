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
            Require(++requests < 200, "Synthetic search did not terminate");
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
        TestNonmonotonicTiersAndCpuShift();
        TestInvalidMeasurementsTerminate();
        std::cout << "Bistro quality search checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
