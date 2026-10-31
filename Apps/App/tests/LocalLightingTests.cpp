#include "App.Base/Systems/LightUpdateSystem.h"
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
        World world(WorldDesc{});
        auto& ecs = world.GetECS();
        auto point = ecs.CreateEntity();
        point.AddComponent<TransformComponent>(Vector3(10, 20, 30));
        point.AddComponent<LightComponent>();
        auto spot = ecs.CreateEntity();
        spot.AddComponent<TransformComponent>(Vector3(0, 100, 0), Vector3(90, 0, 0));
        spot.AddComponent<LightComponent>().Type = LightType::Spot;

        auto frame = CollectWorldLights(world);
        Require(frame.Main.LightRadiance.x == 0 && frame.Main.SunShadowFlags == 0,
            "A world without an enabled directional light must not retain a default sun");
        Require(frame.Local.size() == 2, "ECS point and spot lights must be collected");
        Require(frame.Local[0].ShadowFirstSlice == 0 && frame.Local[1].ShadowFirstSlice == 6,
            "Point lights need six shadow faces and spot lights one");
        const auto cachedPoint = frame.Local[0];
        const auto cachedSpot = frame.Local[1];
        Require(ShouldRenderLocalShadow(cachedPoint, nullptr), "Cached shadows must render at startup");
        Require(!ShouldRenderLocalShadow(cachedPoint, &cachedPoint), "Unchanged cached shadows must be reused");

        ecs.Get<LightComponent>(point).Intensity *= 2.0f;
        frame = CollectWorldLights(world);
        Require(frame.Local[0].Radiance.x > cachedPoint.Radiance.x, "Systems must update light intensity");
        Require(!ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Intensity changes must not rebake depth");
        ecs.Get<TransformComponent>(point).Location.x += 10.0f;
        frame = CollectWorldLights(world);
        Require(ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Moving a cached light must invalidate its shadows");
        ecs.Get<LightComponent>(spot).OuterAngle = 50.0f;
        frame = CollectWorldLights(world);
        Require(ShouldRenderLocalShadow(frame.Local[1], &cachedSpot), "Spot cone changes must invalidate the projection");
        auto unchanged = frame.Local[1];
        ecs.Get<LightComponent>(spot).InvalidateShadow();
        frame = CollectWorldLights(world);
        Require(ShouldRenderLocalShadow(frame.Local[1], &unchanged), "Systems must be able to invalidate a shadow cache explicitly");
        unchanged = frame.Local[1];
        ecs.Get<LightComponent>(spot).ShadowUpdate = ShadowUpdateMode::EveryFrame;
        frame = CollectWorldLights(world);
        Require(ShouldRenderLocalShadow(frame.Local[1], &unchanged), "EveryFrame must update unchanged light shadows");
        ecs.Get<LightComponent>(point).CastShadows = false;
        frame = CollectWorldLights(world);
        Require(!ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Shadow-disabled lights must remain unshadowed");

        const Vector3 directions[] = { Vector3::UnitX, -Vector3::UnitX, Vector3::UnitY, -Vector3::UnitY, Vector3::UnitZ, -Vector3::UnitZ };
        for (int face = 0; face < 6; ++face)
        {
            const Matrix matrix = Matrix(cachedPoint.ShadowViewProj[face]).Transpose();
            const Vector3 projected = Vector3::Transform(Vector3(cachedPoint.Position) + directions[face] * 10.0f, matrix);
            Require(std::abs(projected.x) < 0.001f && std::abs(projected.y) < 0.001f && projected.z > 0 && projected.z < 1,
                "Point shadow faces must cover their corresponding cube directions");
        }
        std::cout << "ECS light updates and cached/every-frame point/spot shadow policy passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
