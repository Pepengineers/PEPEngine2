#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace BistroWaves
{
    inline AutoConsoleVariable<bool> Enabled(L"App.Physics.Waves.Enabled", true,
        L"Disable aborts the partial wave; enable waits a fresh random 5..10 seconds before launch");
    inline AutoConsoleVariable<double> LaunchSpeed(L"App.Physics.Waves.LaunchSpeed", 500,
        L"One upward impulse delta speed, 100..500 cm/s; latched at next launch",
        EConsoleVariableFlags::Default, [](double v) { return v >= 100 && v <= 500; });
    inline AutoConsoleVariable<double> ScatterSpeed(L"App.Physics.Waves.ScatterSpeed", 700,
        L"One random direction impulse 1 second after launch, 600..900 cm/s; next launch; participant speed cap 3000 cm/s",
        EConsoleVariableFlags::Default, [](double v) { return v >= 600 && v <= 900; });
    inline AutoConsoleVariable<uint32_t> Seed(L"App.Physics.Waves.Seed", 12345,
        L"Deterministic intervals/directions keyed by wave serial and entity; next launch, active interval unchanged");

    inline constexpr float MaxSpeed = 3000;
    inline constexpr double ScatterDelay = 1;
    struct Settings
    {
        double LaunchSpeed = 500;
        double ScatterSpeed = 700;
        uint32_t Seed = 12345;

        void Validate() const
        {
            if (!std::isfinite(LaunchSpeed) || LaunchSpeed < 100 || LaunchSpeed > 500 ||
                !std::isfinite(ScatterSpeed) || ScatterSpeed < 600 || ScatterSpeed > 900)
                throw std::invalid_argument("Invalid Bistro wave settings");
        }
    };

    inline Settings ReadSettings() { return {LaunchSpeed.GetValue(), ScatterSpeed.GetValue(), Seed.GetValue()}; }
    inline uint32_t Mix(uint32_t x)
    {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
    }
    inline uint32_t CycleKey(uint32_t seed, uint64_t cycle)
    { return Mix(seed ^ Mix(static_cast<uint32_t>(cycle)) ^ Mix(static_cast<uint32_t>(cycle >> 32))); }
    inline double Interval(uint32_t seed, uint64_t nextCycle)
    { return 5 + static_cast<double>(Mix(CycleKey(seed, nextCycle) ^ 0x5bd1e995u)) / UINT32_MAX * 5; }

    inline Vector3 ScatterVelocity(const Settings& settings, uint64_t cycle, Entity entity)
    {
        const auto key = Mix(CycleKey(settings.Seed, cycle) ^ Mix(static_cast<uint32_t>(entity)));
        const auto signedUnit = [](uint32_t x) { return static_cast<float>(x >> 8) / 8388608.f - 1.f; };
        Vector3 direction(signedUnit(Mix(key)), signedUnit(Mix(key ^ 0x85ebca6bu)), signedUnit(Mix(key ^ 0x9e3779b9u)));
        if (direction.LengthSquared() < 1e-6f) direction = Vector3::Right;
        direction.Normalize();
        return direction * static_cast<float>(settings.ScatterSpeed);
    }

    inline Vector3 BoundScatter(Vector3 delta, const Vector3& current)
    {
        if ((current+delta).LengthSquared() <= MaxSpeed*MaxSpeed) return delta;
        auto normal=current; normal.Normalize();
        const auto speed=delta.Length();
        delta=Vector3::Reflect(delta,normal);
        // Preserve the full impulse at terminal speed; an almost tangent reflection may still exceed the cap.
        if ((current+delta).LengthSquared() > MaxSpeed*MaxSpeed) delta=-normal*speed;
        return delta;
    }

    enum class Phase { Waiting, Lift, Fall };
    class Timeline
    {
    public:
        // dt is fixed physics time, already subject to pause/TimeScale and retained catch-up.
        template<class Launch, class Scatter>
        void Advance(double dt, bool enabled, const Settings& requested, Launch launch, Scatter scatter)
        {
            if (!enabled) { _initialized = false; _active = false; _elapsed = 0; return; }
            if (!std::isfinite(dt) || dt <= 0) return;
            if (!_initialized)
            {
                requested.Validate();
                _interval = Interval(requested.Seed, _startedCycles + 1);
                _elapsed = 0;
                _initialized = true;
            }
            const auto begin = [&]
            {
                requested.Validate();
                _settings = requested;
                ++_startedCycles;
                _elapsed = 0;
                _active = true;
                _scattered = false;
                _interval = Interval(_settings.Seed, _startedCycles + 1);
                launch(_settings, _startedCycles);
            };
            while (dt > 0)
            {
                const double boundary = _active && !_scattered ? ScatterDelay : _interval;
                const double segment = (std::min)(dt, boundary - _elapsed);
                _elapsed += segment;
                dt -= segment;
                if (_elapsed >= boundary)
                {
                    if (!_active) begin();
                    else if (!_scattered) { scatter(_settings, _startedCycles); _scattered = true; }
                    else
                    {
                        ++_completedCycles;
                        _lastCompletedCycleSerial = _startedCycles;
                        begin();
                    }
                }
            }
        }

        Phase GetPhase() const { return !_active ? Phase::Waiting : _scattered ? Phase::Fall : Phase::Lift; }
        uint64_t StartedCycles() const { return _startedCycles; }
        uint64_t CompletedCycles() const { return _completedCycles; }
        uint64_t LastCompletedCycleSerial() const { return _lastCompletedCycleSerial; }
        double Elapsed() const { return _elapsed; }
        double CurrentInterval() const { return _interval; }

    private:
        Settings _settings;
        double _elapsed = 0;
        double _interval = 0;
        uint64_t _startedCycles = 0;
        uint64_t _completedCycles = 0;
        uint64_t _lastCompletedCycleSerial = 0;
        bool _initialized = false;
        bool _active = false;
        bool _scattered = false;
    };

    class Controller
    {
    public:
        // Explicit opt-in for props and future shards. Unregister before destroying/reusing an entity.
        void Register(Entity entity)
        {
            if (std::find(_participants.begin(), _participants.end(), entity) == _participants.end())
                _participants.push_back(entity);
        }
        void Unregister(Entity entity)
        { _participants.erase(std::remove(_participants.begin(), _participants.end(), entity), _participants.end()); }
        size_t ParticipantCount() const { return _participants.size(); }
        const Timeline& GetTimeline() const { return _timeline; }

        void Tick(World& world, PhysicsModule& physics, float dt)
        {
            if (world.IsPaused()) return;
            auto& ecs = world.GetECS();
            const auto impulse = [&](const Settings& settings, uint64_t cycle, bool scatter)
            {
                for (auto entity : _participants)
                    if (ecs.Has<RigidBodyComponent>(entity))
                    {
                        const auto& body = ecs.Get<RigidBodyComponent>(entity);
                        if (body.MotionType != PhysicsMotionType::Dynamic || !body.Body.IsValid()) continue;
                        auto velocity = scatter ? ScatterVelocity(settings, cycle, entity) : Vector3(0, static_cast<float>(settings.LaunchSpeed), 0);
                        if (scatter)
                        {
                            Vector3 current; bool active;
                            if (physics.TryGetBodyVelocity(world,body.Body,current,active)) velocity=BoundScatter(velocity,current);
                        }
                        physics.AddBodyVelocityImpulse(world, body.Body, velocity, MaxSpeed);
                    }
            };
            _timeline.Advance(dt, Enabled.GetValue(), ReadSettings(),
                [&](const Settings& s, uint64_t cycle) { impulse(s,cycle,false); },
                [&](const Settings& s, uint64_t cycle) { impulse(s,cycle,true); });
        }

    private:
        std::vector<Entity> _participants;
        Timeline _timeline;
    };
}
