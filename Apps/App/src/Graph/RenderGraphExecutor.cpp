#ifndef USE_PIX
#define USE_PIX
#endif
#include "RenderGraphExecutor.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <pix/pix3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace BistroRender
{
namespace
{
size_t QueueIndex(Queue queue)
{
	switch (queue)
	{
	case Queue::Direct:
		return 0;
	case Queue::Compute:
		return 1;
	case Queue::Copy:
		return 2;
	}
	throw std::invalid_argument("unknown submission queue");
}
D3D12_COMMAND_LIST_TYPE ListType(Queue queue)
{
	switch (queue)
	{
	case Queue::Direct:
		return D3D12_COMMAND_LIST_TYPE_DIRECT;
	case Queue::Compute:
		return D3D12_COMMAND_LIST_TYPE_COMPUTE;
	case Queue::Copy:
		return D3D12_COMMAND_LIST_TYPE_COPY;
	}
	throw std::invalid_argument("unknown command list queue");
}
D3D12_HEAP_TYPE HeapType(Heap heap)
{
	switch (heap)
	{
	case Heap::Default:
		return D3D12_HEAP_TYPE_DEFAULT;
	case Heap::Upload:
		return D3D12_HEAP_TYPE_UPLOAD;
	case Heap::Readback:
		return D3D12_HEAP_TYPE_READBACK;
	}
	throw std::invalid_argument("unknown resource heap");
}
bool BarrierStateAllowed(Queue queue, State state)
{
	const State copy = D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
	const State compute = copy | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
	                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
	                      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
	                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	return queue == Queue::Direct || (state & ~(queue == Queue::Copy ? copy : compute)) == 0;
}

// A buffer keeps its previous state while packets share one native ExecuteCommandLists call.
void RecordBarriers(ID3D12GraphicsCommandList* commands, const std::vector<Barrier>& barriers,
                    const std::vector<ID3D12Resource*>& resources, Id firstInExecution,
                    const char* packetName, const char* phase)
{
	if (barriers.empty()) return;
	PIXScopedEvent(commands, PIX_COLOR(124, 143, 166), "%s / %s (%u)", packetName, phase, static_cast<UINT>(barriers.size()));
	std::vector<D3D12_RESOURCE_BARRIER> native;
	native.reserve(barriers.size());
	for (auto barrier : barriers)
	{
		if (barrier.PreviousBufferUse != Invalid && barrier.PreviousBufferUse >= firstInExecution)
		{
			barrier.Before = barrier.BeforeWithoutDecay;
			barrier.Uav = barrier.Before == barrier.After && barrier.OrderUavWithoutDecay;
			if (barrier.Before == barrier.After && !barrier.Uav) continue;
		}
		auto* resource = resources.at(barrier.Resource);
		native.push_back(barrier.Uav ? CD3DX12_RESOURCE_BARRIER::UAV(resource)
		                            : CD3DX12_RESOURCE_BARRIER::Transition(resource,
		                                  static_cast<D3D12_RESOURCE_STATES>(barrier.Before),
		                                  static_cast<D3D12_RESOURCE_STATES>(barrier.After)));
	}
	if (!native.empty()) commands->ResourceBarrier(static_cast<UINT>(native.size()), native.data());
}
} // namespace
Executor::Executor(std::shared_ptr<QueueSubmission> submissions, Id device, uint32_t slots, unsigned threads)
    : _submissions(std::move(submissions)), _deviceId(device),
      _device(_submissions ? _submissions->Device(device) : throw std::invalid_argument("missing submission domain")),
      _jobs(threads), _slots(slots)
{
	if (!slots)
	{
		throw std::invalid_argument("empty executor capacity");
	}
}
Executor::~Executor()
{
	try
	{
		Drain();
	}
	catch (...)
	{
		for (auto& s : _slots)
		{
			for (auto& lists : s.Lists)
			{
				for (auto& l : lists)
				{
					l.Commands.Detach();
					l.Allocator.Detach();
				}
			}
		}
	}
}
void Executor::Owner() const
{
	if (std::this_thread::get_id() != _owner)
	{
		throw std::logic_error("executor requires owner thread");
	}
}
CompletionToken Executor::Token(Queue q, uint64_t value) const
{
	return {_deviceId, q, value, _submissions->Incarnation()};
}
namespace
{
double QpcMilliseconds()
{
	static const double multiplier = [] {
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		return 1000.0 / double(frequency.QuadPart);
	}();
	return multiplier;
}
}

void QueueSubmission::CollectBenchmarkSlot(BenchmarkSlot& slot, bool waitForGpu)
{
	auto samples = std::exchange(slot.Samples, {});
	if (samples.empty() || !samples.back().Terminal) return;
	if (waitForGpu) Wait(samples.back().Terminal);
	{
		std::array<const uint64_t*, 3> ticks{};
		std::array<UINT, 3> index{};
		for (size_t q = 0; q < ticks.size(); ++q)
		{
			const auto& timing = slot.Timings[q];
			if (!timing.Count) continue;
			const D3D12_RANGE read{0, size_t(timing.Count) * sizeof(uint64_t)};
			void* mapped = nullptr;
			if (FAILED(timing.Readback->Map(0, &read, &mapped)))
			{
				CheckHealth();
				continue;
			}
			ticks[q] = static_cast<const uint64_t*>(mapped);
		}
		for (auto& sample : samples)
		{
			const auto q = QueueIndex(sample.Token.Target);
			const auto& timing = slot.Timings[q];
			if (!ticks[q]) continue;
			const auto toMs = [&](uint64_t tick) {
				return double(timing.AnchorCpu) * QpcMilliseconds() +
					double(int64_t(tick - timing.AnchorGpu)) * 1000.0 / double(timing.Frequency);
			};
			sample.BeginMs = toMs(ticks[q][index[q]++]);
			sample.EndMs = toMs(ticks[q][index[q]++]);
			sample.Calibrated = timing.Calibrated;
		}
		const D3D12_RANGE noWrites{0, 0};
		for (size_t q = 0; q < ticks.size(); ++q)
			if (ticks[q]) slot.Timings[q].Readback->Unmap(0, &noWrites);
	}
	std::lock_guard lock(_mutex);
	if (slot.Epoch == _nextBenchmarkEpoch)
		_benchmarkPackets.insert(_benchmarkPackets.end(),
		    std::make_move_iterator(samples.begin()), std::make_move_iterator(samples.end()));
}

void Executor::BeginTimings(QueueSubmission::BenchmarkSlot& slot, const Schedule& schedule, const std::vector<ID3D12Resource*>& resources)
{
	for (auto& timing : slot.Timings) timing.Count = 0;
	for (const auto& packet : schedule.Packets) slot.Timings[QueueIndex(packet.Target)].Count += 2;
	for (const auto queue : {Queue::Direct, Queue::Compute, Queue::Copy})
	{
		auto& timing = slot.Timings[QueueIndex(queue)];
		if (!timing.Count) continue;
		auto* native = _submissions->_devices[_deviceId]->Queues[QueueIndex(queue)].Queue->GetCommandQueue().Get();
		if (FAILED(native->GetTimestampFrequency(&timing.Frequency)) || !timing.Frequency)
		{
			timing.Count = 0;
			continue;
		}
		if (timing.Capacity < timing.Count)
		{
			if (queue == Queue::Copy)
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
				if (FAILED(_device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) ||
				    !options.CopyQueueTimestampQueriesSupported)
				{
					timing.Count = 0;
					continue;
				}
			}
			const UINT capacity = (std::max)(timing.Count, 2 * timing.Capacity);
			const D3D12_QUERY_HEAP_DESC desc{queue == Queue::Copy ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP :
				D3D12_QUERY_HEAP_TYPE_TIMESTAMP, capacity, 0};
			if (FAILED(_device.GetDevice()->CreateQueryHeap(&desc, IID_PPV_ARGS(timing.Heap.ReleaseAndGetAddressOf()))))
			{
				timing.Capacity = timing.Count = 0;
				continue;
			}
			try
			{
				timing.Readback = _device.CreateResource(D3D12_HEAP_TYPE_READBACK,
					CD3DX12_RESOURCE_DESC::Buffer(uint64_t(capacity) * sizeof(uint64_t)),
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
			}
			catch (const DxException&)
			{
				_submissions->CheckHealth();
				timing.Capacity = timing.Count = 0;
				continue;
			}
			timing.Capacity = capacity;
		}
		timing.AnchorGpu = timing.AnchorCpu = 0;
		timing.Calibrated = SUCCEEDED(native->GetClockCalibration(&timing.AnchorGpu, &timing.AnchorCpu));
	}
	slot.Samples.resize(schedule.Packets.size());
	for (size_t i = 0; i < schedule.Packets.size(); ++i)
	{
		const auto& packet = schedule.Packets[i];
		auto& sample = slot.Samples[i];
		sample.Name = packet.Name;
		if (packet.TransferBytes)
		{
			sample.SharedBytes = *packet.TransferBytes;
			continue;
		}
		sample.SharedBytes = 0;
		for (const auto& access : packet.Accesses)
		{
			if ((access.Required & (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST)) == 0)
				continue;
			auto* resource = resources[access.Resource];
			const auto desc = resource->GetDesc();
			if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
			    (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER))
				sample.SharedBytes += desc.Width;
		}
	}
}

