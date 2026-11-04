#pragma once
#include <cstdint>
#include <string>

enum class GDX12UpscalerProvider : std::uint8_t { Native, XeSS, FSR, DLSS, NIS, Count };
// Default keeps each SDK's preferred preset; XeSS defaults to Ultra Quality Plus.
enum class GDX12UpscalerQuality : std::uint8_t { Default, NativeAA, Quality, Balanced, Performance, UltraPerformance, UltraQuality, UltraQualityPlus, Count };

struct GDX12ReconstructionSettings
{
    GDX12UpscalerProvider Provider = GDX12UpscalerProvider::XeSS;
    GDX12UpscalerQuality Quality = GDX12UpscalerQuality::Default;
    uint64_t Restart = 0;
    bool operator==(const GDX12ReconstructionSettings& other) const
    { return Provider == other.Provider && Quality == other.Quality && Restart == other.Restart; }
};

struct GDX12ReconstructionStatus
{
    GDX12ReconstructionSettings Requested;
    GDX12UpscalerProvider Effective = GDX12UpscalerProvider::Native;
    std::string FallbackReason;
    uint64_t Generation = 0;
};

GDX12ReconstructionSettings GetReconstructionSettings();
