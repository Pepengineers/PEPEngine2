#pragma once

#include <array>
#include <cstddef>

class BistroBenchmark;
class BistroRenderPipeline;
class RenderModule;

class BistroInfoPanel
{
public:
    static float Height(float width) { return width >= 900 ? 98.f : 204.f; }
    void Draw(RenderModule&, const BistroRenderPipeline&, BistroBenchmark&, float frameSeconds);

private:
    std::array<float, 180> _fps{};
    size_t _next = 0, _count = 0;
    float _elapsed = 0, _displayFps = 0;
    unsigned _frames = 0;
};
