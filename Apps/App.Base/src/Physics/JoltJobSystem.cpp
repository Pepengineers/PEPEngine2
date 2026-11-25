#include "App.Base/Physics/JoltJobSystem.h"
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Core/STLLocalAllocator.h>
#include "TaskScheduler.h"
#include <thread>

struct JoltJobSystem::ReadyBatch final : enki::ITaskSet
{
    struct Completion final : enki::ICompletable
    {
        explicit Completion(ReadyBatch& batch) : Batch(batch) { SetDependency(Link, &batch); }
        void OnDependenciesComplete(enki::TaskScheduler* scheduler, uint32_t thread) override
        {
            auto* batch = &Batch;
            auto& owner = batch->Owner;
            ICompletable::OnDependenciesComplete(scheduler, thread);
            delete batch;
            owner._outstanding.fetch_sub(1, std::memory_order_release);
        }
        ReadyBatch& Batch;
        enki::Dependency Link;
    };

    template<class GetJob>
    ReadyBatch(JoltJobSystem& owner, JPH::uint count, GetJob getJob)
        : enki::ITaskSet(count), Owner(owner), Done(*this)
    {
        Jobs.reserve(count);
        for (JPH::uint i = 0; i < count; ++i)
            Jobs.emplace_back(getJob(i));
    }

    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override
    {
        for (auto i = range.start; i < range.end; ++i)
            Jobs[i].GetPtr()->Execute();
    }

    JoltJobSystem& Owner;
    // The barrier can finish a job before its queued range runs.
    JPH::Array<JobHandle, JPH::STLLocalAllocator<JobHandle, 1>> Jobs;
    Completion Done;
};

template<class GetJob>
void JoltJobSystem::SubmitReadyJobs(JPH::uint count, GetJob getJob)
{
    if (count == 0) return;
    auto* batch = new ReadyBatch(*this, count, getJob);
    _outstanding.fetch_add(1, std::memory_order_relaxed);
    _jobs.QueueTask(*batch);
}

JoltJobSystem::JoltJobSystem(::JobSystem& jobs)
    : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), _jobs(jobs) {}
JoltJobSystem::~JoltJobSystem() { Drain(); }
int JoltJobSystem::GetMaxConcurrency() const { return static_cast<int>(_jobs.GetThreadCount()); }

JPH::JobHandle JoltJobSystem::CreateJob(const char* name, JPH::ColorArg color,
    const JobFunction& function, JPH::uint32 dependencies)
{
    auto* job = new Job(name, color, this, function, dependencies);
    JobHandle handle(job);
    if (dependencies == 0) QueueJob(job);
    return handle;
}

void JoltJobSystem::CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function,
    JobHandle* jobs, JPH::uint32 count, JPH::uint32 dependencies)
{
    for (JPH::uint32 i = 0; i < count; ++i)
        jobs[i] = JobHandle(new Job(name, color, this, function, dependencies));
    if (dependencies == 0)
        SubmitReadyJobs(count, [jobs](JPH::uint i) { return jobs[i].GetPtr(); });
}

void JoltJobSystem::QueueJob(Job* job)
{
    QueueJobs(&job, 1);
}

void JoltJobSystem::QueueJobs(Job** jobs, JPH::uint count)
{
    SubmitReadyJobs(count, [jobs](JPH::uint i) { return jobs[i]; });
}

void JoltJobSystem::FreeJob(Job* job) { delete job; }

void JoltJobSystem::Drain()
{
    while (_outstanding.load(std::memory_order_acquire) != 0)
    {
        _jobs.WaitForTask();
        std::this_thread::yield();
    }
}

void JoltJobSystem::WaitForJobs(Barrier* barrier)
{
    JobSystemWithBarrier::WaitForJobs(barrier);
    // The barrier can run jobs itself; their queued enki wrappers must also finish.
    Drain();
}
