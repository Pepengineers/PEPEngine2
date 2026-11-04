#pragma once
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12AppendBuffer.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"
#include "Engine.RendererDX12/GDX12Sky.h"

class GDX12Device;
class GDX12CommandList;

// Used for collecting & using GPU culling results
// This structured is used by any object that uses culling(cameras, shadowmaps)
struct GDX12VisibilityBuffers
{
    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> VisibleOpaqueCommandsCache;
    std::unique_ptr<GDX12UploadBuffer<UINT>> OpaqueDrawCounter;
    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> VisibleTransparentCommandsCache;
    std::unique_ptr<GDX12UploadBuffer<UINT>> TransparentDrawCounter;
    std::unique_ptr<GDX12InstanceBatcher> OpaqueBatches, TransparentBatches;
};

class GDX12FrameConstants
{
public:
    GDX12FrameConstants(GDX12Device* device);
    ~GDX12FrameConstants();

    bool IsInUseByGPU(UINT64 currentFence);
    void UploadReflectionProbeData(GDX12CommandList* commands);

    std::unique_ptr<GDX12UploadBuffer<GDX12SkyConstants>> SkyCB;

    //Always updating
    std::unique_ptr<GDX12UploadBuffer<GDX12MainConstants>> MainCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12ShadowConstants>> ShadowCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LocalLightCache;

    //Dirty flag notify updating
    std::unique_ptr<GDX12UploadBuffer<GDX12TransformConstants>> TransformCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12MaterialConstants>> MaterialCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12InstanceData>> InstanceCache;
    // DEFAULT scene snapshot shared by all geometry and lighting consumers before the render graph.
    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> ShadowTemplatesUpload, ShadowTemplates;
    std::unique_ptr<GDX12UploadBuffer<GDX12InstanceData>> ShadowInstances;
    std::unique_ptr<GDX12UploadBuffer<GDX12MaterialConstants>> ShadowMaterials;
    std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> ShadowLights;
    std::unique_ptr<GDX12UploadBuffer<GDX12TransformConstants>> ShadowTransforms;
    uint64_t ShadowTemplateRevision = 0;
    bool ShadowTemplatesDirty = false;
    std::unique_ptr<GDX12UploadBuffer<GDX12LightConstants>> LightCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> CameraCB;
    std::vector<GDX12VisibilityBuffers> CameraVisibilityCommands;

    std::unique_ptr<GDX12UploadBuffer<GDX12ReflectionProbeData>> ReflectionProbesUpload, ReflectionProbes;
    uint64_t ReflectionProbeRevision = 0;

    UINT64 FenceValue;
    
private:
    GDX12Device* _device;
};
