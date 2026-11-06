#pragma once

inline void CheckBistroArch(World& world)
{
    const auto require = [](bool value, const char* message)
    { if (!value) throw std::runtime_error(message); };
    unsigned swarms = 0;
    world.GetECS().ForEach<NameComponent, TransformComponent, ParticleEmitterComponent>(
        [&](Entity, const NameComponent& name, const TransformComponent& transform, const ParticleEmitterComponent& emitter)
        {
            if (name.value != "BistroPlanterFireflies") return;
            ++swarms;
            const auto& settings = emitter.GetSettings();
            require(transform.Location == Vector3(1399.6f, 300, 1369) && transform.Rotation.y == 69 &&
                settings.Geometry == ParticleGeometry::Firefly && settings.Shape == ParticleShape::Box && settings.VolumeEnabled,
                "Fireflies must spawn and stay in the rotated arch volume");
            const float radius = 10 * (1 + settings.SizeVariation) * settings.RenderScale * 0.707107f;
            const auto extents = settings.VolumeHalfExtents + Vector3(radius);
            require((extents - Vector3(200,220,530)).Length() < 0.001f && settings.ShapeExtents == settings.VolumeHalfExtents,
                "The whole swarm, including mesh radius, must fit inside the arch box");
            const float bottom = transform.Location.y - extents.y;
            const float top = transform.Location.y + extents.y;
            require(bottom >= 80 && std::abs((top - 23.3f) / (688 - 23.3f) - 0.75f) < 0.005f,
                "Swarm must clear the road and fill the lower three quarters of the arch");
        });
    require(swarms == 1, "Expected one arch swarm");
    world.GetECS().ForEach<GrassRenderComponent, TransformComponent>(
        [&](Entity, const GrassRenderComponent& grass, const TransformComponent& transform)
        {
            require(transform.Location == Vector3(1399.6f,23.3f,1369) && transform.Rotation.y == 69 &&
                grass.ExtentsCm == Vector2(200,530) && GrassBladeCount(grass) == 60000,
                "Grass must cover the entire rotated swarm footprint");
            require(transform.Location.y + grass.HeightCm * (1 + grass.HeightVariation) < 80,
                "Grass tips must remain below the swarm");
            BoundingBox bounds;
            GrassLocalBounds(grass).Transform(bounds, Matrix::CreateRotationY(XMConvertToRadians(transform.Rotation.y)));
            const auto& wind = world.GetECS().Get<WindFieldComponent>(grass.WindEntity);
            require(wind.ExtentsCm.x >= bounds.Extents.x && wind.ExtentsCm.y >= bounds.Extents.z,
                "World-aligned wind must cover all rotated grass roots and bent tips");
        });
}
