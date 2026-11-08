#pragma once

#include "Engine.RendererDX12/GDX12Device.h"


class GDX12CommandList;

// Calls on a device/its queues (including imports/reset) require external serialization.
class GDX12CommandQueue
{
public:
	GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        bool crossAdapterFence = false);
	D3D12_COMMAND_LIST_TYPE GetType() const { return _type; }
	~GDX12CommandQueue();

    // Reset drains issued work and invalidates this queue and all imported views of its timeline.
	void Reset();
    GDX12Submission::QueueId GetIdentity() const;
    GDX12Submission::TimelineId GetTimelineIdentity() const;
    GDX12Submission::CompletionPoint GetLastCompletionPoint() const;
    GDX12Submission::CompletionPoint SignalCompletion();
    GDX12Submission::FenceProgress GetCompletionProgress() const;
    GDX12Submission::FenceProgress GetCompletionProgress(GDX12Submission::TimelineId producer) const;
    void WaitFor(const GDX12Submission::CompletionPoint& producer);
    // Export requires a producer constructed with crossAdapterFence=true; handles never escape.
    GDX12Submission::TimelineId ImportTimeline(GDX12CommandQueue& producer);


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
    void WaitForQueue(GDX12CommandQueue& producer, uint64_t fenceValue);

	// Waits for all work submitted to the native queue, including presentation SDKs.
	void Flush();

	// Legacy read-only observation for callers; external mutation is rejected on submission.
	UINT64 FenceValue;

private:
    struct TimelineState
    {
        GDX12Submission::TimelineId Identity;
        ComPtr<ID3D12Fence> Fence;
        uint64_t IssuedValue = 0;
        bool Active = true;
        bool CrossAdapter = false;
    };
    struct ImportedTimeline
    {
        std::weak_ptr<TimelineState> Producer;
        ComPtr<ID3D12Fence> Fence;
    };
    std::shared_ptr<TimelineState> _timeline;
    std::vector<ImportedTimeline> _imports;
    GDX12Submission::CompletionPoint _lastCompletion;
    GDX12Submission::TimelineId _otherTimeline{};
    uint64_t NextFenceValue() const;
    ComPtr<ID3D12Fence> ResolveTimeline(GDX12Submission::TimelineId producer) const;
    void ValidateCommandList(GDX12CommandList* list) const;
    D3D12_COMMAND_LIST_TYPE _type;
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

};