void Executor::Begin(uint32_t index)
{
	Owner();
	_submissions->CheckHealth();
	auto& slot = _slots.at(index);
	if (_aborted)
	{
		throw std::logic_error("partial frame aborted; drain and recreate execution/resources");
	}
	if (slot.Poisoned)
	{
		throw std::logic_error("executor slot quarantined after submission failure");
	}
	for (auto queue : {Queue::Direct, Queue::Compute, Queue::Copy})
	{
		if (auto value = slot.Fences[QueueIndex(queue)])
		{
			_submissions->Wait(Token(queue, value));
		}
	}
	_slot = index;
	_begun = true;
	_completions.clear();
}
bool Executor::TryBegin(uint32_t index)
{
	Owner();
	_submissions->CheckHealth();
	auto& slot = _slots.at(index);
	if (_aborted || slot.Poisoned)
	{
		throw std::logic_error("recording context aborted/quarantined");
	}
	for (auto queue : {Queue::Direct, Queue::Compute, Queue::Copy})
	{
		if (auto value = slot.Fences[QueueIndex(queue)])
		{
			if (!_submissions->Complete(Token(queue, value)))
			{
				return false;
			}
		}
	}
	_slot = index;
	_begun = true;
	_completions.clear();
	return true;
}
void Executor::Drain()
{
	Owner();
	_submissions->CheckHealth();
	for (auto& slot : _slots)
	{
		for (auto queue : {Queue::Direct, Queue::Compute, Queue::Copy})
		{
			if (auto value = slot.Fences[QueueIndex(queue)])
			{
				_submissions->Wait(Token(queue, value));
			}
		}
	}
	for (auto& slot : _slots)
	{
		if (slot.Poisoned)
		{
			throw std::logic_error("submission failed; executor quarantined");
		}
	}
}
void Executor::Reserve(const Schedule& schedule)
{
	std::array<size_t, 3> counts{};
	for (const auto& packet : schedule.Packets)
	{
		if (!packet.Lists || packet.Lists > (std::numeric_limits<UINT>::max)() - 2)
		{
			throw std::invalid_argument("invalid packet list count");
		}
		counts[QueueIndex(packet.Target)] += packet.Lists;
	}
	Owner();
	if (!_begun)
	{
		throw std::logic_error("Reserve requires a retired slot after Begin");
	}
	for (auto queue : {Queue::Direct, Queue::Compute, Queue::Copy})
	{
		auto& lists = _slots.at(_slot).Lists[QueueIndex(queue)];
		while (lists.size() < counts[QueueIndex(queue)])
		{
			List list;
			ThrowIfFailed(_device.GetDevice()->CreateCommandAllocator(ListType(queue), IID_PPV_ARGS(&list.Allocator)));
			ThrowIfFailed(_device.GetDevice()->CreateCommandList(0, ListType(queue), list.Allocator.Get(), nullptr,
			                                                     IID_PPV_ARGS(&list.Commands)));
			ThrowIfFailed(list.Commands->Close());
			lists.push_back(std::move(list));
		}
	}
}

