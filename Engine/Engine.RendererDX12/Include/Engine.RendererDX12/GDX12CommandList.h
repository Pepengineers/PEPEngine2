#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Device;

class GDX12CommandList
{
public:
	GDX12CommandList(GDX12Device* device);
	~GDX12CommandList();

	ComPtr<ID3D12GraphicsCommandList10> GetCommandList();
	ComPtr<ID3D12CommandAllocator> GetCommandAllocator();
	void Reset();

	UINT64 FenceValue;

private:
	ComPtr<ID3D12GraphicsCommandList10> _commandList;
	ComPtr<ID3D12CommandAllocator> _commandAllocator;

	//TODO:
	// - Add a fuck ton of command wrappers
	// - Cache Current Primitive Topology
	// - Cache Current PSO
	// - Cache Current Root Signature
	// - Cache Current Descriptors Heaps
};
