#pragma once
#include "Engine.Core/Jobs/JobSystem.h"

class World;

class System : public ParallelJob
{
public:
    virtual ~System() = default;

    virtual void Tick(World& world, float dt) = 0;

    JobHandle Schedule(JobSystem& jobs, World& world, float dt, const JobHandle& dependency = {}) &
    {
        return Schedule(jobs, world, dt, jobs.GetDefaultBatchSize(), dependency);
    }

    JobHandle Schedule(JobSystem& jobs, World& world, float dt, size_t batchSize,
        const JobHandle& dependency = {}) &
    {
        EnsureIdle();
        _world = &world;
        _dt = dt;
        Prepare(world, dt);
        return ParallelJob::Schedule(jobs, GetWorkCount(world), batchSize, dependency);
    }

protected:
    virtual void Prepare(World&, float) {}
    virtual size_t GetWorkCount(World&) const { return 1; }
    virtual void TickRange(World& world, float dt, size_t, size_t) { Tick(world, dt); }

private:
    void Execute(size_t index) final { ExecuteRange(index, index + 1); }
    void ExecuteRange(size_t begin, size_t end) final { TickRange(*_world, _dt, begin, end); }
    World* _world = nullptr;
    float _dt = 0;
};
