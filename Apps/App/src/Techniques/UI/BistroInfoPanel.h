#pragma once

#include "BistroComparePanel.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <string>

class BistroBenchmark;
class BistroRenderPipeline;
class RenderModule;
class Window;
struct ImFont;
namespace BistroBenchmarkData { enum class RunMode; }

class BistroInfoPanel
{
public:
    void InitializeFont();
    void Hide() { _hidden = true; _count = _next = 0; _frames = 0; _elapsed = _displayFps = 0; }
    void Show(bool compact = false) { _hidden = false; _compact = compact; _checkPlacement = true; }
    void Toggle(bool compact)
    {
        if (!_hidden && _compact == compact) { Hide(); return; }
        Show(compact);
    }
    void ApplyResolution(Window&);
    void Draw(RenderModule&, BistroRenderPipeline&, BistroBenchmark&, float frameSeconds);

private:
    struct Placement { std::string Technique, Adapter, Queue; };
    void RefreshPlacement(const BistroRenderPipeline&);
    void DrawPlacement(bool compact) const;
    void DrawFull(RenderModule&, BistroBenchmark&);
    void DrawResult(const BistroBenchmark&, BistroBenchmarkData::RunMode tab);
    void DrawCurrentSettings(RenderModule&);
    void DrawLivePlot() const;

    BistroComparePanel _compare;
    ImFont* _fullFont = nullptr;
    std::vector<Placement> _placement;
    std::map<std::pair<std::string, std::string>, std::string> _groups;
    std::vector<std::string> _peers;
    std::pair<uint16_t, uint16_t> _pendingSize{};
    std::filesystem::path _pendingPreset;
    std::filesystem::path _latestResult;
    std::optional<bool> _queueResults, _singleQuality, _multiQuality;
    std::string _displayError;
    bool _plotFps = false;
    std::string _primary;
    bool _compact = false, _hidden = false, _checkPlacement = true;
    std::array<float, 180> _frameMs{};
    size_t _next = 0, _count = 0;
    float _elapsed = 0, _displayFps = 0;
    unsigned _frames = 0;
};
