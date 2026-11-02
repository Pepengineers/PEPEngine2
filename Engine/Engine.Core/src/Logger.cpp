#include "Common/Logger.h"

#include <filesystem>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Windows.h>

namespace
{
    constexpr const char* DefaultLoggerName = "Engine";
    const std::filesystem::path DefaultLogDirectory = "Saved/Logs";
    const std::filesystem::path DefaultLogFilePath = DefaultLogDirectory / "Engine.log";

    std::shared_ptr<spdlog::logger> CreateDefaultLogger()
    {
        std::error_code directoryError;
        std::filesystem::create_directories(DefaultLogDirectory, directoryError);

        std::vector<spdlog::sink_ptr> sinks;
        sinks.reserve(3);
        sinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());

        if (GetConsoleWindow() != nullptr)
        {
            sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        if (!directoryError)
        {
            sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(DefaultLogFilePath.string(), true));
        }

        auto logger = std::make_shared<spdlog::logger>(DefaultLoggerName, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::warn);
        logger->set_pattern("[%d.%m %H:%M:%S.%e] [%n] [%^%l%$] %v");

        return logger;
    }
}

void Logger::Init()
{
    auto logger = CreateDefaultLogger();

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::warn);

    Logger::Info("Logger initialized");
}

void Logger::Shutdown()
{
    Logger::Info("Logger shutdown");
    spdlog::shutdown();
}
