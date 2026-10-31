#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>

namespace JobDetail { struct State; }
class JobSystem;

class JobHandle
{
public:
    JobHandle() = default;
    void Complete() const;
    bool IsCompleted() const;
    static JobHandle CombineDependencies(std::initializer_list<JobHandle> dependencies);

private:
    friend class JobSystem;
    friend class ParallelJob;
    explicit JobHandle(std::shared_ptr<JobDetail::State> state) : _state(std::move(state)) {}
    std::shared_ptr<JobDetail::State> _state;
};

class Job
{
public:
    virtual ~Job() = default;
    virtual void Execute() = 0;
    void Run(const JobHandle& dependency = {}) &;
    void Run(const JobHandle& dependency = {}) && = delete;
};

class ParallelJob
{
public:
    ParallelJob() = default;
    virtual ~ParallelJob();
    ParallelJob(const ParallelJob&) = delete;
    ParallelJob& operator=(const ParallelJob&) = delete;
    virtual void Execute(size_t index) = 0;

    // The job and its data must remain alive and unchanged until Complete.
    JobHandle Schedule(JobSystem& jobs, size_t count, const JobHandle& dependency = {}) &;
    JobHandle Schedule(JobSystem& jobs, size_t count, const JobHandle& dependency = {}) && = delete;
    JobHandle Schedule(JobSystem& jobs, size_t count, size_t batchSize, const JobHandle& dependency = {}) &;
    JobHandle Schedule(JobSystem& jobs, size_t count, size_t batchSize, const JobHandle& dependency = {}) && = delete;

protected:
    void EnsureIdle() const;

private:
    friend class JobSystem;
    JobHandle _active;
};

class JobSystem
{
public:
    // threadCount includes the calling thread; zero uses the hardware thread count.
    explicit JobSystem(unsigned threadCount = 0, size_t defaultBatchSize = 64);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    unsigned GetThreadCount() const;
    size_t GetDefaultBatchSize() const;

private:
    friend class ParallelJob;
    friend struct JobDetail::State;
    struct Impl;
    std::unique_ptr<Impl> _impl;
    JobHandle Schedule(ParallelJob& job, size_t count, size_t batchSize, const JobHandle& dependency);
};
