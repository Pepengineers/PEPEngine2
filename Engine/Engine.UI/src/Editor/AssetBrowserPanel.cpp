#include "Engine.UI/Editor/AssetBrowserPanel.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "imgui/imgui.h"

namespace Engine::UI
{
    namespace 
    {
        bool IsDirectory(const std::filesystem::directory_entry& entry)
        {
            std::error_code errorCode;
            return entry.is_directory(errorCode);
        }
    }
    
    const std::filesystem::path& AssetBrowserPanel::GetSelectedPath() const
    {
        return _selectedPath;
    }

    void AssetBrowserPanel::Draw(const std::filesystem::path& rootPath)
    {
        if (!_isInitialised)
        {
            _currentDirectory = rootPath;
            _isInitialised = true;
        }

        ImGui::Begin("Asset Browser");

        if (!std::filesystem::exists(rootPath))
        {
            ImGui::TextDisabled("Assets folder not found:");
            ImGui::TextWrapped("%s", rootPath.string().c_str());
            ImGui::End();
            return;
        }

        ImGui::BeginChild("AssetTree", ImVec2(220.0f, 0.0f), true);
        DrawDirectoryTree(rootPath);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("AssetContents", ImVec2(0, 0), true);

        if (_currentDirectory != rootPath)
        {
            if (ImGui::Button(".. (Up)"))
            {
                _currentDirectory = _currentDirectory.parent_path();
            }
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", _currentDirectory.string().c_str());
        ImGui::Separator();

        DrawCurrentDirectoryContents();
        ImGui::EndChild();
        ImGui::End();
    }

    void AssetBrowserPanel::DrawDirectoryTree(const std::filesystem::path& root)
    {
        //recursive lambda for directory tree
        struct TreeWalker
        {
            AssetBrowserPanel* self;

            void Walk(const std::filesystem::path& dir)
            {
                std::error_code walkErrorCode;
                std::vector<std::filesystem::directory_entry> subdirs;

                for (const auto& entry : std::filesystem::directory_iterator(dir,
                                                                            std::filesystem::directory_options::skip_permission_denied,
                                                                            walkErrorCode))
                {
                    if (IsDirectory(entry) && entry.path().filename() != ".trash")
                    {
                        subdirs.push_back(entry);
                    }
                }

                std::sort(subdirs.begin(), subdirs.end(), [](const auto& a, const auto& b)
                {
                    return a.path().filename() < b.path().filename();
                });

                for (const auto& entry : subdirs)
                {
                    const std::string label = entry.path().filename().string();
                    const bool isCurrent = (entry.path() == self->_currentDirectory);

                    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                    if (isCurrent)
                    {
                        nodeFlags |= ImGuiTreeNodeFlags_Selected;
                    }

                    bool open = ImGui::TreeNodeEx(label.c_str(), nodeFlags);

                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                    {
                        self->_currentDirectory = entry.path();
                    }

                    if (open)
                    {
                        Walk(entry.path());
                        ImGui::TreePop();
                    }
                }
            }
        };

        const std::string rootLabel = root.filename().empty() ? root.string() : root.filename().string();
        ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
        if (_currentDirectory == root)
        {
            rootFlags |= ImGuiTreeNodeFlags_Selected;
        }

        bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), rootFlags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            _currentDirectory = root;
        }

        if (rootOpen)
        {
            TreeWalker walker{this};
            walker.Walk(root);
            ImGui::TreePop();
        }
    }

    void AssetBrowserPanel::DrawCurrentDirectoryContents()
    {
        std::error_code errorCode;

        std::vector<std::filesystem::directory_entry> dirs;
        std::vector<std::filesystem::directory_entry> files;

        for (const auto& entry : std::filesystem::directory_iterator(_currentDirectory,
                                                                    std::filesystem::directory_options::skip_permission_denied,
                                                                    errorCode))
        {
            if (entry.path().filename() == ".trash")
            {
                continue;
            }

            if (IsDirectory(entry))
            {
                dirs.push_back(entry);
            }
            else
            {
                files.push_back(entry);
            }
        }

        auto byName = [](const auto& a, const auto& b)
        {
            return a.path().filename() < b.path().filename();
        };

        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);

        constexpr float thumbSize = 64.0f;
        const float panelWidth = ImGui::GetContentRegionAvail().x;
        int columns = (std::max)(1, static_cast<int>(panelWidth / (thumbSize + 16.0f)));

        ImGui::Columns(columns, nullptr, false);

        for (const auto& dir : dirs)
        {
            const std::string folderName = dir.path().filename().string();
            const std::string folderLabel = "[Folder]\n" + folderName;

            ImGui::PushID(folderName.c_str());

            if (ImGui::Button(folderLabel.c_str(), ImVec2(thumbSize, thumbSize)))
            {
                _currentDirectory = dir.path();
            }

            ImGui::PopID();
            ImGui::NextColumn();
        }

        for (const auto& file : files)
        {
            const std::string fileId = file.path().string();
            const std::string fileName = file.path().filename().string();

            ImGui::PushID(fileId.c_str());

            const bool isSelected = (file.path() == _selectedPath);
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }

            if (ImGui::Button(fileName.c_str(), ImVec2(thumbSize, thumbSize)))
            {
                _selectedPath = file.path();
            }

            if (isSelected)
            {
                ImGui::PopStyleColor();
            }

            if (ImGui::BeginPopupContextItem("AssetContextMenu"))
            {
                if (ImGui::MenuItem("Delete"))
                {
                    std::filesystem::path trashDir = _currentDirectory / ".trash";
                    std::error_code makeDirErrorCode;
                    std::filesystem::create_directories(trashDir, makeDirErrorCode);

                    std::error_code moveErrorCode;
                    std::filesystem::rename(file.path(), trashDir / file.path().filename(), moveErrorCode);

                    if (_selectedPath == file.path())
                    {
                        _selectedPath.clear();
                    }
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();
            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        if (ImGui::BeginPopupContextWindow("AssetBrowserEmptyContext",
                                            ImGuiPopupFlags_MouseButtonMask_ | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("New Folder"))
            {
                std::error_code makeDirErrorCode;
                std::filesystem::create_directories(_currentDirectory / "New Folder", makeDirErrorCode);
            }
            ImGui::EndPopup();
        }
    }
}
