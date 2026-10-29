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
#include "Engine.RendererDX12/FrameGraph/PassContext.h"

class GameTimer;

enum ERenderPassFlags : uint32_t
{
    RENDER_PASS_FLAG_NONE = 0,
    RENDER_PASS_FLAG_USE_GEOMETRY = 1 << 0,
    RENDER_PASS_FLAG_USE_MATERIALS = 1 << 1,
    RENDER_PASS_FLAG_USE_CAMERAS = 1 << 2,
    RENDER_PASS_FLAG_USE_LIGHTING = 1 << 3,
    RENDER_PASS_FLAG_USE_INSTANCES = 1 << 4,
    RENDER_PASS_FLAG_UPSCALER = 1 << 6,
    RENDER_PASS_FLAG_USE_JITTER = 1 << 8,
    RENDER_PASS_FLAG_USE_STREAMLINE = 1 << 9
};

class RenderPipelineCommonData;

class GDX12RenderPass
{
public:
    virtual ~GDX12RenderPass() = default;
    virtual void Describe(PassDescription&) const = 0;
    virtual void ResolveResourceRequirements(PassResourceRequirementsContext&) {}
    virtual void Initialize(PassInitContext&) {}
    virtual void Execute(PassExecutionContext&) = 0;
    virtual std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context);
    virtual void OnResourcesRecreated(PassInitContext&) {}
    uint32_t GetFlags() { return _flags; }
    void SetFlag(uint32_t flag, bool value)
    {
        if (value) { _flags |= flag; }
        else { _flags &= ~flag; }
    }
    bool GetFlagValue(uint32_t flag) { return _flags & flag; }

protected:
    // These flags control scene uploads and external resource tracking on this GPU.
    uint32_t _flags = RENDER_PASS_FLAG_NONE;

private:
    GDX12Resource _vertexResource;
    GDX12Resource _indexResource;
};

static_assert(sizeof(GDX12IndirectDrawArgs) == DrawStreamStride, "FrameGraph DrawStream ABI must match indirect draw arguments");

class RenderPipelineCommonData
{
public:
    UINT ActiveCameraCBufferIndex = -1;
    float ActiveCameraFOV = 0;
    float ActiveCameraNearPlane = 0;
    float ActiveCameraFarPlane = 0;
    float ActiveCameraJitterOffsetX = 0;
    float ActiveCameraJitterOffsetY = 0;
    Matrix CameraViewToClip = Identity4x4();
    Matrix ClipToCameraView = Identity4x4();
    Matrix ClipToPrevClip = Identity4x4();
    Matrix PrevClipToClip = Identity4x4();
    GameTimer* GameTimer = nullptr;
    UINT WindowWidth = 0;
    UINT WindowHeight = 0;
    UINT DownscaledWidth = 0;
    UINT DownscaledHeight = 0;
};
