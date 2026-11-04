#include "Engine.Core/Jobs/JobSystem.h"
#include "TaskScheduler.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace JobDetail
{
    struct State : std::enable_shared_from_this<State>
    {
        struct Task : enki::ITaskSet
        {
            explicit Task(State& state) : StateRef(state) {}
            void ExecuteRange(enki::TaskSetPartition range, uint32_t) override
            {
                try
                {
                    StateRef.Work->ExecuteRange(range.start, range.end);
                }
                catch (...) { StateRef.SetError(std::current_exception()); }
            }
            State& StateRef;
        };

        struct Completion : enki::ICompletable
        {
            Completion(State& state, Task& task) : StateRef(state) { SetDependency(Link, &task); }
            void OnDependenciesComplete(enki::TaskScheduler* scheduler, uint32_t thread) override
            {
                auto keepAlive = StateRef.shared_from_this();
                ICompletable::OnDependenciesComplete(scheduler, thread);
                StateRef.Finish();
            }
            State& StateRef;
            enki::Dependency Link;
        };

        State() : WorkTask(*this), CompletedTask(*this, WorkTask) {}
        void SetError(std::exception_ptr error)
        {
            std::lock_guard<std::mutex> lock(Mutex);
            if (!Error) Error = error;
        }
        void RegisterDependencies();
        void DependencyFinished(std::exception_ptr error);
        void Start();
        void Finish();
        void Complete();
        bool UsesOnly(const JobSystem* system) const
        {
            if (Owner && Owner != system) return false;
            return std::all_of(Dependencies.begin(), Dependencies.end(),
                [system](const auto& dependency) { return dependency->UsesOnly(system); });
        }

        JobSystem* Owner = nullptr;
        ParallelJob* Work = nullptr;
        std::vector<std::shared_ptr<State>> Dependencies;
        std::atomic<size_t> Remaining{ 0 };
        std::atomic<bool> Finished{ false };
        bool Submitted = false;
        std::mutex Mutex;
        std::condition_variable Changed;
        std::exception_ptr Error;
        std::vector<std::weak_ptr<State>> Waiters;
        Task WorkTask;
        Completion CompletedTask;
    };
}

struct JobSystem::Impl
{
    void CheckThread() const
    {
        if (std::this_thread::get_id() != OwnerThread)
            throw std::logic_error("Schedule and Complete must run on the JobSystem's owning thread");
    }

    enki::TaskScheduler Scheduler;
    std::thread::id OwnerThread = std::this_thread::get_id();
    std::vector<std::shared_ptr<JobDetail::State>> Active;
    size_t DefaultBatchSize;
};

void JobDetail::State::RegisterDependencies()
{
    // The extra count prevents a fast prerequisite from launching us during registration.
    Remaining = Dependencies.size() + 1;
    const auto self = shared_from_this();
    for (const auto& dependency : Dependencies)
    {
        std::unique_lock<std::mutex> lock(dependency->Mutex);
        if (dependency->Finished.load(std::memory_order_acquire))
        {
            const auto error = dependency->Error;
            lock.unlock();
            DependencyFinished(error);
        }
        else dependency->Waiters.push_back(self);
    }
    DependencyFinished(nullptr);
}

void JobDetail::State::DependencyFinished(std::exception_ptr error)
{
    if (Finished.load(std::memory_order_acquire)) return;
    if (error) SetError(error);
    if (Remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) Start();
}

void JobDetail::State::Start()
{
    bool submit;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        submit = !Error && Work && WorkTask.m_SetSize != 0;
        Submitted = submit;
    }
    Changed.notify_all();
    if (submit) Owner->_impl->Scheduler.AddTaskSetToPipe(&WorkTask);
    else Finish();
}

void JobDetail::State::Finish()
{
    auto keepAlive = shared_from_this();
    std::vector<std::weak_ptr<State>> waiters;
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Finished.exchange(true, std::memory_order_acq_rel)) return;
        error = Error;
        waiters.swap(Waiters);
    }
    for (auto& waiter : waiters)
        if (auto next = waiter.lock()) next->DependencyFinished(error);
    Changed.notify_all();
}

void JobDetail::State::Complete()
{
    if (!Finished.load(std::memory_order_acquire))
    {
        if (Owner) Owner->_impl->CheckThread();
        for (const auto& dependency : Dependencies)
        {
            try { dependency->Complete(); }
            catch (...) { SetError(std::current_exception()); }
        }
        {
            std::unique_lock<std::mutex> lock(Mutex);
            Changed.wait(lock, [&] { return Submitted || Finished.load(std::memory_order_acquire); });
        }
        if (Submitted) Owner->_impl->Scheduler.WaitforTask(&CompletedTask);
        std::unique_lock<std::mutex> lock(Mutex);
        Changed.wait(lock, [&] { return Finished.load(std::memory_order_acquire); });
    }
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        error = Error;
    }
    if (error) std::rethrow_exception(error);
}

