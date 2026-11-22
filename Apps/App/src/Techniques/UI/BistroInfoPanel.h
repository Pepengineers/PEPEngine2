#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>

class BistroBenchmark;
class BistroRenderPipeline;
class RenderModule;

class BistroInfoPanel
{
public:
    void Hide() { _hidden = true; _count = _next = 0; _frames = 0; _elapsed = _displayFps = 0; }
    void ShowFull() { _hidden = _compact = false; _checkPlacement = true; }
    void Toggle(bool compact)
    {
        if (!_hidden && _compact == compact) { Hide(); return; }
        _hidden = false;
        _compact = compact;
        _checkPlacement = true;
    }
    void Draw(RenderModule&, BistroRenderPipeline&, BistroBenchmark&, float frameSeconds);

private:
    std::map<std::string, std::string> _owners;
    std::string _primary;
    std::array<std::string, 3> _queueRows;
    bool _compact = false, _hidden = false, _checkPlacement = true;
    uint64_t _settingsRevision = 0;
    uint64_t _snapshotRevision = 0;
    std::array<float, 180> _frameMs{};
    size_t _next = 0, _count = 0;
    float _elapsed = 0, _displayFps = 0;
    unsigned _frames = 0;
};
