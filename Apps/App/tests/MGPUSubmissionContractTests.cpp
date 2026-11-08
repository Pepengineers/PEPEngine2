#include "Engine.RendererDX12/GDX12Submission.h"
#include <type_traits>
#include <iostream>

using namespace GDX12Submission;

// Standalone CPU model of the sole issuer; this executable does not link the DX12 queue.
class GDX12CommandQueue
{
public:
    static constexpr CompletionPoint ModelQueueSignal(TimelineId timeline, std::uint64_t value)
    { return CompletionPoint(timeline, value); }
};

constexpr Placement primary{ AdapterId{1}, QueueKind::Direct };
constexpr TimelineId direct{ { primary, 1 }, 1 };
constexpr TimelineId compute{ { { AdapterId{1}, QueueKind::Compute }, 2 }, 1 };
constexpr TimelineId secondary{ { { AdapterId{2}, QueueKind::Direct }, 1 }, 1 };
constexpr TimelineId replacedFence{ direct.Producer, 2 };
constexpr TimelineId replacedQueue{ { primary, 2 }, 1 };
constexpr auto point = GDX12CommandQueue::ModelQueueSignal(direct, 7);
static_assert(point.IsValid());
static_assert(!CompletionPoint{}.IsValid());
static_assert(!CompletionPoint{}.IsReachedBy({ direct, 100 }));
static_assert(!GDX12CommandQueue::ModelQueueSignal(direct, 0).IsValid());
static_assert(!GDX12CommandQueue::ModelQueueSignal({}, 7).IsValid());
static_assert(!point.IsReachedBy({ direct, 6 }));
static_assert(point.IsReachedBy({ direct, 7 }));
static_assert(point.IsReachedBy({ direct, 8 }));
static_assert(!point.IsReachedBy({ compute, 7 }));
static_assert(!point.IsReachedBy({ secondary, 100 }));
static_assert(!point.IsReachedBy({ replacedFence, 100 }));
static_assert(!point.IsReachedBy({ replacedQueue, 100 }));
static_assert(!point.IsReachedBy({ direct, std::numeric_limits<std::uint64_t>::max() }));
static_assert(!GDX12CommandQueue::ModelQueueSignal(direct,
    std::numeric_limits<std::uint64_t>::max()).IsValid());
static_assert(!std::is_constructible_v<CompletionPoint, TimelineId, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, CompletionPoint>);
static_assert(!std::is_convertible_v<FrameSubmissionId, ProducerInvocationId>);
static_assert(!std::is_convertible_v<TransferId, ResourceId>);
static_assert(!std::is_convertible_v<TransferId, AllocationId>);
static_assert(!Placement{}.IsValid());
static_assert(!Placement{ AdapterId{1}, QueueKind{255} }.IsValid());

constexpr TransferVersion version{ TransferId{3}, Epoch{2}, ProducerInvocationId{9} };
static_assert(version.IsValid());
static_assert(!TransferVersion{}.IsValid());
static_assert(!(version == TransferVersion{ TransferId{3}, Epoch{1}, ProducerInvocationId{9} }));
static_assert(!(version == TransferVersion{ TransferId{4}, Epoch{2}, ProducerInvocationId{9} }));
static_assert(!(version == TransferVersion{ TransferId{3}, Epoch{2}, ProducerInvocationId{10} }));
static_assert(version == TransferVersion{ TransferId{3}, Epoch{2}, ProducerInvocationId{9} });

constexpr ResourceIdentity producerView{ ResourceId{11}, AdapterId{1}, Epoch{2}, TransferId{3}, AllocationId{20} };
constexpr ResourceIdentity importedView{ ResourceId{12}, AdapterId{2}, Epoch{2}, TransferId{3}, AllocationId{20} };
constexpr ResourceIdentity nextRingSlot{ ResourceId{13}, AdapterId{1}, Epoch{2}, TransferId{3}, AllocationId{21} };
static_assert(producerView.Id != importedView.Id && producerView.Allocation == importedView.Allocation);
static_assert(producerView.Transfer == nextRingSlot.Transfer && producerView.Allocation != nextRingSlot.Allocation);

int main()
{
    std::cout << "MGPU submission contract: provenance, device removal, epochs and typed identities passed.\n";
}
