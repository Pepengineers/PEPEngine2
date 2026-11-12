#include "../../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12ReflectionSchedule.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace GDX12ReflectionSchedule;
static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
        Require(DistanceToBox({0,0,0}, {-100,-100,-100}, {100,100,100}) == 0,
            "Distance inside influence box must be zero");
        Require(DistanceToBox({400,500,100}, {-100,-100,-100}, {100,100,100}) == 500,
            "Distance must use closest box point, in world centimetres");
        const std::vector<Probe> probes{{1,1000}, {2,1000}, {3,1000}, {4,1000}};
        const std::array<unsigned,4> periods{1,4,6,24};
        for (unsigned schedule = 0; schedule < 4; ++schedule)
        {
            Scheduler scheduler;
            std::array<unsigned,4> coverage{};
            for (unsigned frame = 0; frame < periods[schedule]; ++frame)
            {
                const auto work = scheduler.Select(probes, {static_cast<Schedule>(schedule), 0});
                for (size_t i = 0; i < work.size(); ++i)
                {
                    coverage[i] |= work[i].Faces;
                }
            }
            for (auto faces : coverage) Require(faces == 63, "Schedule did not visit all six faces of every probe");
        }
        Require(Canonical(Schedule::WholeProbe, 1) == Schedule::All &&
            Canonical(Schedule::OneFace, 1) == Schedule::FaceEach, "Equivalent one-probe schedules not canonicalized");
        Scheduler scheduler;
        Policy policy{Schedule::OneFace, 5};
        auto moving = probes;
        moving[0].Distance = 500;
        auto work = scheduler.Select(moving, policy);
        Require(work[0].Near && work[0].Faces == 63,
            "Entering the near range must schedule all faces on primary");
        moving[0].Distance = 600;
        Require(scheduler.Select(moving, policy)[0].Near, "Exit threshold must be strictly greater than 1.2R");
        moving[0].Distance = 601;
        Require(!scheduler.Select(moving, policy)[0].Near, "Probe did not leave near range");
        moving[0].Distance = 550;
        Require(!scheduler.Select(moving, policy)[0].Near, "Hysteresis oscillated inside the dead band");
        moving[0].Distance = 400;
        std::array<unsigned,4> coverage{};
        for (unsigned frame = 0; frame < 18; ++frame)
        {
            work = scheduler.Select(moving, policy);
            Require(work[0].Faces == 63, "Near probe missed a complete frame");
            for (size_t i = 1; i < work.size(); ++i) coverage[i] |= work[i].Faces;
        }
        for (size_t i = 1; i < coverage.size(); ++i) Require(coverage[i] == 63, "Near probe starved far probes");
        scheduler.Reset();
        moving[0].Distance = 550;
        Require(!scheduler.Select(moving, policy)[0].Near, "Reset inherited the old near classification");
        policy.FullUpdateDistanceMetres = -1;
        for (const auto& item : scheduler.Select(moving, policy)) Require(item.Faces == 63, "Always must update all probes");
        Require(scheduler.Select({}, policy).empty(), "Zero probes must schedule no work");
        bool rejected = false;
        try { scheduler.Select({{1,-1}}, policy); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid distance accepted");
        std::cout << "Reflection schedule, distance, hysteresis and fairness passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