void JobHandle::Complete() const
{
    if (_state) _state->Complete();
}

bool JobHandle::IsCompleted() const
{
    return !_state || _state->Finished.load(std::memory_order_acquire);
}

JobHandle JobHandle::CombineDependencies(std::initializer_list<JobHandle> dependencies)
{
    auto combined = std::make_shared<JobDetail::State>();
    for (const auto& handle : dependencies)
        if (handle._state && std::find(combined->Dependencies.begin(), combined->Dependencies.end(), handle._state) == combined->Dependencies.end())
            combined->Dependencies.push_back(handle._state);
    if (combined->Dependencies.empty()) return {};
    if (combined->Dependencies.size() == 1) return JobHandle(combined->Dependencies.front());
    combined->RegisterDependencies();
    return JobHandle(std::move(combined));
}

void Job::Run(const JobHandle& dependency) &
{
    dependency.Complete();
    Execute();
}

ParallelJob::~ParallelJob()
{
    assert(_active.IsCompleted() && "Complete the job before destroying its object");
}

void ParallelJob::EnsureIdle() const
{
    if (!_active.IsCompleted()) throw std::logic_error("Complete the active job before changing or rescheduling it");
}

JobHandle ParallelJob::Schedule(JobSystem& jobs, size_t count, const JobHandle& dependency) &
{
    return Schedule(jobs, count, jobs.GetDefaultBatchSize(), dependency);
}

JobHandle ParallelJob::Schedule(JobSystem& jobs, size_t count, size_t batchSize, const JobHandle& dependency) &
{
    EnsureIdle();
    return jobs.Schedule(*this, count, batchSize, dependency);
}

JobSystem::JobSystem(unsigned threadCount, size_t defaultBatchSize) : _impl(std::make_unique<Impl>())
{
    if (defaultBatchSize == 0 || defaultBatchSize > (std::numeric_limits<uint32_t>::max)())
        throw std::invalid_argument("Default batch size must be nonzero and fit enkiTS's uint32_t ranges");
    _impl->DefaultBatchSize = defaultBatchSize;
    _impl->Scheduler.Initialize(threadCount ? threadCount : (std::max)(1u, std::thread::hardware_concurrency()));
}

JobSystem::~JobSystem()
{
    for (const auto& state : _impl->Active)
    {
        try { state->Complete(); }
        catch (...) {}
    }
    _impl->Scheduler.WaitforAllAndShutdown();
}

unsigned JobSystem::GetThreadCount() const { return _impl->Scheduler.GetNumTaskThreads(); }
size_t JobSystem::GetDefaultBatchSize() const { return _impl->DefaultBatchSize; }

void JobSystem::QueueTask(enki::ITaskSet& task)
{
    _impl->Scheduler.AddTaskSetToPipe(&task);
}

void JobSystem::WaitForTask(const enki::ICompletable* task)
{
    _impl->CheckThread();
    _impl->Scheduler.WaitforTask(task);
}

JobHandle JobSystem::Schedule(ParallelJob& job, size_t count, size_t batchSize, const JobHandle& dependency)
{
    _impl->CheckThread();
    if (batchSize == 0 || count > (std::numeric_limits<uint32_t>::max)() || batchSize > (std::numeric_limits<uint32_t>::max)())
        throw std::invalid_argument("Job count and nonzero batch size must fit enkiTS's uint32_t ranges");
    if (dependency._state && !dependency._state->UsesOnly(this))
        throw std::invalid_argument("Dependent jobs must use the same JobSystem");
    _impl->Active.erase(std::remove_if(_impl->Active.begin(), _impl->Active.end(),
        [](const auto& state) { return state->Finished.load(std::memory_order_acquire); }), _impl->Active.end());
    auto state = std::make_shared<JobDetail::State>();
    state->Owner = this;
    state->Work = &job;
    state->WorkTask.m_SetSize = static_cast<uint32_t>(count);
    state->WorkTask.m_MinRange = static_cast<uint32_t>(batchSize);
    if (dependency._state) state->Dependencies.push_back(dependency._state);
    _impl->Active.push_back(state);
    job._active = JobHandle(state);
    try { state->RegisterDependencies(); }
    catch (...) { state->SetError(std::current_exception()); state->Finish(); throw; }
    return JobHandle(std::move(state));
}
