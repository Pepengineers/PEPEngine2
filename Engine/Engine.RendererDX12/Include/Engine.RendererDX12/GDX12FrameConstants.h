#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/ClusteredLightingGrid.h"

class GDX12Device;

class GDX12FrameConstants
{
public:
    GDX12FrameConstants(GDX12Device* device);
    void UploadLightingClusters(const ClusteredLightingGrid& grid);
    ~GDX12FrameConstants();

    //Always updating
    std::unique_ptr<GDX12UploadBuffer<GDX12MainConstants>> MainCB;

    //Dirty flag notify updating
    std::unique_ptr<GDX12UploadBuffer<GDX12TransformConstants>> TransformCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12MaterialConstants>> MaterialCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12InstanceData>> InstanceCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> CameraCB;

    GDX12LightGridConstants ClusteredLightingConstants = {};
    std::unique_ptr<GDX12UploadBuffer<GDX12LightConstants>> LightsCache;
    std::unique_ptr<GDX12UploadBuffer<UINT>> ClusterLightMaskBuffer;
};
