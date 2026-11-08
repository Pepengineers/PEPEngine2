#include "Engine.RendererDX12/GDX12SnapshotProtocol.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace GDX12Submission;
using namespace GDX12Snapshot;

// Standalone model issuer ONLY: never include/link the real GDX12CommandQueue in this target.
class GDX12CommandQueue
{
public:
    static constexpr CompletionPoint Signal(TimelineId timeline, std::uint64_t value)
    { return CompletionPoint(timeline, value); }
};

namespace
{
    static_assert(!std::is_constructible_v<Ticket, std::uint64_t>);
    constexpr TimelineId producer{ { { AdapterId{2}, QueueKind::Compute }, 1 }, 1 };
    constexpr TimelineId exporter{ { { AdapterId{2}, QueueKind::Copy }, 2 }, 1 };
    constexpr TimelineId sourceRelease{ { { AdapterId{2}, QueueKind::Direct }, 3 }, 1 };
    constexpr TimelineId importer{ { { AdapterId{1}, QueueKind::Copy }, 4 }, 1 };
    constexpr TimelineId sharedRelease{ { { AdapterId{1}, QueueKind::Compute }, 5 }, 1 };
    constexpr TimelineId graphics{ { { AdapterId{1}, QueueKind::Direct }, 6 }, 1 };
    constexpr TimelineId otherReader{ { { AdapterId{3}, QueueKind::Compute }, 7 }, 1 };
    constexpr TimelineId replacedImporter{ importer.Producer, 2 };
    constexpr TransferId transfer{ 17 };
    constexpr Epoch epoch{ 1 };
    constexpr auto removed = (std::numeric_limits<std::uint64_t>::max)();

    void Check(bool condition, const char* expression, int line)
    {
        if (!condition)
            throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
    }
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

    CompletionPoint Point(TimelineId timeline, std::uint64_t value = 1)
    { return GDX12CommandQueue::Signal(timeline, value); }

    TransferVersion Version(std::uint64_t invocation, Epoch e = epoch)
    { return { transfer, e, ProducerInvocationId{invocation} }; }

    Reservation Reserve(Protocol& p, std::uint64_t invocation,
        SnapshotReadMode mode = SnapshotReadMode::LatestCompleted, Epoch e = epoch)
    {
        auto reservation = p.TryReserve(Version(invocation, e), mode);
        CHECK(reservation.Status == Result::Accepted);
        CHECK(reservation.Publication.Id() != 0 && reservation.SharedSlot != NoSlot);
        return reservation;
    }

    void Export(Protocol& p, Ticket ticket, std::uint64_t value = 1, bool completed = true)
    {
        CHECK(p.BindProducer(ticket, Point(producer, value)));
        const auto dependency = p.BeginExport(ticket);
        CHECK(dependency && dependency->Timeline() == producer && dependency->Value() == value);
        CHECK(p.BindExport(ticket, Point(exporter, value), Point(sourceRelease, value)));
        if (completed)
        {
            p.Observe({ producer, value });
            p.Observe({ exporter, value });
            p.Observe({ sourceRelease, value });
        }
    }

    ImportPlan Import(Protocol& p, Ticket ticket, std::uint64_t value = 1, bool completed = true)
    {
        const auto plan = p.BeginImport(ticket);
        CHECK(plan.Status == Result::Accepted && plan.LocalSlot != NoSlot);
        CHECK(plan.ExportReady.Timeline() == exporter && plan.ExportReady.Value() == value);
        CHECK(p.BindImport(ticket, Point(importer, value), Point(sharedRelease, value)));
        if (completed)
        {
            p.Observe({ importer, value });
            p.Observe({ sharedRelease, value });
        }
        return plan;
    }

