#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"

class GDX12Device;

class GDX12FrameConstants
{
public:
    GDX12FrameConstants(std::shared_ptr<GDX12Device> device, UINT meshCount, UINT materialCount);
    ~GDX12FrameConstants();

    bool IsInUseByGPU(UINT64 currentFence);

    std::unique_ptr<GDX12UploadBuffer<GDX12MainConstants>> MainCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12MeshConstants>> MeshCB;
    std::unique_ptr<GDX12UploadBuffer<GDX12MaterialConstants>> MaterialCB;
    UINT64 FenceValue;
    
private:
    std::shared_ptr<GDX12Device> _device;
};