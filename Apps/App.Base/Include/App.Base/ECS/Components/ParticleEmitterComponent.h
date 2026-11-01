#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include <atomic>
#include <stdexcept>

class ParticleEmitterComponent : public ComponentTag
{
public:
    ParticleEmitterComponent() : _id(NextId()) {}
    ParticleEmitterComponent(const ParticleEmitterComponent& other)
        : _settings(other._settings), _id(NextId()), _burstTotal(other._burstTotal),
        _enabled(other._enabled), _paused(other._paused) {}
    ParticleEmitterComponent& operator=(const ParticleEmitterComponent& other)
    {
        if (this == &other) return *this;
        _settings = other._settings;
        _enabled = other._enabled;
        _paused = other._paused;
        Reset();
        _burstTotal = other._burstTotal;
        return *this;
    }
    ParticleEmitterComponent(ParticleEmitterComponent&&) noexcept = default;
    ParticleEmitterComponent& operator=(ParticleEmitterComponent&&) noexcept = default;

    const GDX12ParticleEmitterSettings& GetSettings() const { return _settings; }
    void SetSettings(const GDX12ParticleEmitterSettings& settings)
    {
        if (!IsValidParticleEmitterSettings(settings)) throw std::invalid_argument("Invalid particle emitter settings");
        _settings = settings;
    }
    void SetEnabled(bool enabled) { _enabled = enabled; }
    bool IsEnabled() const { return _enabled; }
    void SetPaused(bool paused) { _paused = paused; }
    bool IsPaused() const { return _paused; }
    void Reset() { ++_resetRevision; _burstTotal = 0; }
    void Emit(UINT count)
    {
        if (count > 1048576 || UINT64_MAX - _burstTotal < count) throw std::out_of_range("Particle burst exceeds capacity");
        _burstTotal += count;
    }
    uint64_t GetId() const { return _id; }
    uint64_t GetResetRevision() const { return _resetRevision; }
    uint64_t GetBurstTotal() const { return _burstTotal; }

private:
    static uint64_t NextId()
    {
        static std::atomic<uint64_t> next{ 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    GDX12ParticleEmitterSettings _settings;
    uint64_t _id;
    uint64_t _resetRevision = 0;
    uint64_t _burstTotal = 0;
    bool _enabled = true;
    bool _paused = false;
};