    void InvalidTransitions()
    {
        bool rejected = false;
        try { Protocol invalid(transfer, epoch, 0, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        Protocol p(transfer, epoch, 1, 1, 1);
        CHECK(p.TryReserve({}, SnapshotReadMode::ExactVersion).Status == Result::Invalid);
        CHECK(p.TryReserve({ TransferId{99}, epoch, ProducerInvocationId{1} },
            SnapshotReadMode::ExactVersion).Status == Result::Invalid);
        CHECK(p.TryReserve(Version(1), SnapshotReadMode{255}).Status == Result::Invalid);
        CHECK(p.TryReserve(Version(1, Epoch{2}), SnapshotReadMode::ExactVersion).Status == Result::Stale);
        const auto r = Reserve(p, 1, SnapshotReadMode::ExactVersion);
        CHECK(r.LocalSlot != NoSlot);
        CHECK(!p.BeginExport(r.Publication));
        CHECK(p.BeginImport(r.Publication).Status == Result::NotReady);
        CHECK(!p.AcquireReader(r.Publication));
        CHECK(!p.BindProducer(r.Publication, {}));
        CHECK(!p.BindProducer(r.Publication, Point(producer, removed)));
        CHECK(!p.BindExport(r.Publication, Point(exporter), Point(sourceRelease)));
        CHECK(!p.BindImport(r.Publication, Point(importer), Point(sharedRelease)));
        CHECK(!p.ReleaseExact(r.Publication));
        CHECK(!p.DropOptional(r.Publication));
        CHECK(!p.Cancel(Ticket{}));
        CHECK(!p.BindReader(0, Point(graphics)) && !p.CancelReader(0));
        CHECK(p.Cancel(r.Publication));
        CHECK(p.GetUsage().Shared == 0 && p.GetUsage().Local == 0);
        CHECK(!p.BindProducer(r.Publication, Point(producer)));
        CHECK(p.TryReserve(Version(1), SnapshotReadMode::ExactVersion).Status == Result::Invalid);
        const auto next = Reserve(p, 3, SnapshotReadMode::ExactVersion);
        CHECK(next.Publication.Id() != r.Publication.Id());
        CHECK(p.BindProducer(next.Publication, Point(producer, 7)));
        CHECK(!p.BindProducer(next.Publication, Point(producer, 8)));
        CHECK(p.BeginExport(next.Publication));
        CHECK(!p.BeginExport(next.Publication));
        CHECK(!p.BindExport(next.Publication, Point(producer, 6), Point(sourceRelease, 7)));
        CHECK(p.BindExport(next.Publication, Point(exporter, 7), Point(sourceRelease, 7)));
        CHECK(!p.BindExport(next.Publication, Point(exporter, 8), Point(sourceRelease, 8)));
        CHECK(p.BeginImport(next.Publication).Status == Result::Accepted);
        CHECK(!p.BindImport(next.Publication, Point(exporter, 6), Point(sharedRelease, 7)));
        CHECK(!p.BindImport(next.Publication, Point(importer, 7), Point(importer, 6)));
        CHECK(p.BindImport(next.Publication, Point(importer, 7), Point(sharedRelease, 7)));
        CHECK(!p.BindImport(next.Publication, Point(importer, 8), Point(sharedRelease, 8)));
        const auto reader = p.AcquireReader(next.Publication);
        CHECK(reader && reader->View.ImportReady.Value() == 7);
        CHECK(!p.AcquireReader(next.Publication));
        CHECK(!p.BindReader(reader->Id, {}));
        CHECK(!p.BindReader(reader->Id, Point(importer, 6)));
        CHECK(p.CancelReader(reader->Id));
        CHECK(!p.CancelReader(reader->Id));
        CHECK(!p.ReleaseExact(next.Publication));
        CHECK(p.Cancel(next.Publication));
        CHECK(p.GetUsage().Shared == 1 && p.GetUsage().Local == 1);
        p.Observe({ producer, 7 }); p.Observe({ exporter, 7 }); p.Observe({ sourceRelease, 7 });
        p.Observe({ importer, 7 }); p.Observe({ sharedRelease, 7 });
        CHECK(p.GetUsage().Publications == 0);
    }

    void ExactBackpressureAndPinning()
    {
        Protocol p(transfer, epoch, 1, 1, 2);
        const auto busy = Reserve(p, 1);
        CHECK(busy.LocalSlot == NoSlot);
        CHECK(p.TryReserve(Version(2), SnapshotReadMode::ExactVersion).Status == Result::Deferred);
        CHECK(p.GetUsage().Shared == 1 && p.GetUsage().Local == 0);
        CHECK(p.TryReserve(Version(3), SnapshotReadMode::LatestCompleted).Status == Result::Deferred);
        CHECK(!p.CancelDeferredExact(Version(3)));
        CHECK(p.Cancel(busy.Publication));
        CHECK(p.TryReserve(Version(3), SnapshotReadMode::LatestCompleted).Status == Result::Deferred);
        const auto exact = Reserve(p, 2, SnapshotReadMode::ExactVersion);
        CHECK(exact.LocalSlot != NoSlot);
        CHECK(!p.DropOptional(exact.Publication));
        CHECK(!p.ReleaseExact(exact.Publication));
        Export(p, exact.Publication, 2, false);
        const auto plan = Import(p, exact.Publication, 2, false);
        CHECK(plan.LocalSlot == exact.LocalSlot);
        CHECK(!p.SelectLatest(FrameSubmissionId{100}).Current.IsValid());
        const auto reader = p.AcquireReader(exact.Publication);
        CHECK(reader && reader->View.Version == Version(2));
        CHECK(reader->View.ImportReady.Timeline() == importer);
        CHECK(p.ReleaseExact(exact.Publication));
        CHECK(p.BindReader(reader->Id, Point(graphics, 2)));
        CHECK(!p.CancelReader(reader->Id));
        p.Observe({ graphics, 2 });
        CHECK(p.GetUsage().Local == 1);
        p.Observe({ producer, 2 }); p.Observe({ exporter, 2 }); p.Observe({ sourceRelease, 2 });
        p.Observe({ importer, 2 }); p.Observe({ sharedRelease, 2 });
        const auto pair = p.SelectLatest(FrameSubmissionId{101});
        CHECK(pair.Current.Version == Version(2));
        const auto next = Reserve(p, 3);
        Export(p, next.Publication, 3);
        CHECK(p.BeginImport(next.Publication).Status == Result::Deferred);
        CHECK(p.TryReserve(Version(4), SnapshotReadMode::ExactVersion).Status == Result::Deferred);
        CHECK(p.CancelDeferredExact(Version(4)));
        CHECK(p.Cancel(next.Publication));
    }

    void DistinctCompletionsAndReaders()
    {
        Protocol p(transfer, epoch, 1, 1, 3);
        const auto r = Reserve(p, 1, SnapshotReadMode::ExactVersion);
        Export(p, r.Publication, 9, false);
        Import(p, r.Publication, 9, false);
        auto a = p.AcquireReader(r.Publication);
        auto b = p.AcquireReader(r.Publication);
        auto cancelled = p.AcquireReader(r.Publication);
        CHECK(a && b && cancelled && !p.AcquireReader(r.Publication));
        CHECK(p.ReleaseExact(r.Publication));
        CHECK(p.BindReader(a->Id, Point(graphics, 9)));
        CHECK(p.BindReader(b->Id, Point(otherReader, 9)));
        CHECK(!p.BindReader(a->Id, Point(graphics, 10)));
        CHECK(!p.SourceReusable(r.Publication));
        p.Observe({ sourceRelease, 9 });
        CHECK(!p.SourceReusable(r.Publication));
        p.Observe({ producer, 9 });
        CHECK(p.SourceReusable(r.Publication));
        p.Observe({ importer, 9 }); p.Observe({ sharedRelease, 9 });
        CHECK(p.GetUsage().Shared == 1); // Import cannot retire an unfinished export.
        p.Observe({ exporter, removed });
        CHECK(p.GetUsage().Shared == 1);
        p.Observe({ exporter, 8 });
        CHECK(p.GetUsage().Shared == 1);
        p.Observe({ exporter, 9 });
        CHECK(p.GetUsage().Shared == 0 && p.GetUsage().Local == 1);
        CHECK(p.Cancel(r.Publication));
        p.Observe({ graphics, 9 });
        CHECK(p.GetUsage().Readers == 2 && p.GetUsage().Local == 1);
        p.Observe({ otherReader, removed });
        p.Observe({ TimelineId{otherReader.Producer, 2}, 1000 });
        CHECK(p.GetUsage().Readers == 2 && p.GetUsage().Local == 1);
        p.Observe({ otherReader, 9 });
        CHECK(p.GetUsage().Readers == 1 && p.GetUsage().Local == 1);
        CHECK(p.CancelReader(cancelled->Id));
        CHECK(p.GetUsage().Local == 0 && p.GetUsage().Publications == 0);
        CHECK(p.SourceReusable(r.Publication));
        CHECK(!p.SourceReusable(Ticket{}));
        Protocol foreign(transfer, epoch);
        const auto foreignTicket = Reserve(foreign, 1).Publication;
        CHECK(foreignTicket.Id() == r.Publication.Id());
        CHECK(!p.SourceReusable(foreignTicket));
        CHECK(!p.BindProducer(foreignTicket, Point(producer, 100)));
        CHECK(!p.Cancel(foreignTicket));
    }

    void LocalExhaustionIsAtomic()
    {
        Protocol p(transfer, epoch, 2, 1);
        const auto old = Reserve(p, 1, SnapshotReadMode::ExactVersion);
        Export(p, old.Publication);
        Import(p, old.Publication);
        const auto reader = p.AcquireReader(old.Publication);
        CHECK(reader && p.ReleaseExact(old.Publication));
        CHECK(p.BindReader(reader->Id, Point(graphics, 40)));
        const auto deferred = p.TryReserve(Version(2), SnapshotReadMode::ExactVersion);
        CHECK(deferred.Status == Result::Deferred && deferred.Publication.Id() == 0);
        CHECK(deferred.SharedSlot == NoSlot && deferred.LocalSlot == NoSlot);
        CHECK(p.GetUsage().Shared == 0 && p.GetUsage().Local == 1);
        CHECK(p.TryReserve(Version(3), SnapshotReadMode::LatestCompleted).Status == Result::Deferred);
        p.Observe({ otherReader, 400 });
        CHECK(p.TryReserve(Version(2), SnapshotReadMode::ExactVersion).Status == Result::Deferred);
        CHECK(p.GetUsage().Shared == 0);
        p.Observe({ graphics, 40 });
        const auto exact = Reserve(p, 2, SnapshotReadMode::ExactVersion);
        CHECK(exact.LocalSlot == old.LocalSlot);
        CHECK(!p.AcquireReader(old.Publication));
        CHECK(!p.DropOptional(exact.Publication));
        CHECK(p.Cancel(exact.Publication));
        CHECK(p.GetUsage().Publications == 0);
    }

    void SourceReuseAfterReclamation()
    {
        Protocol p(transfer, epoch, 1, 1);
        const auto r = Reserve(p, 1);
        Export(p, r.Publication, 1, false);
        CHECK(p.Cancel(r.Publication));
        p.Observe({ producer, 1 }); p.Observe({ exporter, 1 }); p.Observe({ sourceRelease, 1 });
        CHECK(p.GetUsage().Publications == 0);
        // First source reuse query occurs only AFTER Collect erased the publication.
        CHECK(p.SourceReusable(r.Publication));
        CHECK(!p.SourceReusable(Ticket{}));
        Protocol another(transfer, epoch, 1, 1);
        const auto other = Reserve(another, 1);
        CHECK(!another.SourceReusable(r.Publication));
        CHECK(!p.SourceReusable(other.Publication));
    }

    void ResetWithReaders()
    {
        Protocol p(transfer, epoch, 1, 1, 2);
        const auto old = Reserve(p, 1, SnapshotReadMode::ExactVersion);
        Export(p, old.Publication);
        Import(p, old.Publication);
        const auto submitted = p.AcquireReader(old.Publication);
        const auto pending = p.AcquireReader(old.Publication);
        CHECK(submitted && pending && p.ReleaseExact(old.Publication));
        CHECK(p.BindReader(submitted->Id, Point(graphics, 10)));
        CHECK(p.Reset(Epoch{2}));
        CHECK(!p.AcquireReader(old.Publication));
        CHECK(!p.SelectLatest(FrameSubmissionId{900}).Current.IsValid());
        CHECK(p.TryReserve(Version(1, Epoch{2}), SnapshotReadMode::ExactVersion).Status == Result::Deferred);
        CHECK(p.GetUsage().Shared == 0 && p.GetUsage().Local == 1 && p.GetUsage().Readers == 2);
        p.Observe({ graphics, 10 });
        CHECK(p.GetUsage().Local == 1 && p.GetUsage().Readers == 1);
        CHECK(p.BindReader(pending->Id, Point(otherReader, 20)));
        p.Observe({ otherReader, removed });
        CHECK(p.GetUsage().Local == 1);
        p.Observe({ otherReader, 20 });
        CHECK(p.GetUsage().Publications == 0 && p.GetUsage().Readers == 0);
        const auto fresh = Reserve(p, 1, SnapshotReadMode::ExactVersion, Epoch{2});
        CHECK(p.Cancel(fresh.Publication));
    }

    void LatestKeepsCompletedCandidate()
    {
        Protocol latest(transfer, epoch, 2, 1);
        const auto old = Reserve(latest, 1);
        Export(latest, old.Publication);
        Import(latest, old.Publication);
        const auto next = Reserve(latest, 2);
        Export(latest, next.Publication, 2);
        CHECK(latest.BeginImport(next.Publication).Status == Result::Deferred);
        CHECK(latest.SelectLatest(FrameSubmissionId{40}).Current.Version == Version(1));
        CHECK(latest.Cancel(next.Publication));
        CHECK(latest.Reset(Epoch{2}));
        CHECK(latest.GetUsage().Publications == 0);

        Protocol exact(transfer, epoch, 1, 1);
        const auto optional = Reserve(exact, 1);
        Export(exact, optional.Publication);
        Import(exact, optional.Publication);
        const auto demanded = Reserve(exact, 2, SnapshotReadMode::ExactVersion);
        CHECK(!exact.AcquireReader(optional.Publication));
        CHECK(demanded.LocalSlot == 0 && exact.Cancel(demanded.Publication));
        CHECK(exact.GetUsage().Publications == 0);
    }

    void LatestDisplayedHistoryAndEpoch()
    {
        Protocol p(transfer, epoch, 3, 3);
        CHECK(!p.SelectLatest(FrameSubmissionId{1}).Current.IsValid());
        const auto a = Reserve(p, 10);
        Export(p, a.Publication, 10);
        CHECK(p.BeginImport(a.Publication).Status == Result::Accepted);
        CHECK(p.BindImport(a.Publication, Point(importer, 10), Point(sharedRelease, 10)));
        CHECK(!p.AcquireReader(a.Publication));
        p.Observe({ replacedImporter, 100 });
        p.Observe({ importer, removed });
        CHECK(!p.SelectLatest(FrameSubmissionId{2}).Current.IsValid());
        p.Observe({ importer, 10 }); p.Observe({ sharedRelease, 10 });
        auto pair = p.SelectLatest(FrameSubmissionId{3});
        CHECK(pair.Current.Version == Version(10) && !pair.Previous.IsValid());
        const auto b = Reserve(p, 20);
        Export(p, b.Publication, 20, false);
        CHECK(p.BeginImport(b.Publication).Status == Result::NotReady);
        pair = p.SelectLatest(FrameSubmissionId{4});
        CHECK(pair.Current.Version == Version(10) && pair.Previous.Version == Version(10));
        p.Observe({ producer, 20 }); p.Observe({ exporter, 20 }); p.Observe({ sourceRelease, 20 });
        Import(p, b.Publication, 20, false);
        CHECK(!p.AcquireReader(b.Publication));
        CHECK(p.GetUsage().Readers == 0);
        pair = p.SelectLatest(FrameSubmissionId{5});
        CHECK(pair.Current.Version == Version(10) && pair.Previous.Version == Version(10));
        const auto completedReader = p.AcquireReader(pair.Current.Publication);
        CHECK(completedReader && completedReader->View.Version == Version(10));
        CHECK(p.CancelReader(completedReader->Id));
        p.Observe({ importer, 20 }); p.Observe({ sharedRelease, 20 });
        const auto newReader = p.AcquireReader(b.Publication);
        CHECK(newReader && newReader->View.Version == Version(20));
        CHECK(p.CancelReader(newReader->Id));
        CHECK(p.SelectLatest(FrameSubmissionId{5}).Current.Version == Version(10));
        pair = p.SelectLatest(FrameSubmissionId{6});
        CHECK(pair.Current.Version == Version(20) && pair.Previous.Version == Version(10));
        CHECK(!p.DropOptional(a.Publication));
        const auto previousReader = p.AcquireReader(a.Publication);
        CHECK(previousReader && p.BindReader(previousReader->Id, Point(graphics, 30)));
        pair = p.SelectLatest(FrameSubmissionId{7});
        CHECK(pair.Current.Version == Version(20) && pair.Previous.Version == Version(20));
        CHECK(!p.DropOptional(a.Publication));
        p.Observe({ graphics, 30 });
        CHECK(p.DropOptional(a.Publication));
        CHECK(p.Reset(Epoch{2}));
        CHECK(!p.Reset(epoch) && !p.Reset(Epoch{2}));
        CHECK(!p.AcquireReader(b.Publication));
        CHECK(!p.SelectLatest(FrameSubmissionId{8}).Current.IsValid());
        CHECK(p.TryReserve(Version(21), SnapshotReadMode::ExactVersion).Status == Result::Stale);
        const auto fresh = Reserve(p, 1, SnapshotReadMode::ExactVersion, Epoch{2});
        CHECK(fresh.Publication.Id() != b.Publication.Id());
        CHECK(p.Cancel(fresh.Publication));
    }

    void ResetInFlightAndOptionalDrop()
    {
        Protocol p(transfer, epoch, 1, 1);
        const auto r = Reserve(p, 1);
        CHECK(!p.DropOptional(r.Publication));
        Export(p, r.Publication, 1, false);
        CHECK(!p.DropOptional(r.Publication));
        p.Observe({ producer, 1 }); p.Observe({ exporter, 1 });
        CHECK(!p.DropOptional(r.Publication));
        p.Observe({ sourceRelease, 1 });
        CHECK(p.DropOptional(r.Publication));
        const auto pendingProducer = Reserve(p, 2, SnapshotReadMode::ExactVersion);
        CHECK(p.Reset(Epoch{2}));
        CHECK(p.GetUsage().Shared == 1);
        CHECK(p.TryReserve(Version(1, Epoch{2}), SnapshotReadMode::ExactVersion).Status == Result::Deferred);
        CHECK(p.BindProducer(pendingProducer.Publication, Point(producer, 2)));
        CHECK(!p.BeginExport(pendingProducer.Publication));
        p.Observe({ producer, removed });
        CHECK(p.GetUsage().Shared == 1);
        p.Observe({ producer, 2 });
        CHECK(p.GetUsage().Shared == 0 && p.GetUsage().Local == 0);
        const auto pendingExport = Reserve(p, 1, SnapshotReadMode::ExactVersion, Epoch{2});
        CHECK(p.BindProducer(pendingExport.Publication, Point(producer, 3)));
        CHECK(p.BeginExport(pendingExport.Publication));
        CHECK(p.Reset(Epoch{3}));
        p.Observe({ producer, 3 });
        CHECK(p.GetUsage().Shared == 1);
        CHECK(p.BindExport(pendingExport.Publication, Point(exporter, 3), Point(sourceRelease, 3)));
        CHECK(p.BeginImport(pendingExport.Publication).Status == Result::Stale);
        p.Observe({ exporter, 3 });
        CHECK(p.GetUsage().Shared == 1);
        p.Observe({ sourceRelease, 3 });
        CHECK(p.GetUsage().Shared == 0);
        const auto pendingImport = Reserve(p, 1, SnapshotReadMode::ExactVersion, Epoch{3});
        Export(p, pendingImport.Publication, 4);
        CHECK(p.BeginImport(pendingImport.Publication).Status == Result::Accepted);
        CHECK(p.Reset(Epoch{4}));
        CHECK(p.GetUsage().Shared == 1 && p.GetUsage().Local == 1);
        CHECK(p.BindImport(pendingImport.Publication, Point(importer, 4), Point(sharedRelease, 4)));
        p.Observe({ replacedImporter, 100 }); p.Observe({ importer, removed });
        CHECK(p.GetUsage().Shared == 1 && p.GetUsage().Local == 1);
        p.Observe({ importer, 4 });
        CHECK(p.GetUsage().Shared == 1 && p.GetUsage().Local == 0);
        p.Observe({ sharedRelease, 4 });
        CHECK(p.GetUsage().Publications == 0);
        const auto unsubmitted = Reserve(p, 1, SnapshotReadMode::ExactVersion, Epoch{4});
        CHECK(p.Reset(Epoch{5}));
        CHECK(p.GetUsage().Shared == 1);
        CHECK(p.Cancel(unsubmitted.Publication));
        CHECK(p.GetUsage().Publications == 0);
    }

    struct Payload
    {
        TransferVersion Version;
        std::array<std::uint64_t, 19> Words{};
        std::uint64_t Checksum = 0;
    };

    Payload Pattern(TransferVersion version)
    {
        Payload payload;
        payload.Version = version;
        for (std::size_t i = 0; i < payload.Words.size(); ++i)
        {
            payload.Words[i] = static_cast<std::uint64_t>(version.Invocation) * 101 +
                static_cast<std::uint64_t>(version.VersionEpoch) * 13 + i;
            payload.Checksum ^= payload.Words[i];
        }
        return payload;
    }

    void CheckPayload(const Payload& payload, TransferVersion version)
    {
        const auto expected = Pattern(version);
        CHECK(payload.Version == version && payload.Words == expected.Words &&
            payload.Checksum == expected.Checksum);
    }

    // Event-driven fake copies exercise lifetime, without linking native queues or running a GPU.
    void DifferentRates(int producerPeriod, int consumerPeriod, int exportDelay, int importDelay, int readerDelay)
    {
        Protocol p(transfer, epoch, 3, 3, 32);
        struct Work
        {
            Reservation ReservationInfo;
            TransferVersion Version;
            int ExportAt = 0, ImportAt = 0;
            ImportPlan Plan;
            bool Exported = false, Imported = false;
        };
        struct Read
        {
            Reader Lease;
            int ReleaseAt = 0;
            TimelineId Timeline;
        };
        std::array<Payload, 3> shared{}, local{};
        std::vector<Work> work;
        std::vector<Read> reads;
        std::uint64_t invocation = 0, frame = 4000, readerValue = 0;
        std::uint64_t firstSelected = 0, lastSelected = 0;
        int deferred = 0, repeated = 0, selected = 0;
        Ticket lastDisplayed;
        for (int tick = 1; tick <= 420; ++tick)
        {
            for (auto it = reads.begin(); it != reads.end();)
            {
                CheckPayload(local[it->Lease.View.LocalSlot], it->Lease.View.Version);
                if (tick >= it->ReleaseAt)
                {
                    // Per-timeline values are issued and completed in the same order.
                    p.Observe({ it->Timeline, it->Lease.Id });
                    it = reads.erase(it);
                }
                else ++it;
            }
            for (auto& w : work)
            {
                const auto value = static_cast<std::uint64_t>(w.Version.Invocation);
                if (!w.Exported && tick >= w.ExportAt)
                {
                    shared[w.ReservationInfo.SharedSlot] = Pattern(w.Version);
                    p.Observe({ producer, value });
                    p.Observe({ exporter, value });
                    p.Observe({ sourceRelease, value });
                    w.Exported = true;
                }
                if (w.Exported && w.Plan.Status != Result::Accepted)
                {
                    w.Plan = p.BeginImport(w.ReservationInfo.Publication);
                    if (w.Plan.Status == Result::Accepted)
                    {
                        CheckPayload(shared[w.Plan.SharedSlot], w.Version);
                        CHECK(p.BindImport(w.ReservationInfo.Publication,
                            Point(importer, value), Point(sharedRelease, value)));
                        w.ImportAt = tick + importDelay;
                    }
                }
                if (!w.Imported && w.Plan.Status == Result::Accepted && tick >= w.ImportAt)
                {
                    CheckPayload(shared[w.Plan.SharedSlot], w.Version);
                    local[w.Plan.LocalSlot] = shared[w.Plan.SharedSlot];
                    p.Observe({ importer, value }); p.Observe({ sharedRelease, value });
                    w.Imported = true;
                }
            }
            if (tick % consumerPeriod == 0)
            {
                const auto pair = p.SelectLatest(FrameSubmissionId{++frame});
                if (pair.Current.IsValid())
                {
                    ++selected;
                    if (firstSelected == 0) firstSelected = static_cast<std::uint64_t>(pair.Current.Version.Invocation);
                    lastSelected = static_cast<std::uint64_t>(pair.Current.Version.Invocation);
                    if (pair.Current.Publication == lastDisplayed) ++repeated;
                    lastDisplayed = pair.Current.Publication;
                    CheckPayload(local[pair.Current.LocalSlot], pair.Current.Version);
                    for (const auto& view : { pair.Current, pair.Previous })
                    {
                        if (!view.IsValid()) continue;
                        const auto reader = p.AcquireReader(view.Publication);
                        CHECK(reader);
                        CheckPayload(local[reader->View.LocalSlot], reader->View.Version);
                        const auto timeline = (++readerValue % 2 == 0) ? graphics : otherReader;
                        CHECK(p.BindReader(reader->Id, Point(timeline, reader->Id)));
                        reads.push_back({ *reader, tick + readerDelay, timeline });
                    }
                }
            }
            if (tick % producerPeriod == 0)
            {
                const auto version = Version(++invocation);
                const auto r = p.TryReserve(version, SnapshotReadMode::LatestCompleted);
                if (r.Status == Result::Accepted)
                {
                    CHECK(p.BindProducer(r.Publication, Point(producer, invocation)));
                    CHECK(p.BeginExport(r.Publication));
                    CHECK(p.BindExport(r.Publication, Point(exporter, invocation), Point(sourceRelease, invocation)));
                    work.push_back({ r, version, tick + exportDelay, 0, {}, false, false });
                }
                else { CHECK(r.Status == Result::Deferred); ++deferred; }
            }
            for (const auto& reader : reads)
                CheckPayload(local[reader.Lease.View.LocalSlot], reader.Lease.View.Version);
            const auto usage = p.GetUsage();
            CHECK(usage.Shared <= 3 && usage.Local <= 3 && usage.Readers <= 32 && usage.Publications <= 6);
        }
        std::cout << "Rate fixture producer/consumer=" << producerPeriod << '/' << consumerPeriod
            << " export/import/reader-delay=" << exportDelay << '/' << importDelay << '/' << readerDelay
            << " selected=" << selected << " repeated=" << repeated << " deferred=" << deferred
            << " first/last-invocation=" << firstSelected << '/' << lastSelected << '\n';
        CHECK(selected > 10);
        CHECK(lastSelected > firstSelected);
        if (producerPeriod > consumerPeriod) CHECK(repeated > 0);
        if (producerPeriod < consumerPeriod || exportDelay + importDelay > producerPeriod * 3)
            CHECK(deferred > 0);
        CHECK(frame - 4000 == static_cast<std::uint64_t>(420 / consumerPeriod));
        CHECK(invocation == static_cast<std::uint64_t>(420 / producerPeriod));
    }
}

int main()
{
    try
    {
        InvalidTransitions();
        ExactBackpressureAndPinning();
        DistinctCompletionsAndReaders();
        LocalExhaustionIsAtomic();
        SourceReuseAfterReclamation();
        ResetWithReaders();
        LatestKeepsCompletedCandidate();
        LatestDisplayedHistoryAndEpoch();
        ResetInFlightAndOptionalDrop();
        DifferentRates(1, 5, 2, 3, 9);
        DifferentRates(7, 1, 2, 2, 5);
        DifferentRates(2, 3, 11, 13, 17);
        std::cout << "MGPU snapshot protocol: Exact/Latest, backpressure, epochs, distinct completions, "
            "multi-reader retirement and immutable payload rate fixtures passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MGPU snapshot protocol FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
