#include "Engine.RendererDX12/GDX12SnapshotProtocol.h"
#include <algorithm>
#include <stdexcept>

namespace GDX12Snapshot
{
    namespace
    {
        // Different timelines require native Wait edges; the graph/transport validates those.
        bool Follows(CompletionPoint next, CompletionPoint previous)
        {
            return next.IsValid() && (!(next.Timeline() == previous.Timeline()) ||
                next.Value() >= previous.Value());
        }
    }

    Protocol::Protocol(TransferId transfer, Epoch epoch, std::size_t sharedSlots,
        std::size_t localSlots, std::size_t readerCredits)
        : _transfer(transfer), _epoch(epoch), _shared(sharedSlots), _local(localSlots),
        _readers(readerCredits)
    {
        if (transfer == TransferId::Invalid || epoch == Epoch::Invalid || sharedSlots == 0 ||
            localSlots == 0 || readerCredits == 0 || sharedSlots > NoSlot - localSlots)
            throw std::invalid_argument("Invalid snapshot stream or pool capacity");
        _publications.resize(sharedSlots + localSlots);
    }

    Protocol::Publication* Protocol::Find(Ticket publication)
    {
        return const_cast<Publication*>(static_cast<const Protocol*>(this)->Find(publication));
    }

    const Protocol::Publication* Protocol::Find(Ticket publication) const
    {
        if (publication._owner != _identity || publication.Id() == 0) return nullptr;
        const auto found = std::find_if(_publications.begin(), _publications.end(),
            [publication](const Publication& p) { return p.Handle == publication; });
        return found == _publications.end() ? nullptr : &*found;
    }

    bool Protocol::Current(const Publication& p) const
    {
        return !p.Retiring && p.Version.VersionEpoch == _epoch;
    }

    bool Protocol::Pinned(const Publication& p) const
    {
        return p.Exact || _display.Current.Publication == p.Handle ||
            _display.Previous.Publication == p.Handle ||
            std::any_of(_readers.begin(), _readers.end(), [&p](const ReaderState& r)
                { return r.Id != 0 && r.Publication == p.Handle; });
    }

    bool Protocol::SourceDone(const Publication& p) const
    {
        return (p.ProducerDone || p.ProducerCancelled) &&
            (p.ExportReady.IsValid() ? p.SourceDone : p.Retiring && !p.ExportBegun);
    }

    std::size_t Protocol::LocalCredit(bool preserveLatest) const
    {
        const Publication* latest = nullptr;
        if (preserveLatest)
            for (const auto& p : _publications)
                if (p.Handle.Id() != 0 && Current(p) && p.Local != NoSlot && p.ImportDone &&
                    (!latest || static_cast<std::uint64_t>(p.Version.Invocation) >
                        static_cast<std::uint64_t>(latest->Version.Invocation))) latest = &p;
        std::size_t candidate = NoSlot;
        for (std::size_t i = 0; i < _local.size(); ++i)
        {
            if (_local[i].Id() == 0) return i;
            const auto* p = Find(_local[i]);
            // Keep a completed candidate available between frame selections.
            if (p != latest && p->ImportDone && !Pinned(*p) &&
                (candidate == NoSlot || static_cast<std::uint64_t>(p->Version.Invocation) <
                    static_cast<std::uint64_t>(Find(_local[candidate])->Version.Invocation)))
                candidate = i;
        }
        return candidate;
    }

    void Protocol::TakeLocal(Publication& p, std::size_t slot)
    {
        if (auto* previous = Find(_local[slot])) previous->Local = NoSlot;
        _local[slot] = p.Handle;
        p.Local = slot;
    }

    Snapshot Protocol::View(const Publication& p) const
    {
        return { p.Handle, p.Version, p.Local, p.ImportReady };
    }

    void Protocol::Collect()
    {
        for (auto& p : _publications)
        {
            if (p.Handle.Id() == 0) continue;
            const bool pendingProducer = !p.ProducerReady.IsValid() && !p.ProducerCancelled;
            const bool pendingExport = p.ExportBegun && !p.ExportReady.IsValid();
            const bool pendingImport = p.ImportBegun && !p.ImportReady.IsValid();
            const bool sharedDone = p.ImportReady.IsValid() ?
                p.ImportDone && p.SharedReleased : p.Retiring;
            if (p.Shared != NoSlot && !pendingProducer && !pendingExport && !pendingImport &&
                SourceDone(p) && (!p.ExportReady.IsValid() || p.ExportDone) && sharedDone)
            {
                _shared[p.Shared] = {};
                p.Shared = NoSlot;
            }
            if (p.Local != NoSlot && p.Retiring && !pendingImport &&
                (!p.ImportReady.IsValid() || p.ImportDone) && !Pinned(p))
            {
                _local[p.Local] = {};
                p.Local = NoSlot;
            }
            if (p.Shared == NoSlot && p.Local == NoSlot) p = {};
        }
    }

