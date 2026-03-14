#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"

class GDX12Device;

class GDX12FrameConstants
{
public:
    GDX12FrameConstants(GDX12Device* device, UINT meshCount, UINT materialCount, UINT lightCount);
    ~GDX12FrameConstants();

    bool IsInUseByGPU(UINT64 currentFence);

    //Always updating
    std::unique_ptr<GDX12UploadBuffer<GDX12MainConstants>> MainCB;

    //Dirty flag notify updating
    std::unique_ptr<GDX12UploadBuffer<GDX12MeshConstants>> MeshCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12MaterialConstants>> MaterialCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12LightConstants>> LightCB;
    UINT64 FenceValue;
    
private:
    GDX12Device* _device;
};