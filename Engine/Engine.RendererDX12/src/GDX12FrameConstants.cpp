#include "Engine.RendererDX12/GDX12FrameConstants.h"

#include "Engine.RendererDX12/GDX12Device.h"

GDX12FrameConstants::GDX12FrameConstants(GDX12Device* device)
{
    MainCB = std::make_unique<GDX12UploadBuffer<GDX12MainConstants>>(device, 1, EBufferType::Upload, true);
    TransformCache = std::make_unique<GDX12UploadBuffer<GDX12TransformConstants>>(device, 1, EBufferType::Upload, false);
    // Slot zero is the shared identity transform, including its previous matrix.
    TransformCache->CopyData(0, GDX12TransformConstants{});
    InstanceCache = std::make_unique<GDX12UploadBuffer<GDX12InstanceData>>(device, 0, EBufferType::Upload, false);
    MaterialCache = std::make_unique<GDX12UploadBuffer<GDX12MaterialConstants>>(device, 0, EBufferType::Upload, false);
    LightsCache = std::make_unique<GDX12UploadBuffer<GDX12LightConstants>>(device, 1, EBufferType::Upload, false);
    ClusterLightMaskBuffer = std::make_unique<GDX12UploadBuffer<UINT>>(device, 1, EBufferType::Upload, false);
    CameraCB = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(device, 0, EBufferType::Upload, true);
}

GDX12FrameConstants::~GDX12FrameConstants()
{
    MainCB.reset();
    TransformCache.reset();
    MaterialCache.reset();
    CameraCB.reset();
}

void GDX12FrameConstants::UploadLightingClusters(const ClusteredLightingGrid& grid)
{
    ClusteredLightingConstants = grid.Constants;
    const auto wordCount = static_cast<UINT>(grid.ClusterLightMasks.size());
    if (wordCount)
    {
        ClusterLightMaskBuffer->Resize(wordCount);
        ClusterLightMaskBuffer->CopyData(0, grid.ClusterLightMasks.data(), wordCount);
    }
}
