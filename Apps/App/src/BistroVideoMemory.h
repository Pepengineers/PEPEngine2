#pragma once

#include "BistroReflections.h"
#include "BistroScene.h"
#include "Techniques/ShadowMemory.h"
#include "Techniques/Linden/LindenMesh.h"
#include "Techniques/UI/BistroUICanvases.h"
#include "Techniques/Grass/BistroGrass.h"
#include "Techniques/Water/BistroWater.h"
#include "App.Base/BistroLightPlacements.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12LightGrid.h"
#include <chrono>
#include <map>

// Estimates scene resources before the render passes see a new set of console values.
class BistroVideoMemory
{
    using Bytes = uint64_t;
    using Charges = std::vector<Bytes>;
    struct Level { int Value; Charges Cost; };
    struct Feature
    {
        const wchar_t* Key;
        std::vector<Level> Levels;
        size_t Selected = 0, Requested = 0;
        bool ShowLevels = false;
    };

    std::map<std::wstring, Charges> _accepted;
    Charges _base, _lastBudget, _lastUsage, _lastShadows;
    uint64_t _revision = UINT64_MAX;
    uint32_t _size = 0;
    std::chrono::steady_clock::time_point _nextCheck{};

public:
    enum class Result { Fits, Limited, RetireLocalShadows, SmallerOutputRequired };
    std::wstring LastLimit;

