#pragma once
#include <filesystem>

namespace Engine::UI
{
    
    class AssetBrowserPanel
    {
    public:
        void Draw(const std::filesystem::path& rootPath);

        const std::filesystem::path& GetSelectedPath() const;

    private:
        std::filesystem::path _selectedPath;
        std::filesystem::path _currentDirectory;
        bool _isInitialised = false;

        void DrawDirectoryTree(const std::filesystem::path& root);
        void DrawCurrentDirectoryContents();
    };
}

