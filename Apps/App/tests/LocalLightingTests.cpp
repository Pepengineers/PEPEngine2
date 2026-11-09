#include "App.Base/ECS/WorldLightSnapshot.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static void CheckShadowOutputs(const GDX12LocalLightConstants& point, const GDX12LocalLightConstants& spot)
{
    using Mode = GDX12ShadowSelection::Mode;
    const auto pointKey = GetLocalShadowKey(point);
    const auto spotKey = GetLocalShadowKey(spot);
    Require(pointKey.Generation && spotKey.Generation && !(pointKey == spotKey), "ECS lights need distinct stable keys");
    const GDX12ShadowSelection all;
    const GDX12ShadowSelection sunOnly{ Mode::Sun, {} };
    const GDX12ShadowSelection localOnly{ Mode::AllLocal, {} };
    const GDX12ShadowSelection onePoint{ Mode::SpecificLight, pointKey };
    const GDX12ShadowSelection oneSpot{ Mode::SpecificLight, spotKey };
    Require(all.IncludesSun() && all.IncludesLocal(pointKey) && all.IncludesLocal(spotKey) &&
        sunOnly.IncludesSun() && !sunOnly.IncludesLocal(pointKey) &&
        !localOnly.IncludesSun() && localOnly.IncludesLocal(pointKey) && localOnly.IncludesLocal(spotKey) &&
        !onePoint.IncludesSun() && onePoint.IncludesLocal(pointKey) && !onePoint.IncludesLocal(spotKey) &&
        oneSpot.IncludesLocal(spotKey) && !oneSpot.IncludesLocal(pointKey) && !all.IncludesLocal({}),
        "Selections must respect producer kind and match the complete local key");

    const auto pointOutput = BuildLocalShadowOutput(point, {}, 5, false);
    Require(pointOutput.ViewCount() == 6, "Point publication must describe all six faces");
    for (UINT face = 0; face < 6; ++face)
        Require(pointOutput.Views[face].Region.Slice == face && pointOutput.Views[face].Region.Width == 256 &&
            std::memcmp(&pointOutput.Views[face].ViewProj, &point.ShadowViewProj[face], sizeof(XMFLOAT4X4)) == 0,
            "Point faces must describe their own matrices and atlas slices");
    const auto spotOutput = BuildLocalShadowOutput(spot, {}, 5, false);
    Require(spotOutput.ViewCount() == 1 && spotOutput.Views[0].Region.Slice == 6, "Spot publication must describe one slice");
    auto relocated = spot;
    relocated.ShadowFirstSlice = 0;
    const auto remapped = BuildLocalShadowOutput(relocated, {}, 5, false);
    Require(oneSpot.IncludesLocal(GetLocalShadowKey(relocated)) && remapped.Views[0].Region.Slice == 0 &&
        remapped.MatchesDepthInputs(spotOutput) && ShouldRenderLocalShadow(relocated, &spot),
        "Remapping may reuse matching imported depth, but requires repainting a different local atlas slice");
    auto changed = pointOutput;
    ++changed.Views[5].CasterRevision;
    Require(!changed.MatchesDepthInputs(pointOutput), "A stale point face must reject the entire publication");
    changed = pointOutput;
    changed.Views[4].ViewProj._14 += 1.0f;
    Require(!changed.MatchesDepthInputs(pointOutput), "A changed point projection must reject the entire publication");
    Require(!BuildLocalShadowOutput(point, {}, 5, true).MatchesDepthInputs(pointOutput),
        "Static-only depth must not be reused as final dynamic depth");
    const Vector3 nearPoint = Vector3(point.Position) + Vector3::UnitX * 10.0f;
    std::vector<GDX12LocalShadowCaster> casters{ { BoundingBox(nearPoint, Vector3::One), 1, false },
        { BoundingBox(nearPoint, Vector3::One), 2, true } };
    const auto before = BuildLocalShadowVisibility(point, casters);
    ++casters[1].Signature;
    const auto dynamicChange = BuildLocalShadowVisibility(point, casters);
    Require(BuildLocalShadowOutput(point, before.StaticCasters, 5, true).MatchesDepthInputs(
        BuildLocalShadowOutput(point, dynamicChange.StaticCasters, 5, true)) &&
        !BuildLocalShadowOutput(point, before.AllCasters, 5, false).MatchesDepthInputs(
            BuildLocalShadowOutput(point, dynamicChange.AllCasters, 5, false)),
        "A changed dynamic caster must invalidate final depth and preserve static-only depth");
    ++casters[0].Signature;
    const auto staticChange = BuildLocalShadowVisibility(point, casters);
    Require(!BuildLocalShadowOutput(point, dynamicChange.StaticCasters, 5, true).MatchesDepthInputs(
        BuildLocalShadowOutput(point, staticChange.StaticCasters, 5, true)),
        "A changed static caster must invalidate the static-only group");

    const Vector3 sunDirection(0.35f, -1.0f, 0.25f);
    const auto sun = BuildShadowCascades(Matrix::Identity, XM_PIDIV4, 1.0f, 0.1f, 10000.0f, sunDirection);
    const auto sunOutput = BuildSunShadowOutput(sun, 7, 11, false);
    Require(sunOutput.ViewCount() == 4 && sunOutput.SplitDepths.w == sun.SplitDepths.w &&
        sunOutput.TexelWorldSize.z == sun.TexelWorldSize.z, "Sun publication must include all cascades and split metadata");
    const UINT x[] = { 0, 2048, 0, 2048 };
    const UINT y[] = { 0, 0, 2048, 2048 };
    for (UINT i = 0; i < 4; ++i)
        Require(sunOutput.Views[i].Region.Slice == 0 && sunOutput.Views[i].Region.X == x[i] &&
            sunOutput.Views[i].Region.Y == y[i] && sunOutput.Views[i].Region.Height == 2048,
            "Sun cascades must describe the four quadrants of one atlas");
    const auto cut = BuildShadowCascades(Matrix::CreateTranslation(300.0f, 0.0f, 1000.0f),
        XM_PIDIV4, 1.0f, 0.1f, 10000.0f, sunDirection);
    Require(!BuildSunShadowOutput(cut, 7, 11, false).MatchesDepthInputs(sunOutput) &&
        !BuildSunShadowOutput(sun, 7, 12, false).MatchesDepthInputs(sunOutput),
        "A camera cut or new caster revision must invalidate the sun group");
}

int main()
{
    try
    {
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
        CheckShadowOutputs(cachedPoint, cachedSpot);
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
        const GDX12ShadowSelection oldPoint{ GDX12ShadowSelection::Mode::SpecificLight, pointKey };
        Require(!oldPoint.IncludesLocal(GetLocalShadowKey(frame.Local[0])) &&
            !BuildLocalShadowOutput(frame.Local[0], {}, 5, false).MatchesDepthInputs(BuildLocalShadowOutput(cachedPoint, {}, 5, false)),
            "A replacement light must reject the old selection and its cached output");
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
