#include "Engine.RendererDX12/RenderSettings.h"
#include "Engine.Core/ConsoleModule.h"
#include <cassert>
#include <iostream>

int main()
{
    ConsoleModule console;
    auto set = [&](const wchar_t* name, const char* value)
    {
        return console.FindConsoleVariable(name).SetFromString(value);
    };
    using namespace RenderSettings;
    assert(GetUpscaler().Enabled && GetUpscaler().Level == Quality::High);
    assert(set(L"Render.Upscaler.Enabled", "false"));
    assert(!GetUpscaler().Enabled);
    assert(set(L"Render.Upscaler.Enabled", "true"));
    for (int quality = 0; quality <= 3; ++quality)
    {
        assert(set(L"Render.Upscaler.Quality", std::to_string(quality).c_str()));
        assert(GetUpscaler().Enabled == (quality != 0));
        assert(static_cast<int>(GetUpscaler().Level) == quality);
    }
    assert(!set(L"Render.Upscaler.Quality", "4"));
    assert(!set(L"Render.Upscaler.Quality", "-1"));
    assert(set(L"Render.AO.Quality", "1"));
    const auto low = GetAO();
    assert(low.Enabled && low.Samples > 0);
    assert(set(L"Render.AO.Quality", "3"));
    assert(GetAO().Samples >= low.Samples);
    assert(set(L"Render.AO.Samples", "12"));
    assert(set(L"Render.AO.Quality", "2"));
    assert(GetAO().Samples == 12);
    assert(set(L"Render.AO.Quality", "0"));
    assert(!GetAO().Enabled);
    assert(set(L"Render.AO.Quality", "2"));
    assert(set(L"Render.AO.Enabled", "false"));
    assert(!GetAO().Enabled);
    assert(set(L"Render.AO.Enabled", "true"));
    for (int implementation = 0; implementation < 3; ++implementation)
    {
        const auto value = std::to_string(implementation);
        assert(set(L"Render.AO.Implementation", value.c_str()));
        assert(static_cast<int>(GetAO().Implementation) == implementation);
    }
    assert(!set(L"Render.AO.Implementation", "3"));
    assert(!set(L"Render.AO.Quality", "4"));
    assert(!set(L"Render.AO.Radius", "0"));
    assert(!set(L"Render.AO.Radius", "nan"));
    assert(!set(L"Render.AO.Samples", "0"));
    assert(set(L"Render.AO.Samples", "-1"));
    assert(GetAO().Samples > 0);
    std::cout << "Technique settings: selection, Off, presets, overrides and validation passed.\n";
}