    Reservation Protocol::TryReserve(TransferVersion version, SnapshotReadMode mode)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!version.IsValid() || version.Transfer != _transfer ||
            (mode != SnapshotReadMode::ExactVersion && mode != SnapshotReadMode::LatestCompleted))
            return {};
        if (version.VersionEpoch != _epoch) return { Result::Stale };
        if (static_cast<std::uint64_t>(version.Invocation) <= _lastInvocation) return {};
        if (_deferredExact && !(*_deferredExact == version)) return { Result::Deferred };
        if (_deferredExact && mode != SnapshotReadMode::ExactVersion) return { Result::Deferred };
        if (_nextTicket == (std::numeric_limits<std::uint64_t>::max)()) return {};
        Collect();
        const auto shared = std::find_if(_shared.begin(), _shared.end(),
            [](Ticket t) { return t.Id() == 0; });
        const auto local = mode == SnapshotReadMode::ExactVersion ? LocalCredit(false) : NoSlot;
        if (shared == _shared.end() || (mode == SnapshotReadMode::ExactVersion && local == NoSlot))
        {
            if (mode == SnapshotReadMode::ExactVersion) _deferredExact = version;
            return { Result::Deferred };
        }
        const auto entry = std::find_if(_publications.begin(), _publications.end(),
            [](const Publication& p) { return p.Handle.Id() == 0; });
        if (entry == _publications.end()) return { Result::Deferred };
        entry->Handle = Ticket(_identity, _nextTicket++);
        entry->Version = version;
        entry->Shared = static_cast<std::size_t>(shared - _shared.begin());
        entry->Exact = mode == SnapshotReadMode::ExactVersion;
        *shared = entry->Handle;
        if (entry->Exact) TakeLocal(*entry, local);
        _lastInvocation = static_cast<std::uint64_t>(version.Invocation);
        _deferredExact.reset();
        Collect();
        return { Result::Accepted, entry->Handle, entry->Shared, entry->Local };
    }

    bool Protocol::CancelDeferredExact(TransferVersion version)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_deferredExact || !(*_deferredExact == version)) return false;
        _deferredExact.reset();
        return true;
    }

    bool Protocol::BindProducer(Ticket ticket, CompletionPoint ready)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || p->ProducerCancelled || p->ProducerReady.IsValid() || !ready.IsValid()) return false;
        p->ProducerReady = ready;
        return true;
    }

    std::optional<CompletionPoint> Protocol::BeginExport(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || !Current(*p) || !p->ProducerReady.IsValid() || p->ExportBegun) return {};
        p->ExportBegun = true;
        return p->ProducerReady;
    }

    bool Protocol::BindExport(Ticket ticket, CompletionPoint ready, CompletionPoint sourceComplete)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || !p->ExportBegun || p->ExportReady.IsValid() ||
            !Follows(ready, p->ProducerReady) || !Follows(sourceComplete, p->ProducerReady)) return false;
        p->ExportReady = ready;
        p->SourceCopyComplete = sourceComplete;
        return true;
    }

    ImportPlan Protocol::BeginImport(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p) return {};
        if (!Current(*p)) return { Result::Stale };
        if (p->ImportBegun || !p->ExportReady.IsValid() || (!p->Exact && !p->ExportDone))
            return { Result::NotReady };
        if (p->Local == NoSlot)
        {
            const auto local = LocalCredit(true);
            if (local == NoSlot) return { Result::Deferred };
            TakeLocal(*p, local);
        }
        p->ImportBegun = true;
        return { Result::Accepted, p->Shared, p->Local, p->ExportReady };
    }

    bool Protocol::BindImport(Ticket ticket, CompletionPoint ready, CompletionPoint sharedRelease)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || !p->ImportBegun || p->ImportReady.IsValid() ||
            !Follows(ready, p->ExportReady) || !Follows(sharedRelease, ready)) return false;
        p->ImportReady = ready;
        p->SharedRelease = sharedRelease;
        return true;
    }

    bool Protocol::Cancel(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p) return false;
        p->Retiring = true;
        p->Exact = false;
        if (!p->ProducerReady.IsValid()) p->ProducerCancelled = true;
        if (!p->ExportReady.IsValid()) p->ExportBegun = false;
        if (!p->ImportReady.IsValid()) p->ImportBegun = false;
        Collect();
        return true;
    }

    bool Protocol::DropOptional(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || p->Exact || !p->ExportDone || !SourceDone(*p) || Pinned(*p) ||
            (p->ImportBegun && !p->ImportDone)) return false;
        p->Retiring = true;
        Collect();
        return true;
    }

    bool Protocol::ReleaseExact(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || !p->Exact || !p->ImportReady.IsValid() ||
            !std::any_of(_readers.begin(), _readers.end(), [ticket](const ReaderState& r)
                { return r.Id != 0 && r.Publication == ticket; })) return false;
        p->Exact = false;
        return true;
    }

    DisplayedPair Protocol::SelectLatest(FrameSubmissionId frame)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (frame == FrameSubmissionId::Invalid ||
            static_cast<std::uint64_t>(frame) < static_cast<std::uint64_t>(_frame)) return {};
        if (frame == _frame) return _display;
        const Publication* latest = nullptr;
        for (const auto& p : _publications)
            if (p.Handle.Id() != 0 && Current(p) && p.Local != NoSlot && p.ImportDone &&
                (!latest || static_cast<std::uint64_t>(p.Version.Invocation) >
                    static_cast<std::uint64_t>(latest->Version.Invocation))) latest = &p;
        _frame = frame;
        _display.Previous = _display.Current;
        _display.Current = latest ? View(*latest) : Snapshot{};
        Collect();
        return _display;
    }

    std::optional<Reader> Protocol::AcquireReader(Ticket ticket)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* p = Find(ticket);
        if (!p || !Current(*p) || p->Local == NoSlot || !p->ImportReady.IsValid() ||
            (!p->Exact && !p->ImportDone) ||
            _nextReader == (std::numeric_limits<std::uint64_t>::max)()) return {};
        const auto slot = std::find_if(_readers.begin(), _readers.end(),
            [](const ReaderState& r) { return r.Id == 0; });
        if (slot == _readers.end()) return {};
        *slot = { _nextReader++, ticket, {} };
        return Reader{ slot->Id, View(*p) };
    }

    bool Protocol::BindReader(std::uint64_t reader, CompletionPoint release)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto slot = std::find_if(_readers.begin(), _readers.end(),
            [reader](const ReaderState& r) { return reader != 0 && r.Id == reader; });
        if (slot == _readers.end() || slot->Release.IsValid() ||
            !Follows(release, Find(slot->Publication)->ImportReady)) return false;
        slot->Release = release;
        return true;
    }

    bool Protocol::CancelReader(std::uint64_t reader)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto slot = std::find_if(_readers.begin(), _readers.end(),
            [reader](const ReaderState& r) { return reader != 0 && r.Id == reader; });
        if (slot == _readers.end() || slot->Release.IsValid()) return false;
        *slot = {};
        Collect();
        return true;
    }

    void Protocol::Observe(FenceProgress progress)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& p : _publications)
        {
            p.ProducerDone = p.ProducerDone || p.ProducerReady.IsReachedBy(progress);
            p.ExportDone = p.ExportDone || p.ExportReady.IsReachedBy(progress);
            p.SourceDone = p.SourceDone || p.SourceCopyComplete.IsReachedBy(progress);
            p.ImportDone = p.ImportDone || p.ImportReady.IsReachedBy(progress);
            p.SharedReleased = p.SharedReleased || p.SharedRelease.IsReachedBy(progress);
        }
        for (auto& reader : _readers)
            if (reader.Release.IsReachedBy(progress)) reader = {};
        Collect();
    }

    bool Protocol::Reset(Epoch epoch)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (static_cast<std::uint64_t>(epoch) <= static_cast<std::uint64_t>(_epoch)) return false;
        _epoch = epoch;
        _lastInvocation = 0;
        _deferredExact.reset();
        _display = {};
        for (auto& p : _publications)
            if (p.Handle.Id() != 0) { p.Retiring = true; p.Exact = false; }
        Collect();
        return true;
    }

    bool Protocol::SourceReusable(Ticket ticket) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto* p = Find(ticket);
        return p ? SourceDone(*p) : ticket._owner == _identity && ticket.Id() != 0 && ticket.Id() < _nextTicket;
    }

    Usage Protocol::GetUsage() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        Usage usage;
        for (auto t : _shared) if (t.Id() != 0) ++usage.Shared;
        for (auto t : _local) if (t.Id() != 0) ++usage.Local;
        for (const auto& r : _readers) if (r.Id != 0) ++usage.Readers;
        for (const auto& p : _publications) if (p.Handle.Id() != 0) ++usage.Publications;
        return usage;
    }
}
