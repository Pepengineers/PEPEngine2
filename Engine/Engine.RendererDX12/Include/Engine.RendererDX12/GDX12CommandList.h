#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
#include <pix/pix3.h>
#define GDX12_SCOPED_CPU_EVENT(name) PIXScopedEvent(0, name)
#else
#define GDX12_SCOPED_CPU_EVENT(name) ((void)0)
#endif

class GDX12Device;
class GDX12RootSignature;
class GDX12DescriptorHeap;
class GDX12Texture;
class GDX12GeometryBuffer;

class GDX12CommandList
{
public:
	GDX12CommandList(GDX12Device* device);
	~GDX12CommandList();

	const ComPtr<ID3D12GraphicsCommandList10>& GetCommandList();
	const ComPtr<ID3D12CommandAllocator>& GetCommandAllocator();
	void Reset();
    void InvalidateCachedState();

	UINT64 FenceValue;

	// State Management
	void SetPipelineState(ComPtr<ID3D12PipelineState> pso);
	void SetPipelineState1(ComPtr<ID3D12StateObject> stateObject);
	void SetGraphicsRootSignature(GDX12RootSignature* rootSignature);
	void SetComputeRootSignature(GDX12RootSignature* rootSignature);
	void SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology);
	void SetDescriptorHeaps(std::initializer_list<GDX12DescriptorHeap*> heaps);
	void SetViewport(const D3D12_VIEWPORT& viewport);
	void SetScissorRect(const D3D12_RECT& scissorRect);
	void SetGeometryBuffer(GDX12GeometryBuffer* buffer);

    // View management
	void SetRenderTargets(std::initializer_list<GDX12Texture*> rtvTextures,
		GDX12Texture* dsvTexture);
    void ClearRenderTargetView(GDX12Texture* texture);
    void ClearDepthStencilView(GDX12Texture* texture);
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
	
	void SetTextureAsSRV(UINT registerIndex, GDX12Texture* texture);
	void SetGraphicsSRV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle);

    void SetComputeRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetComputeRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);
    void SetComputeRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation);

	void SetComputeSRV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle);
	void SetComputeUAV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle);

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
	void ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
		ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer,
		UINT64 CountBufferOffset = 0);

	//Resource Barriers
	void ResourceBarrier(std::initializer_list<CD3DX12_RESOURCE_BARRIER> barriers);
	void EnhancedTextureBarrier(std::initializer_list<D3D12_TEXTURE_BARRIER> textureBarriers);


#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
    class ScopedEvent
    {
    public:
        ScopedEvent(GDX12CommandList* commands, const char* name, XMVECTOR color = Colors::White);
        ~ScopedEvent() noexcept;
        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;
    private:
        ID3D12GraphicsCommandList* _commands;
    };
#endif
	//Misc
	void CopyResource(ID3D12Resource* destResource, ID3D12Resource* sourceResource);
	void BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc);


private:
	ComPtr<ID3D12GraphicsCommandList10> _commandList;
	ComPtr<ID3D12CommandAllocator> _commandAllocator;

	//Cached states
	GDX12RootSignature* _currentGraphicsRootSignature = nullptr;
	GDX12RootSignature* _currentComputeRootSignature = nullptr;
	ComPtr<ID3D12PipelineState> _currentPSO;
	D3D_PRIMITIVE_TOPOLOGY _currentTopology;
	std::vector<ID3D12DescriptorHeap*> _currentDescriptorHeaps;
	D3D12_VIEWPORT _currentViewport;
	D3D12_RECT _currentScissorRect;
	GDX12GeometryBuffer* _currentGeometryBuffer;
};

#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
#define GDX12_EVENT_JOIN_IMPL(a, b) a##b
#define GDX12_EVENT_JOIN(a, b) GDX12_EVENT_JOIN_IMPL(a, b)
#define GDX12_SCOPED_EVENT(...) const GDX12CommandList::ScopedEvent GDX12_EVENT_JOIN(gdx12Event, __LINE__)(__VA_ARGS__)
#else
// Do not evaluate marker names, colors or command-list expressions in Release.
#define GDX12_SCOPED_EVENT(...) ((void)0)
#endif
