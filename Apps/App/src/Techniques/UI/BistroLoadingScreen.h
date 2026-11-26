#pragma once

#include <windows.h>
#include <cstddef>
#include <memory>
#include <string>

class BistroLoadingScreen
{
public:
    enum Phase : unsigned { Renderer, SceneAssets, World, FirstFrame };
    BistroLoadingScreen(HWND owner, const std::wstring& mapName);
    ~BistroLoadingScreen();
    void Update(Phase phase, const std::wstring& detail, size_t completed = 0, size_t total = 0);
    bool Cancelled() const;
    void RequestCancel();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
