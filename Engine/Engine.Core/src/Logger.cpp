#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

struct Logger::Impl
{
    struct Record
    {
        uint64_t Sequence;
        std::chrono::system_clock::time_point Time;
        std::thread::id Thread;
        LogLevel Level;
        std::string Category;
        std::function<void(std::ostream&)> Format;
    };

    explicit Impl(std::filesystem::path path) : Path(std::move(path)), Worker([this] { Run(); }) {}

    ~Impl()
    {
        {
            std::scoped_lock lock(Mutex);
            Stopping = true;
        }
        Changed.notify_all();
        Worker.join();
    }

    void Run() noexcept
    {
        try
        {
            if (!Path.parent_path().empty()) std::filesystem::create_directories(Path.parent_path());
            std::ofstream file(Path, std::ios::app | std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open log file");
            {
                std::scoped_lock lock(Mutex);
                Ready = true;
            }
            Changed.notify_all();
            for (;;)
            {
                std::deque<Record> batch;
                {
                    std::unique_lock lock(Mutex);
                    Changed.wait(lock, [&] { return Stopping || !Queue.empty(); });
                    if (Queue.empty() && Stopping) break;
                    batch.swap(Queue);
                }
                for (const auto& record : batch)
                {
                    std::ostringstream body;
                    record.Format(body);
                    if (!body) throw std::runtime_error("Cannot format log message");
                    auto text = body.str();
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0'))
                        text.pop_back();
                    const auto seconds = std::chrono::system_clock::to_time_t(record.Time);
                    std::tm utc{};
                    gmtime_s(&utc, &seconds);
                    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                        record.Time.time_since_epoch()).count() % 1000;
                    const char* level = record.Level == LogLevel::Error ? "ERROR" :
                        record.Level == LogLevel::Warning ? "WARNING" : "INFO";
                    file << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') <<
                        std::setw(3) << milliseconds << "Z [" << level << "] [" << record.Category <<
                        "] [thread " << record.Thread << "] " << text << '\n';
                }
                file.flush();
                if (!file) throw std::runtime_error("Cannot write log file");
                {
                    std::scoped_lock lock(Mutex);
                    Written = batch.back().Sequence;
                }
                Changed.notify_all();
            }
        }
        catch (const std::exception& error)
        {
            Fail(error.what());
        }
        catch (...)
        {
            Fail("Unknown logging failure");
        }
    }

    void Fail(const char* reason) noexcept
    {
        {
            std::scoped_lock lock(Mutex);
            Failed = true;
            Queue.clear();
        }
        Changed.notify_all();
        std::fprintf(stderr, "Logging failed: %s\n", reason);
        OutputDebugStringA(reason);
    }

    const std::filesystem::path Path;
    std::mutex Mutex;
    std::condition_variable Changed;
    // shortcut: finite startup/loading traffic; choose a capacity policy before enabling per-frame logs.
    std::deque<Record> Queue;
    uint64_t Accepted = 0;
    uint64_t Written = 0;
    bool Ready = false;
    bool Stopping = false;
    bool Failed = false;
    std::thread Worker;
};

Logger::Logger(std::filesystem::path path) : State(std::make_unique<Impl>(std::move(path))) {}
Logger::~Logger() = default;

Logger& Logger::Get()
{
    static Logger instance(Engine::Core::GetExecutableDirectory() / L"Logs" / L"Engine.log");
    return instance;
}

const std::filesystem::path& Logger::GetPath() const { return State->Path; }

bool Logger::Enqueue(LogLevel level, std::string category, std::function<void(std::ostream&)> format)
{
    {
        std::scoped_lock lock(State->Mutex);
        if (State->Stopping || State->Failed) return false;
        State->Queue.push_back({ State->Accepted + 1, std::chrono::system_clock::now(),
            std::this_thread::get_id(), level, std::move(category), std::move(format) });
        ++State->Accepted;
    }
    State->Changed.notify_all();
    return true;
}

bool Logger::Flush()
{
    std::unique_lock lock(State->Mutex);
    const auto target = State->Accepted;
    State->Changed.wait(lock, [&] { return State->Failed || (State->Ready && State->Written >= target); });
    return !State->Failed;
}

void Logger::Append(std::ostream& output, const std::filesystem::path& value) { Append(output, value.native()); }

void Logger::Append(std::ostream& output, const std::wstring& value)
{
    if (value.empty()) return;
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) { output << "<invalid UTF-16>"; return; }
    std::string text(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        text.data(), length, nullptr, nullptr);
    output << text;
}
