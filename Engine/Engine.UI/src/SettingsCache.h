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
        struct Setting { std::wstring RegistryName; std::string Name, Label, Description; };
        struct Group
        {
            std::map<std::string, Group> Children;
            std::vector<Setting> Variables;
        };
        const Group& Root() const { return _root; }
        bool Contains(std::wstring_view name) const
        {
            const auto contains = [&](auto&& self, const Group& group) -> bool {
                for (const auto& setting : group.Variables)
                    if (setting.RegistryName == name) return true;
                for (const auto& child : group.Children)
                    if (self(self, child.second)) return true;
                return false;
            };
            return contains(contains, _root);
        }

        bool IsCurrent(uint64_t revision, const ImGuiTextFilter& filter, unsigned viewVersion = 0) const
        {
            return _revision == revision && _filter == filter.InputBuf && _viewVersion == viewVersion;
        }
        bool Update(const std::vector<IConsoleVariable*>& variables, uint64_t revision, const ImGuiTextFilter& filter,
            unsigned viewVersion = 0)
        {
            if (IsCurrent(revision, filter, viewVersion)) return false;
            _root = {};
            for (const auto* variable : variables)
            {
                const auto name = std::filesystem::path(variable->GetName()).u8string();
                const auto description = std::filesystem::path(variable->GetDescription()).u8string();
                if (filter.IsActive() && !filter.PassFilter((name + " " + description).c_str())) continue;
                auto* group = &_root;
                size_t start = 0;
                for (auto dot = name.find('.'); dot != std::string::npos; dot = name.find('.', start))
                {
                    group = &group->Children[name.substr(start, dot - start)];
                    start = dot + 1;
                }
                if (!start) group = &_root.Children["General"];
                const auto label = name == "Render.SSR.Quality" ? "SSR quality" :
                    name == "Render.ReflectionProbes.Quality" ? "Reflection probe quality" : name.substr(start);
                group->Variables.push_back({ std::wstring(variable->GetName()), name, label, description });
            }
            _revision = revision;
            _filter = filter.InputBuf;
            _viewVersion = viewVersion;
            return true;
        }
    private:
        // Cache names, not owners: scoped CVars may be destroyed between UI frames.
        Group _root;
        uint64_t _revision = UINT64_MAX;
        unsigned _viewVersion = 0;
        std::string _filter;
    };
}
