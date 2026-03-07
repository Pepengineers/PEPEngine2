#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12DescriptorHeap;

class GDX12Descriptor
{
public:
	GDX12Descriptor();
	~GDX12Descriptor();

	void InitAsSRV(ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc, std::shared_ptr<GDX12DescriptorHeap> inHeap);
	void InitAsCBV(D3D12_CONSTANT_BUFFER_VIEW_DESC* cbvDesc, std::shared_ptr<GDX12DescriptorHeap> inHeap);
	void InitAsUAV(ID3D12Resource* resource, D3D12_UNORDERED_ACCESS_VIEW_DESC* uavDesc, std::shared_ptr<GDX12DescriptorHeap> inHeap);
	void InitAsDSV(ID3D12Resource* resource, D3D12_DEPTH_STENCIL_VIEW_DESC* dsvDesc, std::shared_ptr<GDX12DescriptorHeap> inHeap);
	void InitAsRTV(ID3D12Resource* resource, D3D12_RENDER_TARGET_VIEW_DESC* dsvDesc, std::shared_ptr<GDX12DescriptorHeap> inHeap);

	UINT HeapIndex;
	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle;
	std::shared_ptr<GDX12DescriptorHeap> _heap;
};