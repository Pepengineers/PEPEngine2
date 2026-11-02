#include "Engine.Core/Logging/Logger.h"
#include "Common/Logger.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    struct Gate
    {
        std::thread::id Producer;
        std::shared_ptr<std::promise<void>> Entered;
        std::shared_future<void> Release;
        std::shared_ptr<std::atomic<bool>> OnWorker;
    };

    std::ostream& operator<<(std::ostream& output, const Gate& gate)
    {
        *gate.OnWorker = std::this_thread::get_id() != gate.Producer;
        gate.Entered->set_value();
        gate.Release.wait();
        return output << "gate";
    }

    struct ThrowOnCopy
    {
        ThrowOnCopy() = default;
        ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("Cannot snapshot argument"); }
    };
    std::ostream& operator<<(std::ostream& output, const ThrowOnCopy&) { return output << "unused"; }

    struct BadFormat {};
    std::ostream& operator<<(std::ostream& output, const BadFormat&)
    {
        output.setstate(std::ios::failbit);
        return output;
    }
}

int main()
{
    try
    {
        const auto directory = std::filesystem::temp_directory_path() /
            ("pep-log-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        struct Cleanup
        {
            std::filesystem::path Directory;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(Directory, error); }
        } cleanup{ directory };
        const auto path = directory / "nested" / "Engine.log";
        std::filesystem::create_directories(path.parent_path());
        { std::ofstream previous(path, std::ios::binary); previous << "previous session\n"; }
        {
            Logger logger(path);
            const ThrowOnCopy uncopyable;
            Require(!logger.Write(LogLevel::Info, "Failure", uncopyable), "Snapshot failures must not throw into application code");
            auto entered = std::make_shared<std::promise<void>>();
            auto enteredFuture = entered->get_future();
            std::promise<void> release;
            auto onWorker = std::make_shared<std::atomic<bool>>(false);
            Require(logger.Write(LogLevel::Info, "Async", Gate{
                std::this_thread::get_id(), entered, release.get_future().share(), onWorker }), "Gate not accepted");
            if (enteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            {
                release.set_value();
                throw std::runtime_error("Background formatter did not start");
            }
            std::string message = "snapshot";
            std::wstring wide = L"\u041f\u0440\u0438\u0432\u0435\u0442";
            std::string category = "Owned";
            auto producer = std::async(std::launch::async, [&]
            {
                return logger.Write(LogLevel::Warning, std::string_view(category),
                    std::string_view(message), " ", std::wstring_view(wide), " ", directory / "asset.obj");
            });
            const bool didNotWaitForWriter = producer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
            if (didNotWaitForWriter)
            {
                message.assign("changed");
                wide.assign(L"changed");
                category.assign("changed");
            }
            release.set_value();
            Require(didNotWaitForWriter && producer.get(), "Enqueue waited for the blocked writer");
            Require(logger.Flush() && *onWorker, "Formatting must happen off the producer thread");
            auto contents = Read(path);
            Require(contents.find("previous session\n") == 0, "Logger must append existing logs");
            Require(contents.find("[WARNING] [Owned]") != std::string::npos &&
                contents.find(u8"snapshot \u041f\u0440\u0438\u0432\u0435\u0442 ") != std::string::npos &&
                contents.find("asset.obj") != std::string::npos, "Deferred arguments must own text and preserve UTF-8");

            std::vector<std::thread> producers;
            std::atomic<bool> accepted{ true };
            for (int worker = 0; worker < 4; ++worker)
                producers.emplace_back([&, worker]
                {
                    for (int item = 0; item < 64; ++item)
                        if (!logger.Write(LogLevel::Info, "Parallel", "item=", worker, ':', item)) accepted = false;
                });
            for (auto& thread : producers) thread.join();
            Require(accepted && logger.Flush(), "Concurrent logging failed");
            contents = Read(path);
            for (int worker = 0; worker < 4; ++worker)
            {
                size_t previous = 0;
                for (int item = 0; item < 64; ++item)
                {
                    const auto marker = "item=" + std::to_string(worker) + ':' + std::to_string(item) + '\n';
                    const auto first = contents.find(marker);
                    Require(first != std::string::npos && first > previous &&
                        contents.find(marker, first + marker.size()) == std::string::npos,
                        "Each producer's records must appear exactly once and in order");
                    previous = first;
                }
            }
            Require(logger.Write(LogLevel::Error, "Exit", "drained on destruction"), "Last record not accepted");
        }
        Require(Read(path).find("[ERROR] [Exit]") != std::string::npos, "Destruction must drain pending messages");
        const auto blockedParent = directory / "not-a-directory";
        { std::ofstream blocker(blockedParent); blocker << "file"; }
        Logger failed(blockedParent / "Engine.log");
        failed.Write(LogLevel::Error, "Failure", "cannot be written");
        Require(!failed.Flush(), "File failure must be reported without hanging Flush");
        Require(!failed.Write(LogLevel::Info, "Failure", "not accepted"), "Failed backend must reject further writes");
        Logger badFormat(directory / "bad-format.log");
        badFormat.Write(LogLevel::Info, "Failure", BadFormat{});
        Require(!badFormat.Flush(), "Formatting failure must not be reported as a successful write");
        Logger::Init();
        Logger::Info("Configured logger value={}", 42);
        Logger::Error("Configured logger failure={}", "test");
        Logger::Shutdown();
        const auto formatted = Read(Logger::Get().GetPath());
        Require(formatted.find("Configured logger value=42") != std::string::npos &&
            formatted.find("[ERROR] [Engine]") != std::string::npos &&
            formatted.find("Configured logger failure=test") != std::string::npos,
            "Both logging interfaces must use the existing async sink");
        std::cout << "Logger tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
