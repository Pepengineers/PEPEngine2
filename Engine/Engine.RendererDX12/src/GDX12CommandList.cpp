#include "Engine.RendererDX12/GDX12CommandList.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

#include <pix/pix3.h>

GDX12CommandList::GDX12CommandList(GDX12Device* device) :
	FenceValue(0),
	_currentTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED),
	_currentScissorRect({}),
	_currentViewport({}),
	_currentGeometryBuffer(nullptr)
{
	device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_commandAllocator));

	device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _commandAllocator.Get(),
			nullptr, IID_PPV_ARGS(&_commandList));
}

GDX12CommandList::~GDX12CommandList()
{
	_commandList.Reset();
	_commandAllocator.Reset();
}

const ComPtr<ID3D12GraphicsCommandList10>& GDX12CommandList::GetCommandList()
{
	return _commandList;
}

const ComPtr<ID3D12CommandAllocator>& GDX12CommandList::GetCommandAllocator()
{
	return _commandAllocator;
}

void GDX12CommandList::Reset()
{
	FenceValue = 0;
	_commandAllocator->Reset();
	_commandList->Reset(_commandAllocator.Get(), nullptr);

	InvalidateCachedState();
}

void GDX12CommandList::InvalidateCachedState()
{
	_currentGraphicsRootSignature = nullptr;
	_currentComputeRootSignature = nullptr;
	_currentPSO = nullptr;
	_currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	for (auto& cachedDescriptorHeap : _currentDescriptorHeaps) { cachedDescriptorHeap = nullptr; }
	_currentViewport = {};
	_currentScissorRect = {};
	_currentGeometryBuffer = nullptr;
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

void GDX12CommandList::SetGraphicsRootSignature(GDX12RootSignature* rootSignature)
{
	if (_currentGraphicsRootSignature == rootSignature) { return; }
	_currentGraphicsRootSignature = rootSignature;
	_commandList->SetGraphicsRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetComputeRootSignature(GDX12RootSignature* rootSignature)
{
	if (_currentComputeRootSignature == rootSignature) { return; }
	_currentComputeRootSignature = rootSignature;
	_commandList->SetComputeRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (_currentTopology == topology) { return; }
	_currentTopology = topology;
	_commandList->IASetPrimitiveTopology(topology);
}

void GDX12CommandList::SetDescriptorHeaps(std::initializer_list<GDX12DescriptorHeap*> heaps)
{
	std::vector<ID3D12DescriptorHeap*> rawHeaps;
	rawHeaps.reserve(heaps.size());

	for (auto& heap : heaps) { rawHeaps.push_back(heap->GetHeap().Get()); }

	if (_currentDescriptorHeaps == rawHeaps) { return; }

	if (!rawHeaps.empty())
	{
		_currentDescriptorHeaps = rawHeaps;
		_commandList->SetDescriptorHeaps(static_cast<UINT>(rawHeaps.size()), rawHeaps.data());
	}
}

void GDX12CommandList::SetViewport(const D3D12_VIEWPORT& viewport)
{
	if (_currentViewport == viewport) { return; }

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

void GDX12CommandList::SetGeometryBuffer(GDX12GeometryBuffer* buffer)
{
	if (_currentGeometryBuffer == buffer) { return; }

	_currentGeometryBuffer = buffer;
	_commandList->IASetVertexBuffers(0, 1, &buffer->_vertexBufferView);
	_commandList->IASetIndexBuffer(&buffer->_indexBufferView);
}

void GDX12CommandList::SetRenderTargets(std::initializer_list<GDX12Texture*> rtvTextures,
	GDX12Texture* dsvTexture)
{
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
	rtvHandles.reserve(rtvTextures.size());

	for (auto& texture : rtvTextures) { rtvHandles.push_back(texture->GetRTV()->CPUHandle); }

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
	if (dsvTexture && dsvTexture->GetDSV()) { dsvHandle = dsvTexture->GetDSV()->CPUHandle; }

	_commandList->OMSetRenderTargets(rtvHandles.size(), rtvHandles.data(), FALSE,
		(dsvHandle.ptr != 0) ? &dsvHandle : nullptr );
}

void GDX12CommandList::ClearRenderTargetView(GDX12Texture* texture)
{
	_commandList->ClearRenderTargetView(texture->GetRTV()->CPUHandle,
		texture->GetClearValue().Color, 0, nullptr);
}

void GDX12CommandList::ClearDepthStencilView(GDX12Texture* texture)
{
	_commandList->ClearDepthStencilView(texture->GetDSV()->CPUHandle,
		(texture->GetClearValue().Format == DXGI_FORMAT_D24_UNORM_S8_UINT || texture->GetClearValue().Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH), texture->GetClearValue().DepthStencil.Depth, texture->GetClearValue().DepthStencil.Stencil, 0, nullptr);
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
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootConstantBufferView(_currentGraphicsRootSignature->GetBRootParamIndex(CregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootShaderResourceView(_currentGraphicsRootSignature->GetTRootParamIndex(TregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootUnorderedAccessView(_currentGraphicsRootSignature->GetURootParamIndex(UregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootDescriptorTable(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootDescriptorTable(registerIndex, baseDescriptor);
}

void GDX12CommandList::SetTextureAsSRV(UINT registerIndex, GDX12Texture* texture)
{
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }

	_commandList->SetGraphicsRootDescriptorTable(_currentGraphicsRootSignature->GetTRootParamIndex(registerIndex),
		texture->GetSRV()->GPUHandle);
}

void GDX12CommandList::SetGraphicsSRV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
	_commandList->SetGraphicsRootDescriptorTable(_currentGraphicsRootSignature->GetTRootParamIndex(registerIndex),
		GPUHandle);
}

void GDX12CommandList::SetComputeRootConstantBufferView(UINT CregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootConstantBufferView(_currentComputeRootSignature->GetBRootParamIndex(CregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeRootShaderResourceView(UINT TregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootShaderResourceView(_currentComputeRootSignature->GetTRootParamIndex(TregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeRootUnorderedAccessView(UINT UregisterIndex, D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootUnorderedAccessView(_currentComputeRootSignature->GetURootParamIndex(UregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeSRV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
	_commandList->SetComputeRootDescriptorTable(_currentComputeRootSignature->GetTRootParamIndex(registerIndex),
		GPUHandle);
}

void GDX12CommandList::SetComputeUAV(UINT registerIndex, D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
	_commandList->SetComputeRootDescriptorTable(_currentComputeRootSignature->GetURootParamIndex(registerIndex),
		GPUHandle);
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

void GDX12CommandList::ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset)
{
	_commandList->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
}

void GDX12CommandList::ResourceBarrier(std::initializer_list<CD3DX12_RESOURCE_BARRIER> barriers)
{
	std::vector<CD3DX12_RESOURCE_BARRIER> filteredBarriers;
	for (auto& barrier : barriers)
	{
		//skip transition with equal before and after states
		const D3D12_RESOURCE_BARRIER& baseBarrier = barrier;
		if (baseBarrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
			baseBarrier.Transition.StateBefore != baseBarrier.Transition.StateAfter)
		{
			filteredBarriers.push_back(barrier);
		}
	}
	if (!filteredBarriers.empty()) { _commandList->ResourceBarrier(filteredBarriers.size(), filteredBarriers.data()); }
}

void GDX12CommandList::EnhancedTextureBarrier(std::initializer_list<D3D12_TEXTURE_BARRIER> textureBarriers)
{
	std::vector<D3D12_BARRIER_GROUP> barrierGroups;
	barrierGroups.reserve(1);

	D3D12_BARRIER_GROUP barrierGroup = {};
	barrierGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
	barrierGroup.NumBarriers = static_cast<UINT>(textureBarriers.size());
	barrierGroup.pTextureBarriers = textureBarriers.begin();
	barrierGroups.push_back(barrierGroup);

	_commandList->Barrier(static_cast<UINT>(barrierGroups.size()), barrierGroups.data());
}

void GDX12CommandList::BeginPixEvent(const std::string& name, XMVECTOR color)
{
	UINT64 pixColor = (static_cast<UINT64>(XMVectorGetW(color) * 255.0f) << 24) |
		(static_cast<UINT64>(XMVectorGetZ(color) * 255.0f) << 16) |
		(static_cast<UINT64>(XMVectorGetY(color) * 255.0f) << 8) |
		(static_cast<UINT64>(XMVectorGetX(color) * 255.0f));

	PIXBeginEvent(_commandList.Get(), pixColor, name.c_str());
}

void GDX12CommandList::EndPixEvent()
{
	PIXEndEvent(_commandList.Get());
}

void GDX12CommandList::CopyResource(ID3D12Resource* destResource, ID3D12Resource* sourceResource)
{
	_commandList->CopyResource(destResource, sourceResource);
}

void GDX12CommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc)
{
	_commandList->BuildRaytracingAccelerationStructure(pDesc, 0, nullptr);
}


