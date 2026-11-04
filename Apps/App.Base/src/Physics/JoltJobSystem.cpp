#include "App.Base/Physics/JoltJobSystem.h"
#include <Jolt/Physics/PhysicsSettings.h>
#include "TaskScheduler.h"
#include <thread>

struct JoltJobSystem::ScheduledJob final : Job, enki::ITaskSet
{
    struct Completion final : enki::ICompletable
    {
        explicit Completion(ScheduledJob& job) : Work(job) { SetDependency(Link, &job); }
        void OnDependenciesComplete(enki::TaskScheduler* scheduler, uint32_t thread) override
        {
            auto* owner = &Work.Owner;
            auto* work = &Work;
            ICompletable::OnDependenciesComplete(scheduler, thread);
            // enki has finished touching the task; releasing earlier can free it in ExecuteRange.
            work->Release();
            owner->_outstanding.fetch_sub(1, std::memory_order_release);
        }
        ScheduledJob& Work;
        enki::Dependency Link;
    };

    ScheduledJob(JoltJobSystem& owner, const char* name, JPH::ColorArg color,
        const JobFunction& function, JPH::uint32 dependencies)
        : Job(name, color, &owner, function, dependencies), enki::ITaskSet(1), Owner(owner), Done(*this) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override { Job::Execute(); }
    JoltJobSystem& Owner;
    Completion Done;
};

JoltJobSystem::JoltJobSystem(::JobSystem& jobs)
    : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), _jobs(jobs) {}
JoltJobSystem::~JoltJobSystem() { Drain(); }
int JoltJobSystem::GetMaxConcurrency() const { return static_cast<int>(_jobs.GetThreadCount()); }

JPH::JobHandle JoltJobSystem::CreateJob(const char* name, JPH::ColorArg color,
    const JobFunction& function, JPH::uint32 dependencies)
{
    auto* job = new ScheduledJob(*this, name, color, function, dependencies);
    JobHandle handle(job);
    if (dependencies == 0) QueueJob(job);
    return handle;
}

void JoltJobSystem::QueueJob(Job* job)
{
    job->AddRef();
    _outstanding.fetch_add(1, std::memory_order_relaxed);
    _jobs.QueueTask(*static_cast<ScheduledJob*>(job));
}

void JoltJobSystem::QueueJobs(Job** jobs, JPH::uint count)
{
    for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
}

void JoltJobSystem::FreeJob(Job* job) { delete static_cast<ScheduledJob*>(job); }

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
