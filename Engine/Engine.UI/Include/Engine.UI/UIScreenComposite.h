#pragma once

#include "Engine.RendererDX12/GDX12ResourceHandle.h"
#include <memory>

class GDX12CommandList;
class GDX12Device;

namespace Engine::UI
{
    class ScreenComposite
    {
    public:
        explicit ScreenComposite(GDX12Device& device);
        ~ScreenComposite();
        ScreenComposite(const ScreenComposite&) = delete;
        ScreenComposite& operator=(const ScreenComposite&) = delete;

        // Input is linear premultiplied RGBA8 in ALL_SHADER_RESOURCE. Target is a
        // local RGBA8 UNORM render target. The caller owns transitions and retirement.
        [[nodiscard]] std::shared_ptr<const void> Record(GDX12CommandList& commands,
            GDX12ResourceHandle input, GDX12ResourceHandle target) const;

    private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };
}
