#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"

GDX12FrameConstants::GDX12FrameConstants(GDX12Device* device)
    : _device(device)
    , FenceValue(0)
{
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, true);
    TransformCache = std::make_unique<GDX12UploadBuffer<GDX12TransformConstants>>(device, 1, false);
    MaterialCache = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, 1, false);
    LightCB = std::make_unique<GDX12UploadBuffer<GDX12LightConstants>>(device, 1, true);
    CameraCB = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(device, 1, true);
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
