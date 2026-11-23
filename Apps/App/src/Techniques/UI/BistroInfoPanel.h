#pragma once

#include "BistroComparePanel.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>
#include <string>

class BistroBenchmark;
class BistroRenderPipeline;
class RenderModule;
class Window;
namespace BistroBenchmarkData { enum class RunMode; }

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
    void ApplyResolution(Window&);
    void Draw(RenderModule&, BistroRenderPipeline&, BistroBenchmark&, float frameSeconds);

private:
    struct Placement { std::string Technique, Adapter, Queue; };
    void RefreshPlacement(const BistroRenderPipeline&);
    void DrawPlacement(bool compact) const;
    void DrawResult(const BistroBenchmark&);
    void DrawCurrentSettings(RenderModule&);
    void DrawLivePlot(float height, BistroBenchmarkData::RunMode mode) const;

    BistroComparePanel _compare;
    std::vector<Placement> _placement;
    std::map<std::pair<std::string, std::string>, std::string> _groups;
    std::vector<std::string> _peers;
    std::pair<uint16_t, uint16_t> _pendingSize{};
    struct Series
    {
        std::filesystem::path Path;
        std::vector<std::pair<float, float>> Points;
        float MaxMs = 0, MaxFps = 0;
    };
    std::array<Series, 6> _series;
    BistroBenchmarkData::RunMode _selectedMode{};
    std::filesystem::path _latestResult;
    bool _quality = false;
    std::string _displayError, _csvError;
    bool _plotFps = false;
    std::string _primary;
    bool _compact = false, _hidden = false, _checkPlacement = true;
    uint64_t _settingsRevision = 0;
    uint64_t _snapshotRevision = 0;
    std::array<float, 180> _frameMs{};
    size_t _next = 0, _count = 0;
    float _elapsed = 0, _displayFps = 0;
    unsigned _frames = 0;
};
