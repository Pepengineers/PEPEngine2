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
#include "Engine.RendererDX12/IRenderPassLink.h"

class GameTimer;

class RenderPipelineCommonData
{
public:
    UINT ActiveCameraCBufferIndex = -1;
    float ActiveCameraFOV = 0;
    float ActiveCameraNearPlane = 0;
    float ActiveCameraFarPlane = 0;
    GameTimer* GameTimer = nullptr;
    UINT WindowWidth = 0;
    UINT WindowHeight = 0;
    UINT DownscaledWidth = 0;
    UINT DownscaledHeight = 0;
};

enum ERenderPassFlags : uint32_t
{
    RENDER_PASS_FLAG_NONE = 0,
    RENDER_PASS_FLAG_USE_GEOMETRY = 1 << 0,
    RENDER_PASS_FLAG_USE_MATERIALS = 1 << 1,
    RENDER_PASS_FLAG_USE_CAMERAS = 1 << 2,
    RENDER_PASS_FLAG_USE_LIGHTING = 1 << 3,
    RENDER_PASS_FLAG_USE_INSTANCES = 1 << 4,
    RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION = 1 << 5,
    RENDER_PASS_FLAG_UPSCALER = 1 << 6
};

class GDX12RenderPass
{
public:
    GDX12RenderPass() : _flags(RENDER_PASS_FLAG_NONE), _resources(nullptr), _commonData(nullptr) {}
    virtual void Initialize(GDX12DeviceResources* resources, RenderPipelineCommonData* commonData) 
    {
        _resources = resources;
        _commonData = commonData;
    };
    virtual void Resize() {};
    virtual void Execute(GDX12CommandList* cmdList) {};
    // Used by upscalers to determine downscaled render target size
    // And share it with all other passes
    virtual void QueryRenderTargetResolution() {};

    uint32_t GetFlags() { return _flags; }
    void SetFlag(uint32_t flag, bool value)
    {
        if (value) { _flags |= flag; }
        else { _flags &= ~flag; }
    }
    bool GetFlagValue(uint32_t flag) { return _flags & flag; }

protected:
    // This will define which resources will be loaded onto respective GPU
    // For example: if no RenderPasses has USE_TEXTURES, then GPU texture upload
    // will be skipped entirely for this GPU
    uint32_t _flags;
    RenderPipelineCommonData* _commonData;
    GDX12DeviceResources* _resources;
};