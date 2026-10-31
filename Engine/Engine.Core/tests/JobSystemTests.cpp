#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TestParallelJob final : public ParallelJob
{
public:
    explicit TestParallelJob(std::function<void(size_t)> execute) : _execute(std::move(execute)) {}
    void Execute(size_t index) override { _execute(index); }
private:
    std::function<void(size_t)> _execute;
};

class TestJob final : public Job
{
public:
    explicit TestJob(std::function<void()> execute) : _execute(std::move(execute)) {}
    void Execute() override { _execute(); }
private:
    std::function<void()> _execute;
};

template<typename T, typename = void>
struct CanScheduleTemporary : std::false_type {};
template<typename T>
struct CanScheduleTemporary<T, std::void_t<decltype(std::declval<T&&>().Schedule(std::declval<JobSystem&>(), 1, 1))>> : std::true_type {};
static_assert(!CanScheduleTemporary<TestParallelJob>::value, "Scheduling a temporary job must not compile");

static void CheckThreads(unsigned threadCount)
{
    JobSystem jobs(threadCount);
    constexpr size_t count = 4097;
    std::vector<std::atomic<unsigned>> visits(count);
    for (auto& value : visits) value = 0;
    std::vector<size_t> first(count), second(count), output(count);
    TestParallelJob a([&](size_t i) { ++visits[i]; first[i] = i + 10; });
    TestParallelJob b([&](size_t i) { second[i] = i * 3; });
    TestParallelJob sum([&](size_t i) {
        Require(first.back() == count - 1 + 10 && second.back() == (count - 1) * 3,
            "A dependent job started before all prerequisite ranges finished");
        output[i] = first[i] + second[i];
    });
    auto firstHandle = a.Schedule(jobs, count, 64);
    auto secondHandle = b.Schedule(jobs, count, 47);
    auto both = JobHandle::CombineDependencies({ firstHandle, secondHandle, firstHandle, {} });
    auto result = sum.Schedule(jobs, count, 32, both);
    result.Complete();
    result.Complete();
    Require(result.IsCompleted(), "Completed handle must report completion");
    for (size_t i = 0; i < count; ++i)
    {
        Require(visits[i] == 1, "Every parallel index must run exactly once");
        Require(output[i] == i * 4 + 10, "Parallel dependency chain produced incorrect output");
    }

    size_t ranEmpty = 0;
    TestParallelJob empty([&](size_t) { ++ranEmpty; });
    empty.Schedule(jobs, 0, 16, result).Complete();
    Require(ranEmpty == 0, "Empty range must not execute an element");
    JobHandle{}.Complete();

    const auto caller = std::this_thread::get_id();
    bool syncRan = false;
    TestJob sync([&] { Require(std::this_thread::get_id() == caller, "Job must run synchronously on its caller"); syncRan = true; });
    sync.Run(result);
    Require(syncRan, "Synchronous Job must finish before Run returns");

    bool failed = false;
    std::atomic<unsigned> dependentExecutions{ 0 };
    TestParallelJob bad([](size_t i) { if (i == 7) throw std::runtime_error("expected job failure"); });
    TestParallelJob afterBad([&](size_t) { ++dependentExecutions; });
    auto failedHandle = bad.Schedule(jobs, 128, 8);
    auto afterFailure = afterBad.Schedule(jobs, 128, 8, failedHandle);
    try { afterFailure.Complete(); }
    catch (const std::runtime_error&) { failed = true; }
    Require(failed && dependentExecutions == 0, "Failure must reach Complete and skip dependent work");

    std::atomic<unsigned> siblingExecutions{ 0 };
    TestParallelJob sibling([&](size_t) { ++siblingExecutions; });
    auto siblingHandle = sibling.Schedule(jobs, 193, 8);
    failed = false;
    try { JobHandle::CombineDependencies({ failedHandle, siblingHandle }).Complete(); }
    catch (const std::runtime_error&) { failed = true; }
    Require(failed && siblingExecutions == 193, "Fan-in must drain all prerequisites even after one fails");

    std::atomic<bool> release{ false };
    TestParallelJob active([&](size_t) { while (!release.load()) std::this_thread::yield(); });
    auto activeHandle = active.Schedule(jobs, 1, 1);
    bool rejected = false;
    try { active.Schedule(jobs, 1, 1); }
    catch (const std::logic_error&) { rejected = true; }
    {
        JobSystem otherJobs(2);
        TestParallelJob foreign([](size_t) {});
        bool foreignRejected = false;
        try { foreign.Schedule(otherJobs, 1, 1, JobHandle::CombineDependencies({ activeHandle, result })); }
        catch (const std::invalid_argument&) { foreignRejected = true; }
        Require(foreignRejected, "Combined dependencies from another JobSystem must be rejected");
    }
    release = true;
    activeHandle.Complete();
    Require(rejected, "Rescheduling an active job must be rejected");
    active.Schedule(jobs, 32, 4).Complete();

    rejected = false;
    try { active.Schedule(jobs, 1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Zero batch size must be rejected");
}

static void CheckAutomaticDependencies()
{
    JobSystem jobs(4);
    std::atomic<bool> release{ false }, finished{ false };
    TestParallelJob first([&](size_t) { while (!release.load()) std::this_thread::yield(); });
    TestParallelJob second([&](size_t) { finished = true; });
    auto a = first.Schedule(jobs, 1, 1);
    auto b = second.Schedule(jobs, 1, 1, a);
    release = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool automatic = finished.load();
    b.Complete();
    Require(automatic, "Dependent jobs must start without the caller invoking Complete");
}

int main()
{
    try
    {
        CheckThreads(1);
        CheckThreads(4);
        CheckAutomaticDependencies();
        std::cout << "Job ranges, dependency chains/fan-in, synchronous work, errors and reuse passed (1/4 threads).\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
