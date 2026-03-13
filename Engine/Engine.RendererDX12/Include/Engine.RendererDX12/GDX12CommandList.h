#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Device;
class GDX12RootSignature;
class GDX12DescriptorHeap;
class GDX12Texture;

class GDX12CommandList
{
public:
	GDX12CommandList(GDX12Device* device);
	~GDX12CommandList();

	ComPtr<ID3D12GraphicsCommandList10> GetCommandList();
	ComPtr<ID3D12CommandAllocator> GetCommandAllocator();
	void Reset();

	UINT64 FenceValue;

	// State Management
	void SetPipelineState(ComPtr<ID3D12PipelineState> pso);
	void SetPipelineState1(ComPtr<ID3D12StateObject> stateObject);
	void SetGraphicsRootSignature(std::shared_ptr<GDX12RootSignature> rootSignature);
	void SetComputeRootSignature(std::shared_ptr<GDX12RootSignature> rootSignature);
	void SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology);
	void SetDescriptorHeaps(std::initializer_list<std::shared_ptr<GDX12DescriptorHeap>> heaps);
	void SetViewport(const D3D12_VIEWPORT& viewport);
	void SetScissorRect(const D3D12_RECT& scissorRect);

    // View management
	void SetRenderTargets(std::initializer_list<std::shared_ptr<GDX12Texture>> rtvTextures,
		std::shared_ptr<GDX12Texture> dsvTexture);
    void ClearRenderTargetView(std::shared_ptr<GDX12Texture> texture);
    void ClearDepthStencilView(std::shared_ptr<GDX12Texture> texture);
    void ClearUnorderedAccessViewFloat(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle,
        const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle,
        ID3D12Resource* resource, const float values[4]);
	void ClearUnorderedAccessViewUINT(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle,
		const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle,
		ID3D12Resource* resource, const UINT values[4]);

    // Root Parameter Management
    void SetGraphicsRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetGraphicsRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetGraphicsRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);

	//GDX12RootSignature stores SRVs & UAVs via single-slot desc tables
	//Use this to bind GDX12Texture SRVs & UAVs
	void SetGraphicsRootDescriptorTable(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);

    void SetComputeRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetComputeRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetComputeRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);

	//GDX12RootSignature stores SRVs & UAVs via single-slot desc tables
	//Use this to bind GDX12Texture SRVs & UAVs
	void SetComputeRootDescriptorTable(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);

	//Draw Calls
	void DrawInstanced(UINT vertexCountPerInstance, UINT instanceCount,
		UINT startVertexLocation, UINT startInstanceLocation);
	void DrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount,
		UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation);
	void Dispatch(UINT threadGroupCountX, UINT threadGroupCountY, UINT threadGroupCountZ);
	void DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc);


	//Misc
	void BeginPixEvent(const std::string& name, const float color[4]);
	void EndPixEvent();

	void BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc);


private:
	ComPtr<ID3D12GraphicsCommandList10> _commandList;
	ComPtr<ID3D12CommandAllocator> _commandAllocator;

	//Cached states
	std::shared_ptr<GDX12RootSignature> _currentRootSignature;
	ComPtr<ID3D12PipelineState> _currentPSO;
	D3D_PRIMITIVE_TOPOLOGY _currentTopology;
	std::vector<ID3D12DescriptorHeap*> _currentDescriptorHeaps;
};
