#include "Engine.RendererDX12/GDX12CommandList.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Texture.h"

#include <pix/pix3.h>

GDX12CommandList::GDX12CommandList(GDX12Device* device) : 
	FenceValue(0),
	_currentTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED),
	_currentScissorRect({}),
	_currentViewport({})
{
	ThrowIfFailed(device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_commandAllocator)));

	ThrowIfFailed(device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator.Get(),
			nullptr, IID_PPV_ARGS(&_commandList)));
}

GDX12CommandList::~GDX12CommandList()
{
	_commandList.Reset();
	_commandAllocator.Reset();
}

ComPtr<ID3D12GraphicsCommandList10> GDX12CommandList::GetCommandList()
{
	return _commandList;
}

ComPtr<ID3D12CommandAllocator> GDX12CommandList::GetCommandAllocator()
{
	return _commandAllocator;
}

void GDX12CommandList::Reset()
{
	_commandAllocator->Reset();
	_commandList->Reset(_commandAllocator.Get(), nullptr);

	_currentRootSignature = nullptr;
	_currentPSO = nullptr;
	_currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	for (auto& cachedDescriptorHeap : _currentDescriptorHeaps) cachedDescriptorHeap = nullptr;
	_currentViewport = {};
	_currentScissorRect = {};
}

void GDX12CommandList::SetPipelineState(ComPtr<ID3D12PipelineState> pso)
{
	if (pso == _currentPSO) { return; }
	_commandList->SetPipelineState(pso.Get());
	_currentPSO = pso;
}

void GDX12CommandList::SetPipelineState1(ComPtr<ID3D12StateObject> stateObject)
{
	_commandList->SetPipelineState1(stateObject.Get());
}

void GDX12CommandList::SetGraphicsRootSignature(std::shared_ptr<GDX12RootSignature> rootSignature)
{
	if (_currentRootSignature == rootSignature) { return; }
	_currentRootSignature = rootSignature;
	_commandList->SetGraphicsRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetComputeRootSignature(std::shared_ptr<GDX12RootSignature> rootSignature)
{
	if (_currentRootSignature == rootSignature) { return; }
	_currentRootSignature = rootSignature;
	_commandList->SetComputeRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (_currentTopology == topology) { return; }
	_currentTopology = topology;
	_commandList->IASetPrimitiveTopology(topology);
}

void GDX12CommandList::SetDescriptorHeaps(std::initializer_list<std::shared_ptr<GDX12DescriptorHeap>> heaps)
{
	std::vector<ID3D12DescriptorHeap*> rawHeaps;
	rawHeaps.reserve(heaps.size());

	for (auto& heap : heaps) { if (heap) { rawHeaps.push_back(heap->GetHeap().Get()); } }

	if (_currentDescriptorHeaps == rawHeaps) return;

	if (!rawHeaps.empty()) 
	{ 
		_currentDescriptorHeaps = rawHeaps;
		_commandList->SetDescriptorHeaps(static_cast<UINT>(rawHeaps.size()), rawHeaps.data()); 
	}
}

void GDX12CommandList::SetViewport(const D3D12_VIEWPORT& viewport)
{
	if (_currentViewport == viewport) return;

	_currentViewport = viewport;
	_commandList->RSSetViewports(1, &viewport);
}

void GDX12CommandList::SetScissorRect(const D3D12_RECT& scissorRect)
{
	if (_currentScissorRect.left == scissorRect.left &&
		_currentScissorRect.top == scissorRect.top &&
		_currentScissorRect.right == scissorRect.right &&
		_currentScissorRect.bottom == scissorRect.bottom)
	{
		return;
	}

	_currentScissorRect = scissorRect;

	_commandList->RSSetScissorRects(1, &scissorRect);
}

void GDX12CommandList::SetRenderTargets(std::initializer_list<std::shared_ptr<GDX12Texture>> rtvTextures,
	std::shared_ptr<GDX12Texture> dsvTexture)
{
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
	rtvHandles.reserve(rtvTextures.size());

	for (auto& texture : rtvTextures) { rtvHandles.push_back(texture->GetRTV()->CPUHandle); }

	if (rtvHandles.empty()) return;

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
	if (dsvTexture && dsvTexture->GetDSV()) { dsvHandle = dsvTexture->GetDSV()->CPUHandle; }

	_commandList->OMSetRenderTargets(rtvHandles.size(), rtvHandles.data(), FALSE, 
		(dsvHandle.ptr != 0) ? &dsvHandle : nullptr );
}

void GDX12CommandList::ClearRenderTargetView(std::shared_ptr<GDX12Texture> texture)
{
	_commandList->ClearRenderTargetView(texture->GetRTV()->CPUHandle, 
		texture->GetClearValue().Color, 0, nullptr);
}

void GDX12CommandList::ClearDepthStencilView(std::shared_ptr<GDX12Texture> texture)
{
	_commandList->ClearDepthStencilView(texture->GetDSV()->CPUHandle, 
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, texture->GetClearValue().DepthStencil.Depth, 0, 0, nullptr);
}

void GDX12CommandList::ClearUnorderedAccessViewFloat(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, ID3D12Resource* resource, const float values[4])
{
	_commandList->ClearUnorderedAccessViewFloat(gpuHandle, cpuHandle, resource, values, 0, nullptr);
}

void GDX12CommandList::ClearUnorderedAccessViewUINT(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, ID3D12Resource* resource, const UINT values[4])
{
	_commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, resource, values, 0, nullptr);
}

void GDX12CommandList::SetGraphicsRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	
	_commandList->SetGraphicsRootConstantBufferView(_currentRootSignature->GetBRootParamIndex(CregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetGraphicsRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	_commandList->SetGraphicsRootShaderResourceView(_currentRootSignature->GetTRootParamIndex(TregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetGraphicsRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	_commandList->SetGraphicsRootUnorderedAccessView(_currentRootSignature->GetURootParamIndex(UregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetGraphicsRootDescriptorTable(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	_commandList->SetGraphicsRootDescriptorTable(registerIndex, baseDescriptor);
}

void GDX12CommandList::SetComputeRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	_commandList->SetComputeRootConstantBufferView(_currentRootSignature->GetBRootParamIndex(CregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetComputeRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	_commandList->SetComputeRootShaderResourceView(_currentRootSignature->GetTRootParamIndex(TregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetComputeRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	_commandList->SetComputeRootUnorderedAccessView(_currentRootSignature->GetURootParamIndex(UregisterIndex)
		, bufferLocation);
}

void GDX12CommandList::SetComputeRootDescriptorTable(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	_commandList->SetComputeRootDescriptorTable(registerIndex, baseDescriptor);
}

void GDX12CommandList::DrawInstanced(UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
{
	_commandList->DrawInstanced(vertexCountPerInstance, instanceCount,
		startVertexLocation, startInstanceLocation);
}

void GDX12CommandList::DrawIndexedInstanced(UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
	_commandList->DrawIndexedInstanced(indexCountPerInstance, instanceCount,
		startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void GDX12CommandList::Dispatch(UINT threadGroupCountX, UINT threadGroupCountY, UINT threadGroupCountZ)
{
	_commandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void GDX12CommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc)
{
	_commandList->DispatchRays(pDesc);
}

void GDX12CommandList::BeginPixEvent(const std::string& name, const float color[4])
{
	BeginPixEvent(name, color);
}

void GDX12CommandList::EndPixEvent()
{
	PIXEndEvent(_commandList.Get());
}

void GDX12CommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc)
{
	_commandList->BuildRaytracingAccelerationStructure(pDesc, 0, nullptr);
}


