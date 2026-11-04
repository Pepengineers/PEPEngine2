#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Common/ConsoleVariables.h"

namespace
{
    AutoConsoleVariable<GDX12UpscalerProvider> Provider(L"Render.Upscaler.Provider", GDX12UpscalerProvider::XeSS,
        L"0 Native, 1 XeSS, 2 FSR, 3 DLSS, 4 NIS", EConsoleVariableFlags::Default);
    AutoConsoleVariable<GDX12UpscalerQuality> Quality(L"Render.Upscaler.Quality", GDX12UpscalerQuality::Default,
        L"0 SDK default, 1 NativeAA, 2 Quality, 3 Balanced, 4 Performance, 5 UltraPerformance, 6 UltraQuality, 7 UltraQualityPlus; unavailable presets fall back to native",
        EConsoleVariableFlags::Default);
    AutoConsoleVariable<uint64_t> Restart(L"Render.Reconstruction.Restart", 0,
        L"Change this revision to recreate active reconstruction contexts at the next frame boundary",
        EConsoleVariableFlags::Default);
}

GDX12ReconstructionSettings GetReconstructionSettings()
{
    return { Provider.GetValue(), Quality.GetValue(), Restart.GetValue() };
}
