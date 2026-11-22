#pragma once

#include "Graph/QueueSubmission.h"
#include "NativeWork.h"
#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace BistroRender
{
// Begin and retirement run after the prior owner work completes, including its
// import wait for GPU0's export.
class ProbeInputMirror
{
  public:
    struct Image
    {
        ComPtr<ID3D12Resource> Source;
        GDX12ResourceHandle Local;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
        UINT64 Bytes = 0;
        bool Initialized = false;
    };

    using Cache = std::unordered_map<ID3D12Resource*, Image>;
    ProbeInputMirror(QueueSubmission& submit, Id remote, std::shared_ptr<Cache> cache = {})
        : _submit(submit), _remote(remote), _cache(cache ? std::move(cache) : std::make_shared<Cache>()), _mirrors(*_cache) {}

    struct BufferPatch
    {
        ID3D12Resource* Source;
        UINT64 SourceOffset, DestinationOffset, Bytes;
    };
    struct BufferRange { UINT64 Offset, Bytes; };

    void Begin()
    {
        _pending.clear();
        _bytes = 0;
        _sealed = false;
    }

    void RetireUnused(const std::unordered_set<ID3D12Resource*>& live,
                      const std::function<void(ID3D12Resource*)>& forgetLocalViews)
    {
        assert(_pending.empty());
        for (auto entry = _mirrors.begin(); entry != _mirrors.end();)
        {
            if (live.count(entry->first))
            {
                ++entry;
                continue;
            }
            forgetLocalViews(entry->second.Local.Get());
            entry = _mirrors.erase(entry);
        }
    }

    ID3D12Resource* Mirror(Graph& primary, std::vector<ID3D12Resource*>& bindings, ID3D12Resource* source,
                          Id primaryResource, bool refresh, std::vector<BufferPatch> patches = {},
                          std::vector<BufferRange> ranges = {}, ID3D12Resource* baseline = nullptr, bool depthTarget = false)
    {
        if (_sealed || !source || primary.Device != 0 || bindings.at(primaryResource) != source)
            throw std::logic_error("Invalid probe input mirror source");
        auto [entry, inserted] = _mirrors.try_emplace(source);
        auto& mirror = entry->second;
        if (inserted)
        {
            try { Initialize(mirror, source, depthTarget); }
            catch (...) { _mirrors.erase(entry); throw; }
        }
        if ((refresh || !mirror.Initialized || !ranges.empty() || baseline) && !_pending.count(source))
        {
            const UINT64 offset = Align(_bytes);
            Pending item{primaryResource, offset};
            if (mirror.Footprints.empty())
            {
                if (baseline && (refresh || baseline->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
                    baseline->GetDesc().Width != mirror.Bytes))
                    throw std::invalid_argument("Incompatible owner-local mirror baseline");
                item.Baseline = baseline;
                if (refresh || (!mirror.Initialized && !baseline)) ranges = {{0, mirror.Bytes}};
                SelectBufferCopies(item, mirror.Bytes, ranges, std::move(patches));
            }
            else
            {
                if (!patches.empty() || !ranges.empty() || baseline)
                    throw std::invalid_argument("Partial mirror copies require a buffer");
                item.TransferBytes = mirror.Bytes;
            }
            if (offset > UINT64_MAX - item.TransferBytes)
                throw std::overflow_error("Probe input transfer exceeds buffer size range");
            _bytes = offset + item.TransferBytes;
            _pending.emplace(source, std::move(item));
        }
        return mirror.Local.Get();
    }

    UINT64 TransferBytes() const { return _bytes; }
    bool Contains(ID3D12Resource* source) const { return _mirrors.count(source) != 0; }
    bool PendingCopy(ID3D12Resource* source) const { return _pending.count(source) != 0; }
    UINT64 PendingTransferBytes(ID3D12Resource* source) const
    {
        const auto entry = _pending.find(source);
        return entry == _pending.end() ? 0 : entry->second.TransferBytes;
    }
    UINT64 PendingBaselineBytes(ID3D12Resource* source) const
    {
        const auto entry = _pending.find(source);
        return entry == _pending.end() ? 0 : entry->second.BaselineBytes;
    }

    // The slot is retired before its next planning pass. Release unusually large bootstrap staging.
    void TrimBootstrapStaging()
    {
        if (_capacity > 64ull * 1024 * 1024 && _bytes < _capacity / 2)
        {
            _transfer = {};
            _capacity = 0;
        }
    }

    Id AppendExport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, Queue queue = Queue::Copy)
    {
        return Append(graph, bindings, work, false, queue);
    }

    // Moving the import before its consumers preserves the graph's access order.
    // The caller must prepend before storing any operation IDs outside this work.
    Id AppendImport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, bool prepend = false)
    {
        const Id id = Append(graph, bindings, work, true, Queue::Copy);
        if (!prepend || id == Invalid || id == 0) return id;
        std::rotate(graph.Operations.begin(), graph.Operations.end() - 1, graph.Operations.end());
        for (auto& op : graph.Operations)
            for (auto& dependency : op.Dependencies)
                ++dependency;
        std::rotate(work.Commands.begin(), work.Commands.end() - 1, work.Commands.end());
        work.Commands.front().first = 0;
        for (auto command = work.Commands.begin() + 1; command != work.Commands.end(); ++command)
            ++command->first;
        return 0;
    }

  private:
    struct Pending
    {
        Id Source;
        UINT64 Offset;
        UINT64 TransferBytes = 0;
        ID3D12Resource* Baseline = nullptr;
        UINT64 BaselineBytes = 0;
        std::vector<BufferRange> Ranges;
        std::vector<BufferRange> BaselineRanges;
        std::vector<BufferPatch> Patches;
    };

    static void SelectBufferCopies(Pending& item, UINT64 bufferBytes, const std::vector<BufferRange>& ranges,
                                   std::vector<BufferPatch> patches)
    {
        std::sort(patches.begin(), patches.end(), [](const auto& a, const auto& b)
            { return a.DestinationOffset < b.DestinationOffset; });
        const auto crossAdapter = [&](UINT64 begin, UINT64 end)
        {
            if (begin == end) return;
            item.Ranges.push_back({begin, end - begin});
            item.TransferBytes += end - begin;
        };
        const auto copyBaseline = [&](UINT64 begin, UINT64 end)
        {
            if (!item.Baseline || begin == end) return;
            item.BaselineRanges.push_back({begin, end - begin});
            item.BaselineBytes += end - begin;
        };
        auto patch = patches.begin();
        UINT64 previousEnd = 0;
        for (const auto& range : ranges)
        {
            if (!range.Bytes || range.Offset < previousEnd || range.Offset > bufferBytes ||
                range.Bytes > bufferBytes - range.Offset)
                throw std::invalid_argument("Invalid mirror buffer range");
            copyBaseline(previousEnd, range.Offset);
            const UINT64 end = range.Offset + range.Bytes;
            UINT64 cursor = range.Offset;
            for (; patch != patches.end() && patch->DestinationOffset < end; ++patch)
            {
                const auto shape = patch->Source->GetDesc();
                if (!patch->Bytes || patch->DestinationOffset < cursor || patch->Bytes > end - patch->DestinationOffset ||
                    shape.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || patch->SourceOffset > shape.Width ||
                    patch->Bytes > shape.Width - patch->SourceOffset)
                    throw std::invalid_argument("Invalid owner-local mirror patch");
                crossAdapter(cursor, patch->DestinationOffset);
                cursor = patch->DestinationOffset + patch->Bytes;
            }
            crossAdapter(cursor, end);
            previousEnd = end;
        }
        if (patch != patches.end()) throw std::invalid_argument("Owner-local patch is outside the mirror update");
        copyBaseline(previousEnd, bufferBytes);
        item.Patches = std::move(patches);
    }

    static UINT64 Align(UINT64 bytes)
    {
        constexpr UINT64 alignmentMask = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1;
        if (bytes > UINT64_MAX - alignmentMask)
            throw std::overflow_error("Probe input transfer offset overflow");
        return (bytes + alignmentMask) & ~alignmentMask;
    }

    void Initialize(Image& mirror, ID3D12Resource* source, bool depthTarget)
    {
        auto shape = source->GetDesc();
        auto* device = _submit.Device(_remote).GetDevice().Get();
        if (shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            mirror.Bytes = shape.Width;
        }
        else if ((shape.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                  shape.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) && shape.SampleDesc.Count == 1)
        {
            D3D12_FEATURE_DATA_FORMAT_INFO format{shape.Format};
            ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)));
            if (format.PlaneCount != 1)
                throw std::invalid_argument("Probe capture expects single-plane texture inputs");
            const UINT count = UINT(shape.MipLevels) * (shape.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : shape.DepthOrArraySize);
            mirror.Footprints.resize(count);
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> local(count);
            UINT64 localBytes = 0;
            _submit.Device(0).GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, mirror.Footprints.data(),
                                                                nullptr, nullptr, &mirror.Bytes);
            device->GetCopyableFootprints(&shape, 0, count, 0, local.data(), nullptr, nullptr, &localBytes);
            if (!mirror.Bytes || mirror.Bytes == UINT64_MAX || localBytes != mirror.Bytes)
                throw std::runtime_error("Probe input texture has incompatible adapter footprints");
            for (UINT i = 0; i < count; ++i)
            {
                const auto& a = mirror.Footprints[i];
                const auto& b = local[i];
                if (a.Offset != b.Offset || a.Footprint.Format != b.Footprint.Format ||
                    a.Footprint.Width != b.Footprint.Width || a.Footprint.Height != b.Footprint.Height ||
                    a.Footprint.Depth != b.Footprint.Depth || a.Footprint.RowPitch != b.Footprint.RowPitch)
                    throw std::runtime_error("Probe input texture has incompatible adapter footprints");
            }
        }
        else
        {
            throw std::invalid_argument("Probe capture expects buffer or non-MSAA Texture2D inputs");
        }
        mirror.Source = source;
        shape.Alignment = 0;
        shape.Flags = depthTarget ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
        mirror.Local = _submit.Device(_remote).CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape,
                                                               D3D12_RESOURCE_STATE_COMMON, nullptr);
    }

    Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, bool importing, Queue queue)
    {
        if (graph.Device != (importing ? _remote : 0))
            throw std::logic_error("Probe input transfer uses the wrong graph device");
        _sealed = true;
        if (_pending.empty()) return Invalid;
        const UINT64 bytes = (std::max)(UINT64(1), _bytes);
        if (bytes > _capacity)
        {
            auto replacement = _submit.CreateTransferBuffer(_remote, bytes);
            std::swap(_transfer, replacement);
            _capacity = bytes;
        }
        auto* shared = _transfer.Resources[importing ? 1 : 0].Get();
        constexpr State copySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
        constexpr State copyDest = D3D12_RESOURCE_STATE_COPY_DEST;
        std::vector<Access> accesses{{NativeWork::ResourceFor(graph, bindings, shared, "probe shared inputs"),
                                      importing ? Mode::Read : Mode::Write, importing ? copySource : copyDest}};
        struct Copy
        {
            ID3D12Resource* Resource;
            const Image* Mirror;
            UINT64 Offset;
            std::vector<BufferRange> Ranges;
            std::vector<BufferPatch> Patches;
            ID3D12Resource* Baseline;
            std::vector<BufferRange> BaselineRanges;
        };
        std::vector<Copy> copies;
        for (const auto& [source, item] : _pending)
        {
            auto& mirror = _mirrors.at(source);
            auto* resource = importing ? mirror.Local.Get() : mirror.Source.Get();
            const Id id = importing ? NativeWork::ResourceFor(graph, bindings, resource, "probe owner-local input") : item.Source;
            const Resource& declaration = graph.Resources.at(id);
            const State read = declaration.Memory == Heap::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ : copySource;
            accesses.push_back({id, importing ? Mode::Write : Mode::Read, importing ? copyDest : read});
            copies.push_back({resource, &mirror, item.Offset, item.Ranges, item.Patches,
                item.Baseline, item.BaselineRanges});
            if (importing && !item.BaselineRanges.empty())
                accesses.push_back({NativeWork::ResourceFor(graph, bindings, item.Baseline, "shadow vertex baseline"),
                    Mode::Read, copySource});
            if (importing)
                for (const auto& patch : item.Patches)
                {
                    const Id patchId = NativeWork::ResourceFor(graph, bindings, patch.Source, "owner-local deformed vertices");
                    if (std::none_of(accesses.begin(), accesses.end(), [patchId](const auto& access)
                        { return access.Resource == patchId; }))
                        accesses.push_back({patchId, Mode::Read, copySource});
                }
            if (importing) mirror.Initialized = true;
        }
        return work.Add(graph, importing ? "probe owner import scene inputs" : "GPU0 export probe scene inputs",
                        std::move(accesses), [shared, importing, copies = std::move(copies)](auto* cmd) {
            for (const auto& copy : copies)
            {
                const auto& footprints = copy.Mirror->Footprints;
                if (footprints.empty())
                {
                    UINT64 sharedOffset = copy.Offset;
                    for (const auto& range : copy.Ranges)
                    {
                        if (importing) cmd->CopyBufferRegion(copy.Resource, range.Offset, shared, sharedOffset, range.Bytes);
                        else cmd->CopyBufferRegion(shared, sharedOffset, copy.Resource, range.Offset, range.Bytes);
                        sharedOffset += range.Bytes;
                    }
                    if (importing)
                    {
                        for (const auto& patch : copy.Patches)
                            cmd->CopyBufferRegion(copy.Resource, patch.DestinationOffset, patch.Source, patch.SourceOffset, patch.Bytes);
                        for (const auto& range : copy.BaselineRanges)
                            cmd->CopyBufferRegion(copy.Resource, range.Offset, copy.Baseline, range.Offset, range.Bytes);
                    }
                    continue;
                }
                for (UINT subresource = 0; subresource < footprints.size(); ++subresource)
                {
                    auto footprint = footprints[subresource];
                    footprint.Offset += copy.Offset;
                    const CD3DX12_TEXTURE_COPY_LOCATION texture(copy.Resource, subresource), buffer(shared, footprint);
                    cmd->CopyTextureRegion(importing ? &texture : &buffer, 0, 0, 0, importing ? &buffer : &texture,
                                           nullptr);
                }
            }
        }, {}, queue);
    }

    QueueSubmission& _submit;
    Id _remote;
    std::shared_ptr<Cache> _cache;
    Cache& _mirrors;
    std::unordered_map<ID3D12Resource*, Pending> _pending;
    QueueSubmission::TransferBuffer _transfer;
    UINT64 _capacity = 0;
    UINT64 _bytes = 0;
    bool _sealed = false;
};
} // namespace BistroRender
