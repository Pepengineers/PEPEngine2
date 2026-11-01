#pragma once
#include "Engine.RendererDX12/GDX12DeviceResources.h"
class GDX12CommandList;

class GDX12GpuRadixSort
{
public:
    GDX12GpuRadixSort(GDX12DeviceResources& resources,UINT capacity);
    ~GDX12GpuRadixSort();
    GDX12UploadBuffer<UINT>& Keys() { return *_keys[0]; }
    GDX12UploadBuffer<UINT>& Payloads() { return *_payloads[0]; }
    void Execute(GDX12CommandList* commands,GDX12UploadBuffer<UINT>& count,UINT countIndex=0);
private:
    GDX12DeviceResources& _resources;
    UINT _capacity;
    std::array<std::unique_ptr<GDX12UploadBuffer<UINT>>,2> _keys,_payloads;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _sum,_reduced,_constants,_arguments;
    std::unique_ptr<GDX12RootSignature> _setupRoot,_sortRoot;
    std::array<ComPtr<ID3D12PipelineState>,6> _pipelines;
    ComPtr<ID3D12CommandSignature> _dispatch;
};
