#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12DescriptorHeap;

enum GDX12DescriptorType { DESC_TYPE_NONE, DESC_TYPE_SRV, DESC_TYPE_CBV, DESC_TYPE_UAV, DESC_TYPE_DSV, DESC_TYPE_RTV };

class GDX12Descriptor
{
public:
	GDX12Descriptor();
	~GDX12Descriptor();

	void InitAsSRV(ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc, GDX12DescriptorHeap* inHeap, UINT heapIndex);
	void InitAsCBV(D3D12_CONSTANT_BUFFER_VIEW_DESC* cbvDesc, GDX12DescriptorHeap* inHeap, UINT heapIndex);
	void InitAsUAV(ID3D12Resource* resource, ID3D12Resource* counterResource, D3D12_UNORDERED_ACCESS_VIEW_DESC* uavDesc, GDX12DescriptorHeap* inHeap, UINT heapIndex);
	void InitAsDSV(ID3D12Resource* resource, D3D12_DEPTH_STENCIL_VIEW_DESC* dsvDesc, GDX12DescriptorHeap* inHeap, UINT heapIndex);
	void InitAsRTV(ID3D12Resource* resource, D3D12_RENDER_TARGET_VIEW_DESC* rtvDesc, GDX12DescriptorHeap* inHeap, UINT heapIndex);
	
	GDX12DescriptorType DescType;
	UINT HeapIndex;
	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle;
	GDX12DescriptorHeap* _heap;
};