    Result Apply(ConsoleModule& console, SceneManagerModule& scenes, RenderModule& render,
                 const std::vector<DeviceDesc>& adapters, uint16_t width, uint16_t height)
    {
        const auto now = std::chrono::steady_clock::now();
        const uint32_t size = (uint32_t(width) << 16) | height;
        if (!width || !height || (_revision == IConsoleVariable::ValueRevision() && size == _size && now < _nextCheck))
            return Result::Fits;
        _nextCheck = now + std::chrono::seconds(1);
        const auto value = [&](const wchar_t* key) { return console.FindConsoleVariable(key).GetInt(); };
        const size_t primary = value(L"Render.PrimaryAdapter");
        const auto owner = [&](int index) { return index >= 0 && size_t(index) < adapters.size() ? size_t(index) : primary; };
        Charges usage(adapters.size()), budget(adapters.size()), raw(adapters.size()), shadows(adapters.size());
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            const auto hr = adapters[i].Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
            shadows[i] = BistroRender::ShadowMemory::For(adapters[i].AdapterLuid).Allocated;
            usage[i] = SUCCEEDED(hr) ? info.CurrentUsage - (std::min)(info.CurrentUsage, shadows[i]) : 0;
            const Bytes available = info.Budget ? info.Budget : adapters[i].DedicatedVideoMemory;
            raw[i] = available;
            budget[i] = available - available / 10; // Leave space for driver heaps and transient allocations.
        }
        // Stable settings need only a periodic DXGI poll, even on a crowded adapter. New allocations
        // are checked against current usage below, including resources left resident by earlier passes.
        if (_revision == IConsoleVariable::ValueRevision() && _size == size && budget == _lastBudget && usage == _lastUsage && shadows == _lastShadows)
            return Result::Fits;
        _lastBudget = budget;
        _lastUsage = usage;
        _lastShadows = shadows;
        if (_base.empty()) _base = usage;
        const Bytes frames = value(L"Render.NumFrames"), pixels = Bytes(width) * height;
        const auto charge = [&](Bytes bytes, size_t gpu)
        {
            Charges result(adapters.size());
            result[gpu] = bytes;
            return result;
        };
        const auto replicated = [&](Bytes bytes, const wchar_t* key)
        {
            auto cost = charge(bytes, primary);
            cost[owner(value(key))] = bytes;
            return cost;
        };
        std::vector<Feature> features;
        const auto add = [&](const wchar_t* key, std::initializer_list<int> levels, auto estimate)
        {
            Feature feature{key};
            const auto& variable = console.FindConsoleVariable(key);
            const auto name = variable.GetName();
            const auto suffix = name.substr(name.find_last_of(L'.') + 1);
            feature.ShowLevels = suffix == L"Resolution" || suffix == L"Quality";
            for (int level : levels)
            {
                if (!variable.IsValidString(std::to_string(level))) continue;
                if (level == variable.GetInt()) feature.Selected = feature.Levels.size();
                feature.Levels.push_back({level, estimate(level)});
            }
            feature.Requested = feature.Selected;
            if (feature.Levels.empty() || feature.Levels[feature.Selected].Value != variable.GetInt())
                throw std::logic_error("VRAM estimate is missing a legal setting level");
            features.push_back(std::move(feature));
        };
        const auto depthBytes = [&](int side, UINT16 layers)
        {
            const UINT r = (std::max)(1, side);
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, r, r, layers, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            return render.GetPrimaryDevice()->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        };
        const Bytes geometry = render.GetPrimaryGeometryBytes();
        add(L"Render.Shadows.Sun.Resolution", {0,512,1024,2048,4096}, [&](int r)
        {
            const Bytes atlas = depthBytes(2 * r, 1) * frames;
            auto cost = charge(atlas, primary);
            const auto gpu = owner(value(L"Render.Shadows.Sun.Adapter"));
            if (r && gpu != primary) cost[gpu] += 2 * atlas + geometry * frames;
            return cost;
        });
        const auto grass = BistroGrass::ReadControls();
        add(L"Render.Grass.Quality", {0,1,2,3,4}, [&](int q)
        {
            const auto settings = ResolveGrassSettings(grass.Grass, q);
            const Bytes ribbons = settings.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2 : 1;
            const Bytes seg = settings.Segments;
            const Bytes stride = ((seg + 1) + ((std::max)(Bytes(1), seg / 2) + 1)) * 2 * ribbons + 4;
            const Bytes count = GrassBladeCount(settings);
            return charge(q && value(L"Render.Grass.Enabled") ? count * BistroGrass::Count.GetValue() *
                ((12 + stride * 44) * frames + seg * 6 * ribbons * 4) : 0, primary);
        });
        add(L"Render.Wind.Quality", {0,1,2,3,4}, [&](int q)
        {
            const Bytes grid = ResolveWindSettings(grass.Wind, q).GridResolution;
            return replicated(q && grass.WindEnabled && value(L"Render.Wind.Enabled") &&
                grass.Wind.Implementation == GDX12WindImplementation::ProjectedGrid ?
                grid * grid * 16 * (frames + 4) * BistroGrass::Count.GetValue() : 0, L"Render.Wind.Adapter");
        });
        add(L"Render.Tree.Quality", {0,1,2,3}, [&](int q)
        {
            const auto mesh = BistroRender::FindLindenMesh(scenes, q);
            return charge(mesh ? mesh->Vertices.size() * (sizeof(BistroRender::LindenVertex) + 3 * 24) +
                mesh->Fragments.size() * (sizeof(BistroRender::LindenFragment) + 28) + mesh->Indices.size() * 4 : 0, primary);
        });
        add(L"App.UI.WorldCanvas.Quality", {0,1,2,3}, [&](int q)
        {
            Charges cost(adapters.size());
            for (const auto& poster : BistroUICanvases::Posters)
            {
                const auto profile = Engine::UI::GetCanvasProfile(poster.Size, static_cast<Engine::UI::CanvasQuality>(q));
                const Bytes bytes = (Bytes(profile.RasterSize.Width) * profile.RasterSize.Height +
                    2 * Bytes(profile.EffectsSize.Width) * profile.EffectsSize.Height) * 4 * frames;
                cost[primary] += bytes;
                if (poster.Adapter && owner(value(poster.Adapter)) != primary)
                    cost[owner(value(poster.Adapter))] += bytes;
            }
            return cost;
        });
        add(L"App.UI.CloudPicture.Enabled", {0,1}, [&](int on)
        {
            return replicated(Bytes(on) * value(L"App.UI.CloudPicture.Width") * value(L"App.UI.CloudPicture.Height") *
                4 * frames, L"App.UI.CloudPicture.Adapter");
        });
        add(L"App.UI.GameHUD.Enabled", {0,1}, [&](int on)
        {
            return replicated(on * pixels * 4 * frames, L"App.UI.GameHUD.Adapter");
        });
        const auto pass = [&](const wchar_t* enabled, const wchar_t* quality, Bytes bytes, const wchar_t* adapter = nullptr)
        {
            add(quality, {0,1,2,3,4}, [&](int q)
            {
                const Bytes cost = q && value(enabled) ? bytes : 0;
                return adapter ? replicated(cost, adapter) : charge(cost, primary);
            });
        };
        const Bytes particles = BistroParticles::GetBudget();
        pass(L"Render.Particles.Enabled", L"Render.Particles.Quality", particles * (84 + frames * 32), L"Render.Particles.Adapter");
        const Bytes water = BistroWater::PresetParticleCount.GetValue() ? BistroWater::PresetParticleCount.GetValue() :
            BistroWater::ParticleCount.GetValue();
        pass(L"Render.Water.Enabled", L"Render.Water.Quality", water * BistroWater::Count.GetValue() *
             (128 + 16 * frames), L"Render.Water.Adapter");
        const Bytes face = BistroReflections::Resolution.GetValue();
        pass(L"Render.ReflectionProbes.Enabled", L"Render.ReflectionProbes.Quality",
             BistroReflections::Count.GetValue() ? face * face * 6 * (32 * BistroReflections::Count.GetValue() + 32) : 0,
             L"Render.ReflectionProbes.First.Adapter");
        // Full output dimensions bound render-sized histories even when reconstruction is enabled.
        pass(L"Render.AO.Enabled", L"Render.AO.Quality", pixels * (value(L"Render.AO.Resolution") ? 4 : 16) * frames, L"Render.AO.Adapter");
        pass(L"Render.SSR.Enabled", L"Render.SSR.Quality", pixels * 64);
        pass(L"Render.MotionBlur.Enabled", L"Render.MotionBlur.Quality", pixels * 16, L"Render.Post.Adapter");
        pass(L"Render.DOF.Enabled", L"Render.DOF.Quality", pixels * 24, L"Render.Post.Adapter");
        pass(L"Render.Sky.Clouds.Enabled", L"Render.Sky.Clouds.Quality", 256 * 256 * 6 * 8);
        for (const bool skyBox : {false, true})
            add(skyBox ? L"Render.Sky.SkyBox.Quality" : L"Render.Sky.Atmosphere.Quality", {0,1,2,3,4}, [&](int q)
            {
                const Bytes side = q ? 128u << (q - 1) : 0;
                return charge(value(skyBox ? L"Render.Sky.SkyBox.Enabled" : L"Render.Sky.Atmosphere.Enabled") ?
                    side * side * 6 * 24 : 0, primary);
            });
        for (const auto key : {L"Render.Upscaler.Provider", L"Render.FrameGeneration.Provider"})
        {
            const int requested = value(key);
            if (requested) add(key, {0, requested}, [&](int mode) { return charge(mode ? pixels * 64 : 0, primary); });
        }
        const auto grid = GetLightGridSettings();
        const Bytes maskBytes = Bytes((width - 1) / grid.TileSize + 1) * ((height - 1) / grid.TileSize + 1) *
            grid.DepthSlices * ((std::size(BistroLightPlacements) + 31) / 32) * sizeof(UINT) * frames;
        // A placement can fall back to local execution if its extra copies exhaust a secondary adapter.
        const auto placement = [&](const wchar_t* key, Bytes bytes)
        {
            const int requested = value(key);
            if (owner(requested) != primary)
                add(key, {-1, requested}, [&](int index)
                {
                    return charge(index < 0 ? 0 : bytes, owner(requested));
                });
        };
        placement(L"Render.LightGrid.Adapter", maskBytes * 2);
        placement(L"Render.WBOIT.Adapter", geometry + pixels * 24 * frames);
        placement(L"Render.Post.Adapter", pixels * 32 * frames);
        placement(L"Render.Sky.SkyView.Adapter", 512 * 256 * 8 * frames);
        placement(L"Render.Sky.AerialPerspective.Adapter", 32 * 32 * 64 * 16 * frames);
        features.push_back({L"", {{0, charge(pixels * 44 * frames + maskBytes, primary)}}});
        return Admit(console, features, adapters, usage, budget, raw, shadows, size);
    }

