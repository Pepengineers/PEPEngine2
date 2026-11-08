#pragma once

#include <cstdint>
#include <limits>

class GDX12CommandQueue;

namespace GDX12Submission
{
    // Full object/frame identities are never reused during the runtime's lifetime, including reset.
    // Epoch is scoped by TransferId; producer invocation is scoped by (TransferId, Epoch).
    // AdapterId identifies a device incarnation; the physical adapter LUID is separate metadata.
    enum class AdapterId : std::uint64_t { Invalid = 0 };
    enum class ResourceId : std::uint64_t { Invalid = 0 };
    enum class AllocationId : std::uint64_t { Invalid = 0 };
    enum class TransferId : std::uint64_t { Invalid = 0 };
    enum class Epoch : std::uint64_t { Invalid = 0 };
    enum class FrameSubmissionId : std::uint64_t { Invalid = 0 };
    enum class ProducerInvocationId : std::uint64_t { Invalid = 0 };
    enum class QueueKind : std::uint8_t { Direct, Compute, Copy };

    struct Placement
    {
        AdapterId Adapter = AdapterId::Invalid;
        QueueKind Queue = QueueKind::Direct;
        constexpr bool IsValid() const
        {
            return Adapter != AdapterId::Invalid &&
                (Queue == QueueKind::Direct || Queue == QueueKind::Compute || Queue == QueueKind::Copy);
        }
        friend constexpr bool operator==(Placement a, Placement b)
        { return a.Adapter == b.Adapter && a.Queue == b.Queue; }
    };

    struct QueueId
    {
        Placement Location;
        std::uint64_t Instance = 0;
        constexpr bool IsValid() const { return Location.IsValid() && Instance != 0; }
        friend constexpr bool operator==(QueueId a, QueueId b)
        { return a.Location == b.Location && a.Instance == b.Instance; }
    };

    struct TimelineId
    {
        QueueId Producer;
        std::uint64_t Instance = 0;
        constexpr bool IsValid() const { return Producer.IsValid() && Instance != 0; }
        friend constexpr bool operator==(TimelineId a, TimelineId b)
        { return a.Producer == b.Producer && a.Instance == b.Instance; }
    };

    // An imported fence retains the ORIGINAL timeline identity, not the importing queue's.
    struct FenceProgress
    {
        TimelineId Timeline;
        std::uint64_t CompletedValue = 0;
    };

    class CompletionPoint
    {
    public:
        constexpr CompletionPoint() = default;
        constexpr TimelineId Timeline() const { return _timeline; }
        constexpr std::uint64_t Value() const { return _value; }
        constexpr bool IsValid() const
        {
            return _timeline.IsValid() && _value != 0 &&
                _value != (std::numeric_limits<std::uint64_t>::max)();
        }
        constexpr bool IsReachedBy(FenceProgress progress) const
        {
            // UINT64_MAX from GetCompletedValue means device removal, not completion.
            return IsValid() && progress.Timeline == _timeline &&
                progress.CompletedValue != (std::numeric_limits<std::uint64_t>::max)() &&
                progress.CompletedValue >= _value;
        }
    private:
        friend class ::GDX12CommandQueue;
        // Only the owning queue may issue this AFTER a successful queue Signal.
        // CPU ID3D12Fence::Signal must never issue a GPU completion point.
        constexpr CompletionPoint(TimelineId timeline, std::uint64_t value)
            : _timeline(timeline), _value(value) {}
        TimelineId _timeline{};
        std::uint64_t _value = 0;
    };

    // Every native device-local view has its own Id and state tracking.
    // Transfer identifies a logical publication stream, NOT a shared allocation or graph SharedResource.
    struct ResourceIdentity
    {
        ResourceId Id = ResourceId::Invalid;
        AdapterId Adapter = AdapterId::Invalid;
        Epoch ResourceEpoch = Epoch::Invalid;
        TransferId Transfer = TransferId::Invalid;
        // Same backing allocation => same key across adapters. One heap's slots may
        // conservatively share a key; distinct allocations must have distinct keys.
        AllocationId Allocation = AllocationId::Invalid;
    };

    struct TransferVersion
    {
        TransferId Transfer = TransferId::Invalid;
        Epoch VersionEpoch = Epoch::Invalid;
        ProducerInvocationId Invocation = ProducerInvocationId::Invalid;
        constexpr bool IsValid() const
        {
            return Transfer != TransferId::Invalid && VersionEpoch != Epoch::Invalid &&
                Invocation != ProducerInvocationId::Invalid;
        }
        friend constexpr bool operator==(TransferVersion a, TransferVersion b)
        {
            return a.Transfer == b.Transfer && a.VersionEpoch == b.VersionEpoch &&
                a.Invocation == b.Invocation;
        }
    };

    enum class SnapshotReadMode : std::uint8_t { ExactVersion, LatestCompleted };
    // Exact requires a reservation BEFORE publishing a dependency; exhaustion defers submit.
    // Latest selects a completed LOCAL import and never waits merely for a newer version.
    // Ready/export, local import-ready and last-reader release are distinct CompletionPoints.
}
