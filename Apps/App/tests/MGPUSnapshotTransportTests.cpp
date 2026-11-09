#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <d3d12sdklayers.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace GDX12Snapshot;

namespace
{
    constexpr TransferId stream{1};
    constexpr Epoch firstEpoch{1}, secondEpoch{2};
    constexpr auto exact = SnapshotReadMode::ExactVersion;
    constexpr auto latest = SnapshotReadMode::LatestCompleted;

    class TestApp final : public App
    {
    public:
        TestApp() : App(GetModuleHandleW(nullptr)) {}
    };

    void Check(bool condition, const char* expression, int line)
    {
        if (!condition)
            throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

    template<class F> void Reject(F action)
    {
        bool rejected = false;
        try { action(); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }

    template<class F> void Await(F done, const char* operation)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(operation);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool Reached(GDX12CommandQueue& queue, CompletionPoint point)
    {
        const auto progress = queue.GetCompletionProgress(point.Timeline());
        if (progress.CompletedValue == (std::numeric_limits<UINT64>::max)())
            ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return point.IsReachedBy(progress);
    }

    // The CPU signals only this test gate, never a P1 completion timeline.
    struct Gate
    {
        ComPtr<ID3D12Fence> Fence;
        explicit Gate(GDX12Device& device)
        { ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence))); }
        ~Gate() { Fence->Signal(1); }
        void Hold(GDX12CommandQueue& queue) { ThrowIfFailed(queue.GetCommandQueue()->Wait(Fence.Get(), 1)); }
        void Open() { ThrowIfFailed(Fence->Signal(1)); }
    };

    struct CopyQueues
    {
        GDX12CommandQueue Export, Import;
        CopyQueues(GDX12Device& producer, GDX12Device& consumer)
            : Export(&producer, D3D12_COMMAND_LIST_TYPE_COPY, true),
              Import(&consumer, D3D12_COMMAND_LIST_TYPE_COPY) {}
    };

    PoolIds Ids(std::uint64_t base)
    {
        PoolIds ids{};
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto id = base + i * 8;
            ids[i] = { ResourceId{id}, ResourceId{id + 1}, ResourceId{id + 2},
                AllocationId{id + 3}, AllocationId{id + 4} };
        }
        return ids;
    }

    TransferVersion Version(std::uint64_t sequence, Epoch epoch = firstEpoch)
    { return { stream, epoch, ProducerInvocationId{sequence} }; }

    void CheckViews(const Transport& transport, GDX12Device& producer, GDX12Device& consumer,
        const PoolIds& ids, Epoch epoch)
    {
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto& slot = transport.GetSlots()[i];
            CHECK(slot.Export.Resource && slot.Import.Resource && slot.Local.Resource);
            CHECK(slot.Export.Resource->D3DResource.Get() != slot.Import.Resource->D3DResource.Get());
            CHECK(slot.Export.Identity.Id == ids[i].Export && slot.Import.Identity.Id == ids[i].Import);
            CHECK(slot.Local.Identity.Id == ids[i].Local);
            CHECK(slot.Export.Identity.Allocation == ids[i].SharedBacking &&
                slot.Import.Identity.Allocation == ids[i].SharedBacking &&
                slot.Local.Identity.Allocation == ids[i].LocalBacking);
            CHECK(slot.Export.Identity.Adapter == producer.GetSubmissionAdapterId());
            CHECK(slot.Import.Identity.Adapter == consumer.GetSubmissionAdapterId() &&
                slot.Local.Identity.Adapter == consumer.GetSubmissionAdapterId());
            CHECK(slot.Export.Identity.ResourceEpoch == epoch && slot.Import.Identity.ResourceEpoch == epoch &&
                slot.Local.Identity.ResourceEpoch == epoch && slot.Local.Identity.Transfer == stream);
            CHECK(slot.Local.Resource->GetCurrentState() == D3D12_RESOURCE_STATE_COMMON);
        }
    }

    struct PayloadLayout
    {
        D3D12_RESOURCE_DESC Desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
        std::vector<UINT> Rows;
        std::vector<UINT64> RowBytes;
        UINT64 Bytes = 0;

        PayloadLayout(GDX12Device& producer, D3D12_RESOURCE_DESC desc) : Desc(desc)
        {
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            {
                Bytes = desc.Width;
                Rows = {1};
                RowBytes = {Bytes};
                return;
            }
            const UINT count = static_cast<UINT>(desc.MipLevels) * desc.DepthOrArraySize;
            Footprints.resize(count);
            Rows.resize(count);
            RowBytes.resize(count);
            producer.GetDevice()->GetCopyableFootprints(&Desc, 0, count, 0, Footprints.data(),
                Rows.data(), RowBytes.data(), &Bytes);
            CHECK(Bytes && Bytes != (std::numeric_limits<UINT64>::max)());
        }

        UINT64 RowOffset(std::size_t subresource, UINT row) const
        {
            if (Footprints.empty()) return 0;
            const auto& placed = Footprints[subresource];
            return placed.Offset + static_cast<UINT64>(row) * placed.Footprint.RowPitch;
        }

        void Copy(ID3D12GraphicsCommandList& list, ID3D12Resource* packed, ID3D12Resource* local, bool unpack) const
        {
            if (Footprints.empty())
            {
                list.CopyBufferRegion(unpack ? local : packed, 0, unpack ? packed : local, 0, Bytes);
                return;
            }
            for (UINT i = 0; i < Footprints.size(); ++i)
            {
                const CD3DX12_TEXTURE_COPY_LOCATION buffer(packed, Footprints[i]);
                const CD3DX12_TEXTURE_COPY_LOCATION texture(local, i);
                list.CopyTextureRegion(unpack ? &texture : &buffer, 0, 0, 0,
                    unpack ? &buffer : &texture, nullptr);
            }
        }
    };

    BYTE Pattern(TransferVersion version, std::size_t subresource, UINT row, UINT64 col)
    {
        const auto sequence = static_cast<UINT64>(version.Invocation);
        const auto epoch = static_cast<UINT64>(version.VersionEpoch);
        if (subresource == 0 && row == 0 && col < 16)
            return static_cast<BYTE>((col < 8 ? sequence : epoch) >> ((col % 8) * 8));
        return static_cast<BYTE>(sequence * 113 + epoch * 37 + subresource * 43 + row * 67 + col * 29);
    }

    struct Data
    {
        GDX12ResourceHandle Source, Upload, Readback;
        Data(GDX12Device& producer, GDX12Device& consumer, const PayloadLayout& layout)
            : Source(producer.CreateResource(D3D12_HEAP_TYPE_DEFAULT, layout.Desc, D3D12_RESOURCE_STATE_COMMON)),
              Upload(producer.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(layout.Bytes),
                  D3D12_RESOURCE_STATE_GENERIC_READ)),
              Readback(consumer.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(layout.Bytes),
                  D3D12_RESOURCE_STATE_COPY_DEST)) {}
    };

    CompletionPoint Write(GDX12CommandQueue& queue, const PayloadLayout& layout, const Data& data, TransferVersion version)
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        ThrowIfFailed(data.Upload->Map(0, &noRead, &mapped));
        auto* bytes = static_cast<BYTE*>(mapped);
        for (std::size_t s = 0; s < layout.Rows.size(); ++s)
            for (UINT row = 0; row < layout.Rows[s]; ++row)
                for (UINT64 col = 0; col < layout.RowBytes[s]; ++col)
                    bytes[layout.RowOffset(s, row) + col] = Pattern(version, s, row, col);
        data.Upload->Unmap(0, nullptr);
        auto* list = queue.GetCommandList();
        layout.Copy(*list->GetCommandList().Get(), data.Upload.Get(), data.Source.Get(), true);
        queue.ExecuteCommandList(list);
        return queue.GetLastCompletionPoint();
    }

    CompletionPoint Read(Transport& transport, GDX12CommandQueue& queue, const PayloadLayout& layout,
        const ReadLease& reader, const GDX12ResourceHandle& readback)
    {
        CHECK(reader.View.Identity.ResourceEpoch == reader.Lease.View.Version.VersionEpoch);
        queue.WaitFor(reader.Lease.View.ImportReady);
        auto* list = queue.GetCommandList();
        layout.Copy(*list->GetCommandList().Get(), readback.Get(), reader.View.Resource->D3DResource.Get(), false);
        queue.ExecuteCommandList(list);
        const auto release = queue.GetLastCompletionPoint();
        CHECK(transport.BindReader(reader.Lease.Id, release));
        return release;
    }

    void Verify(const PayloadLayout& layout, const GDX12ResourceHandle& readback, TransferVersion version)
    {
        void* mapped = nullptr;
        const D3D12_RANGE read{0, layout.Bytes}, noWrite{0, 0};
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        const auto* bytes = static_cast<const BYTE*>(mapped);
        UINT64 actual = 14695981039346656037ull, expected = actual;
        bool equal = true;
        for (std::size_t s = 0; s < layout.Rows.size(); ++s)
            for (UINT row = 0; row < layout.Rows[s]; ++row)
                for (UINT64 col = 0; col < layout.RowBytes[s]; ++col)
                {
                    const auto value = bytes[layout.RowOffset(s, row) + col];
                    const auto pattern = Pattern(version, s, row, col);
                    actual = (actual ^ value) * 1099511628211ull;
                    expected = (expected ^ pattern) * 1099511628211ull;
                    equal &= value == pattern;
                }
        readback->Unmap(0, &noWrite);
        CHECK(equal && actual == expected);
    }

    void CheckReceipt(CopyReceipt receipt)
    {
        CHECK(receipt.Status == Result::Accepted && receipt.Ready.IsValid() && receipt.Release.IsValid());
        CHECK(receipt.Ready.Timeline() == receipt.Release.Timeline());
        CHECK(receipt.Ready.Value() != receipt.Release.Value());
    }

    void ExactBuffer(GDX12Device& producer, GDX12Device& consumer)
    {
        const PayloadLayout layout(producer, CD3DX12_RESOURCE_DESC::Buffer(269));
        std::array<Data, 3> data{ Data(producer, consumer, layout), Data(producer, consumer, layout),
            Data(producer, consumer, layout) };
        const auto readbackB = consumer.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(layout.Bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        GDX12CommandQueue writer(&producer, D3D12_COMMAND_LIST_TYPE_COPY);
        GDX12CommandQueue a(&consumer, D3D12_COMMAND_LIST_TYPE_COPY);
        GDX12CommandQueue b(&consumer, D3D12_COMMAND_LIST_TYPE_COPY);
        const auto beforeWrite = writer.SignalCompletion();
        const auto beforeRead = a.SignalCompletion();
        const auto ids = Ids(100);
        CopyQueues copies(producer, consumer);
        Transport transport(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, layout.Desc, ids);
        CheckViews(transport, producer, consumer, ids, firstEpoch);
        Gate producerGate(producer), readerA(consumer), readerB(consumer);
        producerGate.Hold(writer);
        readerA.Hold(a);
        readerB.Hold(b);
        std::array<Reservation, 3> reservations{};
        CompletionPoint releasedA;
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            const auto version = Version(i + 1);
            auto& reservation = reservations[i];
            reservation = transport.Reserve(version, exact, writer, data[i].Source, AllocationId{1000 + i});
            CHECK(reservation.Status == Result::Accepted && reservation.LocalSlot != NoSlot);
            CHECK(transport.SubmitExport(reservation.Publication).Status == Result::NotReady);
            CHECK(!transport.BindProducer(reservation.Publication, beforeWrite));
            if (i == 0)
            {
                CHECK(transport.PrepareExact(reservation.Publication));
                const auto plannedReader = transport.PrepareExactReader(reservation.Publication, a);
                CHECK(plannedReader && plannedReader->Version == version && transport.GetUsage().Readers == 1);
                CHECK(!transport.Reset(secondEpoch));
                auto* exportList = copies.Export.GetCommandList();
                auto* importList = copies.Import.GetCommandList();
                auto* readList = a.GetCommandList();
                transport.RecordExportCopies(reservation.Publication, *exportList->GetCommandList().Get());
                transport.RecordImportCopies(reservation.Publication, *importList->GetCommandList().Get());
                layout.Copy(*readList->GetCommandList().Get(), data[i].Readback.Get(),
                    plannedReader->View.Resource->D3DResource.Get(), false);
                CHECK(!transport.AcquireReader(reservation.Publication, a));
                const auto produced = Write(writer, layout, data[i], version);
                copies.Export.WaitFor(produced);
                copies.Export.ExecuteCommandList(exportList);
                const auto exported = copies.Export.GetLastCompletionPoint();
                copies.Import.WaitFor(exported);
                copies.Import.ExecuteCommandList(importList);
                const auto imported = copies.Import.GetLastCompletionPoint();
                a.WaitFor(imported);
                a.ExecuteCommandList(readList);
                releasedA = a.GetLastCompletionPoint();
                transport.CommitExact(reservation.Publication, produced, exported, exported, imported, imported);
                CHECK(!transport.BindReader(plannedReader->Id, beforeRead));
                CHECK(!transport.ReleaseExact(reservation.Publication));
                CHECK(transport.BindReader(plannedReader->Id, releasedA));
                continue;
            }
            const auto point = Write(writer, layout, data[i], version);
            CHECK(!transport.BindProducer(reservation.Publication, beforeRead));
            CHECK(transport.BindProducer(reservation.Publication, point));
            CHECK(transport.SubmitImport(reservation.Publication).Status == Result::NotReady);
            if (i == 2) continue; // Independently delay this export after producer completion.
            const auto exported = transport.SubmitExport(reservation.Publication);
            const auto imported = transport.SubmitImport(reservation.Publication);
            CheckReceipt(exported);
            CheckReceipt(imported);
            CHECK(!(exported.Ready.Timeline() == imported.Ready.Timeline()));
        }

        const auto one = reservations[0].Publication;
        const auto second = transport.AcquireReader(one, b);
        CHECK(second && second->Lease.View.Version == Version(1));
        CHECK(transport.ReleaseExact(one));
        CHECK(!transport.BindReader(second->Lease.Id, releasedA));
        const auto releasedB = Read(transport, b, layout, *second, readbackB);
        transport.Poll();
        CHECK(!transport.SourceReusable(one) && !Reached(a, releasedA) && !Reached(b, releasedB));
        CHECK(transport.GetUsage().Shared == 3 && transport.GetUsage().Local == 3);
        const auto blocked = transport.Reserve(Version(4), exact, writer, data[0].Source, AllocationId{1000});
        CHECK(blocked.Status == Result::Deferred && !blocked.Publication.Id());
        CHECK(transport.Reserve(Version(5), latest, writer, data[1].Source, AllocationId{1001}).Status ==
            Result::Deferred);

        producerGate.Open();
        Await([&] { transport.Poll(); return transport.SourceReusable(one); }, "Source-copy timeout");
        Await([&] { return Reached(writer, writer.GetLastCompletionPoint()); }, "Producer completion timeout");
        const auto three = reservations[2].Publication;
        CHECK(!transport.SourceReusable(three) && !transport.AcquireReader(three, a));
        const auto thirdExport = transport.SubmitExport(three);
        CheckReceipt(thirdExport);
        Await([&] { transport.Poll(); return transport.SourceReusable(three); }, "Third export timeout");
        CHECK(!transport.AcquireReader(three, a)); // Completed export alone is not a local import.
        CheckReceipt(transport.SubmitImport(three));
        Await([&] { transport.Poll(); return transport.GetUsage().Shared == 0; }, "Shared retirement timeout");
        CHECK(transport.Reserve(Version(4), exact, writer, data[0].Source, AllocationId{1000}).Status ==
            Result::Deferred);
        readerA.Open();
        Await([&] { transport.Poll(); return transport.GetUsage().Readers == 1; }, "First reader timeout");
        CHECK(Reached(a, releasedA));
        Verify(layout, data[0].Readback, Version(1));
        CHECK(transport.GetUsage().Readers == 1 && !Reached(b, releasedB));
        CHECK(transport.Reserve(Version(4), exact, writer, data[0].Source, AllocationId{1000}).Status ==
            Result::Deferred);
        readerB.Open();
        Await([&] { transport.Poll(); return transport.GetUsage().Readers == 0; }, "Second reader timeout");
        Verify(layout, readbackB, Version(1));
        const auto four = transport.Reserve(Version(4), exact, writer, data[0].Source, AllocationId{1000});
        CHECK(four.Status == Result::Accepted && four.LocalSlot == reservations[0].LocalSlot);
        CHECK(!transport.AcquireReader(one, a));
        CHECK(transport.BindProducer(four.Publication, Write(writer, layout, data[0], Version(4))));
        CheckReceipt(transport.SubmitExport(four.Publication));
        CheckReceipt(transport.SubmitImport(four.Publication));
        const auto fourth = transport.AcquireReader(four.Publication, a);
        CHECK(fourth && transport.ReleaseExact(four.Publication));
        const auto done = Read(transport, a, layout, *fourth, data[0].Readback);
        Await([&] { transport.Poll(); return Reached(a, done); }, "Reused Exact slot readback timeout");
        Verify(layout, data[0].Readback, Version(4));
        transport.Shutdown();
        std::cout << "Exact buffer: 269 bytes, deferred generation retry, independent source/export/import/readers.\n";
    }

    void LatestTextureAndResize(GDX12Device& producer, GDX12Device& consumer)
    {
        const PayloadLayout layout(producer, CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UINT, 37, 19, 2, 5));
        const PayloadLayout resized(producer, CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UINT, 25, 11, 1, 4));
        Data old(producer, consumer, layout), newer(producer, consumer, layout), inFlight(producer, consumer, layout);
        Data fresh(producer, consumer, resized);
        GDX12CommandQueue writer(&producer, D3D12_COMMAND_LIST_TYPE_COPY);
        GDX12CommandQueue reader(&consumer, D3D12_COMMAND_LIST_TYPE_COPY);
        const auto ids = Ids(200), freshIds = Ids(300);
        CopyQueues copies(producer, consumer);
        Transport transport(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, layout.Desc, ids);
        Gate sourceDelay(producer), resetDelay(producer), readerDelay(consumer);
        const auto start = [&](const Data& data, TransferVersion version, SnapshotReadMode mode,
            AllocationId backing, const PayloadLayout& shape)
        {
            const auto reserved = transport.Reserve(version, mode, writer, data.Source, backing);
            CHECK(reserved.Status == Result::Accepted);
            CHECK(transport.BindProducer(reserved.Publication, Write(writer, shape, data, version)));
            CheckReceipt(transport.SubmitExport(reserved.Publication));
            return reserved.Publication;
        };
        const auto optionalImport = [&](Ticket ticket)
        {
            Await([&]
            {
                transport.Poll();
                const auto plan = transport.PrepareImport(ticket);
                if (plan.Status == Result::NotReady) return false;
                CHECK(plan.Status == Result::Accepted && plan.ExportReady.IsValid());
                auto* list = copies.Import.GetCommandList();
                transport.RecordImportCopies(ticket, *list->GetCommandList().Get());
                copies.Import.WaitFor(plan.ExportReady);
                copies.Import.ExecuteCommandList(list);
                const auto imported = copies.Import.GetLastCompletionPoint();
                transport.CommitImport(ticket, imported, imported);
                return true;
            }, "Optional import readiness timeout");
        };
        const auto a = start(old, Version(10), latest, AllocationId{2000}, layout);
        optionalImport(a);
        Await([&] { transport.Poll(); return transport.GetUsage().Shared == 0; }, "Initial local import timeout");
        const auto first = transport.SelectLatest(FrameSubmissionId{100});
        CHECK(first.Current.Version == Version(10) && !first.Previous.IsValid());
        auto firstReader = transport.AcquireReader(a, reader);
        CHECK(firstReader);
        const auto firstRead = Read(transport, reader, layout, *firstReader, old.Readback);
        Await([&] { transport.Poll(); return Reached(reader, firstRead); }, "First texture readback timeout");
        Verify(layout, old.Readback, Version(10));
        firstReader.reset();

        sourceDelay.Hold(writer);
        const auto b = start(newer, Version(20), latest, AllocationId{2001}, layout);
        transport.Poll();
        CHECK(transport.SubmitImport(b).Status == Result::NotReady);
        auto pair = transport.SelectLatest(FrameSubmissionId{101});
        CHECK(pair.Current.Version == Version(10) && pair.Previous.Version == Version(10));
        sourceDelay.Open();
        optionalImport(b);
        CHECK(!transport.AcquireReader(b, reader));
        CHECK(transport.SelectLatest(FrameSubmissionId{102}).Current.Version == Version(10));
        Await([&] { transport.Poll(); return transport.GetUsage().Shared == 0; }, "Second local import timeout");
        CHECK(transport.SelectLatest(FrameSubmissionId{102}).Current.Version == Version(10));
        pair = transport.SelectLatest(FrameSubmissionId{103});
        CHECK(pair.Current.Version == Version(20) && pair.Previous.Version == Version(10));
        auto lease = transport.AcquireReader(b, reader);
        CHECK(lease);
        readerDelay.Hold(reader);
        const auto reading = Read(transport, reader, layout, *lease, newer.Readback);

        resetDelay.Hold(writer);
        const auto pending = start(inFlight, Version(30), exact, AllocationId{2002}, layout);
        CheckReceipt(transport.SubmitImport(pending));
        CHECK(transport.Reset(secondEpoch));
        CHECK(!transport.SelectLatest(FrameSubmissionId{104}).Current.IsValid());
        CHECK(!transport.AcquireReader(b, reader));
        CHECK(transport.SubmitImport(pending).Status == Result::Stale);
        CHECK(transport.Reserve(Version(1, secondEpoch), exact, writer, fresh.Source, AllocationId{2003}).Status ==
            Result::Deferred);
        CHECK(!transport.TryReconfigure(resized.Desc, freshIds));
        resetDelay.Open();
        Await([&] { transport.Poll(); return transport.GetUsage().Shared == 0; }, "Old import retirement timeout");
        CHECK(!transport.TryReconfigure(resized.Desc, freshIds));
        readerDelay.Open();
        Await([&] { transport.Poll(); return Reached(reader, reading) && transport.GetUsage().Publications == 0; },
            "Old reader retirement timeout");
        Verify(layout, newer.Readback, Version(20));
        CHECK(!transport.TryReconfigure(resized.Desc, freshIds)); // A graph/lease view still owns the bank.
        lease.reset();
        CHECK(transport.TryReconfigure(resized.Desc, freshIds));
        CheckViews(transport, producer, consumer, freshIds, secondEpoch);
        CHECK(transport.Reserve(Version(40), exact, writer, fresh.Source, AllocationId{2003}).Status == Result::Stale);
        const auto version = Version(1, secondEpoch);
        const auto ticket = start(fresh, version, exact, AllocationId{2003}, resized);
        CheckReceipt(transport.SubmitImport(ticket));
        const auto resizedReader = transport.AcquireReader(ticket, reader);
        CHECK(resizedReader && transport.ReleaseExact(ticket));
        const auto release = Read(transport, reader, resized, *resizedReader, fresh.Readback);
        Await([&] { transport.Poll(); return Reached(reader, release); }, "Resized texture readback timeout");
        Verify(resized, fresh.Readback, version);
        transport.Shutdown();
        std::cout << "Latest texture: 37x19 x2 slices x5 mips, pending export/import and previous snapshot; "
            "in-flight reset to 25x11 x4 mips with epoch and held-view checks.\n";
    }

    void RejectionsAndHandles(GDX12Device& producer, GDX12Device& consumer)
    {
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(269);
        const auto ids = Ids(400);
        CopyQueues copies(producer, consumer);
        Reject([&] { Transport invalid(producer, producer, copies.Export, copies.Import, stream, firstEpoch, buffer, ids); });
        auto duplicate = ids;
        duplicate[1].Import = duplicate[0].Export;
        Reject([&] { Transport invalid(producer, consumer, copies.Export, copies.Import,
            stream, firstEpoch, buffer, duplicate); });
        const auto depth = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, 19, 13, 1, 1,
            1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        Reject([&] { Transport invalid(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, depth, ids); });
        const auto typeless = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_TYPELESS, 19, 13, 1, 1);
        Reject([&] { Transport invalid(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, typeless, ids); });
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UINT, 19, 13, 1, 2);
            const auto wrongFormat = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 19, 13, 1, 2);
            const auto badSource = producer.CreateResource(D3D12_HEAP_TYPE_DEFAULT, wrongFormat,
                D3D12_RESOURCE_STATE_COMMON);
            const auto remote = consumer.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
            GDX12CommandQueue writer(&producer, D3D12_COMMAND_LIST_TYPE_COPY);
            Transport transport(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, desc, ids);
            Reject([&] { transport.Reserve(Version(1), exact, writer, badSource, AllocationId{4000}); });
            Reject([&] { transport.Reserve(Version(1), exact, writer, remote, AllocationId{4001}); });
            CHECK(transport.GetUsage().Publications == 0);
        }
        {
            auto& device = *producer.GetDevice().Get();
            const auto info = device.GetResourceAllocationInfo(0, 1, &buffer);
            D3D12_HEAP_DESC desc{};
            desc.SizeInBytes = info.SizeInBytes;
            desc.Alignment = info.Alignment;
            desc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            ComPtr<ID3D12Heap> heap;
            ComPtr<ID3D12Resource> first, second;
            ThrowIfFailed(device.CreateHeap(&desc, IID_PPV_ARGS(&heap)));
            ThrowIfFailed(device.CreatePlacedResource(heap.Get(), 0, &buffer, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&first)));
            ThrowIfFailed(device.CreatePlacedResource(heap.Get(), 0, &buffer, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&second)));
            CHECK(first.Get() != second.Get());
            GDX12CommandQueue writer(&producer, D3D12_COMMAND_LIST_TYPE_COPY);
            Transport transport(producer, consumer, copies.Export, copies.Import, stream, firstEpoch, buffer, ids);
            const AllocationId backing{4010};
            const auto active = transport.Reserve(Version(1), latest, writer, first, backing);
            CHECK(active.Status == Result::Accepted);
            CHECK(transport.Reserve(Version(2), exact, writer, second, backing).Status == Result::Deferred);
            CHECK(transport.GetUsage().Shared == 1 && transport.GetUsage().Local == 0);
            CHECK(transport.Cancel(active.Publication));
            const auto demanded = transport.Reserve(Version(2), exact, writer, second, backing);
            CHECK(demanded.Status == Result::Accepted && transport.Cancel(demanded.Publication));
        }
        const auto create = [&](std::uint64_t index)
        {
            CopyQueues iteration(producer, consumer);
            Transport transport(producer, consumer, iteration.Export, iteration.Import,
                TransferId{2 + index}, firstEpoch, buffer, Ids(10000 + 32 * index));
            transport.Shutdown();
        };
        create(0);
        DWORD before = 0, after = 0;
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (std::uint64_t i = 1; i <= 16; ++i) create(i);
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
        CHECK(after <= before + 4);
        std::cout << "16 bank/fence lifecycles; process handles " << before << " -> " << after << ".\n";
    }

    void PartialGraphReceipt(GDX12Device& producer, GDX12Device& consumer)
    {
        const PayloadLayout layout(producer, CD3DX12_RESOURCE_DESC::Buffer(269));
        Data data(producer, consumer, layout);
        GDX12CommandQueue writer(&producer, D3D12_COMMAND_LIST_TYPE_COPY);
        GDX12CommandQueue reader(&consumer, D3D12_COMMAND_LIST_TYPE_COPY);
        CopyQueues copies(producer, consumer);
        Transport transport(producer, consumer, copies.Export, copies.Import, stream, firstEpoch,
            layout.Desc, Ids(500));
        Gate gate(producer);
        const auto cancelled = transport.Reserve(Version(1), exact, writer, data.Source, AllocationId{5000});
        CHECK(cancelled.Status == Result::Accepted && transport.PrepareExact(cancelled.Publication));
        for (std::size_t i = 0; i < 32; ++i) CHECK(transport.PrepareExactReader(cancelled.Publication, reader));
        CHECK(transport.GetUsage().Readers == 32 && !transport.PrepareExactReader(cancelled.Publication, reader));
        CHECK(transport.Cancel(cancelled.Publication) && transport.GetUsage().Publications == 0 &&
            transport.GetUsage().Readers == 0);
        const auto reservation = transport.Reserve(Version(2), exact, writer, data.Source, AllocationId{5000});
        CHECK(reservation.Status == Result::Accepted && transport.PrepareExact(reservation.Publication));
        CHECK(transport.PrepareExactReader(reservation.Publication, reader));
        auto* list = copies.Export.GetCommandList();
        transport.RecordExportCopies(reservation.Publication, *list->GetCommandList().Get());
        gate.Hold(writer);
        const auto produced = Write(writer, layout, data, Version(2));
        copies.Export.WaitFor(produced);
        copies.Export.ExecuteCommandList(list);
        // Model an executor throwing after submitting export but before producing its receipt.
        transport.FaultSubmission();
        CHECK(!transport.SourceReusable(reservation.Publication));
        CHECK(transport.GetUsage().Shared == 1 && transport.GetUsage().Local == 1 &&
            transport.GetUsage().Readers == 1);
        CHECK(transport.GetSlots()[reservation.SharedSlot].Export.Resource);
        bool faulted = false;
        try { transport.Cancel(reservation.Publication); }
        catch (const std::logic_error&) { faulted = true; }
        CHECK(faulted);
        gate.Open();
        transport.Shutdown();
        CHECK(!transport.GetSlots()[reservation.SharedSlot].Export.Resource);
        std::cout << "Partial graph submission: quarantine until teardown drains actual COPY work.\n";
    }

    bool SameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

    ComPtr<IDXGIAdapter4> SecondAdapter(LUID first)
    {
        auto& factory = GDX12DeviceFactory::GetFactory();
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> adapter;
            const auto result = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            ThrowIfFailed(result);
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(adapter->GetDesc1(&desc));
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE || SameLuid(first, desc.AdapterLuid)) continue;
            const auto supported = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                __uuidof(ID3D12Device14), nullptr);
            if (supported == DXGI_ERROR_UNSUPPORTED || supported == E_NOTIMPL) continue;
            ThrowIfFailed(supported);
            return adapter;
        }
        throw std::runtime_error("Physical two-GPU acceptance requires a second compatible hardware LUID");
    }

    void CheckDebug(ID3D12InfoQueue& info)
    {
        for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info.GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info.GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const bool gbv = argc == 2 && std::string_view(argv[1]) == "--gpu-validation";
        if (argc != 1 && !gbv) throw std::invalid_argument("Expected only --gpu-validation");
        TestApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device producer, consumer;
        ThrowIfFailed(producer.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        consumer.Role = DEVICE_ROLE_SECONDARY;
        const auto first = producer.GetAdapterLuid();
        ThrowIfFailed(consumer.Initialize(SecondAdapter(first)));
        const auto second = consumer.GetAdapterLuid();
        CHECK(!SameLuid(first, second));
        std::cout << "Physical LUIDs " << first.HighPart << ':' << first.LowPart << " -> "
            << second.HighPart << ':' << second.LowPart << '\n';
        ComPtr<ID3D12InfoQueue> sourceInfo, destinationInfo;
        ThrowIfFailed(producer.GetDevice().As(&sourceInfo));
        ThrowIfFailed(consumer.GetDevice().As(&destinationInfo));
        sourceInfo->ClearStoredMessages();
        destinationInfo->ClearStoredMessages();
        ExactBuffer(producer, consumer);
        LatestTextureAndResize(producer, consumer);
        PartialGraphReceipt(producer, consumer);
        RejectionsAndHandles(producer, consumer);
        consumer.Reset();
        producer.Reset();
        CheckDebug(*sourceInfo.Get());
        CheckDebug(*destinationInfo.Get());
        std::cout << "P3b native snapshot tests passed; physical two-GPU and clean D3D12 debug layer.\n";
        return 0;
    }
    catch (const DxException& error)
    {
        std::wcerr << L"HRESULT=0x" << std::hex << static_cast<UINT>(error.ErrorCode)
            << L" " << error.ToString() << L'\n';
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
