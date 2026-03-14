#include "Engine.RendererDX12/GDX12Descriptor.h"

#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"

GDX12Descriptor::GDX12Descriptor() : 
    HeapIndex(-1), 
    CPUHandle{0}, 
    GPUHandle{0},
    _heap(nullptr)
{
}

GDX12Descriptor::~GDX12Descriptor()
{
}

void GDX12Descriptor::InitAsSRV(ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc, GDX12DescriptorHeap* inHeap)
{
    _heap = inHeap;
    
    if (_heap->GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        if (HeapIndex == -1) { HeapIndex = _heap->GetAvalibleIndex(); }
        CPUHandle = _heap->GetCPUHandle(HeapIndex);
        GPUHandle = _heap->GetGPUHandle(HeapIndex);

        _heap->_device->GetDevice()->CreateShaderResourceView(resource, srvDesc, CPUHandle);
    }
    else { OutputDebugStringA("ERROR: Cannot create SRV in a non-CBV_SRV_UAV heap\n"); }
}

void GDX12Descriptor::InitAsCBV(D3D12_CONSTANT_BUFFER_VIEW_DESC* cbvDesc, GDX12DescriptorHeap* inHeap)
{
    _heap = inHeap;

    if (_heap->GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        if (HeapIndex == -1) { HeapIndex = _heap->GetAvalibleIndex(); }
        CPUHandle = _heap->GetCPUHandle(HeapIndex);
        GPUHandle = _heap->GetGPUHandle(HeapIndex);

        _heap->_device->GetDevice()->CreateConstantBufferView(cbvDesc, CPUHandle);
    }
    else { OutputDebugStringA("ERROR: Cannot create CBV in a non-CBV_SRV_UAV heap\n"); }
}

void GDX12Descriptor::InitAsUAV(ID3D12Resource* resource, D3D12_UNORDERED_ACCESS_VIEW_DESC* uavDesc, GDX12DescriptorHeap* inHeap)
{
    _heap = inHeap;

    if (_heap->GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        if (HeapIndex == -1) { HeapIndex = _heap->GetAvalibleIndex(); }
        CPUHandle = _heap->GetCPUHandle(HeapIndex);
        if (_heap->GetFlags() & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) { GPUHandle = _heap->GetGPUHandle(HeapIndex); }

        _heap->_device->GetDevice()->CreateUnorderedAccessView(resource, nullptr, uavDesc, CPUHandle);
    }
    else { OutputDebugStringA("ERROR: Cannot create UAV in a non-CBV_SRV_UAV heap\n"); }
}

void GDX12Descriptor::InitAsDSV(ID3D12Resource* resource, D3D12_DEPTH_STENCIL_VIEW_DESC* dsvDesc, GDX12DescriptorHeap* inHeap)
{
    _heap = inHeap;

    if (_heap->GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
    {
        if (HeapIndex == -1) { HeapIndex = _heap->GetAvalibleIndex(); }
        CPUHandle = _heap->GetCPUHandle(HeapIndex);
        if (_heap->GetFlags() & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) { GPUHandle = _heap->GetGPUHandle(HeapIndex); }

        _heap->_device->GetDevice()->CreateDepthStencilView(resource, dsvDesc, CPUHandle);
    }
    else { OutputDebugStringA("ERROR: Cannot create DSV in a non-DSV heap\n"); }
}

void GDX12Descriptor::InitAsRTV(ID3D12Resource* resource, D3D12_RENDER_TARGET_VIEW_DESC* rtvDesc, GDX12DescriptorHeap* inHeap)
{
    _heap = inHeap;

    if (_heap->GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    {
        if (HeapIndex == -1) { HeapIndex = _heap->GetAvalibleIndex(); }
        CPUHandle = _heap->GetCPUHandle(HeapIndex);
        if (_heap->GetFlags() & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) { GPUHandle = _heap->GetGPUHandle(HeapIndex); }
        
        _heap->_device->GetDevice()->CreateRenderTargetView(resource, rtvDesc, CPUHandle);
    }
    else { OutputDebugStringA("ERROR: Cannot create RTV in a non-DSV heap\n"); }
}
