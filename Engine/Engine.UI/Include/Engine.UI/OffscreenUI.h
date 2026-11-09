#pragma once

#include "Engine.RendererDX12/GDX12ResourceHandle.h"
#include <cstdint>
#include <memory>
#include <vector>

struct ImDrawData;
class GDX12CommandList;
class GDX12Device;

namespace Engine::UI
{
    enum class UITextureId : std::uint64_t { Invalid = 0 };
    class DrawData;

    // Called on the ImGui owner thread while the source draw data and atlas are live.
    std::shared_ptr<const DrawData> CaptureDrawData(const ImDrawData& source);
    std::shared_ptr<const DrawData> CaptureScreenFrame();

    inline constexpr DXGI_FORMAT OffscreenFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    struct TextureBinding
    {
        UITextureId Id = UITextureId::Invalid;
        GDX12ResourceHandle Resource;
        DXGI_FORMAT ViewFormat = OffscreenFormat;
    };

    // Use and retire on the existing ImGui owner thread. Each renderer owns a private backend context.
    class OffscreenRenderer
    {
    public:
        OffscreenRenderer(GDX12Device& device, unsigned framesInFlight);
        ~OffscreenRenderer();
        OffscreenRenderer(const OffscreenRenderer&) = delete;
        OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

        // Target is in RENDER_TARGET; bindings are in PIXEL_SHADER_RESOURCE. States stay unchanged.
        // The cleared output is linear premultiplied RGBA8. Input vertex/texture RGB is linear and alpha is straight.
        // Empty return means the next backend slot is busy and no commands were recorded.
        // Retain the returned owner until queue completion, or until the unsubmitted list is discarded.
        [[nodiscard]] std::shared_ptr<const void> TryRecord(GDX12CommandList& commands, GDX12ResourceHandle target,
            std::shared_ptr<const DrawData> frame, const std::vector<TextureBinding>& textures = {});

    private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };
}
