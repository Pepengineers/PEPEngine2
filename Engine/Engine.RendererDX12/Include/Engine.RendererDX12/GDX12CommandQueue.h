#pragma once

#include "Engine.RendererDX12/GDX12Device.h"

class GDX12CommandList;
class GDX12CommandQueue;

struct GDX12CompletionPoint
{
	GDX12Device* Device = nullptr;
	GDX12CommandQueue* Queue = nullptr;
	ComPtr<ID3D12Fence> Fence;
	UINT64 Value = 0;

	explicit operator bool() const { return Fence && Value != 0; }
};

class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
	~GDX12CommandQueue();

	void Reset();

	const ComPtr<ID3D12CommandQueue>& GetCommandQueue();
	const ComPtr<ID3D12CommandQueue>& GetPresentCommandQueue();
	const ComPtr<ID3D12Fence>& GetFence();
	D3D12_COMMAND_LIST_TYPE GetType() const;
	GDX12CompletionPoint Signal();
	GDX12CompletionPoint GetCompletionPoint() const;
	void GPUWait(const GDX12CompletionPoint& point);
	static void CPUwait(const GDX12CompletionPoint& point);

	// Returns a CommandList that you can work with.
	// If all previously created lists are busy - a new one will be created.
	GDX12CommandList* GetCommandList();
	void ExecuteCommandList(GDX12CommandList* commandList);
	void ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists);

	void CPUWaitForFenceValue(uint64_t fenceValue);

	//Waits for execution of all active lists
	void Flush();

private:
	// We can probably make it work in a separate thread
	//Imported PEPEngine::Utils::LockThreadQueue in case it helps
	void ClearCompletedLists();

	ComPtr<ID3D12CommandQueue> _commandQueue;
	ComPtr<ID3D12CommandQueue> _proxyCommandQueue;
	ComPtr<ID3D12Fence> _fence;

	GDX12Device* _device;
	D3D12_COMMAND_LIST_TYPE _type;

	struct OpenedFence
	{
		ComPtr<ID3D12Fence> Source;
		ComPtr<ID3D12Fence> Local;
	};
	std::vector<OpenedFence> _openedFences;

	// Command Lists can be requested via GetCommandList()
	// It returns any list from avalible vector
	// If ther are no avalible lists - new will be created and returned(and placed into working lists)
	// When working list finishes execution - it is moved to avalible lists
	std::vector<std::unique_ptr<GDX12CommandList>> _workingCommandLists;
	std::vector<std::unique_ptr<GDX12CommandList>> _availableCommandLists;

	uint64_t _lastSubmittedFenceValue;
};
