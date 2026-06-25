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
    std::unique_ptr<GDX12GeometryBuffer> GeometryBuffer;

    // All heaps created in one high-capacity instance
    std::unique_ptr<GDX12DescriptorHeap> RTVHeap;
    std::unique_ptr<GDX12DescriptorHeap> SRV_UAV_Heap;
    std::unique_ptr<GDX12DescriptorHeap> DSVHeap;

    std::vector<std::unique_ptr<GDX12FrameConstants>> FrameConstants;
    UINT CurrFrameConstantsIndex;

    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> IndirectCommandsCache;

    std::unordered_map<std::string, ComPtr<ID3DBlob>> Shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> PSOs;
    std::unordered_map<std::string, std::unique_ptr<GDX12RootSignature>> RootSignatures;
    std::unordered_map <std::string, ComPtr<ID3D12CommandSignature>> CommandSignatures;
    std::unordered_map<std::string, std::unique_ptr<GDX12Texture>> Textures;

    GDX12Device* Device;
    void Initialize(GDX12Device* device);
    void UpdateMainCB(UINT width, UINT height, GameTimer* timer);
    void UpdateMaterialCB(std::unordered_map<std::string, std::unique_ptr<GDX12Material>>& materials);
};