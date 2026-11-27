#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12FrameConstants.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

class GDX12Device;
class GameTimer;

class GDX12DeviceResources
{
public:
    GDX12DeviceResources() : CurrFrameConstantsIndex(0), Device(nullptr) {}

    std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> InputLayouts;

    // All heaps created in one high-capacity instance
    std::unique_ptr<GDX12DescriptorHeap> RTVHeap;
    std::unique_ptr<GDX12DescriptorHeap> SRV_UAV_Heap;
    std::unique_ptr<GDX12DescriptorHeap> DSVHeap;

    std::unique_ptr<GDX12GeometryBuffer> GeometryBuffer;

    // Null SRVs for floating-point texture inputs, shared by passes on this device.
    // These descriptors allocate no texture memory and outlive pipeline rebuilds.
    struct NullDescriptors
    {
        GDX12Descriptor Texture2DSRV;
        GDX12Descriptor Texture2DArraySRV;
        GDX12Descriptor TextureCubeSRV;
    };
    NullDescriptors NullViews;

    std::vector<std::unique_ptr<GDX12FrameConstants>> FrameConstants;
    UINT CurrFrameConstantsIndex;

    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> IndirectCommandsCache;

    std::unordered_map<std::string, std::unique_ptr<GDX12Texture>> Textures;

    GDX12Device* Device;
    void Initialize(GDX12Device* device);
    // A null mesh disables the given instance slots. Waits for this adapter's last DIRECT queue read.
    void SetMeshDrawCommands(const std::vector<UINT>& instances, const GPUMesh* mesh);
    void UpdateMainCB(UINT width, UINT height, GameTimer* timer);
    void UpdateMaterialCB(const GDX12Material& material);

private:
    void InitNullDescriptors();
};
