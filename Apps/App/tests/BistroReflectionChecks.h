#pragma once

#include "../src/BistroReflections.h"
#include <stdexcept>

template<class Update> inline void CheckBistroReflectionCounts(World& world, Update&& update)
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    require(BistroReflections::Count.GetValue() == 4, "Bistro must default to four reflection probes");
    require(!BistroReflections::ShowBounds.GetValue(), "Probe gizmo boxes must be disabled by default");
    auto& ecs = world.GetECS();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 4, "Bistro reflection default count mismatch");
    const auto checkAnchors = [&]
    {
        std::vector<size_t> occupied;
        ecs.ForEach<ReflectionProbeComponent, TransformComponent>([&](Entity entity, const ReflectionProbeComponent& probe,
            const TransformComponent& transform)
        {
            GDX12ReflectionProbe capture;
            static_cast<GDX12ReflectionCaptureSettings&>(capture) = probe;
            capture.Position = transform.Location;
            capture.BoxMin = transform.Location - probe.Extents;
            capture.BoxMax = transform.Location + probe.Extents;
            capture.BlendDistance = probe.BlendDistance;
            capture.FarPlane = probe.FarPlane;
            require(probe.Implementation == 2 && IsValidReflectionProbe(capture), "Invalid Bistro frozen capture");
            require(ecs.Has<StaticMeshRenderComponent>(entity), "Probe preview must be a normal mesh component");
            size_t table = 0;
            for (; table < BistroReflections::Tables.size(); ++table)
            {
                const auto& anchor = BistroReflections::Tables[table];
                if (Vector3::Distance(transform.Location, anchor.Centre + Vector3(0, anchor.HalfSize - 5, 0)) < 0.01f &&
                    probe.Extents == Vector3(anchor.HalfSize)) break;
            }
            require(table < BistroReflections::Tables.size(), "Reflection must use an authored tabletop spawn");
            require(std::find(occupied.begin(), occupied.end(), table) == occupied.end(), "Two captures occupied the same table");
            occupied.push_back(table);
        });
        std::sort(occupied.begin(), occupied.end());
        return occupied;
    };
    const auto initialAnchors = checkAnchors();
    require(!BistroReflections::Resolution.SetFromString("192") &&
        !BistroReflections::PrefilterSamples.SetFromString("0"), "Invalid frozen capture budgets accepted");
    for (const UINT faceSize : {64u, 128u, 256u, 512u, 64u, 128u})
    {
        require(BistroReflections::Resolution.SetValue(faceSize) && BistroReflections::PrefilterSamples.SetValue(faceSize / 2),
            "Frozen capture tier rejected");
        update();
        ecs.ForEach<ReflectionProbeComponent>([&](Entity, const ReflectionProbeComponent& probe)
        {
            require(probe.Resolution == faceSize && probe.PrefilterSamples == faceSize / 2 && probe.FacesPerFrame == 6,
                "Bistro probe quality must update actual frozen capture budgets");
        });
        checkAnchors();
    }
    require(!BistroReflections::Count.SetFromString("-1") && !BistroReflections::Count.SetFromString("22") &&
        !BistroReflections::Count.SetFromString("1.5"), "Invalid Bistro reflection count accepted");
    require(BistroReflections::Count.SetFromString("21"), "Maximum table capture count rejected");
    update();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 21, "Maximum must spawn one capture at every table");
    checkAnchors();
    require(BistroReflections::Count.SetFromString("8"), "Bistro reflection count change rejected");
    update();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 8, "Changed Bistro reflection count mismatch");
    require(BistroReflections::Count.SetFromString("2"), "Bistro reflection count reduction rejected");
    update();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 2, "Bistro reflection count did not shrink");
    require(BistroReflections::Count.SetFromString("0"), "Bistro reflection disable rejected");
    update();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 0, "Bistro reflection disable leaked owned entities");
    require(BistroReflections::Count.SetFromString("4"), "Bistro reflection count restore rejected");
    update();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 4, "Bistro reflection count did not restore");
    require(checkAnchors() == initialAnchors, "Restoring the probe count must reuse the same authored tables");
}
