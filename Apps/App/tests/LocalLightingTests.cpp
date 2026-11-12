#include "App.Base/ECS/WorldLightSnapshot.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
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
        std::vector<uint64_t> everyOther;
        for (uint64_t frame = 1; frame <= 8; ++frame)
            if (ShadowUpdateDue(frame, 2, frame > 1)) everyOther.push_back(frame);
        Require(everyOther == std::vector<uint64_t>({ 1, 2, 4, 6, 8 }) &&
            ShadowUpdateDue(5, 4, false) && !ShadowUpdateDue(5, 4, true),
            "Shadow cadence must retain nominal deadlines and initialize immediately after reset");
        ECSStorage<TransformComponent, LightComponent> ecs;
        auto point = ecs.CreateEntity();
        point.AddComponent<TransformComponent>(Vector3(10, 20, 30));
        point.AddComponent<LightComponent>();
        auto spot = ecs.CreateEntity();
        spot.AddComponent<TransformComponent>(Vector3(0, 100, 0), Vector3(90, 0, 0));
        spot.AddComponent<LightComponent>().Type = LightType::Spot;

        auto frame = CollectWorldLights(ecs);
        Require(frame.Main.LightRadiance.x == 0 && frame.Main.SunShadowFlags == 0,
            "A world without an enabled directional light must not retain a default sun");
        Require(frame.Local.size() == 2, "ECS point and spot lights must be collected");
        Require(frame.Local[0].ShadowFirstSlice == 0 && frame.Local[1].ShadowFirstSlice == 6,
            "Point lights need six shadow faces and spot lights one");
        const auto cachedPoint = frame.Local[0];
        const auto cachedSpot = frame.Local[1];
        auto localOff = CollectWorldLights(ecs, false);
        Require(localOff.Main.LocalLightCount == 2 && localOff.Local.size() == 2 &&
            localOff.Local[0].Radiance.x == cachedPoint.Radiance.x && localOff.Local[1].Radiance.x == cachedSpot.Radiance.x &&
            localOff.Local[0].ShadowFirstSlice == UINT_MAX && localOff.Local[1].ShadowFirstSlice == UINT_MAX,
            "Local shadow Off must preserve point and spot lighting while reserving no shadow slices");
        auto unshadowed = frame;
        unshadowed.Main.SunShadowFlags = 3;
        unshadowed.Main.LightRadiance = { 1, 2, 3 };
        ApplyShadowQuality({ 0, 0 }, unshadowed.Main, unshadowed.Local);
        Require(unshadowed.Main.SunShadowFlags == 0 && unshadowed.Main.LightRadiance.y == 2 &&
            unshadowed.Local[0].ShadowFlags == 0 && unshadowed.Local[0].ShadowFirstSlice == UINT_MAX,
            "Applying Off to submitted lights must remove only their shadow participation");
        Require(ShouldRenderLocalShadow(cachedPoint, nullptr), "Cached shadows must render at startup");
        Require(!ShouldRenderLocalShadow(cachedPoint, &cachedPoint), "Unchanged cached shadows must be reused");

        ecs.Get<LightComponent>(point).Intensity *= 2.0f;
        frame = CollectWorldLights(ecs);
        Require(frame.Local[0].Radiance.x > cachedPoint.Radiance.x, "Systems must update light intensity");
        Require(!ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Intensity changes must not rebake depth");
        ecs.Get<TransformComponent>(point).Location.x += 10.0f;
        frame = CollectWorldLights(ecs);
        Require(ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Moving a cached light must invalidate its shadows");
        ecs.Get<LightComponent>(spot).OuterAngle = 50.0f;
        frame = CollectWorldLights(ecs);
        Require(ShouldRenderLocalShadow(frame.Local[1], &cachedSpot), "Spot cone changes must invalidate the projection");
        auto unchanged = frame.Local[1];
        ecs.Get<LightComponent>(spot).InvalidateShadow();
        frame = CollectWorldLights(ecs);
        Require(ShouldRenderLocalShadow(frame.Local[1], &unchanged), "Systems must be able to invalidate a shadow cache explicitly");
        unchanged = frame.Local[1];
        ecs.Get<LightComponent>(spot).ShadowUpdate = ShadowUpdateMode::EveryFrame;
        frame = CollectWorldLights(ecs);
        Require(ShouldRenderLocalShadow(frame.Local[1], &unchanged), "EveryFrame must update unchanged light shadows");
        ecs.Get<LightComponent>(point).CastShadows = false;
        frame = CollectWorldLights(ecs);
        Require(!ShouldRenderLocalShadow(frame.Local[0], &cachedPoint), "Shadow-disabled lights must remain unshadowed");

        const Vector3 directions[] = { Vector3::UnitX, -Vector3::UnitX, Vector3::UnitY, -Vector3::UnitY, Vector3::UnitZ, -Vector3::UnitZ };
        for (int face = 0; face < 6; ++face)
        {
            const Matrix matrix = Matrix(cachedPoint.ShadowViewProj[face]).Transpose();
            const Vector3 projected = Vector3::Transform(Vector3(cachedPoint.Position) + directions[face] * 10.0f, matrix);
            Require(std::abs(projected.x) < 0.001f && std::abs(projected.y) < 0.001f && projected.z > 0 && projected.z < 1,
                "Point shadow faces must cover their corresponding cube directions");
        }

        const auto pointKey = GetLocalShadowKey(cachedPoint);
        const auto spotKey = GetLocalShadowKey(cachedSpot);
        ecs.DestroyEntity(point);
        frame = CollectWorldLights(ecs);
        Require(frame.Local.size() == 1 && GetLocalShadowKey(frame.Local[0]) == spotKey &&
            frame.Local[0].ShadowFirstSlice == 0, "Dense pool movement must preserve the remaining spot identity");
        auto replacement = ecs.CreateEntity();
        replacement.AddComponent<TransformComponent>(Vector3(10, 20, 30));
        replacement.AddComponent<LightComponent>();
        frame = CollectWorldLights(ecs);
        Require(frame.Local[0].EntityId == pointKey.EntityId && !(GetLocalShadowKey(frame.Local[0]) == pointKey) &&
            ShouldRenderLocalShadow(frame.Local[0], &cachedPoint),
            "Immediate entity-slot reuse must invalidate matching matrices, revisions and slices");
        Require(!(GetLocalShadowKey(frame.Local[0]) == pointKey) && ShouldRenderLocalShadow(frame.Local[0], &cachedPoint),
            "A replacement light must reject the old cache identity");
        const auto replacementKey = GetLocalShadowKey(frame.Local[0]);
        auto& replacementLight = ecs.Get<LightComponent>(replacement);
        const auto copy = replacementLight;
        replacementLight = copy;
        Require(copy.GetShadowGeneration() != replacementKey.Generation &&
            replacementLight.GetShadowGeneration() == replacementKey.Generation,
            "Copy construction must create an identity while copy assignment keeps the existing light identity");
        auto copiedConstants = frame.Local[0];
        SetLocalShadowGeneration(copiedConstants, 0x123456789abcdef0ull);
        Require(GetLocalShadowKey(copiedConstants).Generation == 0x123456789abcdef0ull,
            "Both halves of the generation must survive in the CPU light snapshot");
        std::cout << "ECS light lifetime, shadow output mapping, and cached/every-frame policy passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
