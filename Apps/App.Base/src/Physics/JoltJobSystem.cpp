#include "App.Base/Physics/JoltJobSystem.h"
#include <Jolt/Physics/PhysicsSettings.h>
#include "TaskScheduler.h"
#include <algorithm>
#include <thread>

struct JoltJobSystem::ReadyWorkers final : enki::ITaskSet
{
    struct Completion final : enki::ICompletable
    {
        explicit Completion(ReadyWorkers& batch) : Batch(batch) { SetDependency(Link, &batch); }
        void OnDependenciesComplete(enki::TaskScheduler* scheduler, uint32_t thread) override
        {
            auto* batch = &Batch;
            auto& owner = batch->Owner;
            ICompletable::OnDependenciesComplete(scheduler, thread);
            delete batch;
            owner._outstanding.fetch_sub(1, std::memory_order_release);
        }
        ReadyWorkers& Batch;
        enki::Dependency Link;
    };

    ReadyWorkers(JoltJobSystem& owner, unsigned workers)
        : enki::ITaskSet(workers), Owner(owner), Done(*this) {}

    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override
    {
        for (auto i = range.start; i < range.end; ++i)
            Owner.RunReadyJobs();
    }

    JoltJobSystem& Owner;
    Completion Done;
};

template<class GetJob>
void JoltJobSystem::SubmitReadyJobs(JPH::uint count, GetJob getJob)
{
    if (count == 0) return;
    ReadyWorkers* batch;
    {
        std::lock_guard lock(_readyMutex);
        for (JPH::uint i = 0; i < count; ++i)
            _ready.emplace_back(getJob(i));
        const auto wanted = (std::min<size_t>)(_stepConcurrency, _ready.size());
        if (_workers >= wanted) return;
        batch = new ReadyWorkers(*this, static_cast<unsigned>(wanted) - _workers);
        _workers = static_cast<unsigned>(wanted);
        _outstanding.fetch_add(1, std::memory_order_relaxed);
    }
    _jobs.QueueTask(*batch);
}

void JoltJobSystem::RunReadyJobs()
{
    for (;;)
    {
        JobHandle job;
        {
            std::lock_guard lock(_readyMutex);
            if (_ready.empty())
            {
                --_workers;
                return;
            }
            job = std::move(_ready.front());
            _ready.pop_front();
        }
        // The barrier may already have executed the job. Keep its handle until Execute returns.
        job.GetPtr()->Execute();
    }
}

JoltJobSystem::JoltJobSystem(::JobSystem& jobs)
    : JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), _jobs(jobs), _stepConcurrency(jobs.GetThreadCount()) {}
JoltJobSystem::~JoltJobSystem() { Drain(); }
int JoltJobSystem::GetMaxConcurrency() const { return static_cast<int>(_stepConcurrency); }

void JoltJobSystem::PrepareRigidBodyStep(JPH::uint32 activeBodies)
{
    // Match Jolt's gravity/integration grain while leaving room for collision and constraint jobs in small scenes.
    constexpr unsigned bodiesPerJob = 64;
    const unsigned batches = activeBodies / bodiesPerJob + (activeBodies % bodiesPerJob != 0);
    _stepConcurrency = (std::min)(_jobs.GetThreadCount(), (std::max)(8u, batches));
}

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
