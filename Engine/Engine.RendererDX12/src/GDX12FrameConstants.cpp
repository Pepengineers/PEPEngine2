#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandList.h"

GDX12FrameConstants::GDX12FrameConstants(GDX12Device* device)
    : _device(device)
    , FenceValue(0)
{
    ReflectionProbesUpload = std::make_unique<GDX12UploadBuffer<GDX12ReflectionProbeData>>(device, MaxReflectionProbes, EBufferType::Upload, false);
    ReflectionProbes = std::make_unique<GDX12UploadBuffer<GDX12ReflectionProbeData>>(device, MaxReflectionProbes, EBufferType::Default, false);
    for (UINT i = 0; i < MaxReflectionProbes; ++i) ReflectionProbesUpload->CopyData(i, GDX12ReflectionProbeData{});
    ReflectionProbesUpload->TrackChanges();
    SkyCB = std::make_unique<GDX12UploadBuffer<GDX12SkyConstants>>(device, 2);
    SkyCB->CopyData(0, GDX12SkyConstants{});
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, EBufferType::Upload, true);
    ShadowCB = std::make_unique<GDX12UploadBuffer<GDX12ShadowConstants>>(device, 1, EBufferType::Upload, true);
    DisplayShadowCB = std::make_unique<GDX12UploadBuffer<GDX12ShadowConstants>>(device, 1, EBufferType::Upload, true);
    DisplayShadowLights = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(device, 1, EBufferType::Upload, false);
    LocalLightCache = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(device, 1, EBufferType::Upload, false);
    TransformCache = std::make_unique<GDX12UploadBuffer<GDX12TransformConstants>>(device, 0, EBufferType::Upload, false);
    InstanceCache = std::make_unique<GDX12UploadBuffer<GDX12InstanceData>>(device, 0, EBufferType::Upload, false);
    TransformCache->TrackChanges();
    InstanceCache->TrackChanges();
    MaterialCache = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, 0, EBufferType::Upload, false);
    LightCB = std::make_unique<GDX12UploadBuffer<GDX12LightConstants>>(device, 0, EBufferType::Upload, true);
    CameraCB = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(device, 0, EBufferType::Upload, true);
}

void GDX12FrameConstants::UploadReflectionProbeData(GDX12CommandList* commands)
{
    const auto revision = ReflectionProbesUpload->GetRevision();
    if (ReflectionProbeRevision == revision) return;
    commands->ResourceBarrier({ReflectionProbes->GetResource().GetCopyDestBarrier()});
    commands->GetCommandList()->CopyBufferRegion(ReflectionProbes->GetResource().D3DResource.Get(), 0,
        ReflectionProbesUpload->GetResource().D3DResource.Get(), 0, UINT64(MaxReflectionProbes) * sizeof(GDX12ReflectionProbeData));
    commands->ResourceBarrier({ReflectionProbes->GetResource().GetSRVBarrier()});
    ReflectionProbeRevision = revision;
}

GDX12FrameConstants::~GDX12FrameConstants()
{
    MainCB.reset();
    TransformCache.reset();
    MaterialCache.reset();
    LightCB.reset();
    CameraCB.reset();
}

bool GDX12FrameConstants::IsInUseByGPU(const UINT64 currentFence)
{
    return currentFence < FenceValue;
}