private:
    Result Admit(ConsoleModule& console, std::vector<Feature>& features, const std::vector<DeviceDesc>& adapters,
                 const Charges& usage, const Charges& budget, const Charges& raw, const Charges& shadows, uint32_t size)
    {
        const Bytes frames = console.FindConsoleVariable(L"Render.NumFrames").GetUInt();
        Charges total, growth;
        for (;;)
        {
            total = _base;
            growth.assign(budget.size(), 0);
            for (const auto& f : features)
            {
                const auto previous = _accepted.find(f.Key);
                const auto& costs = f.Levels[f.Selected].Cost;
                for (size_t i = 0; i < costs.size(); ++i)
                {
                    total[i] += costs[i];
                    const Bytes old = previous == _accepted.end() ? 0 : previous->second[i];
                    if (costs[i] > old)
                        growth[i] += costs[i]; // CurrentUsage may still contain all slots of the previous configuration.
                }
            }
            size_t gpu = 0;
            while (gpu < budget.size() && (total[gpu] == _base[gpu] ||
                   (total[gpu] <= budget[gpu] && (!growth[gpu] || usage[gpu] + growth[gpu] <= budget[gpu])))) ++gpu;
            if (gpu == budget.size()) break;
            Feature* best = nullptr;
            size_t next = 0;
            Bytes saving = 0;
            bool reducingIncrease = false;
            for (auto& f : features)
            {
                const Bytes current = f.Levels[f.Selected].Cost[gpu];
                const auto previous = _accepted.find(f.Key);
                const bool increase = !_accepted.empty() && current > (previous == _accepted.end() ? 0 : previous->second[gpu]);
                for (size_t lower = f.Selected; lower-- > 0;)
                {
                    const Bytes smaller = f.Levels[lower].Cost[gpu];
                    if (smaller >= current) continue;
                    if (!best || (increase && !reducingIncrease) || (increase == reducingIncrease && current - smaller > saving))
                    {
                        best = &f;
                        next = lower;
                        saving = current - smaller;
                        reducingIncrease = increase;
                    }
                    break;
                }
            }
            if (!best)
            {
                Log::Warning("Bistro", "GPU [", gpu, "] VRAM budget cannot accommodate ", size >> 16, "x", size & 0xffff,
                    "; estimated MiB=", total[gpu] >> 20, "; usable MiB=", budget[gpu] >> 20);
                return Result::SmallerOutputRequired;
            }
            Log::Info("Bistro", "GPU [", gpu, "] VRAM estimate MiB=", total[gpu] >> 20,
                "; current MiB=", usage[gpu] >> 20, "; usable MiB=", budget[gpu] >> 20);
            best->Selected = next;
        }
        bool reclaim = false;
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            const Bytes peak = (std::max)(total[i], usage[i] + growth[i]);
            auto& account = BistroRender::ShadowMemory::For(adapters[i].AdapterLuid);
            account.Limit = raw[i] > peak ? raw[i] - peak : 0;
            reclaim |= growth[i] && shadows[i] > account.Limit;
        }
        if (reclaim)
        {
            _revision = UINT64_MAX; // Re-evaluate actual usage after the shadow generations retire.
            return Result::RetireLocalShadows;
        }
        std::map<std::wstring, std::vector<int>> available;
        available[L"Render.Shadows.Local.Resolution"] = {0, 128, 256, 384, 512, 1024};
        for (const auto& f : features)
        {
            if (!f.ShowLevels) continue;
            const auto& selected = f.Levels[f.Selected].Cost;
            for (const auto& candidate : f.Levels)
            {
                bool fits = true;
                for (size_t i = 0; i < budget.size(); ++i)
                {
                    if (candidate.Cost[i] <= selected[i]) continue;
                    const Bytes extra = candidate.Cost[i] - selected[i];
                    if (total[i] + extra > budget[i] ||
                        usage[i] + growth[i] + extra + selected[i] / frames > budget[i]) fits = false;
                }
                if (fits) available[f.Key].push_back(candidate.Value);
            }
        }
        Engine::UI::SetAvailableSettingLevels(std::move(available));
        ConsoleVariablePatch patch;
        for (const auto& f : features)
        {
            const auto& level = f.Levels[f.Selected];
            const int requested = f.Levels[f.Requested].Value;
            if (*f.Key && level.Value != requested)
            {
                Log::Warning("Bistro", "VRAM budget limited ", f.Key, " from ", requested, " to ", level.Value);
                LastLimit = std::wstring(f.Key) + L" " + std::to_wstring(requested) + L" -> " + std::to_wstring(level.Value);
                patch.emplace_back(f.Key, std::to_string(level.Value));
            }
        }
        if (!patch.empty())
        {
            const auto result = console.ApplyPatch(patch);
            if (!result.Error.empty()) throw ConsoleConfigError(result.Error);
        }
        _accepted.clear();
        for (const auto& f : features) _accepted.emplace(f.Key, f.Levels[f.Selected].Cost);
        _revision = IConsoleVariable::ValueRevision();
        _size = size;
        return patch.empty() ? Result::Fits : Result::Limited;
    }
};
