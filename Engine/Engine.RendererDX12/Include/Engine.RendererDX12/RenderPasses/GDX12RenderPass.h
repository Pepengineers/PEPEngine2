#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"

enum ERenderPassFlags : uint32_t
{
    RENDER_PASS_FLAG_NONE = 0,
    RENDER_PASS_FLAG_USE_TEXTURES = 1 << 0,
    RENDER_PASS_FLAG_USE_MATERIALS = 1 << 1,
    RENDER_PASS_FLAG_USE_CAMERAS = 1 << 2,
    RENDER_PASS_FLAG_USE_LIGHTING = 1 << 3,
    RENDER_PASS_FLAG_PRESENTING = 1 << 4
};

class GDX12RenderPass
{
public:
    GDX12RenderPass() : _flags(RENDER_PASS_FLAG_NONE) {}

    virtual void Initialize(GDX12Device* device) {};
    virtual void Execute(GDX12CommandList* cmdList) {};
    virtual void Resize(UINT width, UINT height) {};

    uint32_t GetFlags() { return _flags; }

private:
    // This will define which resources will be loaded onto respective GPU
    // For example: if no RenderPasses has USE_TEXTURES, then GPU texture upload
    // will be skipped entirely for this GPU
    uint32_t _flags;
};