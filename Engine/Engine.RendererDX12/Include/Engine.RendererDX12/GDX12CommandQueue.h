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

	// Returns a CommandList that you can work with
	// If all lists are busy - new will be created
	std::shared_ptr<GDX12CommandList>& GetCommandList();
	void ExecuteCommandList(std::shared_ptr<GDX12CommandList> commandList);
	void ExecuteCommandLists(std::shared_ptr<GDX12CommandList>* lists, UINT count);

	void WaitForFenceValue(uint64_t fenceValue);

	// Do I even need this?
	UINT64 FenceValue;

private:
	void ProcessInFlightCommandLists();

	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12Fence> _fence;
	GDX12Device* _device;

	// Command Lists can be requested via GetCommandList()
	// It returns any list from avalible vector
	// If ther are no avalible lists - new will be created and returned(and placed into working lists)
	// When working list finishes execution - it is moved to avalible lists
	PEPEngine::Utils::LockThreadQueue<std::shared_ptr<GDX12CommandList>> _workingCommandLists;
	PEPEngine::Utils::LockThreadQueue<std::shared_ptr<GDX12CommandList>> _availableCommandLists;

	// working -> avalible list movement is done in a separate while(true) thread
	std::thread _commandListExecutorThread;
	std::atomic_bool _isExecutorAlive{ true };
	std::mutex _executorMutex;
	std::condition_variable _executorCondition;
};