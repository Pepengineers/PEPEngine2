#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

enum class LogLevel { Info, Warning, Error };

class Logger final
{
public:
    explicit Logger(std::filesystem::path path);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& Get();
    const std::filesystem::path& GetPath() const;
    bool Flush();

    template <class... Args>
    bool Write(LogLevel level, std::string_view category, Args&&... args) noexcept
    {
        try
        {
            auto values = std::make_tuple(Own(std::forward<Args>(args))...);
            return Enqueue(level, std::string(category),
                [values = std::move(values)](std::ostream& output)
                {
                    std::apply([&](const auto&... value) { (Append(output, value), ...); }, values);
                });
        }
        catch (...) { return false; }
    }

private:
    template <class T>
    static auto Own(T&& value)
    {
        using Value = std::decay_t<T>;
        if constexpr (std::is_same_v<Value, std::string_view>) return std::string(value);
        else if constexpr (std::is_same_v<Value, std::wstring_view>) return std::wstring(value);
        else if constexpr (std::is_same_v<Value, const char*> || std::is_same_v<Value, char*>)
            return std::string(value ? value : "<null>");
        else if constexpr (std::is_same_v<Value, const wchar_t*> || std::is_same_v<Value, wchar_t*>)
            return std::wstring(value ? value : L"<null>");
        else return Value(std::forward<T>(value));
    }

    template <class T>
    static void Append(std::ostream& output, const T& value) { output << value; }
    static void Append(std::ostream& output, const std::wstring& value);
    static void Append(std::ostream& output, const std::filesystem::path& value);
    bool Enqueue(LogLevel level, std::string category, std::function<void(std::ostream&)> format);

    struct Impl;
    std::unique_ptr<Impl> State;
};

namespace Log
{
    template <class... Args>
    bool Write(LogLevel level, std::string_view category, Args&&... args) noexcept
    {
        try { return Logger::Get().Write(level, category, std::forward<Args>(args)...); }
        catch (...) { return false; }
    }

    template <class... Args>
    bool Info(std::string_view category, Args&&... args) noexcept
    {
        return Log::Write(LogLevel::Info, category, std::forward<Args>(args)...);
    }

    template <class... Args>
    bool Warning(std::string_view category, Args&&... args) noexcept
    {
        return Log::Write(LogLevel::Warning, category, std::forward<Args>(args)...);
    }

    template <class... Args>
    bool Error(std::string_view category, Args&&... args) noexcept
    {
        return Log::Write(LogLevel::Error, category, std::forward<Args>(args)...);
    }

    inline bool Flush() noexcept
    {
        try { return Logger::Get().Flush(); }
        catch (...) { return false; }
    }
}
