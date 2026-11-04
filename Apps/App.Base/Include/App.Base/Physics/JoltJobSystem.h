#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>

class JoltJobSystem final : public JPH::JobSystemWithBarrier
{
public:
    explicit JoltJobSystem(::JobSystem& jobs);
    ~JoltJobSystem() override;
    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
        JPH::uint32 dependencies = 0) override;
    void WaitForJobs(Barrier* barrier) override;

private:
    struct ScheduledJob;
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;
    void Drain();

    ::JobSystem& _jobs;
    std::atomic<unsigned> _outstanding{0};
};
