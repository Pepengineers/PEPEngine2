#include "Engine.RendererDX12/GDX12CommandList.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"

GDX12CommandList::GDX12CommandList(GDX12Device* device) : FenceValue(0)
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
}