#pragma once

#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

inline void MigrateBistroIniKey(std::wstring& key)
{
    constexpr std::pair<const wchar_t*, const wchar_t*> renamed[]{
        {L"App.RenderInBackground", L"App.Window.RenderInBackground"},
        {L"Render.NumFrames", L"Render.Device.FramesInFlight"},
        {L"Render.PrimaryAdapter", L"Render.Device.PrimaryAdapter"},
        {L"App.Async.Queues", L"Render.Queues.AsyncEnabled"},
        {L"App.Async.AO", L"Render.AO.AsyncCompute"},
        {L"App.Async.Environment", L"Render.Sky.AsyncCompute"},
        {L"App.Async.LightGrid", L"Render.LightGrid.AsyncCompute"},
        {L"App.Async.Particles", L"Render.Particles.AsyncCompute"},
        {L"App.Async.Water", L"Render.Water.AsyncCompute"},
        {L"App.Grass.Wind.Enabled", L"Render.Wind.FieldEnabled"},
        {L"App.Particles.Simulation", L"Render.Particles.Integrator"},
        {L"Render.Reflection.Schedule", L"Render.ReflectionProbes.Schedule.Mode"},
        {L"Render.Reflection.FullUpdateDistanceMetres", L"Render.ReflectionProbes.Schedule.FullUpdateDistanceMetres"},
        {L"App.ReflectionProbes.FirstAdapter", L"Render.ReflectionProbes.First.Adapter"},
        {L"App.ReflectionProbes.FirstFacesPerFrame", L"Render.ReflectionProbes.First.FacesPerFrame"},
        {L"App.ReflectionProbes.FirstImplementation", L"Render.ReflectionProbes.First.Implementation"},
        {L"App.DepthNormalsPrepass", L"Render.DepthNormals.PrepassEnabled"},
        {L"App.NPC.Skinning.Adapter", L"Render.Skinning.NPC.Adapter"},
        {L"App.Fox.Skinning.Adapter", L"Render.Skinning.Fox.Adapter"},
        {L"App.Async.CloudPicture", L"Render.CloudPicture.AsyncCompute"},
        {L"App.UI.WorldCanvas.Adapter.Atrium", L"Render.WorldCanvas.Atrium.Adapter"},
        {L"App.UI.WorldCanvas.Adapter.Terrace", L"Render.WorldCanvas.Terrace.Adapter"},
        {L"App.UI.WorldCanvas.CrossSync.Atrium", L"Render.WorldCanvas.Atrium.CrossSync"},
        {L"App.UI.WorldCanvas.CrossSync.Terrace", L"Render.WorldCanvas.Terrace.CrossSync"},
        {L"App.UI.WorldCanvas.Adapter.Clouds", L""},
        {L"App.UI.WorldCanvas.CrossSync.Clouds", L""}
    };
    for (const auto& [previous, current] : renamed)
        if (_wcsicmp(key.c_str(), previous) == 0) { key = current; return; }
    constexpr std::pair<std::wstring_view, std::wstring_view> lightPrefixes[]{
        {L"App.Shadows.Local.Adapter.", L".Adapter"},
        {L"App.Shadows.Local.CrossSync.", L".CrossSync"}
    };
    for (const auto& [prefix, suffix] : lightPrefixes) {
        if (_wcsnicmp(key.c_str(), prefix.data(), prefix.size()) == 0) {
            key = L"Render.Shadows.Local." + key.substr(prefix.size()) + std::wstring(suffix);
            return;
        }
    }
    constexpr std::pair<std::wstring_view, std::wstring_view> groups[]{
        {L"App.UI.CloudPicture.", L"Render.CloudPicture."},
        {L"App.UI.GameHUD.", L"Render.GameHUD."},
        {L"App.UI.WorldCanvas.", L"Render.WorldCanvas."},
        {L"App.CloudPicture.", L"Render.CloudPicture."},
        {L"App.AO.", L"Render.AO."},
        {L"App.Grass.Wind.", L"Render.Wind."},
        {L"App.Grass.", L"Render.Grass."},
        {L"App.Particles.", L"Render.Particles."},
        {L"Render.Fluid.", L"Render.Water."},
        {L"App.Water.", L"Render.Water."},
        {L"App.LightGrid.", L"Render.LightGrid."},
        {L"Render.Lighting.Cluster.", L"Render.LightGrid.Cluster."},
        {L"App.ReflectionProbes.", L"Render.ReflectionProbes."},
        {L"Render.Reflection.", L"Render.ReflectionProbes."},
        {L"App.Shadows.", L"Render.Shadows."},
        {L"App.Sky.", L"Render.Sky."},
        {L"Render.SkyAtmosphere.", L"Render.Sky.Atmosphere."},
        {L"Render.SkyBox.", L"Render.Sky.SkyBox."},
        {L"Render.Clouds.", L"Render.Sky.Clouds."},
        {L"App.WBOIT.", L"Render.WBOIT."},
        {L"App.Post.", L"Render.Post."},
        {L"Render.UI.GameHUD.", L"Render.GameHUD."},
        {L"Render.UI.WorldCanvas.", L"Render.WorldCanvas."}
    };
    for (const auto& [previous, current] : groups) {
        if (_wcsnicmp(key.c_str(), previous.data(), previous.size()) == 0) {
            key.replace(0, previous.size(), current);
            break;
        }
    }
    for (const auto* retired : {L"Render.ReflectionProbes.ShowSpheres", L"Render.ReflectionProbes.Seed",
                                L"Render.ReflectionProbes.Reset"})
        if (_wcsicmp(key.c_str(), retired) == 0) { key.clear(); return; }
}
