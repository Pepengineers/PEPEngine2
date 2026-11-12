#pragma once

#include "Engine.RendererDX12/GDX12Device.h"


class GDX12CommandList;

// Calls on a device and its queues require external serialization.
class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
	D3D12_COMMAND_LIST_TYPE GetType() const { return _type; }
	~GDX12CommandQueue();

    void Reset();
    UINT64 Signal();

	const ComPtr<ID3D12CommandQueue>& GetCommandQueue();
	const ComPtr<ID3D12CommandQueue>& GetPresentCommandQueue();
	const ComPtr<ID3D12Fence>& GetFence();

	// Returns a CommandList that you can work with.
	// If all previously created lists are busy - a new one will be created.
	GDX12CommandList* GetCommandList();
    // Releases resources from retired lists without waiting or allocating another list.
    void CollectCompletedLists();
	void ExecuteCommandList(GDX12CommandList* commandList);
	void ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists);

	void CPUWaitForFenceValue(uint64_t fenceValue);
    void WaitForQueue(GDX12CommandQueue& producer, uint64_t fenceValue);

	// Waits for all work submitted to the native queue, including presentation SDKs.
	void Flush();

	// Legacy read-only observation for callers; external mutation is rejected on submission.
	UINT64 FenceValue;

private:
    UINT64 _issuedValue = 0;
    uint64_t NextFenceValue() const;
    void ValidateCommandList(GDX12CommandList* list) const;
    D3D12_COMMAND_LIST_TYPE _type;

	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12CommandQueue> _proxyCommandQueue;
	ComPtr<ID3D12Fence> _fence;

	GDX12Device* _device;

	// Command Lists can be requested via GetCommandList()
	// It returns any list from avalible vector
	// If ther are no avalible lists - new will be created and returned(and placed into working lists)
	// When working list finishes execution - it is moved to avalible lists
	std::vector<std::unique_ptr<GDX12CommandList>> _workingCommandLists;
	std::vector<std::unique_ptr<GDX12CommandList>> _availableCommandLists;

};
