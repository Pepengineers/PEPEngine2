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

enum ERenderPassFlags : uint32_t
{
    RENDER_PASS_FLAG_NONE = 0,
    RENDER_PASS_FLAG_USE_GEOMETRY = 1 << 0,
    RENDER_PASS_FLAG_USE_MATERIALS = 1 << 1,
    RENDER_PASS_FLAG_USE_CAMERAS = 1 << 2,
    RENDER_PASS_FLAG_USE_LIGHTING = 1 << 3,
    RENDER_PASS_FLAG_USE_INSTANCES = 1 << 4,
    RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION = 1 << 5,
    RENDER_PASS_FLAG_UPSCALER = 1 << 6,
    RENDER_PASS_FLAG_SYNC_DEVICES = 1 << 7,
    RENDER_PASS_FLAG_USE_JITTER = 1 << 8,
    RENDER_PASS_FLAG_USE_STREAMLINE = 1 << 9
};

class RenderPipelineCommonData;

class GDX12RenderPass
{
public:
    GDX12RenderPass() : _flags(RENDER_PASS_FLAG_NONE), _resources(nullptr), 
        _otherResources(nullptr), _commonData(nullptr), _numInputs(0), _numOutputs(0) {}
    virtual ~GDX12RenderPass() = default;
    virtual void Setup(GDX12DeviceResources* initOnResources, GDX12DeviceResources* otherResources, RenderPipelineCommonData* commonData)
    {
        _resources = initOnResources;
        _otherResources = otherResources;
        _commonData = commonData;
    };
    virtual void Resize() {};
    virtual void Execute(GDX12CommandList* cmdList) {};
    // Used by upscalers to determine downscaled render target size
    // And share it with all other passes
    virtual void QueryRenderTargetResolution() {};

    virtual void ClearDenendencies() 
    {
        _inputs.clear();
        _outputs.clear();
    }
    // Called after linking with other passes
    virtual void Initialize() {}

    uint32_t GetFlags() { return _flags; }
    void SetFlag(uint32_t flag, bool value)
    {
        if (value) { _flags |= flag; }
        else { _flags &= ~flag; }
    }
    bool GetFlagValue(uint32_t flag) { return _flags & flag; }

    void SetInputs(std::initializer_list<IRenderPassLink*> inputs) { SetInputs(std::vector<IRenderPassLink*>(inputs)); }
    virtual void SetInputs(std::vector<IRenderPassLink*> inputs)
    {
        if (inputs.size() < _numInputs) { OutputDebugStringA("ERROR: Not enough inputs provided into render pass\n"); }
        _inputs = inputs;
    }
    // returns true if all inputs are initialized
    bool ValidateInputs()
    {
        for (auto* input : _inputs)
        {
            if (!input->IsInitialized()) { return false; }
        }
        return true;
    }
    std::vector<IRenderPassLink*>& GetOutputs() { return _outputs; }
    UINT GetNumInputs() { return _numInputs; }
    UINT GetNumOutputs() { return _numOutputs; }

protected:
    // This will define which resources will be loaded onto respective GPU
    // For example: if no RenderPasses has USE_TEXTURES, then GPU texture upload
    // will be skipped entirely for this GPU
    uint32_t _flags;
    RenderPipelineCommonData* _commonData;

    // These are the resources the pass was initialized on
    GDX12DeviceResources* _resources;
    // These are the other resources, that might be needed in mGPU passes
    // Might be null
    GDX12DeviceResources* _otherResources;

    std::vector<IRenderPassLink*> _inputs;
    std::vector<IRenderPassLink*> _outputs;
    UINT _numInputs;
    UINT _numOutputs;
};

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
    GDX12RenderPass* Upscaler = nullptr;
};