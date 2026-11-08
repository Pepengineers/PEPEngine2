#pragma once

#include "GDX12Submission.h"
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace GDX12Snapshot
{
    using namespace GDX12Submission;
    constexpr std::size_t NoSlot = (std::numeric_limits<std::size_t>::max)();

    class Protocol;
    class Ticket
    {
    public:
        Ticket() = default;
        std::uint64_t Id() const { return _id; }
        friend bool operator==(const Ticket& a, const Ticket& b)
        { return a._owner == b._owner && a._id == b._id; }
    private:
        friend class Protocol;
        struct Identity {};
        Ticket(std::shared_ptr<const Identity> owner, std::uint64_t id) : _owner(owner), _id(id) {}
        // Retained by tickets so even destruction/reconstruction cannot reuse owner identity.
        std::shared_ptr<const Identity> _owner;
        std::uint64_t _id = 0;
    };

    enum class Result { Accepted, Deferred, Invalid, Stale, NotReady };

    // A symbolic dependency, never a fabricated GPU completion point.
    struct Reservation
    {
        Result Status = Result::Invalid;
        Ticket Publication;
        std::size_t SharedSlot = NoSlot;
        std::size_t LocalSlot = NoSlot;
    };

    struct ImportPlan
    {
        Result Status = Result::Invalid;
        std::size_t SharedSlot = NoSlot;
        std::size_t LocalSlot = NoSlot;
        CompletionPoint ExportReady;
    };

    struct Snapshot
    {
        Ticket Publication;
        TransferVersion Version;
        std::size_t LocalSlot = NoSlot;
        CompletionPoint ImportReady;
        bool IsValid() const { return Publication.Id() != 0 && ImportReady.IsValid(); }
    };

    struct DisplayedPair
    {
        Snapshot Current;
        Snapshot Previous;
    };

    struct Reader
    {
        std::uint64_t Id = 0;
        Snapshot View;
    };

    struct Usage
    {
        std::size_t Shared = 0;
        std::size_t Local = 0;
        std::size_t Readers = 0;
        std::size_t Publications = 0;
    };

    // One stream, one compatibility epoch, bounded slots and bounded outstanding readers.
    // Payload/layout and native submissions belong to the technique/transport. Serialize native
    // submit+Bind with Reset/Cancel; Cancel certifies that any unbound work was NOT submitted.
    class Protocol
    {
    public:
        Protocol(TransferId transfer, Epoch epoch, std::size_t sharedSlots = 3,
            std::size_t localSlots = 3, std::size_t readerCredits = 32);

        // Successful invocation IDs increase within an epoch. Deferred Exact retains priority
        // until retried or explicitly cancelled; no dependency is returned without both credits.
        Reservation TryReserve(TransferVersion version, SnapshotReadMode mode);
        bool CancelDeferredExact(TransferVersion version);
        bool BindProducer(Ticket publication, CompletionPoint producerReady);
        std::optional<CompletionPoint> BeginExport(Ticket publication);
        bool BindExport(Ticket publication, CompletionPoint exportReady, CompletionPoint sourceCopyComplete);
        ImportPlan BeginImport(Ticket publication);
        bool BindImport(Ticket publication, CompletionPoint importReady, CompletionPoint sharedRelease);

        // Explicit cancellation is allowed for Exact before dependent submit; submitted work retires.
        bool Cancel(Ticket publication);
        bool DropOptional(Ticket publication);
        bool ReleaseExact(Ticket publication);

        // Same frame returns the same pair even if Observe completes a newer import meanwhile.
        // Acquire readers for all passes (including previous-state reads) before advancing frames.
        DisplayedPair SelectLatest(FrameSubmissionId frame);
        std::optional<Reader> AcquireReader(Ticket publication);
        bool BindReader(std::uint64_t reader, CompletionPoint release);
        bool CancelReader(std::uint64_t reader);

        void Observe(FenceProgress progress);
        bool Reset(Epoch epoch);
        // Also true for a retired ticket issued by this instance; unknown tickets return false.
        bool SourceReusable(Ticket publication) const;
        Usage GetUsage() const;

    private:
        struct Publication
        {
            Ticket Handle;
            TransferVersion Version;
            std::size_t Shared = NoSlot;
            std::size_t Local = NoSlot;
            bool Exact = false;
            bool Retiring = false;
            bool ProducerCancelled = false;
            bool ExportBegun = false;
            bool ImportBegun = false;
            CompletionPoint ProducerReady, ExportReady, SourceCopyComplete, ImportReady, SharedRelease;
            bool ProducerDone = false, ExportDone = false, SourceDone = false;
            bool ImportDone = false, SharedReleased = false;
        };
        struct ReaderState
        {
            std::uint64_t Id = 0;
            Ticket Publication;
            CompletionPoint Release;
        };

        Publication* Find(Ticket publication);
        const Publication* Find(Ticket publication) const;
        bool Current(const Publication& publication) const;
        bool Pinned(const Publication& publication) const;
        bool SourceDone(const Publication& publication) const;
        std::size_t LocalCredit(bool preserveLatest) const;
        void TakeLocal(Publication& publication, std::size_t slot);
        Snapshot View(const Publication& publication) const;
        void Collect();

        TransferId _transfer;
        Epoch _epoch;
        const std::shared_ptr<const Ticket::Identity> _identity = std::make_shared<const Ticket::Identity>();
        std::uint64_t _lastInvocation = 0, _nextTicket = 1, _nextReader = 1;
        std::optional<TransferVersion> _deferredExact;
        std::vector<Ticket> _shared, _local;
        std::vector<Publication> _publications;
        std::vector<ReaderState> _readers;
        FrameSubmissionId _frame = FrameSubmissionId::Invalid;
        DisplayedPair _display;
        mutable std::mutex _mutex;
    };
}
