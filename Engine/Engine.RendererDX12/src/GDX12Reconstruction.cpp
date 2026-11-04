#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Common/ConsoleVariables.h"

namespace
{
    AutoConsoleVariable<int> Provider(L"Render.Upscaler.Provider", 1,
        L"0 Native, 1 XeSS, 2 FSR, 3 DLSS, 4 NIS", EConsoleVariableFlags::Default,
        [](const int v) { return v >= 0 && v <= 4; });
    AutoConsoleVariable<int> Quality(L"Render.Upscaler.Quality", 0,
        L"0 SDK default, 1 NativeAA, 2 Quality, 3 Balanced, 4 Performance, 5 UltraPerformance, 6 UltraQuality, 7 UltraQualityPlus; unavailable presets fall back to native",
        EConsoleVariableFlags::Default, [](const int v) { return v >= 0 && v <= 7; });
    AutoConsoleVariable<uint64_t> Restart(L"Render.Reconstruction.Restart", 0,
        L"Change this revision to recreate active reconstruction contexts at the next frame boundary",
        EConsoleVariableFlags::Default);
}

GDX12ReconstructionSettings GetReconstructionSettings()
{
    return { static_cast<GDX12UpscalerProvider>(Provider.GetValue()),
        static_cast<GDX12UpscalerQuality>(Quality.GetValue()), Restart.GetValue() };
}
