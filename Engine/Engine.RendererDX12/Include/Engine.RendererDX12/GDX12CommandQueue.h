#pragma once

#include "Engine.RendererDX12/GDX12Device.h"

#include "Engine.Core/LockThreadQueue.h"

class GDX12CommandList;

class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device);
	~GDX12CommandQueue();

	void Reset();

	ComPtr<ID3D12CommandQueue> GetCommandQueue();
	ComPtr<ID3D12Fence> GetFence();

	// Returns a CommandList that you can work with.
	// If all previously created lists are busy - a new one will be created.
	std::shared_ptr<GDX12CommandList> GetCommandList();
	void ExecuteCommandList(std::shared_ptr<GDX12CommandList> commandList);
	void ExecuteCommandLists(std::shared_ptr<GDX12CommandList>* lists, UINT count);

	void WaitForFenceValue(uint64_t fenceValue);

	//Waits for execution of all active lists
	void Flush();

	// Do I even need this?
	UINT64 FenceValue;

private:
	// We can probably make it work in a separate thread
	//Imported PEPEngine::Utils::LockThreadQueue in case it helps
	void ClearCompletedLists();

	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12Fence> _fence;
	GDX12Device* _device;

	// Command Lists can be requested via GetCommandList()
	// It returns any list from avalible vector
	// If ther are no avalible lists - new will be created and returned(and placed into working lists)
	// When working list finishes execution - it is moved to avalible lists
	std::vector<std::shared_ptr<GDX12CommandList>> _workingCommandLists;
	std::vector<std::shared_ptr<GDX12CommandList>> _availableCommandLists;

	std::shared_ptr<GDX12CommandList> _lastDispatchedList;
};