#pragma once

#include "../src/BistroReflections.h"
#include <stdexcept>

inline void CheckBistroReflectionCounts()
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    World world(WorldDesc{});
    BistroReflections::Scene scene;
    require(BistroReflections::Count.GetValue() == 4, "Bistro must default to four reflection probes");
    require(!BistroReflections::ShowBounds.GetValue(), "Probe gizmo boxes must be disabled by default");
    scene.Update(world, 0);
    auto& ecs = world.GetECS();
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 4, "Bistro reflection default count mismatch");
    ecs.ForEach<ReflectionProbeComponent, TransformComponent>([&](Entity, const ReflectionProbeComponent& probe,
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
        require(probe.Extents.x >= 58 && probe.Extents.x <= 63 &&
            probe.Extents.x == probe.Extents.y && probe.Extents.y == probe.Extents.z,
            "Reflection bounds must match an exterior table-sized cube");
        require(std::abs(transform.Location.y - probe.Extents.y - 102.503f) < 0.01f,
            "Reflection cube must stand over the tabletop with a 5 cm blend inset");
    });
    require(!BistroReflections::Count.SetFromString("-1") && !BistroReflections::Count.SetFromString("33") &&
        !BistroReflections::Count.SetFromString("1.5"), "Invalid Bistro reflection count accepted");
    require(BistroReflections::Count.SetFromString("8"), "Bistro reflection count change rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 8, "Bistro reflection count did not grow");
    require(BistroReflections::Count.SetFromString("2"), "Bistro reflection count reduction rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 2, "Bistro reflection count did not shrink");
    require(BistroReflections::Count.SetFromString("0"), "Bistro reflection disable rejected");
    scene.Update(world, 0);
    require(ecs.GetEntityCount() == 0, "Bistro reflection disable leaked owned entities");
    require(BistroReflections::Count.SetFromString("4"), "Bistro reflection count restore rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<ReflectionProbeComponent>().Size() == 4, "Bistro reflection count did not restore");
}
