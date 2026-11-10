#include <cstdint>
#include "Common/ConsoleVariables.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <process.h>
#include <io.h>
#include <sys/stat.h>
#include <thread>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    enum class Quality : std::uint8_t { Low, High, Count };
    enum class SignedQuality : std::int8_t { Low, High, Count };
    enum class LegacyMode : std::int8_t { Automatic = -3, Manual = 11 };
    enum class NativeQuality : std::int8_t { Inherit = -1, Low, High, Count, First = Inherit };
    PEP_ENUM_METADATA(Quality,
        PEP_DISPLAY_NAME(High, "High quality")
    )
}

int main(int argc, char**)
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
        Require(count.GetType() == EConsoleVariableType::Integer &&
            frames.GetType() == EConsoleVariableType::UnsignedInteger &&
            enabled.GetType() == EConsoleVariableType::Boolean &&
            scale.GetType() == EConsoleVariableType::FloatingPoint &&
            quality.GetType() == EConsoleVariableType::Enumeration &&
            reference.GetType() == EConsoleVariableType::Integer,
            "Registry must expose the declared types, including enum and reference variables");
        Require(quality.GetEnumCount() == 2 && count.GetEnumCount() == 0 &&
            !quality.SetFromString("2") && !quality.SetFromString("-1") && !quality.SetValue(Quality::Count),
            "Enum Count must expose the UI range and reject the sentinel and invalid values");
        Require(quality.GetEnumValueName(0) == "Low" && quality.GetEnumValueName(1) == "High quality" &&
            quality.GetEnumValueName(-1).empty() && quality.GetEnumValueName(2).empty() &&
            count.GetEnumValueName(0).empty() && quality.GetString() == "1",
            "Enum names and display-name metadata must remain separate from numeric serialization");
        {
            enum class LocalMode : std::uint8_t { First, Second, Count };
            AutoConsoleVariable<LocalMode> local(L"Test.LocalMode", LocalMode::Second);
            Require(local.GetEnumValueName(1) == "Second", "Function-local enums must expose their declared names");
        }
        const auto snapshot = console.GetConsoleVariables();
        Require(std::is_sorted(snapshot.begin(), snapshot.end(), [](const auto* left, const auto* right)
            { return left->GetName() < right->GetName(); }), "Registry snapshots must have deterministic order");
        Require(console.GetConsoleVariables(L"tEsT.").size() == snapshot.size() &&
            console.GetConsoleVariables(L"Test.Count").size() == 1 &&
            console.GetConsoleVariables(L"Missing.").empty(), "Registry prefix filtering failed");
        {
            AutoConsoleVariable<SignedQuality> dynamic(L"Test.Dynamic.Option", SignedQuality::Low, L"0 Low, 1 High");
            const auto discovered = console.GetConsoleVariables(L"TEST.DYNAMIC.");
            Require(discovered.size() == 1 && discovered.front() == &dynamic &&
                dynamic.GetEnumCount() == 2 && dynamic.SetFromString("1") &&
                !dynamic.SetFromString("-1") && !dynamic.SetFromString("2") && dynamic.GetString() == "1",
                "Runtime discovery must expose enum types, counts and range validation");
            SignedQuality enumBacking = SignedQuality::Low;
            AutoConsoleVariableRef<SignedQuality> enumRef(L"Test.Dynamic.Ref", enumBacking);
            Require(enumRef.GetEnumCount() == 2 && enumRef.SetFromString("1") &&
                !enumRef.SetValue(SignedQuality::Count) && enumBacking == SignedQuality::High &&
                enumRef.GetEnumValueName(1) == "High",
                "Referenced enums must preserve type and enforce Count");
            AutoConsoleVariable<LegacyMode> legacy(L"Test.Dynamic.Legacy", LegacyMode::Automatic);
            Require(legacy.GetEnumCount() == 0 && legacy.SetFromString("11") && legacy.GetValue() == LegacyMode::Manual,
                "Enums without Count must retain their existing numeric behavior");
        }
        Require(console.GetConsoleVariables(L"Test.Dynamic.").empty(), "Enumeration retained a destroyed CVar");
        Require(count.GetString() == "-7" && frames.GetString() == "3" && scale.GetString() == "1.25" &&
            enabled.GetString() == "true" && quality.GetString() == "1" && reference.GetString() == "5",
            "Console display must preserve values, enums and references");
        AutoConsoleVariable<uint64_t> wide(L"Test.Wide", UINT64_MAX);
        AutoConsoleVariable<double> precise(L"Test.Precise", 1.2345678901234567);
        if (argc > 1)
        {
            console.Initialize();
            Require(count.GetInt() == 43 && frames.GetUInt() == 5 && backing == 7 &&
                scale.GetValue() == 1.75f && enabled.GetBool() && quality.GetValue() == Quality::Low &&
                wide.GetValue() == 42 && precise.GetValue() == 0.125,
                "Saved runtime and pending readonly values must load in a new process");
            std::cout << "Saved CVar startup reload passed\n";
            return 0;
        }
        Require(wide.GetString() == "18446744073709551615" && wide.SetFromString(wide.GetString()) &&
            precise.SetFromString(precise.GetString()) && precise.GetValue() == 1.2345678901234567,
            "Console display must round-trip without narrowing integers or floating point values");
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
        {
            AutoConsoleVariable<NativeQuality> native(L"Test.NativeQuality", NativeQuality::Inherit);
            Require(native.GetEnumFirstValue() == -1 && native.GetEnumCount() == 3 &&
                native.SetFromString("-1") && native.SetFromString("1") &&
                !native.SetFromString("-2") && !native.SetFromString("2") &&
                native.GetEnumValueName(-1) == "Inherit" && native.GetEnumValueName(0) == "Low" &&
                native.GetEnumValueName(1) == "High" && native.GetEnumValueName(2).empty(),
                "Native enum ranges must include the inherited preset without renumbering INI values");
        }

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
        { std::ofstream file(startupIni, std::ios::binary); file <<
            "\xEF\xBB\xBF; Keep this comment\r\n[ConsoleVariables]\r\n"
            "test.count = 20 ; duplicate\r\nTest.Count=28\r\nTest.Frames=4\r\nTest.Ref=6\r\n"; }
        const auto startupOverlay = directory / "startup.ini";
        { std::ofstream file(startupOverlay); file << "[ConsoleVariables]\nTest.Frames=3\nTest.Unknown=1\n"; }
        bool overlayRejected = false;
        try { console.InitializeWithOverlay(startupOverlay); }
        catch (const ConsoleConfigError&) { overlayRejected = true; }
        Require(overlayRejected && frames.GetUInt() == 4,
            "An invalid startup overlay must leave all base INI values in place");
        { std::ofstream file(startupOverlay); file << "[ConsoleVariables]\nTest.Frames=4\nTest.Count=29\n"; }
        console.InitializeWithOverlay(startupOverlay);
        const auto originalDirectory = std::filesystem::current_path();
        std::filesystem::current_path(directory);
        StartupProbe app(constructed, frames);
        std::filesystem::current_path(originalDirectory);
        Require(constructed && app.Initialize() && count.GetInt() == 29,
            "The overlay must take precedence before startup settings are frozen");
        console.Initialize();
        Require(!frames.SetValue(5) && !frames.SetFromString("5") && frames.GetUInt() == 4,
            "ReadOnly must reject typed and text updates after startup");
        Require(!reference.SetValue(7) && backing == 6, "Reference setter must obey ReadOnly");
        Require(count.SetValue(42) && console.FindConsoleVariable(L"Test.Count").SetFromString("43") &&
            count.GetInt() == 43, "Default must remain mutable at runtime");
        Require(frames.SetConfigFromString("5") && frames.GetUInt() == 4 && frames.GetConfigString() == "5" &&
            reference.SetConfigFromString("7") && backing == 6 && reference.GetConfigString() == "7",
            "Readonly config edits must save pending values without changing active resources");
        Require(!frames.SetConfigFromString("0") && frames.GetConfigString() == "5",
            "Pending readonly edits must obey the same validator");
        Require(frames.SetConfigFromString("4") && frames.GetConfigString() == "4" && frames.SetConfigFromString("5"),
            "A pending readonly change must be reversible");
        Require(scale.SetConfigFromString("1.75") && scale.GetValue() == 1.75f && enabled.SetValue(true) &&
            quality.SetValue(Quality::Low) && wide.SetValue(42) && precise.SetValue(0.125),
            "All supported CVar types must save through UI and typed setters");
        const auto readSaved = [&]
        {
            std::ifstream file(startupIni, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        bool countSaved = true, scaleSaved = true;
        std::thread countWriter([&] { for (int i = 0; i < 4; ++i) countSaved &= count.SetValue(50 + i); });
        std::thread scaleWriter([&] { for (int i = 0; i < 4; ++i) scaleSaved &= scale.SetValue(2.0f + i); });
        countWriter.join();
        scaleWriter.join();
        Require(countSaved && scaleSaved && readSaved().find("Test.Count=53") != std::string::npos &&
            readSaved().find("Test.Scale = 5") != std::string::npos,
            "Concurrent edits of different CVars must not lose persisted values");
        Require(count.SetValue(43) && scale.SetValue(1.75f), "Cannot restore concurrent-test values");
        const auto saved = readSaved();
        const auto patchPath = directory / "candidate.ini";
        const auto readPatch = [&](std::string_view contents, ConsoleVariablePatch& patch)
        {
            { std::ofstream file(patchPath, std::ios::binary); file << contents; }
            return ConsoleModule::ReadPatchIni(patchPath, patch);
        };
        ConsoleVariablePatch patch;
        Require(!ConsoleModule::ReadPatchIni(directory / "missing.ini", patch).Error.empty(),
            "An explicitly requested overlay must exist");
        Require(!readPatch("[ConsoleVariables]\nTest.Count=70\ninvalid syntax\n", patch).Error.empty() &&
            patch.empty(), "Invalid syntax must not expose a partial patch");
        Require(readPatch("[ConsoleVariables]\nTest.Count=70\nTest.Scale=3.5\n", patch).Error.empty(),
            "A valid overlay must parse");
        ConsoleVariablePatch previous;
        Require(console.ApplyPatch(patch, &previous).Error.empty() && count.GetInt() == 70 &&
            scale.GetFloat() == 3.5f && readSaved() == saved && frames.GetConfigString() == "5",
            "Trial patches must affect only live values, without saving");
        Require(console.ApplyPatch(previous).Error.empty() && count.GetInt() == 43 &&
            scale.GetFloat() == 1.75f && readSaved() == saved, "Rollback must restore the live values");
        for (const auto& invalidPatch : std::vector<ConsoleVariablePatch>{
            {{L"Test.Count", "70"}, {L"test.count", "80"}},
            {{L"Test.Count", "70"}, {L"Test.Missing", "1"}},
            {{L"Test.Count", "70"}, {L"Test.Scale", "nan"}},
            {{L"Test.Count", "70"}, {L"Test.Frames", "5"}}})
        {
            Require(!console.ApplyPatch(invalidPatch).Error.empty() && count.GetInt() == 43 &&
                readSaved() == saved, "Invalid patches must leave every live and persisted value intact");
        }
        const auto profile = directory / "profile.ini";
        for (const auto* values : {&patch, &previous})
        {
            const auto result = console.SavePatchIni(profile, *values);
            if (!result.Error.empty()) throw std::runtime_error("Profile save failed: " + result.Error);
        }
        const auto loadedProfile = ConsoleModule::ReadPatchIni(profile, patch);
        if (!loadedProfile.Error.empty()) throw std::runtime_error("Profile load failed: " + loadedProfile.Error);
        Require(patch == previous && std::filesystem::exists(directory / "profile.ini.bak") &&
            readSaved() == saved, "Explicit profile save must round-trip separately and retain a backup");
        Require(saved.find("\xEF\xBB\xBF; Keep this comment\r\n") == 0 &&
            saved.find("test.count = 43 ; duplicate\r\nTest.Count=43\r\n") != std::string::npos &&
            saved.find("Test.Frames=5\r\nTest.Ref=7\r\n") != std::string::npos &&
            saved.find("Test.Scale = 1.75") != std::string::npos,
            "Autosave must update duplicate keys, append missing keys and preserve comments/BOM/line endings");
        Require(!count.SetFromString("invalid") && !quality.SetConfigFromString("2") && readSaved() == saved,
            "Rejected edits must leave the INI untouched");
        {
            struct RestoreWrite
            {
                std::filesystem::path Path;
                ~RestoreWrite()
                {
                    _wchmod(Path.c_str(), _S_IREAD | _S_IWRITE);
                }
            } restore{startupIni};
            Require(_wchmod(startupIni.c_str(), _S_IREAD) == 0, "Cannot make test INI read-only");
            Require(!count.SetValue(99) && count.GetInt() == 43 && !frames.SetConfigFromString("6") &&
                frames.GetConfigString() == "5" && readSaved() == saved,
                "Failed atomic replacement must retain the original INI and active/pending values");
        }
        const auto executable = startupIni.parent_path().parent_path() / "ConsoleVariableTests.exe";
        Require(_wspawnl(_P_WAIT, executable.c_str(), executable.c_str(), L"--verify-saved", nullptr) == 0,
            "Saved INI did not survive a fresh application startup");
        Require(!console.LoadIni(ini).Error.empty() && count.GetInt() == 43,
            "INI loading is a startup-only operation");
        AutoConsoleVariable<int> late(L"Test.Late", 1, {}, EConsoleVariableFlags::ReadOnly);
        Require(!late.SetValue(2), "Late registration must not reopen startup writes");
        std::filesystem::remove(startupIni);
        Require(count.SetValue(44) && readSaved() == "[ConsoleVariables]\nTest.Count = 44\n",
            "First runtime edit must create a missing INI with the required section");
        { std::ofstream file(startupIni, std::ios::binary); file << "; Example: [ConsoleVariables]\r\n"; }
        Require(count.SetValue(45) && readSaved() ==
            "; Example: [ConsoleVariables]\r\n[ConsoleVariables]\r\nTest.Count = 45\r\n",
            "A section mentioned only in a comment must not suppress the real header or change newlines");
        std::cout << "Console variable tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
