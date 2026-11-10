#include "Engine.RendererDX12/GDX12CommandList.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include <limits>
#include <algorithm>
#include <stdexcept>

#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
#include <pix/pix3.h>
#endif

GDX12CommandList::GDX12CommandList(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type) :
	FenceValue(0),
    _type(type),
	_currentTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED),
	_currentScissorRect({}),
	_currentViewport({}),
	_currentGeometryBuffer(nullptr)
{
    if (!device || !device->GetDevice()) throw std::invalid_argument("Command list requires a live device");
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        throw std::invalid_argument("Unsupported command list type");
	ThrowIfFailed(device->GetDevice()->CreateCommandAllocator(type, IID_PPV_ARGS(&_commandAllocator)));

	ThrowIfFailed(device->GetDevice()->CreateCommandList(0, type, _commandAllocator.Get(),
			nullptr, IID_PPV_ARGS(&_commandList)));
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
    if (_submitted && (!_retirementFence || !_retirement.IsReachedBy(
        { _retirement.Timeline(), _retirementFence->GetCompletedValue() })))
        throw std::logic_error("Cannot reset an allocator before its queue-issued retirement point");
	ThrowIfFailed(_commandAllocator->Reset());
	ThrowIfFailed(_commandList->Reset(_commandAllocator.Get(), nullptr));
    FenceValue = 0;
    _retirement = {};
    _retirementFence.Reset();
    _submitted = false;
    _legacyOwners.clear();
    InvalidateCachedState();
}

void GDX12CommandList::InvalidateCachedState()
{
	_currentGraphicsRootSignature = nullptr;
	_currentComputeRootSignature = nullptr;
	_currentPSO = nullptr;
	_currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    _currentDescriptorHeaps.clear();
	_currentViewport = {};
	_currentScissorRect = {};
	_currentGeometryBuffer = nullptr;
}

