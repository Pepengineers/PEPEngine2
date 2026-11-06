#pragma once

inline void CheckBistroBounds(BistroApp& app, World& world)
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    ConsoleModule console;
    // Eight emitters, one grass patch, one water volume, ten soldiers and five foxes.
    require(console.GetConsoleVariables(L"App.Bounds.").size() == 25 * 7 + 1,
        "Every Bistro effect must have its own layout bounds controls");
    auto& center = console.FindConsoleVariable(L"App.Bounds.BistroEntranceFountain.CenterX");
    auto& extent = console.FindConsoleVariable(L"App.Bounds.BistroEntranceFountain.HalfExtentX");
    require(center.GetFloat() == -170 && extent.GetFloat() == 8, "Particle spawn bounds are incorrect");
    auto& layoutExtent = console.FindConsoleVariable(L"App.Bounds.BistroInteriorDust.HalfExtentX");
    auto& layoutCenter = console.FindConsoleVariable(L"App.Bounds.BistroInteriorDust.CenterX");
    require(!layoutExtent.SetFromString("0") && !layoutExtent.SetFromString("-1") && !layoutExtent.SetFromString("nan") &&
        !layoutCenter.SetFromString("inf"), "Invalid editable bounds coordinates/extents accepted");
    require(!center.SetFromString("700") && !extent.SetFromString("50"),
        "Tracked bounds must report the real object instead of accepting detached edits");
    require(console.FindConsoleVariable(L"App.Bounds.BistroWater0.CenterY").GetFloat() == BistroWater::Center.y &&
        std::abs(console.FindConsoleVariable(L"App.Bounds.BistroWater0.HalfExtentX").GetFloat() -
            (BistroWater::HalfExtent.x + BistroWater::HalfExtent.z) * std::sqrt(0.5f)) < 0.01f,
        "Water bounds must use the fluid container");
    require(console.FindConsoleVariable(L"App.Bounds.BistroGrass0.HalfExtentY").GetFloat() > 15,
        "Grass bounds must include blade height and wind displacement");
    require(console.FindConsoleVariable(L"App.Bounds.BistroInteriorDust.CenterX").GetFloat() == 559 &&
        console.FindConsoleVariable(L"App.Bounds.BistroInteriorDust.HalfExtentZ").GetFloat() == 779 &&
        console.FindConsoleVariable(L"App.Bounds.BistroTableSteam.CenterX").GetFloat() == 922 &&
        console.FindConsoleVariable(L"App.Bounds.BistroTableSteam.HalfExtentX").GetFloat() == 305,
        "Dust and Steam must use the approved room/bar layout boxes");
    Entity dustEntity = InvalidEntity;
    Entity steamEntity = InvalidEntity;
    size_t linked = 0;
    world.GetECS().ForEach<ParticleEmitterComponent, TransformComponent, NameComponent>(
        [&](Entity entity, const ParticleEmitterComponent& emitter, const TransformComponent& transform, const NameComponent& name)
        {
            const bool dust = name.value == "BistroInteriorDust";
            if (!dust && name.value != "BistroTableSteam") return;
            const auto& settings = emitter.GetSettings();
            require(transform.Location == (dust ? Vector3(559,221,-348) : Vector3(922,227,-293)) &&
                transform.Rotation == Vector3(0,-28.5f,0) && transform.Scale == Vector3::One &&
                settings.Shape == ParticleShape::Box && settings.VolumeEnabled &&
                settings.ShapeExtents == settings.VolumeHalfExtents,
                "Dust/Steam spawn and confinement must use their approved rotated bounds");
            const Vector3 expected = dust ? Vector3(692.409f,179.409f,777.409f) : Vector3(290.858f,160.858f,71.358f);
            require((settings.ShapeExtents - expected).Length() < 0.01f,
                "Bounded particle centres must leave room for the billboard corners");
            if (dust) dustEntity = entity;
            else steamEntity = entity;
            ++linked;
        });
    require(linked == 2, "Exactly Dust and Steam must be linked to layout volumes");
    size_t tracked = 0;
    world.GetECS().ForEach<TransformComponent, NameComponent>(
        [&](Entity, const TransformComponent&, const NameComponent& name)
        {
            if (name.value == "BistroInteriorDust" || name.value == "BistroTableSteam" || name.value == "BistroPlanterFireflies") return;
            const auto key = L"App.Bounds." + std::wstring(name.value.begin(), name.value.end()) + L".CenterY";
            IConsoleVariable* setting = nullptr;
            if (!console.TryFindConsoleVariable(key, setting)) return;
            require(!setting->SetFromString("1000"), "Tracked coordinates must be read-only");
            ++tracked;
        });
    require(tracked == 22 && console.FindConsoleVariable(L"App.Bounds.BistroNPC0.HalfExtentY").GetFloat() > 25 &&
        console.FindConsoleVariable(L"App.Bounds.BistroFox0.HalfExtentY").GetFloat() > 1,
        "Character bounds must cover the scaled mesh");
    const auto window = app.GetWindow()->GetWindowHandle();
    const auto paused = world.IsPaused();
    world.SetPaused(true);
    auto& dust = world.GetECS().Get<ParticleEmitterComponent>(dustEntity);
    const auto revision = dust.GetResetRevision();
    auto& dustX = console.FindConsoleVariable(L"App.Bounds.BistroInteriorDust.CenterX");
    auto& steamY = console.FindConsoleVariable(L"App.Bounds.BistroTableSteam.CenterY");
    require(dustX.SetFromString("600") && steamY.SetFromString("250"), "Bounded layout edit rejected");
    app.BenchmarkFrame();
    require(world.GetECS().Get<TransformComponent>(dustEntity).Location.x == 600 &&
        world.GetECS().Get<TransformComponent>(steamEntity).Location.y == 250 &&
        dust.GetResetRevision() > revision, "Moving a live bound must relocate and reset its particle pool");
    const auto movedRevision = dust.GetResetRevision();
    app.BenchmarkFrame();
    require(dust.GetResetRevision() == movedRevision, "Unchanged bounds must not reset particles each frame");
    require(BistroParticles::VolumeEnabled.SetValue(true), "Global volume override rejected");
    app.BenchmarkFrame();
    require(dust.GetSettings().VolumeHalfExtents.x > 690 &&
        world.GetECS().Get<ParticleEmitterComponent>(steamEntity).GetSettings().VolumeHalfExtents.z < 72 &&
        dust.GetResetRevision() == movedRevision,
        "Global particle volumes must not override the linked Dust/Steam bounds");
    require(BistroParticles::VolumeEnabled.SetValue(false), "Cannot restore global volume override");
    require(dustX.SetFromString("559") && steamY.SetFromString("227"), "Cannot restore linked layout fixture");
    app.MsgProc(window, WM_KEYUP, VK_F1, 0);
    console.FindConsoleVariable(L"App.Bounds.Show").SetFromString("false");
    app.BenchmarkFrame();
    require(ImGui::GetDrawData()->TotalVtxCount == 0, "Disabled bounds must not draw geometry");
    console.FindConsoleVariable(L"App.Bounds.Show").SetFromString("true");
    app.BenchmarkFrame();
    require(ImGui::GetDrawData()->TotalVtxCount > 0, "Enabled effect bounds must draw labeled boxes while paused");
    struct Moved { Entity Id; Vector3 Location; float CenterX; std::wstring Key; };
    std::vector<Moved> moved;
    world.GetECS().ForEach<TransformComponent, NameComponent>(
        [&](Entity entity, TransformComponent& transform, const NameComponent& name)
        {
            const auto key = L"App.Bounds." + std::wstring(name.value.begin(), name.value.end()) + L".CenterX";
            IConsoleVariable* setting = nullptr;
            if (!console.TryFindConsoleVariable(key, setting)) return;
            moved.push_back({entity, transform.Location, setting->GetFloat(), key});
            transform.Location.x += 123;
            transform.DirtyFlag = true;
        });
    require(moved.size() == 25, "Every existing bounds owner must participate in the movement check");
    app.BenchmarkFrame();
    for (const auto& object : moved)
    {
        require(std::abs(console.FindConsoleVariable(object.Key).GetFloat() - object.CenterX - 123) < 0.01f,
            "Bounds must follow the current transform, including authored volumes while paused");
        auto& transform = world.GetECS().Get<TransformComponent>(object.Id);
        transform.Location = object.Location;
        transform.DirtyFlag = true;
    }
    app.BenchmarkFrame();
    Entity fountain = InvalidEntity;
    world.GetECS().ForEach<NameComponent>([&](Entity entity, const NameComponent& name)
        { if (name.value == "BistroEntranceFountain") fountain = entity; });
    auto& transform = world.GetECS().Get<TransformComponent>(fountain);
    const auto savedTransform = transform;
    transform.Rotation = {25, 63, -17};
    transform.Scale = {2, 3, 4};
    transform.DirtyFlag = true;
    app.BenchmarkFrame();
    BoundingBox expected;
    const auto matrix = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
        XMConvertToRadians(63.f), XMConvertToRadians(25.f), XMConvertToRadians(-17.f)) *
        Matrix::CreateTranslation(transform.Location);
    BoundingBox({0,0,0}, {8,2,8}).Transform(expected, matrix);
    const auto prefix = L"App.Bounds.BistroEntranceFountain.";
    require(std::abs(console.FindConsoleVariable(std::wstring(prefix) + L"HalfExtentX").GetFloat() - expected.Extents.x) < 0.01f &&
        std::abs(console.FindConsoleVariable(std::wstring(prefix) + L"HalfExtentY").GetFloat() - expected.Extents.y) < 0.01f &&
        std::abs(console.FindConsoleVariable(std::wstring(prefix) + L"HalfExtentZ").GetFloat() - expected.Extents.z) < 0.01f,
        "Tracked bounds must include pitch, yaw, roll and nonuniform scale");
    transform = savedTransform;
    transform.DirtyFlag = true;
    auto& dustTransform = world.GetECS().Get<TransformComponent>(dustEntity);
    const auto savedDustTransform = dustTransform;
    dustTransform.Scale = {0.5f, 2, -1.5f};
    dustTransform.Rotation = {15, 35, -20};
    dustTransform.DirtyFlag = true;
    app.BenchmarkFrame();
    const auto& dustSettings = dust.GetSettings();
    const float radius = (std::max)(dustSettings.SizeStart, dustSettings.SizeEnd) *
        (1 + dustSettings.SizeVariation) * dustSettings.RenderScale * 0.707107f;
    const auto extents = dustSettings.VolumeHalfExtents;
    require(std::abs((694 - extents.x) * 0.5f - radius) < 0.01f &&
        std::abs((181 - extents.y) * 2 - radius) < 0.01f &&
        std::abs((779 - extents.z) * 1.5f - radius) < 0.01f,
        "Scaled confined volumes must still reserve the world-space sprite radius");
    require(dustTransform.Rotation == Vector3(15,35,-20) && dustTransform.Scale == Vector3(0.5f,2,-1.5f),
        "Live volume bounds must preserve the current rotation and scale");
    dustTransform = savedDustTransform;
    dustTransform.DirtyFlag = true;
    world.SetPaused(false);
    app.BenchmarkFrame();
    console.FindConsoleVariable(L"App.Bounds.Show").SetFromString("false");
    {
        World fixture(WorldDesc{});
        auto& ecs = fixture.GetECS();
        const Entity owner = ecs.CreateEntity();
        ecs.Add<NameComponent>(owner, "BoundsOwnerTest");
        ecs.Add<TransformComponent>(owner, Vector3(10,20,30));
        ecs.Add<ParticleEmitterComponent>(owner);
        BistroBounds::Scene bounds(fixture);
        ecs.DestroyEntity(owner);
        const Entity replacement = ecs.CreateEntity();
        require(replacement == owner, "Bounds lifecycle fixture must exercise a recycled entity ID");
        ecs.Add<NameComponent>(replacement, "BoundsReplacementTest");
        ecs.Add<TransformComponent>(replacement, Vector3(999,20,30));
        ecs.Add<ParticleEmitterComponent>(replacement);
        bounds.DrawBounds(fixture, *BenchmarkEngine::GetLocator().GetModule<RenderModule>());
        require(console.FindConsoleVariable(L"App.Bounds.BoundsOwnerTest.CenterX").GetFloat() == 10,
            "A removed object's bounds must not follow a different owner with a recycled ID");
    }
    world.SetPaused(paused);
    app.MsgProc(window, WM_KEYUP, VK_F1, 0);
}
