#pragma once

#include "Engine.RendererDX12/GDX12Device.h"

class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device);
	~GDX12CommandQueue();

	void Reset();

	ComPtr<ID3D12CommandQueue> GetCommandQueue();
	ComPtr<ID3D12Fence> GetFence();

	UINT64 FenceValue;

private:
	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12Fence> _fence;
};