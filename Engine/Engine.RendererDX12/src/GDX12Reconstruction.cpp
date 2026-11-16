#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Common/ConsoleVariables.h"
#include <cmath>

namespace
{
    AutoConsoleVariable<GDX12UpscalerProvider> Provider(L"Render.Upscaler.Provider", GDX12UpscalerProvider::XeSS,
        L"0 Native, 1 XeSS, 2 FSR, 3 DLSS, 4 NIS, 5 Native TAA", EConsoleVariableFlags::Default);
    AutoConsoleVariable<GDX12UpscalerQuality> Quality(L"Render.Upscaler.Quality", GDX12UpscalerQuality::Default,
        L"0 SDK default, 1 NativeAA, 2 Quality, 3 Balanced, 4 Performance, 5 UltraPerformance, 6 UltraQuality, 7 UltraQualityPlus; unavailable presets fall back to native",
        EConsoleVariableFlags::Default);
    AutoConsoleVariable<uint64_t> Restart(L"Render.Reconstruction.Restart", 0,
        L"Change this revision to recreate active reconstruction contexts at the next frame boundary",
        EConsoleVariableFlags::Default);
    AutoConsoleVariable<float> HistoryWeight(L"Render.TAA.HistoryWeight", 0.9f,
        L"Native TAA maximum history contribution, finite [0,1).", EConsoleVariableFlags::Default,
        [](float v) { return std::isfinite(v) && v >= 0 && v < 1; });
    AutoConsoleVariable<float> VarianceGamma(L"Render.TAA.VarianceGamma", 1.0f,
        L"Native TAA neighborhood standard deviation multiplier, finite and positive.", EConsoleVariableFlags::Default,
        [](float v) { return std::isfinite(v) && v > 0; });
}

float GetTAAHistoryWeight() { return HistoryWeight.GetValue(); }
float GetTAAVarianceGamma() { return VarianceGamma.GetValue(); }

GDX12ReconstructionSettings GetReconstructionSettings()
{
    return { Provider.GetValue(), Quality.GetValue(), Restart.GetValue() };
}
