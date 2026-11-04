#pragma once

inline void CheckBistroBounds(BistroApp& app, World& world)
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    ConsoleModule console;
    // Eight emitters, one grass patch, one water volume, four soldiers and one fox.
    require(console.GetConsoleVariables(L"App.Bounds.").size() == 15 * 7 + 1,
        "Every Bistro effect must have its own detached bounds controls");
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
        "Dust and Steam must use independent room/bar layout boxes");
    world.GetECS().ForEach<ParticleEmitterComponent, TransformComponent, NameComponent>(
        [&](Entity, const ParticleEmitterComponent& emitter, const TransformComponent& transform, const NameComponent& name)
        {
            if (name.value == "BistroInteriorDust")
                require(transform.Location == Vector3(160,180,-160) &&
                    emitter.GetSettings().ShapeExtents == Vector3(250,100,180) && !emitter.GetSettings().VolumeEnabled,
                    "Dust layout must not change its emitter before approval");
            if (name.value == "BistroTableSteam")
                require(transform.Location == Vector3(550,130,-350) &&
                    emitter.GetSettings().ShapeExtents == Vector3(35,1,20) && !emitter.GetSettings().VolumeEnabled,
                    "Steam layout must not change its emitter before approval");
        });
    size_t edited = 0;
    world.GetECS().ForEach<TransformComponent, NameComponent>(
        [&](Entity, const TransformComponent& transform, const NameComponent& name)
        {
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
    require(edited == 15 && console.FindConsoleVariable(L"App.Bounds.BistroNPC0.HalfExtentY").GetFloat() > 25 &&
        console.FindConsoleVariable(L"App.Bounds.BistroFox0.HalfExtentY").GetFloat() > 1,
        "Character bounds must cover the scaled animated mesh, with independent controls for every effect");
    const auto foxBefore = console.FindConsoleVariable(L"App.Bounds.BistroFox0.CenterX").GetString();
    const auto window = app.GetWindow()->GetWindowHandle();
    const auto paused = world.IsPaused();
    world.SetPaused(true);
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
