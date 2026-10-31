#include "Common/ConsoleVariables.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    enum class Quality : unsigned { Low, High };
}

int main()
{
    try
    {
        ConsoleModule console;
        // These fixtures stay local to exercise scoped registration and destruction.
        AutoConsoleVariable<int> count(L"Test.Count", 3);
        AutoConsoleVariable<unsigned> frames(L"Test.Frames", 3, L"Startup setting",
            EConsoleVariableFlags::ReadOnly, [](unsigned value) { return value > 0 && value <= 8; });
        AutoConsoleVariable<float> scale(L"Test.Scale", 1.0f);
        AutoConsoleVariable<bool> enabled(L"Test.Enabled", false);
        AutoConsoleVariable<Quality> quality(L"Test.Quality", Quality::Low);
        int backing = 4;
        AutoConsoleVariableRef<int> reference(L"Test.Ref", backing, {}, EConsoleVariableFlags::ReadOnly);
        IConsoleVariable* found = nullptr;
        Require(console.TryFindConsoleVariable(L"tEsT.cOuNt", found) && found == &count,
            "Names must be case insensitive");
        Require(console.RegisterConsoleVariable(&count), "Same-pointer registration must be idempotent");
        Require(frames.GetFlags() == EConsoleVariableFlags::ReadOnly, "Flags must be visible");
        Require(count.SetFromString("-7") && count.GetInt() == -7, "Signed parsing failed");
        Require(!count.SetFromString("2147483648") && !count.SetFromString("7junk") && count.GetInt() == -7,
            "Invalid integers must not modify the value");
        Require(!frames.SetFromString("-1") && !frames.SetFromString("4294967296") &&
            !frames.SetValue(0) && !frames.SetValue(9) && frames.GetUInt() == 3,
            "Unsigned range and setting validation failed");
        Require(scale.SetFromString("1.25") && scale.GetFloat() == 1.25f &&
            !scale.SetFromString("nan") && !scale.SetFromString("inf") &&
            !scale.SetValue(std::numeric_limits<float>::infinity()), "Finite float validation failed");
        Require(enabled.SetFromString("true") && enabled.GetBool() && !enabled.SetFromString("2"),
            "Boolean validation failed");
        Require(quality.SetFromString("1") && quality.GetValue() == Quality::High, "Enum parsing failed");
        Require(reference.SetValue(5) && backing == 5, "Reference update failed");
        bool duplicateRejected = false;
        try { AutoConsoleVariable<int> duplicate(L"TEST.COUNT", 99); }
        catch (const std::invalid_argument&) { duplicateRejected = true; }
        Require(duplicateRejected && &console.FindConsoleVariable(L"Test.Count") == &count,
            "Duplicates must be rejected in Debug and Release");
        { AutoConsoleVariable<int> temporary(L"Test.Temporary", 1); }
        Require(!console.TryFindConsoleVariable(L"Test.Temporary", found) && !found,
            "Destruction must unregister the variable");
        bool missingRejected = false;
        try { console.FindConsoleVariable(L"Test.Missing"); }
        catch (const std::out_of_range&) { missingRejected = true; }
        Require(missingRejected, "Required lookup must fail explicitly");

        const auto directory = std::filesystem::temp_directory_path() /
            ("pep-cvar-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        struct Cleanup
        {
            std::filesystem::path Directory;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(Directory, error); }
        } cleanup{ directory };
        const auto ini = directory / L"settings.ini";
        const auto load = [&](const std::string& contents)
        {
            { std::ofstream file(ini, std::ios::binary); file << contents; }
            return console.LoadIni(ini);
        };
        Require(console.LoadIni(directory / "missing.ini").Error.empty() && frames.GetUInt() == 3,
            "Missing INI must retain code defaults");
        const auto valid = load("\xEF\xBB\xBF; UTF-8 BOM\r\n[ConsoleVariables]\r\n"
            "  test.frames = 2 ; startup override\r\nTest.Count=10\r\nTest.Scale=2.5\r\nTest.Enabled=0\r\n");
        Require(valid.Error.empty() && frames.GetUInt() == 2 && count.GetInt() == 10 &&
            scale.GetFloat() == 2.5f && !enabled.GetBool(), "INI settings were not applied");
        const auto invalid = load("[ConsoleVariables]\nTest.Count=11\nTest.Frames=0\nTest.Count=99\n");
        Require(invalid.ErrorLine == 3 && !invalid.Error.empty() && count.GetInt() == 11 && frames.GetUInt() == 2,
            "Value error must preserve earlier lines and stop before later lines");
        const auto syntax = load("[ConsoleVariables]\nTest.Count=12\ninvalid syntax\nTest.Count=99\n");
        Require(syntax.ErrorLine == 3 && count.GetInt() == 12, "Syntax error must stop parsing");
        const auto unknown = load("[ConsoleVariables]\nTest.Count=13\nTest.Unknown=1\nTest.Count=99\n");
        Require(unknown.ErrorLine == 3 && count.GetInt() == 13, "Unknown CVar must stop parsing");
        const auto section = load("[WrongSection]\nTest.Count=99\n");
        Require(section.ErrorLine == 1 && count.GetInt() == 13, "Unknown section must stop parsing");

        const auto startupIni = ConsoleModule::GetDefaultConfigPath();
        Require(!std::filesystem::exists(startupIni), "Test executable must not have an existing startup INI");
        std::filesystem::create_directories(startupIni.parent_path());
        struct ConfigCleanup
        {
            std::filesystem::path Path;
            ~ConfigCleanup() { std::error_code error; std::filesystem::remove(Path, error); }
        } configCleanup{ startupIni };
        struct StartupProbe : BenchmarkEngine
        {
            struct Member
            {
                Member(bool& constructed, AutoConsoleVariable<unsigned>& frames)
                {
                    Require(frames.GetUInt() == 4 && !frames.SetValue(5),
                        "Configuration must be applied and frozen before application members are constructed");
                    constructed = true;
                }
            } Initialized;
            StartupProbe(bool& constructed, AutoConsoleVariable<unsigned>& frames) : Initialized(constructed, frames) {}
        };
        const auto logPath = startupIni.parent_path().parent_path() / "Logs" / "Engine.log";
        const auto previousLogSize = std::filesystem::exists(logPath) ? std::filesystem::file_size(logPath) : 0;
        { std::ofstream file(startupIni); file << "[ConsoleVariables]\nTest.Count=27\ninvalid syntax\n"; }
        bool constructed = false;
        bool startupFailed = false;
        try { StartupProbe app(constructed, frames); }
        catch (const ConsoleConfigError&) { startupFailed = true; }
        Require(startupFailed && !constructed, "Invalid configuration must abort before application construction");
        std::ifstream log(logPath, std::ios::binary);
        log.seekg(static_cast<std::streamoff>(previousLogSize));
        const std::string logText((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        Require(logText.find("Engine.ini:3:") != std::string::npos &&
            logText.find("Application startup aborted") != std::string::npos, "Startup failure must be logged to disk");
        { std::ofstream file(startupIni); file << "[ConsoleVariables]\nTest.Count=28\nTest.Frames=4\nTest.Ref=6\n"; }
        const auto originalDirectory = std::filesystem::current_path();
        std::filesystem::current_path(directory);
        StartupProbe app(constructed, frames);
        std::filesystem::current_path(originalDirectory);
        Require(constructed && app.Initialize() && count.GetInt() == 28,
            "Valid startup INI must work independently of the current directory");
        console.Initialize();
        Require(!frames.SetValue(5) && !frames.SetFromString("5") && frames.GetUInt() == 4,
            "ReadOnly must reject typed and text updates after startup");
        Require(!reference.SetValue(7) && backing == 6, "Reference setter must obey ReadOnly");
        Require(count.SetValue(42) && console.FindConsoleVariable(L"Test.Count").SetFromString("43") &&
            count.GetInt() == 43, "Default must remain mutable at runtime");
        Require(!console.LoadIni(ini).Error.empty() && count.GetInt() == 43,
            "INI loading is a startup-only operation");
        AutoConsoleVariable<int> late(L"Test.Late", 1, {}, EConsoleVariableFlags::ReadOnly);
        Require(!late.SetValue(2), "Late registration must not reopen startup writes");
        std::cout << "Console variable tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
