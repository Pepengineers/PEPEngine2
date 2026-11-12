// Isolate ranking and final confirmation; candidate generation is tested in BistroQualitySearchTests.
#include "../src/BistroMGPUQuality.h"
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <iostream>
#define private public
#include "../src/BistroQualitySearch.h"
#undef private

int main()
{
    using namespace BistroQualitySearch;
    const auto require = [](bool value, const char* message)
    { if (!value) throw std::runtime_error(message); };
    try
    {
        for (const auto profile : {Profile::Performance, Profile::Quality})
        {
            MGPUSettings pair;
            pair.Enabled = true; pair.Primary = 47217; pair.Secondary = 59621;
            const std::vector<Axis> axes{{OffGroup::Simulation, false, 1, 1}, {OffGroup::Never, false, 1, 1}};
            Search search(profile, axes, 768, 480, pair, 0, std::nullopt, {}, 1);
            Candidate waterOnly{{1, 1}, 768, 480, pair};
            waterOnly.MGPU.Reflections.Count = 0;
            auto probesOnly = waterOnly;
            probesOnly.Tiers[0] = 0;
            probesOnly.MGPU.Reflections.Count = 1;
            require(search.Rank(waterOnly)[0] == search.Rank(probesOnly)[0],
                "Disabled reflection probes inflated active-technique count");
            const double fps = profile == Profile::Performance ? 65.0 : 30.0;
            search._finalists = {{waterOnly, fps, true, true}, {probesOnly, fps, true, true}};
            search._refined.insert(waterOnly);
            search._refined.insert(probesOnly);
            search.StartConfirmation();
            const auto confirmation = search.Next();
            require(confirmation && confirmation->RequireFull && confirmation->Settings == probesOnly,
                "Reflection-off ghost tier displaced the better finalist");
            search.Submit({fps, std::nullopt, true});
            require(!search.Next() && search.Winner() && search.Winner()->Stable &&
                search.Winner()->Settings == probesOnly, "Ranked winner was not separately confirmed");

            pair.Enabled = false;
            Search legacy(profile, axes, 768, 480, pair, 0, std::nullopt, {}, 1);
            waterOnly.MGPU = pair;
            waterOnly.MGPU.Reflections.Count = 0;
            require(legacy.Rank(waterOnly)[0] == 2, "Non-MGPU legacy tier ranking changed");
            bool rejected = false;
            try { Search invalid(profile, axes, 768, 480, pair, {}, {}, {}, axes.size()); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "Invalid reflection axis accepted");
        }
        std::cout << "Reflection Count0 ranking and finalist confirmation checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
