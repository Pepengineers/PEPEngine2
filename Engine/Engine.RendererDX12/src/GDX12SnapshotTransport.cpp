#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace GDX12Snapshot
{
    namespace
    {
        constexpr auto Max = (std::numeric_limits<UINT64>::max)();

        struct SharedHandle
        {
            HANDLE Value = nullptr;
            ~SharedHandle() { if (Value) CloseHandle(Value); }
        };

        bool SameLuid(LUID a, LUID b)
        {
            return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
        }

        bool SameShape(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
        {
            return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
                a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
                a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
                a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout;
        }

        UINT Subresources(const D3D12_RESOURCE_DESC& desc)
        {
            constexpr auto incompatible = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            if (desc.Width == 0 || desc.Height == 0 || desc.MipLevels == 0 ||
                desc.DepthOrArraySize == 0 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
                (desc.Flags & incompatible))
                throw std::invalid_argument("Invalid snapshot payload extent, samples or flags");
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            {
                if (desc.Height != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
                    desc.Format != DXGI_FORMAT_UNKNOWN || desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
                    throw std::invalid_argument("Invalid snapshot buffer layout");
                return 0;
            }
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
                desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                desc.DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
                !DirectX::IsValid(desc.Format) || desc.Format == DXGI_FORMAT_UNKNOWN ||
                DirectX::IsTypeless(desc.Format) || DirectX::IsDepthStencil(desc.Format) ||
                DirectX::IsVideo(desc.Format) || DirectX::IsPlanar(desc.Format, true))
                throw std::invalid_argument("Snapshot textures require a typed single-plane 2D color layout");

            UINT mips = 1;
            for (UINT64 extent = (std::max)(desc.Width, static_cast<UINT64>(desc.Height)); extent > 1; extent >>= 1)
                ++mips;
            if (desc.MipLevels > mips) throw std::invalid_argument("Snapshot texture has too many mips");
            return static_cast<UINT>(desc.MipLevels) * desc.DepthOrArraySize;
        }

        struct Layout
        {
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
            std::vector<UINT> Rows;
            std::vector<UINT64> RowBytes;
            UINT64 Bytes = 0;
        };

        Layout Describe(ID3D12Device& device, const D3D12_RESOURCE_DESC& desc, UINT count)
        {
            Layout layout;
            if (count == 0)
            {
                layout.Bytes = desc.Width;
                return layout;
            }
            D3D12_FEATURE_DATA_FORMAT_INFO format{ desc.Format, 0 };
            ThrowIfFailed(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)));
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
            support.Format = desc.Format;
            ThrowIfFailed(device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
            if (format.PlaneCount != 1 || !(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
                throw std::invalid_argument("Adapter lacks the requested single-plane snapshot texture format");

            layout.Footprints.resize(count);
            layout.Rows.resize(count);
            layout.RowBytes.resize(count);
            device.GetCopyableFootprints(&desc, 0, count, 0, layout.Footprints.data(),
                layout.Rows.data(), layout.RowBytes.data(), &layout.Bytes);
            if (layout.Bytes == 0 || layout.Bytes == Max)
                throw std::invalid_argument("Invalid D3D12 snapshot copy footprint");
            UINT64 previousEnd = 0;
            for (UINT i = 0; i < count; ++i)
            {
                const auto& placed = layout.Footprints[i];
                const auto& footprint = placed.Footprint;
                const auto rows = layout.Rows[i];
                const auto rowBytes = layout.RowBytes[i];
                if (placed.Offset < previousEnd || placed.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                    placed.Offset > layout.Bytes || footprint.Depth != 1 || footprint.Width == 0 ||
                    footprint.Height == 0 || rows == 0 || rowBytes == 0 ||
                    footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT || footprint.RowPitch < rowBytes ||
                    rowBytes > layout.Bytes - placed.Offset ||
                    rows - 1 > (layout.Bytes - placed.Offset - rowBytes) / footprint.RowPitch)
                    throw std::invalid_argument("Snapshot texture footprint exceeds its shared buffer");
                previousEnd = placed.Offset + static_cast<UINT64>(rows - 1) * footprint.RowPitch + rowBytes;
            }
            return layout;
        }

        bool Compatible(const Layout& a, const Layout& b)
        {
            if (a.Bytes != b.Bytes || a.Rows != b.Rows || a.RowBytes != b.RowBytes) return false;
            for (std::size_t i = 0; i < a.Footprints.size(); ++i)
            {
                const auto& x = a.Footprints[i];
                const auto& y = b.Footprints[i];
                if (x.Offset != y.Offset || x.Footprint.Format != y.Footprint.Format ||
                    x.Footprint.Width != y.Footprint.Width || x.Footprint.Height != y.Footprint.Height ||
                    x.Footprint.Depth != y.Footprint.Depth || x.Footprint.RowPitch != y.Footprint.RowPitch)
                    return false;
            }
            return true;
        }

        D3D12_HEAP_DESC SharedHeapDesc(ID3D12Device& producer, ID3D12Device& consumer,
            const D3D12_RESOURCE_DESC& shared)
        {
            const auto a = producer.GetResourceAllocationInfo(0, 1, &shared);
            const auto b = consumer.GetResourceAllocationInfo(0, 1, &shared);
            if (!a.Alignment || !b.Alignment || !a.SizeInBytes || !b.SizeInBytes ||
                a.SizeInBytes == Max || b.SizeInBytes == Max)
                throw std::invalid_argument("Invalid cross-adapter buffer allocation");
            D3D12_HEAP_DESC heap{};
            heap.Alignment = (std::max)(a.Alignment, b.Alignment);
            heap.SizeInBytes = (std::max)(a.SizeInBytes, b.SizeInBytes);
            const auto remainder = heap.SizeInBytes % heap.Alignment;
            if (remainder)
            {
                const auto pad = heap.Alignment - remainder;
                if (heap.SizeInBytes > Max - pad) throw std::overflow_error("Shared heap alignment overflow");
                heap.SizeInBytes += pad;
            }
            heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            // Cross-adapter heaps require the unrestricted heap-tier flag (zero).
            heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
            return heap;
        }

        void CheckIds(const PoolIds& ids)
        {
            std::array<ResourceId, SlotCount * 3> resources{};
            std::array<AllocationId, SlotCount * 2> allocations{};
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                resources[i * 3] = ids[i].Export;
                resources[i * 3 + 1] = ids[i].Import;
                resources[i * 3 + 2] = ids[i].Local;
                allocations[i * 2] = ids[i].SharedBacking;
                allocations[i * 2 + 1] = ids[i].LocalBacking;
            }
            std::sort(resources.begin(), resources.end());
            std::sort(allocations.begin(), allocations.end());
            if (resources.front() == ResourceId::Invalid || allocations.front() == AllocationId::Invalid ||
                std::adjacent_find(resources.begin(), resources.end()) != resources.end() ||
                std::adjacent_find(allocations.begin(), allocations.end()) != allocations.end())
                throw std::invalid_argument("Snapshot slots require unique resource and backing identities");
        }

        void CheckQueue(const GDX12CommandQueue& queue, AdapterId adapter)
        {
            const auto timeline = queue.GetTimelineIdentity();
            if (!timeline.IsValid() || timeline.Producer.Location.Adapter != adapter)
                throw std::invalid_argument("Snapshot queue has the wrong device or an inactive timeline");
        }

        void CheckCopyList(ID3D12GraphicsCommandList& list, GDX12Device& device)
        {
            ComPtr<ID3D12Device14> owner;
            ThrowIfFailed(list.GetDevice(IID_PPV_ARGS(&owner)));
            if (list.GetType() != D3D12_COMMAND_LIST_TYPE_COPY || owner.Get() != device.GetDevice().Get())
                throw std::invalid_argument("Snapshot copies require a COPY list on their physical device");
        }

        bool IssuedAfter(CompletionPoint point, TimelineId timeline, std::uint64_t before)
        {
            return point.IsValid() && point.Timeline() == timeline && point.Value() > before;
        }

        bool CopyPoints(CompletionPoint first, CompletionPoint second, TimelineId timeline, std::uint64_t before)
        {
            return IssuedAfter(first, timeline, before) && IssuedAfter(second, timeline, before) &&
                first.Value() <= second.Value();
        }

        void Drain(GDX12CommandQueue& queue, GDX12Device& device)
        {
            if (FAILED(device.GetDevice()->GetDeviceRemovedReason())) return;
            try { queue.Flush(); }
            catch (...)
            {
                if (SUCCEEDED(device.GetDevice()->GetDeviceRemovedReason())) throw;
            }
        }
    }

    Transport::Transport(GDX12Device& producer, GDX12Device& consumer, GDX12CommandQueue& exportQueue,
        GDX12CommandQueue& importQueue, TransferId transfer, Epoch epoch,
        const D3D12_RESOURCE_DESC& desc, const PoolIds& ids)
        : _producer(producer), _consumer(consumer), _export(exportQueue), _import(importQueue),
          _exportTimeline(exportQueue.GetTimelineIdentity()), _importTimeline(importQueue.GetTimelineIdentity()),
          _producerId(producer.GetSubmissionAdapterId()),
          _consumerId(consumer.GetSubmissionAdapterId()), _transfer(transfer), _epoch(epoch),
          _protocol(transfer, epoch, SlotCount, SlotCount, ReaderCount)
    {
        RequireOpen();
        if (SameLuid(producer.GetAdapterLuid(), consumer.GetAdapterLuid()) ||
            producer.GetDevice()->GetNodeCount() != 1 || consumer.GetDevice()->GetNodeCount() != 1)
            throw std::invalid_argument("Snapshot transport requires two distinct single-node physical adapters");
        CheckQueue(_export, _producerId);
        CheckQueue(_import, _consumerId);
        if (_export.GetType() != D3D12_COMMAND_LIST_TYPE_COPY || _import.GetType() != D3D12_COMMAND_LIST_TYPE_COPY)
            throw std::invalid_argument("Snapshot transport requires two COPY queues");
        Configure(desc, ids);
        _import.ImportTimeline(_export);
    }

    Transport::~Transport()
    {
        try { Shutdown(); }
        catch (...)
        {
            OutputDebugStringA("Snapshot transport could not prove queue retirement during teardown.\n");
            std::terminate();
        }
    }

    void Transport::RequireOpen()
    {
        if (_closed || _faulted) throw std::logic_error("Snapshot transport is closed or faulted");
        if (!_producer.GetDevice() || !_consumer.GetDevice() ||
            _producer.GetSubmissionAdapterId() != _producerId || _consumer.GetSubmissionAdapterId() != _consumerId ||
            !(_export.GetTimelineIdentity() == _exportTimeline) || !(_import.GetTimelineIdentity() == _importTimeline))
        {
            _faulted = true;
            throw std::logic_error("Snapshot transport requires its original device and COPY timeline incarnations");
        }
        const auto producer = _producer.GetDevice()->GetDeviceRemovedReason();
        const auto consumer = _consumer.GetDevice()->GetDeviceRemovedReason();
        if (FAILED(producer) || FAILED(consumer))
        {
            _faulted = true;
            ThrowIfFailed(FAILED(producer) ? producer : consumer);
        }
    }

    void Transport::Configure(const D3D12_RESOURCE_DESC& desc, const PoolIds& ids)
    {
        CheckIds(ids);
        auto& producer = *_producer.GetDevice().Get();
        auto& consumer = *_consumer.GetDevice().Get();
        const auto count = Subresources(desc);
        auto layout = Describe(producer, desc, count);
        if (!Compatible(layout, Describe(consumer, desc, count)))
            throw std::invalid_argument("Adapters disagree on snapshot copy footprints");
        const auto shared = CD3DX12_RESOURCE_DESC::Buffer(layout.Bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        const auto heap = SharedHeapDesc(producer, consumer, shared);

        // Reconfiguration reaches here only after old users retire.
        _views = {};
        std::array<SlotViews, SlotCount> views{};
        for (std::size_t i = 0; i < views.size(); ++i)
        {
            auto slot = std::make_shared<Slot>();
            ThrowIfFailed(producer.CreateHeap(&heap, IID_PPV_ARGS(&slot->ExportHeap)));
            {
                SharedHandle handle;
                ThrowIfFailed(producer.CreateSharedHandle(slot->ExportHeap.Get(), nullptr, GENERIC_ALL, nullptr,
                    &handle.Value));
                ThrowIfFailed(consumer.OpenSharedHandle(handle.Value, IID_PPV_ARGS(&slot->ImportHeap)));
            }
            ComPtr<ID3D12Resource> exportResource, importResource;
            ThrowIfFailed(producer.CreatePlacedResource(slot->ExportHeap.Get(), 0, &shared,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&exportResource)));
            ThrowIfFailed(consumer.CreatePlacedResource(slot->ImportHeap.Get(), 0, &shared,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&importResource)));
            slot->Export.D3DResource = GDX12ResourceHandle(std::move(exportResource));
            slot->Import.D3DResource = GDX12ResourceHandle(std::move(importResource));
            slot->Local.D3DResource = _consumer.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
            views[i] = {
                { {slot, &slot->Export}, {ids[i].Export, _producerId, _epoch, _transfer, ids[i].SharedBacking} },
                { {slot, &slot->Import}, {ids[i].Import, _consumerId, _epoch, _transfer, ids[i].SharedBacking} },
                { {slot, &slot->Local}, {ids[i].Local, _consumerId, _epoch, _transfer, ids[i].LocalBacking} }
            };
        }
        _desc = desc;
        _footprints = std::move(layout.Footprints);
        _views = std::move(views);
    }

    void Transport::CheckSource(const GDX12ResourceHandle& source) const
    {
        if (!source) throw std::invalid_argument("Snapshot source is null");
        ComPtr<ID3D12Device14> owner;
        ThrowIfFailed(source->GetDevice(IID_PPV_ARGS(&owner)));
        const auto desc = source->GetDesc();
        Subresources(desc);
        if (owner.Get() != _producer.GetDevice().Get() || !SameShape(desc, _desc))
            throw std::invalid_argument("Snapshot source device or payload shape does not match");
        D3D12_HEAP_PROPERTIES properties{};
        ThrowIfFailed(source->GetHeapProperties(&properties, nullptr));
        if (properties.Type == D3D12_HEAP_TYPE_UPLOAD || properties.Type == D3D12_HEAP_TYPE_READBACK ||
            properties.Type == D3D12_HEAP_TYPE_GPU_UPLOAD)
            throw std::invalid_argument("Snapshot source must support a COMMON handoff");
    }

    void Transport::ReapSources()
    {
        for (auto& use : _sources)
            if (use.Publication.Id() && _protocol.SourceReusable(use.Publication)) use = {};
    }

    std::size_t Transport::SourceSlot(Ticket ticket) const
    {
        if (ticket.Id())
            for (std::size_t i = 0; i < _sources.size(); ++i)
                if (_sources[i].Publication == ticket) return i;
        return NoSlot;
    }

    std::size_t Transport::ImportSlot(Ticket ticket) const
    {
        if (ticket.Id())
            for (std::size_t i = 0; i < _imports.size(); ++i)
                if (_imports[i].Publication == ticket) return i;
        return NoSlot;
    }

    void Transport::RejectReceipt(const char* message)
    {
        FaultSubmission();
        throw std::logic_error(message);
    }

    Usage Transport::GetUsage() const
    {
        if (_closed) return {};
        auto usage = _protocol.GetUsage();
        for (const auto& reader : _readers)
            if (reader.Lease && !reader.ModelLease) ++usage.Readers;
        return usage;
    }

    Transport::ReaderUse* Transport::FreeReader()
    {
        if (_nextReader == Max) return nullptr;
        for (auto& reader : _readers) if (!reader.Lease) return &reader;
        return nullptr;
    }

    Reservation Transport::Reserve(TransferVersion version, SnapshotReadMode mode, GDX12CommandQueue& queue,
        const GDX12ResourceHandle& source, AllocationId sourceAllocation)
    {
        RequireOpen();
        const bool exact = mode == SnapshotReadMode::ExactVersion;
        if (!version.IsValid() || version.Transfer != _transfer || sourceAllocation == AllocationId::Invalid ||
            (!exact && mode != SnapshotReadMode::LatestCompleted)) return {};
        if (version.VersionEpoch != _epoch) return { Result::Stale };
        if (_resetting) return { Result::Deferred };
        if (version.Invocation <= _lastInvocation) return {};
        if (_deferredExact && (!exact || !(*_deferredExact == version))) return { Result::Deferred };
        CheckQueue(queue, _producerId);
        CheckSource(source);
        ReapSources();
        for (const auto& use : _sources)
            if (use.Resource.Get() == source.Get() || use.Allocation == sourceAllocation)
            {
                if (exact) _deferredExact = version;
                return { Result::Deferred };
            }
        const auto reservation = _protocol.TryReserve(version, mode);
        if (reservation.Status == Result::Accepted)
        {
            _lastInvocation = version.Invocation;
            _deferredExact.reset();
            _sources[reservation.SharedSlot] = { reservation.Publication, source, sourceAllocation,
                { &queue, queue.GetTimelineIdentity(), queue.GetLastCompletionPoint().Value(), {} },
                reservation.LocalSlot, version };
        }
        else if (exact && reservation.Status == Result::Deferred) _deferredExact = version;
        return reservation;
    }

    bool Transport::BindProducer(Ticket ticket, CompletionPoint point)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (slot == NoSlot) return false;
        auto& use = _sources[slot];
        if (use.ExactPrepared || !IssuedAfter(point, use.Producer.Timeline, use.Producer.Before) ||
            !(use.Producer.Queue->GetTimelineIdentity() == use.Producer.Timeline) ||
            !_protocol.BindProducer(ticket, point)) return false;
        use.Producer.Done = point;
        return true;
    }

    bool Transport::PrepareExact(Ticket ticket)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (_resetting || slot == NoSlot) return false;
        auto& source = _sources[slot];
        if (source.Local == NoSlot || source.Producer.Done.IsValid() || source.ExportBefore) return false;
        source.ExactPrepared = true;
        source.ExportBefore = _export.GetLastCompletionPoint().Value();
        _imports[slot] = { ticket, {Result::Accepted, slot, source.Local, {}},
            _import.GetLastCompletionPoint().Value() };
        return true;
    }

    std::optional<PreparedRead> Transport::PrepareExactReader(Ticket ticket, GDX12CommandQueue& queue)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (_resetting || slot == NoSlot || !_sources[slot].ExactPrepared) return {};
        CheckQueue(queue, _consumerId);
        auto* reader = FreeReader();
        if (!reader) return {};
        *reader = { _nextReader++, 0, ticket,
            { &queue, queue.GetTimelineIdentity(), queue.GetLastCompletionPoint().Value(), {} }, true };
        const auto& source = _sources[slot];
        return PreparedRead{ reader->Lease, _views[source.Local].Local, source.Version };
    }

    std::optional<CompletionPoint> Transport::PrepareExport(Ticket ticket)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (_resetting || slot == NoSlot || _sources[slot].ExportBefore) return {};
        auto ready = _protocol.BeginExport(ticket);
        if (ready) _sources[slot].ExportBefore = _export.GetLastCompletionPoint().Value();
        return ready;
    }

    ImportPlan Transport::PrepareImport(Ticket ticket)
    {
        RequireOpen();
        if (_resetting) return { Result::Stale };
        if (ImportSlot(ticket) != NoSlot) return { Result::NotReady };
        const auto plan = _protocol.BeginImport(ticket);
        if (plan.Status == Result::Accepted)
            _imports[plan.SharedSlot] = { ticket, plan, _import.GetLastCompletionPoint().Value() };
        return plan;
    }

    void Transport::RecordExportCopies(Ticket ticket, ID3D12GraphicsCommandList& list) const
    {
        const auto slot = SourceSlot(ticket);
        if (_closed || _faulted || slot == NoSlot || !_sources[slot].ExportBefore)
            throw std::logic_error("Snapshot export was not prepared");
        CheckCopyList(list, _producer);
        Copy(list, _views[slot].Export.Resource->D3DResource.Get(), _sources[slot].Resource.Get(), true);
    }

    void Transport::RecordImportCopies(Ticket ticket, ID3D12GraphicsCommandList& list) const
    {
        const auto slot = ImportSlot(ticket);
        if (_closed || _faulted || slot == NoSlot) throw std::logic_error("Snapshot import was not prepared");
        CheckCopyList(list, _consumer);
        const auto local = _imports[slot].Plan.LocalSlot;
        Copy(list, _views[slot].Import.Resource->D3DResource.Get(),
            _views[local].Local.Resource->D3DResource.Get(), false);
    }

    void Transport::CommitExact(Ticket ticket, CompletionPoint produced, CompletionPoint exported,
        CompletionPoint sourceCopied, CompletionPoint imported, CompletionPoint sharedReleased)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (slot == NoSlot || !_sources[slot].ExactPrepared || ImportSlot(ticket) != slot)
            RejectReceipt("Exact snapshot receipt has no prepared reservation");
        auto& source = _sources[slot];
        const auto prepared = _imports[slot];
        if (!IssuedAfter(produced, source.Producer.Timeline, source.Producer.Before) ||
            !(source.Producer.Queue->GetTimelineIdentity() == source.Producer.Timeline) ||
            !CopyPoints(sourceCopied, exported, _exportTimeline, *source.ExportBefore) ||
            !CopyPoints(imported, sharedReleased, _importTimeline, prepared.Before))
            RejectReceipt("Exact snapshot receipt has the wrong timelines or submission values");
        if (!_protocol.BindProducer(ticket, produced) || !_protocol.BeginExport(ticket) ||
            !_protocol.BindExport(ticket, exported, sourceCopied))
            RejectReceipt("Exact snapshot producer/export receipt binding failed");
        const auto plan = _protocol.BeginImport(ticket);
        if (plan.Status != Result::Accepted || plan.SharedSlot != prepared.Plan.SharedSlot ||
            plan.LocalSlot != prepared.Plan.LocalSlot || !_protocol.BindImport(ticket, imported, sharedReleased))
            RejectReceipt("Exact snapshot import receipt binding failed");
        for (auto& reader : _readers)
        {
            if (!reader.Lease || !reader.Prepared || !(reader.Publication == ticket)) continue;
            if (!(reader.Reader.Queue->GetTimelineIdentity() == reader.Reader.Timeline))
                RejectReceipt("Prepared Exact reader changed timeline");
            const auto model = _protocol.AcquireReader(ticket);
            if (!model) RejectReceipt("Prepared Exact reader lost its admission credit");
            reader.ModelLease = model->Id;
        }
        source.Producer.Done = produced;
        source.ExactPrepared = false;
        source.ExportBefore.reset();
        _imports[slot] = {};
    }

    void Transport::CommitExport(Ticket ticket, CompletionPoint exported, CompletionPoint sourceCopied)
    {
        RequireOpen();
        const auto slot = SourceSlot(ticket);
        if (slot == NoSlot || _sources[slot].ExactPrepared || !_sources[slot].ExportBefore ||
            !CopyPoints(sourceCopied, exported, _exportTimeline, *_sources[slot].ExportBefore) ||
            !_protocol.BindExport(ticket, exported, sourceCopied))
            RejectReceipt("Snapshot export receipt has no matching prepared submission");
        _sources[slot].ExportBefore.reset();
    }

    void Transport::CommitImport(Ticket ticket, CompletionPoint imported, CompletionPoint sharedReleased)
    {
        RequireOpen();
        const auto slot = ImportSlot(ticket);
        if (slot == NoSlot || !_imports[slot].Plan.ExportReady.IsValid() ||
            !CopyPoints(imported, sharedReleased, _importTimeline, _imports[slot].Before) ||
            !_protocol.BindImport(ticket, imported, sharedReleased))
            RejectReceipt("Snapshot import receipt has no matching prepared submission");
        _imports[slot] = {};
    }

    void Transport::Copy(ID3D12GraphicsCommandList& list, ID3D12Resource* shared, ID3D12Resource* local,
        bool exporting) const
    {
        // COPY batches promote COMMON on access and decay at the ExecuteCommandLists boundary.
        if (_footprints.empty())
        {
            list.CopyBufferRegion(exporting ? shared : local, 0, exporting ? local : shared, 0, _desc.Width);
            return;
        }
        for (UINT i = 0; i < _footprints.size(); ++i)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION buffer(shared, _footprints[i]);
            const CD3DX12_TEXTURE_COPY_LOCATION texture(local, i);
            list.CopyTextureRegion(exporting ? &buffer : &texture, 0, 0, 0,
                exporting ? &texture : &buffer, nullptr);
        }
    }

    CopyReceipt Transport::SubmitExport(Ticket ticket)
    {
        RequireOpen();
        if (_resetting) return { Result::Stale };
        if (SourceSlot(ticket) == NoSlot) return {};
        const auto ready = PrepareExport(ticket);
        if (!ready) return { Result::NotReady };
        try
        {
            auto* list = _export.GetCommandList();
            RecordExportCopies(ticket, *list->GetCommandList().Get());
            _export.WaitFor(*ready);
            _export.ExecuteCommandList(list);
            const auto copied = _export.GetLastCompletionPoint();
            const auto exported = _export.SignalCompletion();
            CommitExport(ticket, exported, copied);
            return { Result::Accepted, exported, copied };
        }
        catch (...) { _faulted = true; throw; }
    }

    CopyReceipt Transport::SubmitImport(Ticket ticket)
    {
        const auto plan = PrepareImport(ticket);
        if (plan.Status != Result::Accepted) return { plan.Status };
        try
        {
            auto* list = _import.GetCommandList();
            RecordImportCopies(ticket, *list->GetCommandList().Get());
            _import.WaitFor(plan.ExportReady);
            _import.ExecuteCommandList(list);
            const auto imported = _import.GetLastCompletionPoint();
            const auto released = _import.SignalCompletion();
            CommitImport(ticket, imported, released);
            return { Result::Accepted, imported, released };
        }
        catch (...) { _faulted = true; throw; }
    }

    bool Transport::Cancel(Ticket ticket)
    {
        RequireOpen();
        for (const auto& reader : _readers)
            if (reader.Prepared && reader.ModelLease && !reader.Reader.Done.IsValid() &&
                reader.Publication == ticket) return false;
        const bool cancelled = _protocol.Cancel(ticket);
        if (cancelled)
        {
            const auto prepared = ImportSlot(ticket);
            if (prepared != NoSlot) _imports[prepared] = {};
            for (auto& reader : _readers)
                if (reader.Prepared && !reader.ModelLease && reader.Publication == ticket) reader = {};
            const auto source = SourceSlot(ticket);
            if (source != NoSlot)
            {
                _sources[source].ExactPrepared = false;
                _sources[source].ExportBefore.reset();
            }
        }
        ReapSources();
        return cancelled;
    }

    bool Transport::CancelDeferredExact(TransferVersion version)
    {
        RequireOpen();
        if (!_deferredExact || !(*_deferredExact == version)) return false;
        _protocol.CancelDeferredExact(version);
        _deferredExact.reset();
        return true;
    }

    bool Transport::DropOptional(Ticket ticket)
    {
        RequireOpen();
        const bool dropped = _protocol.DropOptional(ticket);
        ReapSources();
        return dropped;
    }

    bool Transport::ReleaseExact(Ticket ticket)
    {
        RequireOpen();
        for (const auto& reader : _readers)
            if (reader.Prepared && reader.Publication == ticket && !reader.Reader.Done.IsValid()) return false;
        return _protocol.ReleaseExact(ticket);
    }

    DisplayedPair Transport::SelectLatest(FrameSubmissionId frame)
    {
        RequireOpen();
        return _resetting ? DisplayedPair{} : _protocol.SelectLatest(frame);
    }

    std::optional<ReadLease> Transport::AcquireReader(Ticket ticket, GDX12CommandQueue& queue)
    {
        RequireOpen();
        if (_resetting) return {};
        CheckQueue(queue, _consumerId);
        auto* slot = FreeReader();
        if (!slot) return {};
        const auto reader = _protocol.AcquireReader(ticket);
        if (!reader) return {};
        *slot = { _nextReader++, reader->Id, ticket,
            { &queue, queue.GetTimelineIdentity(), queue.GetLastCompletionPoint().Value(), {} } };
        return ReadLease{ {slot->Lease, reader->View}, _views[reader->View.LocalSlot].Local };
    }

    bool Transport::BindReader(std::uint64_t lease, CompletionPoint release)
    {
        RequireOpen();
        if (!lease || !release.IsValid()) return false;
        const auto use = std::find_if(_readers.begin(), _readers.end(),
            [lease](const ReaderUse& r) { return r.Lease == lease; });
        if (use == _readers.end() || !use->ModelLease || !(release.Timeline() == use->Reader.Timeline) ||
            !(use->Reader.Queue->GetTimelineIdentity() == use->Reader.Timeline) ||
            release.Value() <= use->Reader.Before || !_protocol.BindReader(use->ModelLease, release)) return false;
        use->Reader.Done = release;
        return true;
    }

    bool Transport::CancelReader(std::uint64_t lease)
    {
        RequireOpen();
        if (!lease) return false;
        for (auto& use : _readers)
        {
            if (use.Lease != lease) continue;
            if (use.ModelLease && (use.Prepared || !_protocol.CancelReader(use.ModelLease))) return false;
            use = {};
            return true;
        }
        return false;
    }

    FenceProgress Transport::Observe(const QueueUse& use)
    {
        if (!(use.Queue->GetTimelineIdentity() == use.Timeline))
            throw std::logic_error("Snapshot user changed its submission timeline before retirement");
        const auto progress = use.Queue->GetCompletionProgress();
        if (progress.CompletedValue == Max) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        _protocol.Observe(progress);
        return progress;
    }

    void Transport::Poll()
    {
        RequireOpen();
        try
        {
            Observe({ &_export, _exportTimeline, 0, {} });
            Observe({ &_import, _importTimeline, 0, {} });
            for (const auto& use : _sources)
                if (use.Producer.Done.IsValid()) Observe(use.Producer);
            for (auto& use : _readers)
                if (use.Reader.Done.IsValid() && use.Reader.Done.IsReachedBy(Observe(use.Reader))) use = {};
            ReapSources();
        }
        catch (...) { _faulted = true; throw; }
    }

    bool Transport::Reset(Epoch epoch)
    {
        RequireOpen();
        for (const auto& source : _sources) if (source.ExportBefore) return false;
        for (const auto& import : _imports) if (import.Publication.Id()) return false;
        for (const auto& reader : _readers)
            if (reader.Prepared && !reader.Reader.Done.IsValid()) return false;
        if (!_protocol.Reset(epoch)) return false;
        _epoch = epoch;
        _lastInvocation = ProducerInvocationId::Invalid;
        _deferredExact.reset();
        _resetting = true;
        ReapSources();
        return true;
    }

    bool Transport::TryReconfigure(const D3D12_RESOURCE_DESC& desc, const PoolIds& ids)
    {
        RequireOpen();
        if (!_resetting) throw std::logic_error("Reset is required before changing snapshot storage");
        if (_protocol.GetUsage().Publications != 0) return false;
        // Each slot owns exactly its three aliasing views until graph/lease owners release them.
        for (const auto& slot : _views)
            if (slot.Export.Resource.use_count() > 3) return false;
        ReapSources();
        Configure(desc, ids);
        _resetting = false;
        return true;
    }

    void Transport::Shutdown()
    {
        if (_closed) return;
        _faulted = true;
        // This is teardown, including unresolved submits. Keep the bank until all live queues drain.
        for (const auto& use : _sources)
            if (use.Producer.Queue) Drain(*use.Producer.Queue, _producer);
        Drain(_export, _producer);
        Drain(_import, _consumer);
        for (const auto& use : _readers)
            if (use.Reader.Queue) Drain(*use.Reader.Queue, _consumer);
        _readers = {};
        _sources = {};
        _imports = {};
        _views = {};
        _closed = true;
    }
}
