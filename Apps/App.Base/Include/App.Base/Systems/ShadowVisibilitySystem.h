#pragma once
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"

class ShadowVisibilitySystem final : public ParallelJob
{
public:
    JobHandle Schedule(JobSystem& jobs, const std::vector<GDX12LocalLightConstants>& lights,
        const std::vector<GDX12LocalShadowCaster>& casters, std::vector<GDX12LocalShadowVisibility>& visibility) &
    {
        EnsureIdle();
        _lights = &lights;
        _casters = &casters;
        _visibility = &visibility;
        visibility.resize(lights.size());
        // Nearby lights can intersect far more casters than their neighbours.
        return ParallelJob::Schedule(jobs, lights.size(), 1);
    }

private:
    void Execute(size_t index) override
    {
        (*_visibility)[index] = BuildLocalShadowVisibility((*_lights)[index], *_casters);
    }
    const std::vector<GDX12LocalLightConstants>* _lights = nullptr;
    const std::vector<GDX12LocalShadowCaster>* _casters = nullptr;
    std::vector<GDX12LocalShadowVisibility>* _visibility = nullptr;
};
