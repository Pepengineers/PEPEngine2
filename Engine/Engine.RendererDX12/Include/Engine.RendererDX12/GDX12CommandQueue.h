#pragma once

#include "Engine.RendererDX12/GDX12Device.h"


class GDX12CommandList;

class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device);
	~GDX12CommandQueue();

	void Reset();

	const ComPtr<ID3D12CommandQueue>& GetCommandQueue();
	const ComPtr<ID3D12CommandQueue>& GetPresentCommandQueue();
	const ComPtr<ID3D12Fence>& GetFence();
	ComPtr<ID3D12Fence>& GetOtherFence();

	// Returns a CommandList that you can work with.
	// If all previously created lists are busy - a new one will be created.
	GDX12CommandList* GetCommandList();
	void ExecuteCommandList(GDX12CommandList* commandList);
	void ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists);

	void CPUWaitForFenceValue(uint64_t fenceValue);
	void WaitForOtherFence(uint64_t otherFenceValue);

	//Waits for execution of all active lists
	void Flush();

	// Do I even need this?
	UINT64 FenceValue;

private:
	// We can probably make it work in a separate thread
	//Imported PEPEngine::Utils::LockThreadQueue in case it helps
	void ClearCompletedLists();

	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12CommandQueue> _proxyCommandQueue;
	ComPtr<ID3D12Fence> _fence;
	//shared fence from another device
	//null if _dualGPUMode is false
	ComPtr<ID3D12Fence> _otherFence;

	GDX12Device* _device;

	// Command Lists can be requested via GetCommandList()
	// It returns any list from avalible vector
	// If ther are no avalible lists - new will be created and returned(and placed into working lists)
	// When working list finishes execution - it is moved to avalible lists
	std::vector<std::unique_ptr<GDX12CommandList>> _workingCommandLists;
	std::vector<std::unique_ptr<GDX12CommandList>> _availableCommandLists;

	uint64_t _lastDispatchedFenceValue;
};
