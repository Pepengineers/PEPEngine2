#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/AssetHandles.h"
#include <directxtk/SimpleMath.h>
#include <vector>
#include <cmath>
#include <stdexcept>

class GDX12Material;
class RenderModule;

class SkeletalMeshRenderComponent : public ComponentTag
{
public:
    explicit SkeletalMeshRenderComponent(Engine::Core::SceneHandle scene = {}) : _scene(scene) {}
    Engine::Core::SceneHandle GetScene() const { return _scene; }
    int GetClip() const { return _clip; }
    double GetTimeSeconds() const { return _time; }
    double GetPlaybackSpeed() const { return _speed; }
    double GetUpdateRateHz() const { return _rate; }
    bool IsFrozen() const { return _frozen; }
    bool IsLooping() const { return _loop; }
    bool GetCastsShadows() const { return _castsShadows; }
    void SetCastsShadows(bool enabled) { _castsShadows = enabled; }
    const auto& GetLocalPose() const { return _localPose; }
    const auto& GetMaterials() const { return _materials; }

    void SetScene(Engine::Core::SceneHandle scene) { _scene = scene; ++_revision; }
    void SetClip(int clip)
    {
        if (clip < -1) throw std::invalid_argument("Clip must be -1 (bind) or a clip index");
        _clip = clip;
        ++_revision;
    }
    void SetTimeSeconds(double time)
    {
        if (!std::isfinite(time)) throw std::invalid_argument("Animation time must be finite");
        _time = time;
        ++_revision;
    }
    void SetPlaybackSpeed(double speed)
    {
        if (!std::isfinite(speed) || std::abs(speed) > 100)
            throw std::invalid_argument("Playback speed must be finite and within [-100,100]");
        _speed = speed;
        ++_revision;
    }
    void SetUpdateRateHz(double rate)
    {
        if (!std::isfinite(rate) || rate < 0 || rate > 240)
            throw std::invalid_argument("Update rate is 0 (preset) or (0,240] Hz");
        _rate = rate;
        ++_revision;
    }
    void SetFrozen(bool frozen) { _frozen = frozen; ++_revision; }
    void SetLooping(bool loop) { _loop = loop; ++_revision; }
    void SetLocalPose(std::vector<DirectX::SimpleMath::Matrix> pose)
    {
        for (const auto& matrix : pose)
        {
            const auto* values = reinterpret_cast<const float*>(&matrix);
            for (size_t i = 0; i < 16; ++i)
                if (!std::isfinite(values[i])) throw std::invalid_argument("Pose must be finite");
        }
        _localPose = std::move(pose);
        ++_revision;
    }
    // Empty restores imported material slots; the renderer owns material lifetime.
    void SetMaterials(std::vector<GDX12Material*> materials)
    {
        for (auto* material : materials)
            if (!material) throw std::invalid_argument("Material override contains null");
        _materials = std::move(materials);
        ++_revision;
    }

private:
    friend class RenderModule;
    Engine::Core::SceneHandle _scene;
    int _clip = -1;
    double _time = 0;
    double _speed = 1;
    double _rate = 0;
    bool _frozen = false;
    bool _loop = true;
    bool _castsShadows = true;
    std::uint64_t _revision = 1;
    std::vector<DirectX::SimpleMath::Matrix> _localPose;
    std::vector<GDX12Material*> _materials;
};
