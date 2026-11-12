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
        const std::vector<Probe> probes{{1,1000,0}, {2,1000,63}, {3,1000,21}, {4,1000,42}};
        const std::array<unsigned,4> periods{1,4,6,24};
        for (unsigned schedule = 0; schedule < 4; ++schedule)
        {
            Scheduler scheduler;
            std::array<unsigned,4> coverage{};
            for (unsigned frame = 0; frame < periods[schedule]; ++frame)
            {
                const auto work = scheduler.Select(probes, {static_cast<Schedule>(schedule), 0, Placement::Fixed});
                for (size_t i = 0; i < work.size(); ++i)
                {
                    coverage[i] |= work[i].Faces;
                    Require(work[i].SecondaryFaces == (work[i].Faces & probes[i].SecondaryFaces),
                        "Face placement changed the scheduled work");
                }
            }
            for (auto faces : coverage) Require(faces == 63, "Schedule did not visit all six faces of every probe");
        }
        Require(Canonical(Schedule::WholeProbe, 1) == Schedule::All &&
            Canonical(Schedule::OneFace, 1) == Schedule::FaceEach, "Equivalent one-probe schedules not canonicalized");
        Scheduler scheduler;
        Policy policy{Schedule::OneFace, 5, Placement::NearPrimary};
        auto moving = probes;
        moving[0].Distance = 500;
        auto work = scheduler.Select(moving, policy);
        Require(work[0].Near && work[0].Faces == 63 && work[0].SecondaryFaces == 0,
            "Entering the near range must schedule all faces on primary");
        moving[0].Distance = 600;
        Require(scheduler.Select(moving, policy)[0].Near, "Exit threshold must be strictly greater than 1.2R");
        moving[0].Distance = 601;
        Require(!scheduler.Select(moving, policy)[0].Near, "Probe did not leave near range");
        moving[0].Distance = 550;
        Require(!scheduler.Select(moving, policy)[0].Near, "Hysteresis oscillated inside the dead band");
        moving[0].Distance = 400;
        policy.Gpu = Placement::NearSecondary;
        work = scheduler.Select(moving, policy);
        Require(work[0].Faces == 63 && work[0].SecondaryFaces == 63,
            "Reverse near/far placement was not applied");
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
        try { scheduler.Select({{1,-1,0}}, policy); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid distance accepted");
        PublicationClock publication;
        std::array<double, 6> sources{};
        for (unsigned face = 0; face < 6; ++face)
        {
            sources[face] = 10 + face;
            publication.ScheduleFaces(1u << face, sources[face]);
        }
        Require(publication.AdditionalLag(15.1) > 0.099, "An overdue cube moved its nominal deadline");
        publication.Publish(sources, 15.02);
        Require(publication.FullAge(16, 0) == 6 && publication.AdditionalLag(16) < 0.021,
            "Intentional rare-update age was confused with additional lag");
        publication.ScheduleFaces(63, 17);
        Require(publication.AdditionalLag(17.2) > 0.199, "Near transition reused a stale publication as current");
        publication.ScheduleFaces(63, 18);
        publication.ScheduleFaces(63, 19);
        Require(publication.AdditionalLag(19.1) > 2.099, "A later cycle hid an unfinished publication");
        sources.fill(18);
        publication.Publish(sources, 19.2);
        Require(publication.AdditionalLag(19.3) > 2.199, "Catch-up erased the measured overdue publication");
        sources.fill(19);
        publication.Publish(sources, 19.4);
        Require(publication.AdditionalLag(19.5) > 0.399 && publication.AdditionalLag(19.5) < 0.401,
            "A completed new publication retained obsolete debt");
        publication.Reset();
        Require(!publication.Ready() && publication.AdditionalLag(20) == 0, "Reset kept another epoch's publication");
        std::cout << "Reflection schedule, distance, hysteresis, placement and fairness passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
