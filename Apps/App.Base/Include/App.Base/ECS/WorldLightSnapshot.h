#pragma once
#include "App.Base/ECS/Components/LightComponent.h"
#include "App.Base/ECS/Components/TransformComponent.h"
#include "Engine.Core/ECS/ECSStorage.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>

struct WorldLighting
{
    GDX12MainConstants Main;
    std::vector<GDX12LocalLightConstants> Local;
};

template<typename... Components>
inline WorldLighting CollectWorldLights(ECSStorage<Components...>& ecs)
{
    WorldLighting result;
    result.Main.LightRadiance = {};
    result.Main.SunShadowFlags = 0;
    ecs.template ForEach<TransformComponent, LightComponent>(
        [&result](Entity entity, TransformComponent& transform, LightComponent& light)
        {
            if (!light.Enabled) return;
            const Matrix rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
                XMConvertToRadians(transform.Rotation.x), XMConvertToRadians(transform.Rotation.z));
            const Vector3 direction = -rotation.Forward();
            const Vector3 radiance = light.Color * (std::max)(light.Intensity, 0.0f);
            if (light.Type == LightType::Directional)
            {
                result.Main.LightDirection = direction;
                result.Main.LightRadiance = radiance;
                result.Main.SunShadowFlags = (light.CastShadows ? 1u : 0u) | (light.ShadowUpdate == ShadowUpdateMode::EveryFrame ? 2u : 0u);
                result.Main.SunShadowRevision = light.ShadowRevision;
                return;
            }
            GDX12LocalLightConstants data;
            data.EntityId = entity;
            SetLocalShadowGeneration(data, light.GetShadowGeneration());
            data.Type = static_cast<UINT>(light.Type);
            data.Position = transform.Location;
            data.Direction = direction;
            data.Radiance = radiance;
            data.Range = std::clamp(light.Range, 1.0f, 100000.0f);
            const float outerAngle = std::clamp(light.OuterAngle, 1.0f, 89.0f);
            data.OuterCos = std::cos(XMConvertToRadians(outerAngle));
            data.InnerCos = std::cos(XMConvertToRadians(std::clamp(light.InnerAngle, 0.0f, outerAngle)));
            data.ShadowFlags = (light.CastShadows ? 1u : 0u) | (light.ShadowUpdate == ShadowUpdateMode::EveryFrame ? 2u : 0u);
            data.ShadowRevision = light.ShadowRevision;
            if (light.CastShadows) BuildLocalLightShadowMatrices(data);
            result.Local.push_back(data);
        });
    std::sort(result.Local.begin(), result.Local.end(), [](const auto& a, const auto& b) { return a.EntityId < b.EntityId; });
    UINT slice = 0;
    for (auto& light : result.Local)
        if (light.ShadowFlags & 1u)
        {
            light.ShadowFirstSlice = slice;
            slice += LocalShadowFaceCount(light);
        }
    result.Main.LocalLightCount = static_cast<UINT>(result.Local.size());
    return result;
}
