#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"

GDX12FrameConstants::GDX12FrameConstants(GDX12Device* device, UINT meshCount,
    UINT materialCount, UINT lightCount)
    : _device(device)
    , FenceValue(0)
{
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, true);
    TransformCB = std::make_unique<GDX12UploadBuffer<GDX12TransformConstants>>(device, meshCount, true);
    MaterialCB = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, materialCount, true);
    LightCB = std::make_unique<GDX12UploadBuffer<GDX12LightConstants>>(device, lightCount, false);
}

GDX12FrameConstants::~GDX12FrameConstants()
{
    MainCB.reset();
    TransformCB.reset();
    MaterialCB.reset();
    LightCB.reset();
}

bool GDX12FrameConstants::IsInUseByGPU(UINT64 currentFence)
{
    return currentFence < FenceValue;
}
