#pragma once
#include "Engine.Core/ConsoleModule.h"
#include <imgui/imgui.h>
#include <filesystem>
#include <map>

namespace Engine::UI
{
    class SettingsCache
    {
    public:
        struct Setting { std::string Name, Label, Description; size_t VariableIndex; };
        struct Group
        {
            std::map<std::string, Group> Children;
            std::vector<Setting> Variables;
        };
        const Group& Root() const { return _root; }

        bool Update(const std::vector<IConsoleVariable*>& variables, uint64_t revision, const ImGuiTextFilter& filter)
        {
            if (_revision == revision && _filter == filter.InputBuf) return false;
            _root = {};
            for (size_t index = 0; index < variables.size(); ++index)
            {
                const auto* variable = variables[index];
                const auto name = std::filesystem::path(variable->GetName()).u8string();
                if (!filter.PassFilter(name.c_str())) continue;
                auto* group = &_root;
                size_t start = 0;
                for (auto dot = name.find('.'); dot != std::string::npos; dot = name.find('.', start))
                {
                    group = &group->Children[name.substr(start, dot - start)];
                    start = dot + 1;
                }
                if (!start) group = &_root.Children["General"];
                group->Variables.push_back({ name, name.substr(start),
                    std::filesystem::path(variable->GetDescription()).u8string(), index });
            }
            _revision = revision;
            _filter = filter.InputBuf;
            return true;
        }
    private:
        // Cache names, not owners: scoped CVars may be destroyed between UI frames.
        Group _root;
        uint64_t _revision = UINT64_MAX;
        std::string _filter;
    };
}
