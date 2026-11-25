#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>
#include <deque>
#include <mutex>

class JoltJobSystem final : public JPH::JobSystemWithBarrier
{
public:
    explicit JoltJobSystem(::JobSystem& jobs);
    ~JoltJobSystem() override;
    void PrepareRigidBodyStep(JPH::uint32 activeBodies);
    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
        JPH::uint32 dependencies = 0) override;
    void CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function,
        JobHandle* jobs, JPH::uint32 count, JPH::uint32 dependencies = 0) override;
    void WaitForJobs(Barrier* barrier) override;

private:
    struct ReadyWorkers;
    template<class GetJob>
    void SubmitReadyJobs(JPH::uint count, GetJob getJob);
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;
    void RunReadyJobs();
    void Drain();

    ::JobSystem& _jobs;
    unsigned _stepConcurrency;
    std::mutex _readyMutex;
    std::deque<JobHandle> _ready;
    unsigned _workers = 0;
    std::atomic<unsigned> _outstanding{0};
};
