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
    require(!extent.SetFromString("0") && !extent.SetFromString("-1") && !extent.SetFromString("nan") &&
        !center.SetFromString("inf"), "Invalid bounds coordinates/extents accepted");
    const auto initial = center.GetString();
    require(center.SetFromString("700") && extent.SetFromString("50"), "Cannot edit detached particle bounds");
    world.GetECS().ForEach<ParticleEmitterComponent, TransformComponent, NameComponent>(
        [&](Entity, const ParticleEmitterComponent& emitter, const TransformComponent& transform, const NameComponent& name)
        {
            if (name.value == "BistroEntranceFountain")
                require(transform.Location == Vector3(-170,45,-170) && emitter.GetSettings().ShapeExtents == Vector3(8,2,8),
                    "Bounds editing must not change the real emitter");
        });
    require(console.FindConsoleVariable(L"App.Bounds.BistroWater0.CenterY").GetFloat() == 41.3f &&
        console.FindConsoleVariable(L"App.Bounds.BistroWater0.HalfExtentX").GetFloat() == 44,
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
    size_t edited = 0;
    world.GetECS().ForEach<TransformComponent, NameComponent>(
        [&](Entity, const TransformComponent& transform, const NameComponent& name)
        {
            if (name.value == "BistroInteriorDust" || name.value == "BistroTableSteam" || name.value == "BistroPlanterFireflies") return;
            const auto key = L"App.Bounds." + std::wstring(name.value.begin(), name.value.end()) + L".CenterY";
            IConsoleVariable* setting = nullptr;
            if (!console.TryFindConsoleVariable(key, setting)) return;
            const auto before = transform.Location;
            const auto original = setting->GetString();
            require(setting->SetFromString("1000") && transform.Location == before,
                "Editing a bounds centre changed a real effect or character transform");
            require(setting->SetFromString(original), "Cannot restore detached box centre");
            ++edited;
        });
    require(edited == 22 && console.FindConsoleVariable(L"App.Bounds.BistroNPC0.HalfExtentY").GetFloat() > 25 &&
        console.FindConsoleVariable(L"App.Bounds.BistroFox0.HalfExtentY").GetFloat() > 1,
        "Character bounds must cover the scaled animated mesh, with independent controls for every effect");
    const auto foxBefore = console.FindConsoleVariable(L"App.Bounds.BistroFox0.CenterX").GetString();
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
    world.SetPaused(false);
    app.BenchmarkFrame();
    require(center.GetFloat() == 700 &&
        console.FindConsoleVariable(L"App.Bounds.BistroFox0.CenterX").GetString() == foxBefore,
        "Scene updates must not overwrite detached bounds");
    require(center.SetFromString(initial) && extent.SetFromString("8"), "Cannot restore bounds fixture");
    console.FindConsoleVariable(L"App.Bounds.Show").SetFromString("false");
    world.SetPaused(paused);
    app.MsgProc(window, WM_KEYUP, VK_F1, 0);
}
