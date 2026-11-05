#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include <stdexcept>

inline void TestPreparedShadowViews()
{
    const auto require = [](bool value, const char* message)
    { if (!value) throw std::runtime_error(message); };
    GDX12DeviceResources resources;
    RenderPipelineCommonData common;
    resources.ShadowSettings = {};
    resources.Lighting.SunShadowFlags = 0;
    GDX12CascadedShadowPass disabled;
    disabled.Setup(&resources, nullptr, &common);
    disabled.Prepare();
    for (UINT slot = 0; slot < disabled.GetGeometryViewCapacity(); ++slot)
        require(!disabled.GetGeometryView(slot), "Disabled sun published geometry preparation views");

    resources.Lighting.SunShadowFlags = 1;
    resources.CurrFrameConstantsIndex = 2;
    common.CameraView = Matrix::CreateTranslation(-3, -4, -5);
    GDX12CascadedShadowPass sun;
    sun.Setup(&resources, nullptr, &common);
    require(!sun.GetGeometryView(0), "Unprepared sun published a view");
    sun.Prepare();
    require(sun.GetGeometryViewCapacity() == 4 && !sun.GetGeometryView(4), "Sun view topology changed");
    for (UINT slot = 0; slot < sun.GetGeometryViewCapacity(); ++slot)
    {
        const auto* view = sun.GetGeometryView(slot);
        require(view && view->Stage == GDX12GeometryStage::Shadow && view->Key == ((2ull << 60) | slot) &&
            view->FrameSlot == 2 && view->DepthFormat == DXGI_FORMAT_D32_FLOAT &&
            view->Filter == GDX12GeometryFilter::All && (view->Eye - Vector3(3, 4, 5)).LengthSquared() < 0.00001f,
            "Sun preparation lost a cascade, current camera eye or target format");
    }
    common.CameraView = Matrix::Identity;
    sun.Prepare();
    require((sun.GetGeometryView(0)->Eye - Vector3(3, 4, 5)).LengthSquared() < 0.00001f,
        "Repeated preparation replaced a pending sun plan");

    resources.LocalLights.resize(3);
    resources.LocalLights[0].Type = 1;
    resources.LocalLights[0].ShadowFirstSlice = UINT_MAX;
    resources.LocalLights[1].Type = 1;
    resources.LocalLights[1].ShadowFirstSlice = 0;
    resources.LocalLights[2].Type = 2;
    resources.LocalLights[2].ShadowFirstSlice = 6;
    GDX12LocalShadowPass local, staticLocal(true);
    local.Setup(&resources, nullptr, &common);
    staticLocal.Setup(&resources, nullptr, &common);
    require(local.GetGeometryViewCapacity() == 14 && staticLocal.GetGeometryViewCapacity() == 7,
        "Local shadow topology allocated unshadowed lights or six slots for a spot");
    for (UINT slot = 0; slot < local.GetGeometryViewCapacity(); ++slot)
        require(!local.GetGeometryView(slot), "Unprepared local shadow published a view");
}
