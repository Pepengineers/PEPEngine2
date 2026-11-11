#pragma once
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>

class ShadowVisibilitySystem final : public ParallelJob
{
public:
    JobHandle Schedule(JobSystem& jobs, const std::vector<GDX12LocalLightConstants>& lights,
        const std::vector<GDX12LocalShadowCaster>& casters, std::vector<GDX12LocalShadowVisibility>& visibility,
        const std::vector<GDX12LocalShadowCaster>& meshStatic = {}, uint64_t meshRevision = 0) &
    {
        EnsureIdle();
        _lights = &lights;
        _nextStatic.clear();
        _dynamic.clear();
        for (const auto& caster : casters)
            (caster.Dynamic ? _dynamic : _nextStatic).push_back(caster);
        _staticChanged = !_hasStatic || _meshRevision != meshRevision || _otherStaticCount != _nextStatic.size() ||
            !std::equal(_nextStatic.begin(), _nextStatic.end(), _static.begin(), [](const auto& a, const auto& b)
            {
                return a.Signature == b.Signature && std::memcmp(&a.Bounds, &b.Bounds, sizeof(a.Bounds)) == 0;
            });
        if (_staticChanged)
        {
            _hasStatic = true;
            _meshRevision = meshRevision;
            _otherStaticCount = _nextStatic.size();
            _nextStatic.insert(_nextStatic.end(), meshStatic.begin(), meshStatic.end());
            _static.swap(_nextStatic);
        }
        _cache.resize(lights.size());
        _visibility = &visibility;
        visibility.resize(lights.size());
        // Nearby lights can intersect far more casters than their neighbours.
        return ParallelJob::Schedule(jobs, lights.size(), 1);
    }

private:
    void Execute(size_t index) override
    {
        const auto& light = (*_lights)[index];
        auto& cached = _cache[index];
        if (_staticChanged || !cached.Valid || !SameView(light, cached.Light))
        {
            cached.Visibility = BuildLocalShadowVisibility(light, _static);
            cached.Light = light;
            cached.Valid = true;
        }
        (*_visibility)[index] = BuildLocalShadowVisibility(light, _dynamic, &cached.Visibility);
    }
    static bool SameView(const GDX12LocalLightConstants& a, const GDX12LocalLightConstants& b)
    {
        return a.Type == b.Type && a.ShadowFirstSlice == b.ShadowFirstSlice && a.Range == b.Range &&
            a.OuterCos == b.OuterCos && std::memcmp(&a.Position, &b.Position, sizeof(a.Position)) == 0 &&
            std::memcmp(&a.Direction, &b.Direction, sizeof(a.Direction)) == 0 &&
            std::memcmp(a.ShadowViewProj, b.ShadowViewProj, sizeof(a.ShadowViewProj)) == 0;
    }
    struct CachedStatic
    {
        GDX12LocalLightConstants Light;
        GDX12LocalShadowVisibility Visibility;
        bool Valid = false;
    };
    std::vector<CachedStatic> _cache;
    std::vector<GDX12LocalShadowCaster> _static, _nextStatic, _dynamic;
    uint64_t _meshRevision = 0;
    size_t _otherStaticCount = 0;
    bool _staticChanged = false;
    bool _hasStatic = false;
    const std::vector<GDX12LocalLightConstants>* _lights = nullptr;
    std::vector<GDX12LocalShadowVisibility>* _visibility = nullptr;
};