void Executor::List::RecordPacket(const Packet& packet, uint32_t listIndex, Id firstInExecution,
                                  const std::vector<ID3D12Resource*>& resources, const Record& record,
                                  const QueueSubmission::BenchmarkSlot::Timing* timing, UINT query)
{
	auto* commands = Commands.Get();
	ThrowIfFailed(Allocator->Reset());
	ThrowIfFailed(commands->Reset(Allocator.Get(), nullptr));
	try
	{
		if (listIndex == 0)
		{
			if (timing) commands->EndQuery(timing->Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
			RecordBarriers(commands, packet.Before, resources, firstInExecution, packet.Name.c_str(), "before barriers");
		}
		if (packet.Operation != Invalid)
		{
			PIXScopedEvent(commands, PIX_COLOR(57, 158, 117), "%s", packet.Name.c_str());
			record(packet.Operation, listIndex, commands);
		}
		if (listIndex + 1 == packet.Lists)
		{
			RecordBarriers(commands, packet.After, resources, firstInExecution, packet.Name.c_str(), "after barriers");
			if (timing)
			{
				commands->EndQuery(timing->Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
				commands->ResolveQueryData(timing->Heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query,
				                           2, timing->Readback.Get(), uint64_t(query) * sizeof(uint64_t));
			}
		}
	}
	catch (...)
	{
		commands->Close();
		throw;
	}
	ThrowIfFailed(commands->Close());
}

uint64_t Executor::Execute(const Schedule& schedule, const std::vector<ID3D12Resource*>& resources,
                           const Record& record, GDX12CommandList* prefix, GDX12CommandList* suffix,
                           const std::atomic<bool>* cancellation, const std::vector<PacketWait>& waits,
                           const std::vector<PacketExchange>& exchanges, Id signalAfter, FrameStart frameStart)
{
	Owner();
	if (!_begun)
	{
		throw std::logic_error("Begin required");
	}
	_begun = false;
	auto& slot = _slots[_slot];
	_completions.assign(schedule.Packets.size(), {});
	if (schedule.Device != _deviceId)
	{
		throw std::invalid_argument("schedule belongs to another device context");
	}
	for (const auto& exchange : exchanges)
		if (exchange.Export >= schedule.Packets.size() || !exchange.SubmitRemote ||
		    (exchange.Import != Invalid && (exchange.Export >= exchange.Import || exchange.Import >= schedule.Packets.size())))
			throw std::invalid_argument("invalid cross-device packet exchange");
	_exchangeCompletions.assign(exchanges.size(), {});
	for (const auto& wait : waits)
	{
		if (wait.Packet >= schedule.Packets.size())
		{
			throw std::invalid_argument("external wait has no packet");
		}
		_submissions->Complete(wait.Token); // rejects unissued/stale tokens, does NOT wait
	}
	if (resources.size() != schedule.Decays.size() || resources.size() != schedule.Memory.size())
	{
		throw std::invalid_argument("resource binding count mismatch");
	}
	for (size_t i = 0; i < resources.size(); ++i)
	{
		const auto info = _submissions->ValidateResource(_deviceId, resources[i]);
		if ((info.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0 || info.Heap == D3D12_HEAP_TYPE_CUSTOM)
		{
			throw std::invalid_argument("simultaneous-access textures/custom heaps require an explicit decay contract");
		}
		if (info.Heap != HeapType(schedule.Memory[i]))
		{
			throw std::invalid_argument("physical resource heap contract mismatch");
		}
		const bool decays = info.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && info.Heap == D3D12_HEAP_TYPE_DEFAULT;
		if (decays != schedule.Decays[i])
		{
			throw std::invalid_argument("physical buffer decay contract mismatch");
		}
	}
	if (schedule.Packets.empty())
	{
		throw std::invalid_argument("missing terminal packet");
	}
	const auto& terminal = schedule.Packets.back();
	if (terminal.Target != schedule.TerminalQueue || terminal.Operation != Invalid ||
	    (schedule.FrameBoundary && terminal.Target != Queue::Direct))
	{
		throw std::invalid_argument("invalid terminal queue boundary");
	}
	std::vector<bool> joined(schedule.Packets.size() - 1, false);
	for (const Id dependency : terminal.Dependencies)
	{
		if (dependency < joined.size())
		{
			joined[dependency] = true;
		}
	}
	if (std::find(joined.begin(), joined.end(), false) != joined.end())
	{
		throw std::invalid_argument("terminal packet must join all prior packets");
	}
	if ((!schedule.FrameBoundary && (prefix || suffix)) ||
	    (prefix && prefix->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) ||
	    (suffix && suffix->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT))
	{
		throw std::invalid_argument("prefix/suffix must be DIRECT lists");
	}
	std::vector<bool> endBatch(schedule.Packets.size(), false);
	if (_lastOpaqueFrame <= slot.Fences[0]) _lastOpaqueFrame = 0; // Begin retired this slot.
	const bool allowSlotOverlap = schedule.FrameBoundary && frameStart == FrameStart::TimestampOnly;
	std::vector<bool> waitForFrameStart(schedule.Packets.size(), false);
	std::array<bool, 3> ordered{};
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		const auto& packet = schedule.Packets[i];
		if (!schedule.FrameBoundary || packet.Target == Queue::Direct || ordered[QueueIndex(packet.Target)]) continue;
		if (allowSlotOverlap && packet.FrameSlotLocal) continue;
		ordered[QueueIndex(packet.Target)] = true;
		waitForFrameStart[i] = true;
		// Submit any preceding slot-local work before the first persistent-resource wait.
		if (i) endBatch[i - 1] = true;
	}
	if (signalAfter != Invalid)
	{
		if (signalAfter >= schedule.Packets.size())
			throw std::invalid_argument("submission boundary has no packet");
		endBatch[signalAfter] = true;
	}
	for (const auto& wait : waits)
		if (wait.Packet) endBatch[wait.Packet - 1] = true;
	for (const auto& exchange : exchanges)
	{
		endBatch[exchange.Export] = true;
		if (exchange.Import != Invalid) endBatch[exchange.Import - 1] = true;
	}
	std::array<size_t, 3> counts{};
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		const auto& p = schedule.Packets[i];
		if (!p.Lists || p.Lists > (std::numeric_limits<UINT>::max)() - 2)
		{
			throw std::invalid_argument("invalid packet list count");
		}
		counts[QueueIndex(p.Target)] += p.Lists;
		for (auto dep : p.Dependencies)
		{
			if (dep >= i)
			{
				throw std::invalid_argument("non-topological packet dependency");
			}
			if (schedule.Packets[dep].Target != p.Target)
			{
				endBatch[i - 1] = true;
				// The terminal joins whole queues and needs no intermediate signals.
				if (i + 1 < schedule.Packets.size()) endBatch[dep] = true;
			}
		}
		const auto validateBarriers = [&](const std::vector<Barrier>& barriers)
		{
			for (const auto& b : barriers)
			{
				if (b.Resource >= resources.size() || schedule.Memory[b.Resource] != Heap::Default)
				{
					throw std::invalid_argument("barrier requires a bound DEFAULT-heap resource");
				}
				if (b.Uav ? p.Target == Queue::Copy
				          : !BarrierStateAllowed(p.Target, b.Before) || !BarrierStateAllowed(p.Target, b.After))
				{
					throw std::invalid_argument("barrier state is unsupported by packet queue");
				}
			}
		};
		validateBarriers(p.Before);
		validateBarriers(p.After);
	}
	for (size_t q = 0; q < counts.size(); ++q)
	{
		if (counts[q] > slot.Lists[q].size())
		{
			throw std::length_error("fixed command list capacity exceeded");
		}
	}
	// Keep fence publication boundaries independent from native ExecuteCommandLists calls.
	std::vector<std::vector<ID3D12CommandList*>> executions;
	std::vector<size_t> executionForPacket;
	std::vector<RecordingGroup> groups;
	executions.reserve(schedule.Packets.size());
	executionForPacket.reserve(schedule.Packets.size());
	groups.reserve(schedule.Packets.size());
	std::vector<std::vector<std::function<void()>>> ownerRecords(schedule.Packets.size());
	std::array<size_t, 3> used{};
	QueueSubmission::BenchmarkSlot* capture = nullptr;
	std::unique_lock<std::mutex> captureLock;
	if (const auto epoch = _submissions->BenchmarkEpoch())
	{
		if (!slot.Benchmark) slot.Benchmark = std::make_shared<QueueSubmission::BenchmarkSlot>();
		captureLock = std::unique_lock(slot.Benchmark->Mutex);
		if (_submissions->TrackBenchmarkSlot(slot.Benchmark, epoch))
		{
			capture = slot.Benchmark.get();
			// Begin/TryBegin has retired the previous submission for this executor slot.
			_submissions->CollectBenchmarkSlot(*capture, false);
			capture->Epoch = epoch;
			BeginTimings(*capture, schedule, resources);
		}
		else captureLock.unlock();
	}
	std::array<UINT, 3> queries{};
	Id firstInExecution = 0;
	Id firstInBatch = 0;
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		const auto& p = schedule.Packets[i];
		// Bound the recording work the GPU waits for before the next submission.
		if (i + 1 == schedule.Packets.size() || schedule.Packets[i + 1].Target != p.Target || i - firstInBatch + 1 == 8)
			endBatch[i] = true;
		const bool newExecution = i == firstInBatch || p.EarliestSubmitStart > firstInExecution ||
			schedule.Packets[i - 1].EarliestSubmitStart == Invalid ||
			executions.back().size() > (std::numeric_limits<UINT>::max)() - p.Lists;
		if (newExecution)
		{
			firstInExecution = i;
			executions.emplace_back();
		}
		executionForPacket.push_back(executions.size() - 1);
		if (endBatch[i]) firstInBatch = i + 1;
		RecordingGroup group;
		const auto q = QueueIndex(p.Target);
		const auto* timing = capture && capture->Timings[q].Count ? &capture->Timings[q] : nullptr;
		const UINT query = std::exchange(queries[q], queries[q] + 2);
		for (uint32_t j = 0; j < p.Lists; ++j)
		{
			// Reserve froze this slot's allocator/list storage before any callbacks were launched.
			auto* list = &slot.Lists[q][used[q]++];
			if (list->Commands->GetType() != ListType(p.Target))
				throw std::logic_error("packet/list queue mismatch");
			executions.back().push_back(list->Commands.Get());
			RecordingGroup leaf{{}, [&, i, j, list, timing, query, firstInExecution]
			{
				list->RecordPacket(schedule.Packets[i], j, firstInExecution, resources, record, timing, query);
			}};
			if (p.OwnerRecording)
			{
				ownerRecords[i].push_back(std::move(leaf.Continue));
				continue;
			}
			if (p.Lists == 1)
				group = std::move(leaf);
			else
				group.Children.push_back(std::move(leaf));
		}
		groups.push_back(std::move(group));
	}
	Recorder recorder(_jobs, std::move(groups), cancellation);
	bool submitted = false;
	uint64_t prefixFence = 0;
	std::array<bool, 3> opaqueFrameJoined{};
	Id batchBegin = 0;
	std::vector<CompletionToken> dependencies;
	try
	{
		for (Id i = 0; i < schedule.Packets.size(); ++i)
		{
			recorder.Wait(i);
			for (auto& ownerRecord : ownerRecords[i])
			{
				if (cancellation && cancellation->load())
				{
					throw std::runtime_error("owner recording cancelled");
				}
				ownerRecord();
			}
			if (i == 0 && prefix)
			{
				slot.Poisoned = true;
				submitted = true;
				_submissions->Submit(_deviceId, Queue::Direct, nullptr, 0, {}, prefix);
				slot.Fences[0] = prefixFence = prefix->FenceValue;
				slot.Poisoned = false;
			}
			const auto target = schedule.Packets[i].Target;
			// Only published fences enter native waits; failed recording cannot
			// strand another queue behind a token that will never be signalled.
			if (waitForFrameStart[i])
			{
				// The prefix follows the previous terminal on the same DIRECT timeline.
				const auto fence = prefixFence ? prefixFence : _previousCompletion;
				if (fence) dependencies.push_back(Token(Queue::Direct, fence));
				opaqueFrameJoined[QueueIndex(target)] = true;
			}
			else if (allowSlotOverlap && _lastOpaqueFrame && target != Queue::Direct && !opaqueFrameJoined[QueueIndex(target)])
			{
				dependencies.push_back(Token(Queue::Direct, _lastOpaqueFrame));
				opaqueFrameJoined[QueueIndex(target)] = true;
			}
			for (auto dep : schedule.Packets[i].Dependencies)
			{
				if (schedule.Packets[dep].Target == target) continue;
				if (!_completions[dep])
				{
					throw std::logic_error("dependency has no submitted completion token");
				}
				if (dependencies.empty() || dependencies.back().Target != _completions[dep].Target ||
				    dependencies.back().Value != _completions[dep].Value)
					dependencies.push_back(_completions[dep]);
			}
			for (const auto& wait : waits)
			{
				if (wait.Packet == i)
				{
					dependencies.push_back(wait.Token);
				}
			}
			for (size_t j = 0; j < exchanges.size(); ++j)
				if (i == exchanges[j].Import)
				{
					if (!_exchangeCompletions[j]) throw std::logic_error("cross-device packet did not return a completion");
					dependencies.push_back(_exchangeCompletions[j]);
				}
			if (!endBatch[i])
				continue;
			slot.Poisoned = true;
			submitted = true;
			if (capture)
			{
				auto& first = capture->Samples[batchBegin];
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				first.SubmitMs = double(now.QuadPart) * QpcMilliseconds();
				first.Dependencies = dependencies;
			}
			const auto firstExecution = executionForPacket[batchBegin];
			const auto count = executionForPacket[i] - firstExecution + 1;
			const auto token = _submissions->Submit(_deviceId, target, executions.data() + firstExecution, count,
			    dependencies, nullptr, capture ? capture->Epoch : 0);
			slot.Fences[QueueIndex(target)] = token.Value;
			for (Id packet = batchBegin; packet <= i; ++packet)
			{
				_completions[packet] = token;
				if (capture) capture->Samples[packet].Token = token;
			}
			batchBegin = i + 1;
			dependencies.clear();
			slot.Poisoned = false;
			for (size_t j = 0; j < exchanges.size(); ++j)
				if (i == exchanges[j].Export)
					_exchangeCompletions[j] = exchanges[j].SubmitRemote(token);
		}
		if (suffix)
		{
			slot.Poisoned = true;
			submitted = true;
			_submissions->Submit(_deviceId, Queue::Direct, nullptr, 0, {}, suffix);
			slot.Fences[0] = suffix->FenceValue;
			slot.Poisoned = false;
		}
		if (schedule.FrameBoundary)
		{
			_previousCompletion = slot.Fences[0];
			if (suffix || (prefix && frameStart == FrameStart::Ordered)) _lastOpaqueFrame = _previousCompletion;
		}
		if (capture)
			for (auto& sample : capture->Samples) sample.Terminal = _completions.back();
		return slot.Fences[QueueIndex(schedule.TerminalQueue)];
	}
	catch (...)
	{
		if (capture) capture->Samples.clear();
		recorder.Finish();
		if (submitted)
		{
			_aborted = true;
		}
		throw;
	}
}
} // namespace BistroRender
