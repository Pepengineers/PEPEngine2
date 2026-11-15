#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <filesystem>
#include <string>

namespace AppYaml
{
    ryml::Tree LoadFile(const std::filesystem::path& path);

    inline bool HasChild(const ryml::NodeRef& node, const char* name)
    {
        return node.has_child(ryml::to_csubstr(name));
    }   
    
    inline std::string ReadString(const ryml::NodeRef& node, const char* name, const std::string& defaultValue = "")
    {
        const ryml::csubstr key = ryml::to_csubstr(name);
        if (!node.is_map() || !node.has_child(key))
        {
            return defaultValue;
        }

        std::string value;
        const auto child = node[key];
        if (child.has_val())
        {
            child >> value;
        }
        return value;
    }
    
    inline int ReadInt(const ryml::NodeRef& node, const char* name, int defaultValue = 0)
    {
        const ryml::csubstr key = ryml::to_csubstr(name);
        
        if (!node.has_child(key))
        {
            return defaultValue;
        }

        int value = defaultValue;
        node[key] >> value;
        return value;
    }
}
