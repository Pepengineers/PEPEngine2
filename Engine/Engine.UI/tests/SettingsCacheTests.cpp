#include "../src/SettingsCache.h"
#include "Common/ConsoleVariables.h"
#include <cstdio>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        ConsoleModule console;
        Engine::UI::SettingsCache cache;
        ImGuiTextFilter filter;
        const auto setFilter = [&](const char* text)
        {
            std::snprintf(filter.InputBuf, sizeof(filter.InputBuf), "%s", text);
            filter.Build();
        };
        AutoConsoleVariable<int> hidden(L"Alpha.Hidden", 1);
        const auto update = [&]
        {
            uint64_t revision;
            const auto variables = console.GetConsoleVariables({}, &revision);
            return cache.Update(variables, revision, filter);
        };
        Require(update() && !update(), "Warm tree rebuilt");
        auto revision = console.GetConsoleVariableRevision();
        {
            AutoConsoleVariable<int> value(L"Test.Settings.Count", 7, L"Live count");
            Require(console.GetConsoleVariableRevision() != revision && update(),
                "Registration did not rebuild the tree");
            const auto& setting = cache.Root().Children.at("Test").Children.at("Settings").Variables.at(0);
            Require(&console.FindConsoleVariable(setting.RegistryName) == &value,
                "Cached setting name did not resolve to the live variable");
            Require(setting.Label == "Count" && setting.Description == "Live count", "Cached labels changed");
            revision = console.GetConsoleVariableRevision();
            Require(console.RegisterConsoleVariable(&value) && console.GetConsoleVariableRevision() == revision,
                "Idempotent registration invalidated structure");
            Require(value.SetFromString("9") && !update() &&
                console.FindConsoleVariable(setting.RegistryName).GetInt() == 9, "Value edits rebuilt the tree or went stale");
            setFilter("Missing");
            Require(update() && cache.Root().Children.empty(), "Search retained unmatched groups");
            setFilter("Settings,-Missing");
            Require(update() && cache.Root().Children.count("Test"), "Search lost matching groups");
        }
        Require(console.GetConsoleVariableRevision() != revision && update() &&
            cache.Root().Children.empty(), "Destroyed scoped variable remained in the tree");
        {
            AutoConsoleVariable<int> replacement(L"Test.Settings.Count", 12, L"Replacement");
            Require(update() && cache.Root().Children.at("Test").Children.at("Settings")
                .Variables.at(0).Description == "Replacement", "Reused name retained the previous owner");
        }
        filter.Clear();
        AutoConsoleVariable<bool> general(L"Standalone", true);
        Require(update() && cache.Root().Children.at("General").Variables.at(0).Label == "Standalone",
            "Unqualified setting lost its General group");
        std::cout << "Settings cache: warm reuse, live values, registration/lifetime, replacement and search passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
