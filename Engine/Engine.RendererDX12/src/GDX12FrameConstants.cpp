#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"

GDX12FrameConstants::GDX12FrameConstants(std::shared_ptr<GDX12Device> device, UINT meshCount,
    UINT materialCount)
    : _device(device)
    , FenceValue(0)
{
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, true);
    MeshCB = std::make_unique<GDX12UploadBuffer<GDX12MeshConstants>>(device, meshCount, true);
    MaterialCB = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, materialCount, false);
}

GDX12FrameConstants::~GDX12FrameConstants()
{
    MainCB.reset();
    MeshCB.reset();
    MaterialCB.reset();
}

bool GDX12FrameConstants::IsInUseByGPU(UINT64 currentFence)
{
    return FenceValue <= currentFence;
}
