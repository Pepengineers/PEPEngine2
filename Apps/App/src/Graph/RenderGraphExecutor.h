#pragma once
#include "RenderGraph.h"
#include "RenderGraphRecorder.h"
#include "QueueSubmission.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <functional>
#include <array>
#include <memory>
#include <thread>
class GDX12CommandList;
class GDX12CommandQueue;
namespace BistroRender
{
// Each recording context has one Begin/Execute/drain owner; QueueSubmission
// serializes native transactions for all device/loop contexts. Record callbacks run on
// workers, while OwnerRecording packets record just before their submission.
// Callbacks cannot submit, wait for GPU, or call tracker-mutating helpers.
// Caller freezes resources/descriptors/upload ranges through the slot fence;
// Begin permits slot rewrites; replacing shared resources requires Drain.
// After a partial error drain
// and recreate the executor and resources (texture final states may not hold).
class Executor
{
  public:
	using Record = std::function<void(Id, uint32_t, ID3D12GraphicsCommandList*)>;
	struct PacketExchange
	{
		Id Export, Import;
		std::function<CompletionToken(CompletionToken)> SubmitRemote;
	};
	Executor(std::shared_ptr<QueueSubmission>, Id device, uint32_t slots, unsigned threads = 4);
	~Executor();
	void Begin(uint32_t slot);
	bool TryBegin(uint32_t slot); // independent loops never CPU-wait for busy slots
	// Explicit owner-only capacity growth after Begin retired the selected slot.
	// Execute still rejects an unreserved plan; workers never allocate lists.
	void Reserve(const Schedule& schedule);
	// Only a timestamp-only prefix may run concurrently with slot-local packets.
	enum class FrameStart { Ordered, TimestampOnly };
	uint64_t Execute(const Schedule&, const std::vector<ID3D12Resource*>&, const Record&,
	                 GDX12CommandList* prefix = nullptr, GDX12CommandList* suffix = nullptr,
	                 const std::atomic<bool>* cancellation = nullptr, const std::vector<PacketWait>& waits = {},
	                 const std::vector<PacketExchange>& exchanges = {}, Id signalAfter = Invalid,
	                 FrameStart frameStart = FrameStart::Ordered);
	void Drain();
	const auto& ExchangeCompletions() const { return _exchangeCompletions; }
	const std::vector<CompletionToken>& Completions() const { return _completions; }
	const std::shared_ptr<QueueSubmission>& Submissions() const { return _submissions; }
	uint64_t Retirement(uint32_t slot) const { return _slots.at(slot).Fences[0]; }

  private:
	struct List
	{
		ComPtr<ID3D12CommandAllocator> Allocator;
		ComPtr<ID3D12GraphicsCommandList> Commands;
	};
	struct Slot
	{
		std::array<std::vector<List>, 3> Lists;
		std::array<uint64_t, 3> Fences{};
		std::shared_ptr<QueueSubmission::BenchmarkSlot> Benchmark;
		bool Poisoned = false;
	};
	void BeginTimings(QueueSubmission::BenchmarkSlot&, const Schedule&, const std::vector<ID3D12Resource*>&);
	void Owner() const;
	CompletionToken Token(Queue q, uint64_t value) const;
	std::shared_ptr<QueueSubmission> _submissions;
	Id _deviceId;
	GDX12Device& _device;
	JobSystem _jobs;
	std::thread::id _owner = std::this_thread::get_id();
	std::vector<Slot> _slots;
	std::vector<CompletionToken> _completions, _exchangeCompletions;
	uint64_t _previousCompletion = 0; // DIRECT terminal protects cross-frame persistent resources
	uint64_t _lastOpaqueFrame = 0; // external lists may reference resources from any slot
	uint32_t _slot = 0;
	bool _begun = false;
	bool _aborted = false; // partial frame requires resource-state rebuild
};
} // namespace BistroRender