void GDX12CommandList::RequireDirect() const
{
    if (_type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        throw std::logic_error("Command requires a DIRECT list");
}

void GDX12CommandList::RequireCompute() const
{
    if (_type == D3D12_COMMAND_LIST_TYPE_COPY)
        throw std::logic_error("Command is not supported by a COPY list");
}

void GDX12CommandList::SetPipelineState(ComPtr<ID3D12PipelineState> pso)
{
    RequireCompute();
	if (pso == _currentPSO) { return; }
	_commandList->SetPipelineState(pso.Get());
	_currentPSO = pso;
}

void GDX12CommandList::SetPipelineState1(ComPtr<ID3D12StateObject> stateObject)
{
    RequireCompute();
	_commandList->SetPipelineState1(stateObject.Get());
    _currentPSO.Reset();
}

void GDX12CommandList::SetGraphicsRootSignature(GDX12RootSignature* rootSignature)
{
    RequireDirect();
	if (_currentGraphicsRootSignature == rootSignature) { return; }
	_currentGraphicsRootSignature = rootSignature;
	_commandList->SetGraphicsRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetComputeRootSignature(GDX12RootSignature* rootSignature)
{
    RequireCompute();
	if (_currentComputeRootSignature == rootSignature) { return; }
	_currentComputeRootSignature = rootSignature;
	_commandList->SetComputeRootSignature(rootSignature->GetRootSignature().Get());
}

void GDX12CommandList::SetPrimitiveTopology(const D3D_PRIMITIVE_TOPOLOGY topology)
{
    RequireDirect();
	if (_currentTopology == topology) { return; }
	_currentTopology = topology;
	_commandList->IASetPrimitiveTopology(topology);
}

void GDX12CommandList::SetDescriptorHeaps(const std::initializer_list<GDX12DescriptorHeap*> heaps)
{
    RequireCompute();
	if (_currentDescriptorHeaps.size() == heaps.size() &&
		std::equal(heaps.begin(), heaps.end(), _currentDescriptorHeaps.begin(),
			[](auto* heap, auto* current) { return heap->GetHeap().Get() == current; })) return;

	if (heaps.size())
	{
		_currentDescriptorHeaps.reserve(heaps.size());
		_currentDescriptorHeaps.clear();
		for (auto* heap : heaps) _currentDescriptorHeaps.push_back(heap->GetHeap().Get());
		_commandList->SetDescriptorHeaps(static_cast<UINT>(_currentDescriptorHeaps.size()), _currentDescriptorHeaps.data());
	}
}

void GDX12CommandList::SetViewport(const D3D12_VIEWPORT& viewport)
{
    RequireDirect();
	if (_currentViewport == viewport) { return; }

	_currentViewport = viewport;
	_commandList->RSSetViewports(1, &viewport);
}

void GDX12CommandList::SetScissorRect(const D3D12_RECT& scissorRect)
{
    RequireDirect();
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
    RequireDirect();
    if (buffer && buffer->_vertexBuffer &&
        buffer->_vertexBuffer->GetCurrentState() != GDX12GeometryBuffer::VertexReadState)
        ResourceBarrier({ buffer->_vertexBuffer->GetTransitionBarrier(GDX12GeometryBuffer::VertexReadState) });
    if (buffer && buffer->_indexBuffer && buffer->_indexBuffer->GetCurrentState() != D3D12_RESOURCE_STATE_INDEX_BUFFER)
        ResourceBarrier({ buffer->_indexBuffer->GetTransitionBarrier(D3D12_RESOURCE_STATE_INDEX_BUFFER) });
	if (_currentGeometryBuffer == buffer) { return; }

	_currentGeometryBuffer = buffer;
	_commandList->IASetVertexBuffers(0, 1, &buffer->_vertexBufferView);
	_commandList->IASetIndexBuffer(&buffer->_indexBufferView);
}

void GDX12CommandList::SetRenderTargets(const std::initializer_list<GDX12Texture*> rtvTextures,
	GDX12Texture* dsvTexture)
{
    RequireDirect();
	if (rtvTextures.size() > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
		throw std::out_of_range("Too many render targets");
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
	rtvHandles.reserve(rtvTextures.size());

	for (auto& texture : rtvTextures) { rtvHandles.push_back(texture->GetRTV()->CPUHandle); }

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
	if (dsvTexture && dsvTexture->GetDSV()) { dsvHandle = dsvTexture->GetDSV()->CPUHandle; }

	_commandList->OMSetRenderTargets(static_cast<UINT>(rtvHandles.size()), rtvHandles.data(), FALSE,
		(dsvHandle.ptr != 0) ? &dsvHandle : nullptr );
}

void GDX12CommandList::ClearRenderTargetView(GDX12Texture* texture)
{
    RequireDirect();
	_commandList->ClearRenderTargetView(texture->GetRTV()->CPUHandle,
		texture->GetClearValue().Color, 0, nullptr);
}

void GDX12CommandList::ClearDepthStencilView(GDX12Texture* texture)
{
    RequireDirect();
	_commandList->ClearDepthStencilView(texture->GetDSV()->CPUHandle,
		(texture->GetClearValue().Format == DXGI_FORMAT_D24_UNORM_S8_UINT || texture->GetClearValue().Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH), texture->GetClearValue().DepthStencil.Depth, texture->GetClearValue().DepthStencil.Stencil, 0, nullptr);
}

void GDX12CommandList::ClearUnorderedAccessViewFloat(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, ID3D12Resource* resource, const float values[4])
{
    RequireCompute();
	_commandList->ClearUnorderedAccessViewFloat(gpuHandle, cpuHandle, resource, values, 0, nullptr);
}

void GDX12CommandList::ClearUnorderedAccessViewUINT(const D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, ID3D12Resource* resource, const UINT values[4])
{
    RequireCompute();
	_commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, resource, values, 0, nullptr);
}

void GDX12CommandList::SetGraphicsRootConstantBufferView(const UINT CregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireDirect();
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootConstantBufferView(_currentGraphicsRootSignature->GetBRootParamIndex(CregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootShaderResourceView(const UINT TregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireDirect();
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootShaderResourceView(_currentGraphicsRootSignature->GetTRootParamIndex(TregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootUnorderedAccessView(const UINT UregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireDirect();
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootUnorderedAccessView(_currentGraphicsRootSignature->GetURootParamIndex(UregisterIndex), bufferLocation);
}

void GDX12CommandList::SetGraphicsRootDescriptorTable(const UINT registerIndex, const D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
    RequireDirect();
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetGraphicsRootDescriptorTable(registerIndex, baseDescriptor);
}

void GDX12CommandList::SetTextureAsSRV(const UINT registerIndex, GDX12Texture* texture)
{
    RequireDirect();
	if (!_currentGraphicsRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }

	_commandList->SetGraphicsRootDescriptorTable(_currentGraphicsRootSignature->GetTRootParamIndex(registerIndex),
		texture->GetSRV()->GPUHandle);
}

void GDX12CommandList::SetGraphicsSRV(const UINT registerIndex, const D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
    RequireDirect();
	_commandList->SetGraphicsRootDescriptorTable(_currentGraphicsRootSignature->GetTRootParamIndex(registerIndex),
		GPUHandle);
}

void GDX12CommandList::SetComputeRootConstantBufferView(const UINT CregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireCompute();
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootConstantBufferView(_currentComputeRootSignature->GetBRootParamIndex(CregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeRootShaderResourceView(const UINT TregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireCompute();
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootShaderResourceView(_currentComputeRootSignature->GetTRootParamIndex(TregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeRootUnorderedAccessView(const UINT UregisterIndex, const D3D12_GPU_VIRTUAL_ADDRESS bufferLocation)
{
    RequireCompute();
	if (!_currentComputeRootSignature) { OutputDebugStringA("ERROR: Command List tries to set a root parameter but GDX12RootSignature is not set.\n"); }
	_commandList->SetComputeRootUnorderedAccessView(_currentComputeRootSignature->GetURootParamIndex(UregisterIndex), bufferLocation);
}

void GDX12CommandList::SetComputeSRV(const UINT registerIndex, const D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
    RequireCompute();
	_commandList->SetComputeRootDescriptorTable(_currentComputeRootSignature->GetTRootParamIndex(registerIndex),
		GPUHandle);
}

void GDX12CommandList::SetComputeUAV(const UINT registerIndex, const D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle)
{
    RequireCompute();
	_commandList->SetComputeRootDescriptorTable(_currentComputeRootSignature->GetURootParamIndex(registerIndex),
		GPUHandle);
}

void GDX12CommandList::SetComputeRootDescriptorTable(const UINT registerIndex, const D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
    RequireCompute();
	_commandList->SetComputeRootDescriptorTable(registerIndex, baseDescriptor);
}

void GDX12CommandList::DrawInstanced(const UINT vertexCountPerInstance, const UINT instanceCount, const UINT startVertexLocation, const UINT startInstanceLocation)
{
    RequireDirect();
	_commandList->DrawInstanced(vertexCountPerInstance, instanceCount,
		startVertexLocation, startInstanceLocation);
}

void GDX12CommandList::DrawIndexedInstanced(const UINT indexCountPerInstance, const UINT instanceCount, const UINT startIndexLocation, const INT baseVertexLocation, const UINT startInstanceLocation)
{
    RequireDirect();
	_commandList->DrawIndexedInstanced(indexCountPerInstance, instanceCount,
		startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void GDX12CommandList::Dispatch(const UINT threadGroupCountX, const UINT threadGroupCountY, const UINT threadGroupCountZ)
{
    RequireCompute();
	_commandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void GDX12CommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc)
{
    RequireCompute();
	_commandList->DispatchRays(pDesc);
}

void GDX12CommandList::ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, const UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, const UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, const UINT64 CountBufferOffset)
{
    RequireCompute();
	_commandList->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
}

void GDX12CommandList::ResourceBarrier(const std::initializer_list<CD3DX12_RESOURCE_BARRIER> barriers)
{
	if (barriers.size() > (std::numeric_limits<UINT>::max)())
		throw std::out_of_range("Too many resource barriers");
    if (_type == D3D12_COMMAND_LIST_TYPE_COPY)
    {
        for (const auto& barrier : barriers)
        {
            const auto validState = [](D3D12_RESOURCE_STATES state)
                { return state == D3D12_RESOURCE_STATE_COMMON || state == D3D12_RESOURCE_STATE_COPY_SOURCE || state == D3D12_RESOURCE_STATE_COPY_DEST; };
            const D3D12_RESOURCE_BARRIER& native = barrier;
            if (native.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ||
                (native.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                    (!validState(native.Transition.StateBefore) || !validState(native.Transition.StateAfter) ||
                        native.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)))
                throw std::invalid_argument("Unsupported COPY list barrier/state");
        }
    }
	const auto needed = [](const D3D12_RESOURCE_BARRIER& barrier)
	{
		return barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
			barrier.Transition.StateBefore != barrier.Transition.StateAfter;
	};
	const auto* firstNoOp = barriers.begin();
	while (firstNoOp != barriers.end() && needed(*firstNoOp)) ++firstNoOp;
	if (firstNoOp == barriers.end())
	{
		if (barriers.size()) _commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.begin());
		return;
	}
	if (barriers.size() == 1) return;

	D3D12_RESOURCE_BARRIER smallBatch[8];
	std::vector<D3D12_RESOURCE_BARRIER> largeBatch;
	auto* filtered = smallBatch;
	if (barriers.size() > std::size(smallBatch))
	{
		largeBatch.resize(barriers.size() - 1);
		filtered = largeBatch.data();
	}
	UINT count = 0;
	for (auto* barrier = barriers.begin(); barrier != firstNoOp; ++barrier) filtered[count++] = *barrier;
	for (auto* barrier = firstNoOp + 1; barrier != barriers.end(); ++barrier)
		if (needed(*barrier)) filtered[count++] = *barrier;
	if (count) _commandList->ResourceBarrier(count, filtered);
}

void GDX12CommandList::EnhancedTextureBarrier(const std::initializer_list<D3D12_TEXTURE_BARRIER> textureBarriers)
{
    if (_type == D3D12_COMMAND_LIST_TYPE_COPY)
        throw std::logic_error("COPY texture transfers use COMMON and legacy copy-state barriers");
	std::vector<D3D12_BARRIER_GROUP> barrierGroups;
	barrierGroups.reserve(1);

	D3D12_BARRIER_GROUP barrierGroup = {};
	barrierGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
	barrierGroup.NumBarriers = static_cast<UINT>(textureBarriers.size());
	barrierGroup.pTextureBarriers = textureBarriers.begin();
	barrierGroups.push_back(barrierGroup);

	_commandList->Barrier(static_cast<UINT>(barrierGroups.size()), barrierGroups.data());
}

#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
GDX12CommandList::ScopedEvent::ScopedEvent(GDX12CommandList* commands, const char* name, const XMVECTOR color)
    : _commands(commands->_commandList.Get())
{
	UINT64 pixColor = (static_cast<UINT64>(XMVectorGetW(color) * 255.0f) << 24) |
		(static_cast<UINT64>(XMVectorGetZ(color) * 255.0f) << 16) |
		(static_cast<UINT64>(XMVectorGetY(color) * 255.0f) << 8) |
		(static_cast<UINT64>(XMVectorGetX(color) * 255.0f));

	PIXBeginEvent(_commands, pixColor, "%s", name);
}

GDX12CommandList::ScopedEvent::~ScopedEvent() noexcept
{
	PIXEndEvent(_commands);
}
#endif

void GDX12CommandList::CopyResource(ID3D12Resource* destResource, ID3D12Resource* sourceResource)
{
	_commandList->CopyResource(destResource, sourceResource);
}

void GDX12CommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc)
{
    RequireCompute();
	_commandList->BuildRaytracingAccelerationStructure(pDesc, 0, nullptr);
}


