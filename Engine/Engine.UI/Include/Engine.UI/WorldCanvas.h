#pragma once

#include "Engine.UI/OffscreenUI.h"
#include <string>

namespace Engine::UI
{
    enum class CanvasId : std::uint64_t { Invalid = 0 };
    enum class CanvasQuality { Low = 0, Medium = 1, Count };
    enum class CanvasStyle { Neon, Graffiti, Image };
    inline constexpr UITextureId CanvasEffectTextureId{1};

    struct CanvasSize
    {
        UINT Width = 0;
        UINT Height = 0;
        friend bool operator==(CanvasSize a, CanvasSize b) { return a.Width == b.Width && a.Height == b.Height; }
        friend bool operator!=(CanvasSize a, CanvasSize b) { return !(a == b); }
    };

    struct CanvasProfile
    {
        CanvasSize RasterSize;
        CanvasSize EffectsSize;
        UINT Stars;
        UINT BlurRadius;
    };

    CanvasQuality GetCanvasQuality();
    CanvasProfile GetCanvasProfile(CanvasSize logicalSize, CanvasQuality quality);

    struct CanvasFrame
    {
        CanvasId Id;
        CanvasSize LogicalSize;
        double TimeSeconds;
        CanvasQuality Quality;
        std::uint64_t SurfaceRevision;
        std::shared_ptr<const DrawData> Draw;
        CanvasStyle Style = CanvasStyle::Neon;
    };

    // Construct, build and destroy on the ImGui owner thread.
    class WorldCanvas
    {
    public:
        WorldCanvas(CanvasId id, std::string title, CanvasStyle style = CanvasStyle::Neon);
        ~WorldCanvas();
        WorldCanvas(const WorldCanvas&) = delete;
        WorldCanvas& operator=(const WorldCanvas&) = delete;

        std::shared_ptr<const CanvasFrame> BuildFrame(CanvasSize logicalSize, double timeSeconds, CanvasQuality quality);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    class CanvasEffects
    {
    public:
        explicit CanvasEffects(GDX12Device& device);
        ~CanvasEffects();
        CanvasEffects(const CanvasEffects&) = delete;
        CanvasEffects& operator=(const CanvasEffects&) = delete;

        struct Prepared;
        // Prepare on the owner; reuse only after all GPU users of the previous recording complete.
        std::shared_ptr<Prepared> Prepare(const CanvasFrame& frame, GDX12ResourceHandle output) const;
        [[nodiscard]] std::shared_ptr<const void> RecordPrepared(GDX12CommandList& commands, const CanvasFrame& frame,
            const std::shared_ptr<Prepared>& prepared) const;

        struct PreparedPost;
        std::shared_ptr<PreparedPost> PreparePost(const CanvasFrame& frame, GDX12ResourceHandle raster,
            GDX12ResourceHandle output) const;
        // Raster remains NON_PIXEL_SHADER_RESOURCE, output remains UAV. Reuse after prior GPU users complete.
        [[nodiscard]] std::shared_ptr<const void> RecordPost(GDX12CommandList& commands, const CanvasFrame& frame,
            const std::shared_ptr<PreparedPost>& prepared) const;

        // Output is in UAV state before and after this DIRECT or COMPUTE recording.
        // Retain the returned owner until queue completion or discard of the unsubmitted list.
        [[nodiscard]] std::shared_ptr<const void> Record(GDX12CommandList& commands, const CanvasFrame& frame,
            GDX12ResourceHandle output) const;

    private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };
}
