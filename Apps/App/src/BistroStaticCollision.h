#pragma once

#include "App.Base/Physics/PhysicsTypes.h"
#include "BistroRoutes.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace BistroStaticCollision
{
    inline constexpr float FloorHeight = 39.622f;
    inline constexpr float RaisedFloorHeight = 51.803f;
    inline constexpr float CeilingHeight = 402.78f;

    inline CompoundShapeDesc BuildWalkway()
    {
        auto walkway = std::make_shared<PhysicsCompound>();
        const auto& route = BistroRoutes::People();
        walkway->Children.reserve(route.Edges.size());
        constexpr float halfDepth = 5;
        for (const auto& edge : route.Edges)
        {
            const auto a = edge.Points.front(), b = edge.Points.back();
            const auto delta = b - a;
            const float run = std::hypot(delta.x, delta.z);
            const auto rotation = Quaternion::CreateFromYawPitchRoll(
                -std::atan2(delta.z, delta.x), 0, std::atan2(delta.y, run));
            const auto top = (a + b) * .5f - Vector3(0, BistroRoutes::PeopleGroundOffset, 0);
            walkway->Children.push_back({BoxShapeDesc{{delta.Length() * .5f + 5, halfDepth, 55}, 0},
                top - Vector3::Transform({0, halfDepth, 0}, rotation), rotation});
        }
        return {walkway};
    }

    inline CompoundShapeDesc BuildRoom()
    {
        auto room = std::make_shared<PhysicsCompound>();
        // Centimeter dimensions surveyed from interior.obj in the restaurant's plan frame.
        constexpr float yaw = DirectX::XMConvertToRadians(-28.5f);
        const auto frame = Quaternion::CreateFromYawPitchRoll(yaw, 0, 0);
        const auto addBox = [&](const Vector3& center, const Vector3& half, float localYaw = 0)
        {
            room->Children.push_back({BoxShapeDesc{half, 0}, Vector3::Transform(center, frame),
                Quaternion::CreateFromYawPitchRoll(yaw + localYaw, 0, 0)});
        };
        const auto slab = [&](float left, float right, float back, float front, float bottom, float top)
        {
            const Vector3 lo(left, bottom, back), hi(right, top, front);
            addBox((lo + hi) * .5f, (hi - lo) * .5f);
        };
        const auto wall = [&](const Vector3& a, const Vector3& b, float bottom, float top,
            float halfThickness, float outwardOffset = 0)
        {
            const auto direction = b - a;
            const auto length = direction.Length();
            auto center = (a + b) * .5f + Vector3(-direction.z, 0, direction.x) * (outwardOffset / length);
            center.y = (bottom + top) * .5f;
            addBox(center, {length * .5f + halfThickness, (top - bottom) * .5f, halfThickness},
                -std::atan2(direction.z, direction.x));
        };

        // The dining room widens along the angled facade and has a narrow rear wing.
        constexpr Vector3 facade[] = {{-241,0,-155}, {-160,0,-94}, {-80,0,-34}, {0,0,26},
            {80,0,70}, {980,0,-38}, {1003,0,-86}};
        constexpr Vector3 rear[] = {{1003,0,-86}, {1003,0,-553}, {994,0,-553}, {994,0,-795},
            {979,0,-830}, {423,0,-830}, {423,0,-796}, {346,0,-796}, {346,0,-818},
            {250,0,-818}, {250,0,-1316}, {20,0,-1316}, {20,0,-856}, {-241,0,-856}, {-241,0,-155}};
        // Give fast tableware extra depth behind the collision face without moving the face inward.
        for (size_t i = 1; i < std::size(facade); ++i)
            wall(facade[i - 1], facade[i], FloorHeight - 20, CeilingHeight + 20, 15, 10);
        for (size_t i = 1; i < std::size(rear); ++i)
            wall(rear[i - 1], rear[i], FloorHeight - 20, CeilingHeight + 20, 15, 10);

        const auto shell = [&](float left, float right, float back, float front)
        {
            // Keep the nearest floor face above a prop even after a full-speed substep into the slab.
            slab(left, right, back, front, FloorHeight - 36, FloorHeight);
            slab(left, right, back, front, CeilingHeight, CeilingHeight + 20);
        };
        shell(-249, -160, -864, -150);
        shell(-168, 1011, -864, -86);
        shell(12, 258, -1324, -852);
        for (size_t i = 1; i < std::size(facade); ++i)
        {
            const auto a = facade[i - 1], b = facade[i];
            const int strips = static_cast<int>(std::ceil(std::abs(b.z - a.z) / 30));
            for (int strip = 0; strip < strips; ++strip)
            {
                const auto from = Vector3::Lerp(a, b, float(strip) / strips);
                const auto to = Vector3::Lerp(a, b, float(strip + 1) / strips);
                shell(from.x - 8, to.x + 8, i == 1 ? -156.f : -92.f, (std::max)(from.z, to.z) + 8);
            }
        }

        // The step and counter use a few straight spans around their curved ends.
        const auto step = [&](float left, float right, float back, float front)
        {
            slab(left, right, back, front, RaisedFloorHeight - 36, RaisedFloorHeight);
        };
        step(488, 1008, -830, -460);
        step(257, 492, -830, -692);
        step(262, 492, -697, -608);
        step(282, 492, -613, -548);
        step(326, 492, -553, -498);
        step(390, 492, -503, -471);
        step(453, 492, -476, -460);
        constexpr Vector3 counter[] = {{380,0,-694}, {385,0,-651}, {407,0,-613},
            {444,0,-591}, {488,0,-586}, {979,0,-586}};
        for (size_t i = 1; i < std::size(counter); ++i)
            wall(counter[i - 1], counter[i], RaisedFloorHeight, 163, 22);
        return {room};
    }
}
