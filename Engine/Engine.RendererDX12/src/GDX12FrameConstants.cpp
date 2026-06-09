#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"

GDX12FrameConstants::GDX12FrameConstants(GDX12Device* device)
    : _device(device)
    , FenceValue(0)
{
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, EBufferType::Upload, true);
    TransformCache = std::make_unique<GDX12UploadBuffer<GDX12TransformConstants>>(device, 0, EBufferType::Upload, false);
    InstanceCache = std::make_unique<GDX12UploadBuffer<GDX12InstanceData>>(device, 0, EBufferType::Upload, false);
    MaterialCache = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, 0, EBufferType::Upload, false);
    LightCB = std::make_unique<GDX12UploadBuffer<GDX12LightConstants>>(device, 0, EBufferType::Upload, true);
    CameraCB = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(device, 0, EBufferType::Upload, true);
    VisibleCommandsCache = std::make_unique<GDX12AppendBuffer<GDX12IndirectDrawArgs>>(device, 10000);
    DrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(device, 1, EBufferType::Default, false);
}

GDX12FrameConstants::~GDX12FrameConstants()
{
    MainCB.reset();
    TransformCache.reset();
    MaterialCache.reset();
    LightCB.reset();
    CameraCB.reset();
}

bool GDX12FrameConstants::IsInUseByGPU(UINT64 currentFence)
{
    return currentFence < FenceValue;
}
