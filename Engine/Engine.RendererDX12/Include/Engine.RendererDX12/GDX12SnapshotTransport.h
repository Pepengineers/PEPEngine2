#pragma once

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12SnapshotProtocol.h"
#include <array>

namespace GDX12Snapshot
{
    constexpr std::size_t SlotCount = 3;

    struct SlotIds
    {
        ResourceId Export = ResourceId::Invalid;
        ResourceId Import = ResourceId::Invalid;
        ResourceId Local = ResourceId::Invalid;
        AllocationId SharedBacking = AllocationId::Invalid;
        AllocationId LocalBacking = AllocationId::Invalid;
    };
    using PoolIds = std::array<SlotIds, SlotCount>;

    struct PhysicalView
    {
        std::shared_ptr<GDX12Resource> Resource;
        ResourceIdentity Identity;
    };

    struct SlotViews
    {
        PhysicalView Export, Import, Local;
    };

    struct CopyReceipt
    {
        Result Status = Result::Invalid;
        CompletionPoint Ready;
        CompletionPoint Release;
    };

    struct ReadLease
    {
        Reader Lease;
        PhysicalView View;
    };

    struct PreparedRead
    {
        std::uint64_t Id = 0;
        PhysicalView View;
        TransferVersion Version;
    };

    // External serialization covers this object and its P1 queues. Declared caller queues
    // and devices must remain alive and registered through retirement and teardown.
    class Transport
    {
    public:
        Transport(GDX12Device& producer, GDX12Device& consumer, GDX12CommandQueue& exportQueue,
            GDX12CommandQueue& importQueue, TransferId transfer, Epoch epoch,
            const D3D12_RESOURCE_DESC& desc, const PoolIds& ids);
        ~Transport();
        Transport(const Transport&) = delete;
        Transport& operator=(const Transport&) = delete;

        const std::array<SlotViews, SlotCount>& GetSlots() const { return _views; }
        Usage GetUsage() const;

        // Reserve before submitting producer work. Source and local readers hand off in COMMON.
        Reservation Reserve(TransferVersion version, SnapshotReadMode mode, GDX12CommandQueue& producerQueue,
            const GDX12ResourceHandle& source, AllocationId sourceAllocation);
        bool BindProducer(Ticket ticket, CompletionPoint producerReady);
        // Graph preparation and receipt binding are one serialized transaction. A partial
        // native submit faults the bank; Cancel is only for known unsubmitted stages.
        bool PrepareExact(Ticket ticket);
        std::optional<PreparedRead> PrepareExactReader(Ticket ticket, GDX12CommandQueue& queue);
        std::optional<CompletionPoint> PrepareExport(Ticket ticket);
        ImportPlan PrepareImport(Ticket ticket);
        void RecordExportCopies(Ticket ticket, ID3D12GraphicsCommandList& list) const;
        void RecordImportCopies(Ticket ticket, ID3D12GraphicsCommandList& list) const;
        void CommitExact(Ticket ticket, CompletionPoint produced, CompletionPoint exported,
            CompletionPoint sourceCopied, CompletionPoint imported, CompletionPoint sharedReleased);
        void CommitExport(Ticket ticket, CompletionPoint exported, CompletionPoint sourceCopied);
        void CommitImport(Ticket ticket, CompletionPoint imported, CompletionPoint sharedReleased);
        void FaultSubmission() noexcept { _faulted = true; }
        CopyReceipt SubmitExport(Ticket ticket);
        CopyReceipt SubmitImport(Ticket ticket);
        bool SourceReusable(Ticket ticket) const { return !_faulted && _protocol.SourceReusable(ticket); }
        bool Cancel(Ticket ticket);
        bool CancelDeferredExact(TransferVersion version);
        bool DropOptional(Ticket ticket);
        bool ReleaseExact(Ticket ticket);

        DisplayedPair SelectLatest(FrameSubmissionId frame);
        std::optional<ReadLease> AcquireReader(Ticket ticket, GDX12CommandQueue& queue);
        bool BindReader(std::uint64_t lease, CompletionPoint release);
        bool CancelReader(std::uint64_t lease);
        void Poll();

        // Reset invalidates selection immediately. Reconfigure waits for no CPU/GPU work;
        // the caller retries after Poll and retires graph registrations before replacing views.
        bool Reset(Epoch epoch);
        bool TryReconfigure(const D3D12_RESOURCE_DESC& desc, const PoolIds& freshIds);
        void Shutdown();

    private:
        static constexpr std::size_t ReaderCount = 32;
        struct Slot
        {
            ComPtr<ID3D12Heap> ExportHeap, ImportHeap;
            GDX12Resource Export, Import, Local;
        };
        struct QueueUse
        {
            GDX12CommandQueue* Queue = nullptr;
            TimelineId Timeline;
            std::uint64_t Before = 0;
            CompletionPoint Done;
        };
        struct SourceUse
        {
            Ticket Publication;
            GDX12ResourceHandle Resource;
            AllocationId Allocation = AllocationId::Invalid;
            QueueUse Producer;
            std::size_t Local = NoSlot;
            TransferVersion Version;
            bool ExactPrepared = false;
            std::optional<std::uint64_t> ExportBefore;
        };
        struct PreparedImport
        {
            Ticket Publication;
            ImportPlan Plan;
            std::uint64_t Before = 0;
        };
        struct ReaderUse
        {
            std::uint64_t Lease = 0;
            std::uint64_t ModelLease = 0;
            Ticket Publication;
            QueueUse Reader;
            bool Prepared = false;
        };

        void RequireOpen();
        void Configure(const D3D12_RESOURCE_DESC& desc, const PoolIds& ids);
        void CheckSource(const GDX12ResourceHandle& source) const;
        void ReapSources();
        std::size_t SourceSlot(Ticket ticket) const;
        std::size_t ImportSlot(Ticket ticket) const;
        ReaderUse* FreeReader();
        [[noreturn]] void RejectReceipt(const char* message);
        FenceProgress Observe(const QueueUse& use);
        void Copy(ID3D12GraphicsCommandList& list, ID3D12Resource* shared, ID3D12Resource* local,
            bool exporting) const;

        GDX12Device& _producer;
        GDX12Device& _consumer;
        GDX12CommandQueue& _export;
        GDX12CommandQueue& _import;
        const TimelineId _exportTimeline, _importTimeline;
        const AdapterId _producerId, _consumerId;
        const TransferId _transfer;
        Epoch _epoch;
        Protocol _protocol;
        ProducerInvocationId _lastInvocation = ProducerInvocationId::Invalid;
        std::optional<TransferVersion> _deferredExact;
        std::array<SlotViews, SlotCount> _views;
        std::array<SourceUse, SlotCount> _sources;
        std::array<PreparedImport, SlotCount> _imports;
        std::array<ReaderUse, ReaderCount> _readers;
        std::uint64_t _nextReader = 1;
        D3D12_RESOURCE_DESC _desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> _footprints;
        bool _resetting = false;
        bool _faulted = false;
        bool _closed = false;
    };
}
