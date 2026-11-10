#include "App.Base/Rendering/CrossAdapterSceneReplica.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace CrossAdapterSceneReplica
{
namespace
{
    using namespace GDX12Submission;
    using Vertex = Engine::Core::Vertex;
    using Mode = GDX12RenderGraph::Mode;
    constexpr auto ShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto PixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    static_assert(sizeof(Vertex) == 56);
    static_assert(sizeof(GDX12IndirectDrawArgs) == 24);

    bool Fits(UINT64 start, UINT64 count, UINT64 size)
    { return start <= size && count <= size - start; }

    bool SameIdentity(const ResourceIdentity& a, const ResourceIdentity& b)
    {
        return a.Id == b.Id && a.Adapter == b.Adapter && a.ResourceEpoch == b.ResourceEpoch &&
            a.Transfer == b.Transfer && a.Allocation == b.Allocation;
    }

    bool ValidIdentity(const ResourceView& view, AdapterId adapter)
    {
        const auto& id = view.Identity;
        return id.Id != ResourceId::Invalid && id.Adapter == adapter && id.ResourceEpoch != Epoch::Invalid &&
            id.Allocation != AllocationId::Invalid &&
            (id.Transfer == TransferId::Invalid ? !view.Version.IsValid() :
                view.Version.IsValid() && view.Version.Transfer == id.Transfer && view.Version.VersionEpoch == id.ResourceEpoch);
    }

    bool IsSecondaryView(const ResourceView& view, GDX12Device& device)
    {
        if (!view.Native || !view.Native->D3DResource || !view.Owner || !ValidIdentity(view, device.GetSubmissionAdapterId()))
            return false;
        ComPtr<ID3D12Device14> owner;
        D3D12_HEAP_PROPERTIES heap{};
        auto* native = view.Native->D3DResource.Get();
        return SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device.GetDevice().Get() &&
            SUCCEEDED(native->GetHeapProperties(&heap, nullptr)) && heap.Type == D3D12_HEAP_TYPE_DEFAULT;
    }

    void AddAccess(std::vector<Access>& accesses, const ResourceView& view, Mode mode, D3D12_RESOURCE_STATES state)
    {
        for (auto& access : accesses)
        {
            const auto& old = access.View;
            if (old.Identity.Id != view.Identity.Id && old.Identity.Allocation != view.Identity.Allocation &&
                old.Native->D3DResource.Get() != view.Native->D3DResource.Get())
                continue;
            if (old.Native != view.Native || !SameIdentity(old.Identity, view.Identity) ||
                !(old.Version == view.Version) || access.Mode != mode)
                throw std::invalid_argument("Conflicting scene replica view identity");
            access.State |= state;
            return;
        }
        accesses.push_back({ view, view.Native->D3DResource, mode, state });
    }

    bool Finite(const XMFLOAT3& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    bool Finite(const XMFLOAT4X4& matrix)
    {
        for (const auto& row : matrix.m)
            for (float value : row)
                if (!std::isfinite(value)) return false;
        return true;
    }

    UINT RelativeIndex(const MeshRange& mesh, UINT index)
    {
        return mesh.Addressing == IndexAddressing::AbsoluteInPayload ? index - mesh.FirstVertex : index;
    }

    bool BufferRange(const ResourceView& view, GDX12Device& device, UINT64 offset, UINT64 bytes);

    bool ValidMesh(const MeshRange& mesh, GDX12Device& device)
    {
        if (mesh.Resident)
        {
            const auto& range = *mesh.Resident;
            return mesh.Mesh.IsValid() && mesh.ContentRevision && mesh.Vertices.empty() && mesh.Indices.empty() &&
                mesh.FirstVertex == 0 && mesh.FirstIndex == 0 && range.VertexCount == mesh.VertexCount &&
                range.IndexCount == mesh.IndexCount && range.VertexCount && range.IndexCount &&
                range.IndexCount % 3 == 0 && range.MaxRelativeIndex < range.VertexCount &&
                BufferRange(range.Vertices, device, 0, UINT64{range.VertexCount} * sizeof(Vertex)) &&
                BufferRange(range.Indices, device, 0, UINT64{range.IndexCount} * sizeof(UINT));
        }
        if (!mesh.Mesh.IsValid() || !mesh.ContentRevision || !mesh.VertexCount || !mesh.IndexCount || mesh.IndexCount % 3 ||
            !Fits(mesh.FirstVertex, mesh.VertexCount, mesh.Vertices.size()) ||
            !Fits(mesh.FirstIndex, mesh.IndexCount, mesh.Indices.size()) ||
            (mesh.Addressing != IndexAddressing::RelativeToRange && mesh.Addressing != IndexAddressing::AbsoluteInPayload))
            return false;
        const auto* indices = mesh.Indices.data() + mesh.FirstIndex;
        for (UINT i = 0; i < mesh.IndexCount; ++i)
            if ((mesh.Addressing == IndexAddressing::AbsoluteInPayload && indices[i] < mesh.FirstVertex) ||
                RelativeIndex(mesh, indices[i]) >= mesh.VertexCount)
                return false;
        const auto* vertices = mesh.Vertices.data() + mesh.FirstVertex;
        for (UINT i = 0; i < mesh.VertexCount; ++i)
            if (!Finite(vertices[i].Position) || !std::isfinite(vertices[i].TexCoord.x) || !std::isfinite(vertices[i].TexCoord.y))
                return false;
        return true;
    }

    bool SameMesh(const MeshRange& a, const MeshRange& b)
    {
        if (a.Resident || b.Resident)
        {
            if (!a.Resident || !b.Resident || a.VertexCount != b.VertexCount || a.IndexCount != b.IndexCount ||
                a.Resident->MaxRelativeIndex != b.Resident->MaxRelativeIndex) return false;
            const auto same = [](const ResourceView& x, const ResourceView& y)
            { return x.Native == y.Native && SameIdentity(x.Identity, y.Identity) && x.Version == y.Version; };
            return same(a.Resident->Vertices, b.Resident->Vertices) && same(a.Resident->Indices, b.Resident->Indices);
        }
        if (a.VertexCount != b.VertexCount || a.IndexCount != b.IndexCount ||
            std::memcmp(a.Vertices.data() + a.FirstVertex, b.Vertices.data() + b.FirstVertex,
                static_cast<size_t>(a.VertexCount) * sizeof(Vertex)) != 0)
            return false;
        for (UINT i = 0; i < a.IndexCount; ++i)
            if (RelativeIndex(a, a.Indices[static_cast<size_t>(a.FirstIndex) + i]) !=
                RelativeIndex(b, b.Indices[static_cast<size_t>(b.FirstIndex) + i]))
                return false;
        return true;
    }

    bool BufferRange(const ResourceView& view, GDX12Device& device, UINT64 offset, UINT64 bytes)
    {
        if (!IsSecondaryView(view, device) || offset % sizeof(UINT) || bytes > UINT_MAX) return false;
        const auto desc = view.Native->D3DResource->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && Fits(offset, bytes, desc.Width);
    }

    bool ValidDynamic(const DynamicRange& range, FrameStamp stamp, GDX12Device& device)
    {
        if (!(range.Stamp == stamp) || !range.VertexCount || !range.IndexCount ||
            range.IndexCount % 3 || range.MaxRelativeIndex >= range.VertexCount)
            return false;
        for (const auto* view : { &range.Vertices, &range.Indices })
            if (view->Version.IsValid() && view->Version.Invocation != stamp.Invocation) return false;
        return BufferRange(range.Vertices, device, range.VertexByteOffset, UINT64{range.VertexCount} * sizeof(Vertex)) &&
            BufferRange(range.Indices, device, range.IndexByteOffset, UINT64{range.IndexCount} * sizeof(UINT));
    }

    bool ValidGrass(const GrassBatch& batch, GDX12Device& device)
    {
        if (batch.Device != &device || !batch.Owner || batch.SamePlan == batch.Ready.IsValid() ||
            (batch.Ready.IsValid() && batch.Ready.Timeline().Producer.Location.Adapter != device.GetSubmissionAdapterId()))
            return false;
        for (const auto& read : batch.Reads)
            if (read.Mode != Mode::Read || !IsSecondaryView(read.View, device) ||
                read.Handle.Get() != read.View.Native->D3DResource.Get() ||
                (read.State & ~(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                    D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) != 0 ||
                read.State == D3D12_RESOURCE_STATE_COMMON)
                return false;
        return true;
    }

    struct AlphaLayout
    {
        D3D12_RESOURCE_DESC Desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
        std::vector<UINT> Rows;
        std::vector<UINT64> RowBytes;
        UINT64 UploadBytes = 0;
    };

    std::optional<AlphaLayout> DescribeAlpha(const AlphaTexture& alpha, GDX12Device& device)
    {
        if (!alpha.Texture.IsValid() || !alpha.ContentRevision || !alpha.Width || !alpha.Height ||
            alpha.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || alpha.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            !alpha.MipLevels || !DirectX::IsValid(alpha.Format) || !DirectX::HasAlpha(alpha.Format) ||
            DirectX::IsTypeless(alpha.Format) || DirectX::IsDepthStencil(alpha.Format) || DirectX::IsVideo(alpha.Format) ||
            DirectX::IsPlanar(alpha.Format, true))
            return {};
        UINT maxMips = 1;
        for (UINT extent = (std::max)(alpha.Width, alpha.Height); extent > 1; extent >>= 1) ++maxMips;
        if (alpha.MipLevels > maxMips) return {};
        D3D12_FEATURE_DATA_FORMAT_INFO planes{ alpha.Format, 0 };
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = alpha.Format;
        auto& native = *device.GetDevice().Get();
        if (FAILED(native.CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes))) ||
            planes.PlaneCount != 1 || FAILED(native.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return {};

        AlphaLayout layout;
        layout.Desc = CD3DX12_RESOURCE_DESC::Tex2D(alpha.Format, alpha.Width, alpha.Height, 1, static_cast<UINT16>(alpha.MipLevels));
        if (alpha.SecondaryView.Native)
        {
            if (!alpha.Mips.empty() || !IsSecondaryView(alpha.SecondaryView, device)) return {};
            const auto desc = alpha.SecondaryView.Native->D3DResource->GetDesc();
            if (desc.Dimension != layout.Desc.Dimension || desc.Width != alpha.Width || desc.Height != alpha.Height ||
                desc.DepthOrArraySize != 1 || desc.MipLevels != alpha.MipLevels || desc.Format != alpha.Format ||
                desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                return {};
            return layout;
        }

        if (alpha.Mips.size() != alpha.MipLevels) return {};
        layout.Footprints.resize(alpha.MipLevels);
        layout.Rows.resize(alpha.MipLevels);
        layout.RowBytes.resize(alpha.MipLevels);
        native.GetCopyableFootprints(&layout.Desc, 0, alpha.MipLevels, 0, layout.Footprints.data(), layout.Rows.data(),
            layout.RowBytes.data(), &layout.UploadBytes);
        if (!layout.UploadBytes || layout.UploadBytes >= SIZE_MAX) return {};
        for (UINT i = 0; i < alpha.MipLevels; ++i)
        {
            const auto& mip = alpha.Mips[i];
            const auto bytes = layout.RowBytes[i];
            const auto rows = layout.Rows[i];
            if (!rows || !bytes || mip.RowPitch < bytes || mip.Bytes.size() < bytes ||
                rows - 1 > (mip.Bytes.size() - bytes) / mip.RowPitch)
                return {};
        }
        return layout;
    }

    AlphaUpload MakeAlphaUpload(GDX12Device& device, const AlphaTexture& alpha, const AlphaLayout& layout,
        const GDX12ResourceHandle& destination)
    {
        auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(layout.UploadBytes),
            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        ThrowIfFailed(upload->Map(0, &noRead, &mapped));
        for (UINT mip = 0; mip < alpha.MipLevels; ++mip)
        {
            const auto& input = alpha.Mips[mip];
            const auto& footprint = layout.Footprints[mip];
            auto* dst = static_cast<std::byte*>(mapped) + footprint.Offset;
            for (UINT row = 0; row < layout.Rows[mip]; ++row)
                std::memcpy(dst + UINT64{row} * footprint.Footprint.RowPitch,
                    input.Bytes.data() + UINT64{row} * input.RowPitch, static_cast<size_t>(layout.RowBytes[mip]));
        }
        upload->Unmap(0, nullptr);
        return { std::move(upload), destination, layout.Footprints };
    }

    bool IsMaterialHeap(GDX12DescriptorHeap& heap, GDX12Device& device)
    {
        ComPtr<ID3D12Device14> owner;
        return heap.GetHeap() && SUCCEEDED(heap.GetHeap()->GetDevice(IID_PPV_ARGS(&owner))) &&
            owner.Get() == device.GetDevice().Get() && heap.GetType() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            (heap.GetFlags() & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
            heap.GetNumDescriptors() >= Texture2D_StartIndex + Texture2D_RangeLength;
    }

    void BindAlphaDescriptor(GDX12Descriptor& descriptor, const AlphaTexture& alpha, const ResourceView& view,
        GDX12DescriptorHeap& heap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = alpha.Format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = alpha.MipLevels;
        const auto index = heap.GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        descriptor.InitAsSRV(view.Native->D3DResource.Get(), &desc, &heap, index);
    }

    bool SameAlpha(const AlphaTexture& a, const AlphaTexture& b)
    {
        if (a.Format != b.Format || a.Width != b.Width || a.Height != b.Height ||
            a.MipLevels != b.MipLevels || a.Mips.size() != b.Mips.size() || a.SecondaryView.Native != b.SecondaryView.Native ||
            a.SecondarySRVIndex != b.SecondarySRVIndex || a.SecondarySRVHeap != b.SecondarySRVHeap)
            return false;
        if (a.SecondaryView.Native)
            return SameIdentity(a.SecondaryView.Identity, b.SecondaryView.Identity) && a.SecondaryView.Version == b.SecondaryView.Version;
        for (size_t i = 0; i < a.Mips.size(); ++i)
            if (a.Mips[i].RowPitch != b.Mips[i].RowPitch || a.Mips[i].Bytes != b.Mips[i].Bytes) return false;
        return true;
    }

    template<class Records, class MakeKey, class Equal>
    std::vector<UINT> Canonicalize(const Records& records, std::vector<bool>& valid, MakeKey key, Equal equal)
    {
        std::map<decltype(key(records.front())), UINT> first;
        std::vector<UINT> canonical(records.size(), UINT_MAX);
        for (UINT i = 0; i < records.size(); ++i)
        {
            if (!valid[i]) continue;
            const auto [it, inserted] = first.emplace(key(records[i]), i);
            canonical[i] = it->second;
            if (!inserted && !equal(records[it->second], records[i])) valid[it->second] = false;
        }
        return canonical;
    }

    struct Upload
    {
        GDX12ResourceHandle Source, Destination;
        UINT64 Bytes;
    };

    D3D12_VERTEX_BUFFER_VIEW Vertices(const ResourceView& view, UINT64 offset, UINT count)
    {
        return { view.Native->D3DResource->GetGPUVirtualAddress() + offset,
            static_cast<UINT>(UINT64{count} * sizeof(Vertex)), sizeof(Vertex) };
    }

    D3D12_INDEX_BUFFER_VIEW Indices(const ResourceView& view, UINT64 offset, UINT count)
    {
        return { view.Native->D3DResource->GetGPUVirtualAddress() + offset,
            static_cast<UINT>(UINT64{count} * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
    }
}

struct Slot::Storage
{
    GDX12Device* Device;
    std::shared_ptr<GDX12DescriptorHeap> Heap;
    PreparedFrame Frame;
    std::vector<std::unique_ptr<GDX12Descriptor>> Descriptors;
    std::vector<Upload> Uploads;
    std::vector<AlphaUpload> AlphaUploads;
    std::vector<std::shared_ptr<const void>> ExternalOwners;

    ResourceView Destination(const D3D12_RESOURCE_DESC& desc, const NewIdentity& newIdentity)
    {
        ResourceView view{ std::make_shared<GDX12Resource>(), newIdentity() };
        if (!ValidIdentity(view, Device->GetSubmissionAdapterId()) || view.Identity.Transfer != TransferId::Invalid)
            throw std::invalid_argument("Invalid owned scene replica resource identity");
        view.Native->D3DResource = Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
        AddAccess(Frame.UploadAccesses, view, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST);
        return view;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE CreateSRV(const ResourceView& view, D3D12_SHADER_RESOURCE_VIEW_DESC desc, UINT start, UINT count)
    {
        auto descriptor = std::make_unique<GDX12Descriptor>();
        descriptor->InitAsSRV(view.Native->D3DResource.Get(), &desc, Heap.get(), Heap->GetAvailableIndex(start, count));
        const auto handle = descriptor->GPUHandle;
        Descriptors.push_back(std::move(descriptor));
        return handle;
    }

    template<class T>
    Buffer UploadBuffer(const std::vector<T>& data, const NewIdentity& newIdentity, D3D12_RESOURCE_STATES state, bool srv)
    {
        const UINT64 bytes = UINT64{data.size()} * sizeof(T);
        auto view = Destination(CD3DX12_RESOURCE_DESC::Buffer(bytes), newIdentity);
        auto upload = Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        ThrowIfFailed(upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, data.data(), static_cast<size_t>(bytes));
        upload->Unmap(0, nullptr);
        Uploads.push_back({ std::move(upload), view.Native->D3DResource, bytes });
        AddAccess(Frame.RasterAccesses, view, Mode::Read, state);
        Buffer buffer{ view };
        if (srv)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.NumElements = static_cast<UINT>(data.size());
            desc.Buffer.StructureByteStride = sizeof(T);
            buffer.SRV = CreateSRV(view, desc, TextureResources_StartIndex, TextureResources_RangeLength);
        }
        return buffer;
    }

    UINT BindAlpha(const AlphaTexture& alpha, const AlphaLayout& layout, const NewIdentity& newIdentity)
    {
        auto view = alpha.SecondaryView;
        if (!view.Native)
        {
            view = Destination(layout.Desc, newIdentity);
            AlphaUploads.push_back(MakeAlphaUpload(*Device, alpha, layout, view.Native->D3DResource));
        }
        else ExternalOwners.push_back(view.Owner);
        AddAccess(Frame.RasterAccesses, view, Mode::Read, PixelRead);

        if (alpha.SecondarySRVIndex != UINT_MAX) return alpha.SecondarySRVIndex - Texture2D_StartIndex;
        auto descriptor = std::make_unique<GDX12Descriptor>();
        BindAlphaDescriptor(*descriptor, alpha, view, *Heap);
        const auto index = descriptor->HeapIndex;
        Descriptors.push_back(std::move(descriptor));
        return index - Texture2D_StartIndex;
    }
};

std::optional<StagedAlpha> StageAlpha(GDX12Device& device, GDX12DescriptorHeap& heap,
    const AlphaTexture& source, const NewIdentity& newIdentity)
{
    if (source.SecondarySRVIndex != UINT_MAX || source.SecondarySRVHeap || !IsMaterialHeap(heap, device)) return {};
    const auto layout = DescribeAlpha(source, device);
    if (!layout) return {};
    auto view = source.SecondaryView;
    std::optional<AlphaUpload> upload;
    if (!view.Native)
    {
        view = { std::make_shared<GDX12Resource>(), newIdentity() };
        if (!ValidIdentity(view, device.GetSubmissionAdapterId()) || view.Identity.Transfer != TransferId::Invalid)
            throw std::invalid_argument("Invalid resident alpha resource identity");
        view.Native->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, layout->Desc, D3D12_RESOURCE_STATE_COMMON);
        upload = MakeAlphaUpload(device, source, *layout, view.Native->D3DResource);
    }
    auto descriptor = std::make_shared<GDX12Descriptor>();
    BindAlphaDescriptor(*descriptor, source, view, heap);
    AlphaTexture texture;
    texture.Texture = source.Texture;
    texture.ContentRevision = source.ContentRevision;
    texture.Format = source.Format;
    texture.Width = source.Width;
    texture.Height = source.Height;
    texture.MipLevels = source.MipLevels;
    texture.SecondaryView = std::move(view);
    texture.SecondarySRVIndex = descriptor->HeapIndex;
    texture.SecondarySRVHeap = &heap;
    return StagedAlpha{ std::move(texture), std::move(descriptor), std::move(upload) };
}

void RecordAlphaUpload(GDX12CommandList& commands, const AlphaUpload& upload)
{
    auto* list = commands.GetCommandList().Get();
    for (UINT mip = 0; mip < upload.Footprints.size(); ++mip)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION dst(upload.Destination.Get(), mip);
        const CD3DX12_TEXTURE_COPY_LOCATION src(upload.Source.Get(), upload.Footprints[mip]);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
}

bool FrameStamp::IsValid() const
{
    return Frame != GDX12Submission::FrameSubmissionId::Invalid &&
        Invocation != GDX12Submission::ProducerInvocationId::Invalid;
}

Slot::Slot() = default;
Slot::~Slot() = default;

const PreparedFrame& Slot::Get() const
{
    if (!_storage) throw std::logic_error("Scene replica slot has not been prepared");
    return _storage->Frame;
}

void Prepare(const std::shared_ptr<Slot>& slot, GDX12Device& secondary,
    const std::shared_ptr<GDX12DescriptorHeap>& heap, FrameStamp expected, const Snapshot& snapshot,
    const NewIdentity& newIdentity)
{
    if (!slot || slot.use_count() != 1) throw std::invalid_argument("Scene replica slot is retained");
    slot->_storage.reset();
    if (!expected.IsValid() || !(snapshot.Stamp == expected) || !secondary.GetDevice() || !heap || !newIdentity)
        throw std::invalid_argument("Scene replica snapshot, device or heap is unavailable");
    if (!IsMaterialHeap(*heap, secondary))
        throw std::invalid_argument("Scene replica needs the secondary bindless SRV heap");

    for (const auto count : { snapshot.Meshes.size(), snapshot.DynamicGeometry.size(), snapshot.GrassBatches.size(),
        snapshot.AlphaTextures.size(), snapshot.Materials.size(), snapshot.Casters.size(), snapshot.Groups.size() })
        if (count >= UINT_MAX) throw std::length_error("Scene replica source count exceeds the shader index range");

    std::vector<bool> validMeshes;
    for (const auto& mesh : snapshot.Meshes) validMeshes.push_back(ValidMesh(mesh, secondary));
    const auto meshCanonical = Canonicalize(snapshot.Meshes, validMeshes, [](const MeshRange& mesh)
    {
        return std::make_tuple(mesh.Mesh.GetValue(), mesh.Mesh.GetGeneration(), mesh.ContentRevision, mesh.Submesh);
    }, SameMesh);
    std::vector<bool> validDynamic;
    for (const auto& range : snapshot.DynamicGeometry) validDynamic.push_back(ValidDynamic(range, expected, secondary));
    std::vector<bool> validGrass;
    for (const auto& batch : snapshot.GrassBatches) validGrass.push_back(ValidGrass(batch, secondary));
    std::vector<std::optional<AlphaLayout>> alphaLayouts;
    std::vector<bool> validAlpha;
    for (const auto& alpha : snapshot.AlphaTextures)
    {
        const bool descriptorMatches = alpha.SecondarySRVIndex == UINT_MAX ? !alpha.SecondarySRVHeap :
            alpha.SecondaryView.Native && alpha.SecondarySRVHeap == heap.get() &&
                alpha.SecondarySRVIndex >= Texture2D_StartIndex &&
                alpha.SecondarySRVIndex < Texture2D_StartIndex + Texture2D_RangeLength;
        alphaLayouts.push_back(descriptorMatches ? DescribeAlpha(alpha, secondary) : std::nullopt);
        validAlpha.push_back(alphaLayouts.back().has_value());
    }
    const auto alphaCanonical = Canonicalize(snapshot.AlphaTextures, validAlpha, [](const AlphaTexture& alpha)
    {
        return std::make_tuple(alpha.Texture.GetValue(), alpha.Texture.GetGeneration(), alpha.ContentRevision);
    }, SameAlpha);

    const auto check = [&](UINT index, size_t group)
    {
        if (index >= snapshot.Casters.size() || snapshot.Casters[index].SourceId == UINT64_MAX)
            return Rejection::MissingCaster;
        const auto& caster = snapshot.Casters[index];
        if (!caster.Transform || !caster.Bounds || !Finite(caster.Transform->WorldMatrix) ||
            !Finite(caster.Transform->PrevWorldMatrix) || !Finite(caster.Bounds->Center) ||
            !Finite(caster.Bounds->Extents) || caster.Bounds->Extents.x < 0 ||
            caster.Bounds->Extents.y < 0 || caster.Bounds->Extents.z < 0)
            return Rejection::MissingTransform;
        if (caster.Kind == GeometryKind::ExpandedGrass)
        {
            if (caster.Range >= validGrass.size()) return Rejection::MissingGeometry;
            const auto& grass = snapshot.GrassBatches[caster.Range];
            if (grass.GroupIndex != group || grass.SourceId != caster.SourceId) return Rejection::MissingGeometry;
            if (!(grass.Stamp == expected)) return Rejection::StaleGeometry;
            if (!validGrass[caster.Range]) return Rejection::MissingGeometry;
            if (caster.MaterialIndex == UINT_MAX) return Rejection::None;
        }
        else if (caster.Kind == GeometryKind::Static)
        {
            if (caster.Range >= meshCanonical.size() || meshCanonical[caster.Range] == UINT_MAX ||
                !validMeshes[meshCanonical[caster.Range]])
                return Rejection::MissingGeometry;
            if (caster.GeometryRevision != snapshot.Meshes[caster.Range].ContentRevision)
                return Rejection::StaleGeometry;
        }
        else if (caster.Kind == GeometryKind::Skinned)
        {
            if (caster.Range >= validDynamic.size()) return Rejection::MissingGeometry;
            if (!(snapshot.DynamicGeometry[caster.Range].Stamp == expected)) return Rejection::StaleGeometry;
            if (!validDynamic[caster.Range]) return Rejection::MissingGeometry;
        }
        else return Rejection::MissingGeometry;
        if (caster.MaterialIndex >= snapshot.Materials.size()) return Rejection::MissingMaterial;
        const auto& material = snapshot.Materials[caster.MaterialIndex];
        if (material.Value.RenderLayer > 1) return Rejection::MissingMaterial;
        if (material.Value.RenderLayer == 1 &&
            (material.AlphaTextureIndex >= alphaCanonical.size() || alphaCanonical[material.AlphaTextureIndex] == UINT_MAX ||
             !validAlpha[alphaCanonical[material.AlphaTextureIndex]] ||
             material.AlphaRevision != snapshot.AlphaTextures[material.AlphaTextureIndex].ContentRevision))
            return Rejection::MissingAlpha;
        return Rejection::None;
    };

    auto storage = std::make_unique<Slot::Storage>();
    storage->Device = &secondary;
    storage->Heap = heap;
    auto& frame = storage->Frame;
    frame.Stamp = expected;
    frame.Groups.resize(snapshot.Groups.size());
    frame.MeshToLocal.assign(snapshot.Meshes.size(), UINT_MAX);
    frame.MaterialToLocal.assign(snapshot.Materials.size(), UINT_MAX);
    for (size_t i = 0; i < snapshot.Groups.size(); ++i)
        for (const auto caster : snapshot.Groups[i])
        {
            const auto reason = check(caster, i);
            if (reason == Rejection::None) continue;
            frame.Groups[i].Reason = reason;
            break;
        }

    std::vector<UINT> casterToDraw(snapshot.Casters.size(), UINT_MAX);
    std::vector<UINT> alphaSlots(snapshot.AlphaTextures.size(), UINT_MAX);
    std::vector<Vertex> vertices;
    std::vector<UINT> indices;
    std::vector<GDX12MaterialConstants> materials;
    std::vector<GDX12TransformConstants> transforms;
    std::vector<GDX12InstanceData> instances;
    std::vector<GDX12IndirectDrawArgs> indirect;
    const auto bindMaterial = [&](UINT sourceIndex)
    {
        auto& localIndex = frame.MaterialToLocal[sourceIndex];
        if (localIndex != UINT_MAX) return localIndex;

        const auto& original = snapshot.Materials[sourceIndex];
        auto material = original.Value;
        material.DiffuseIndex = material.NormalIndex = material.DisplacementIndex = 0;
        material.SpecularIndex = material.RoughnessIndex = material.EmissiveIndex = 0;
        if (material.RenderLayer == 1)
        {
            const UINT alpha = alphaCanonical[original.AlphaTextureIndex];
            if (alphaSlots[alpha] == UINT_MAX)
                alphaSlots[alpha] = storage->BindAlpha(snapshot.AlphaTextures[alpha], *alphaLayouts[alpha], newIdentity);
            material.DiffuseIndex = alphaSlots[alpha];
        }
        localIndex = static_cast<UINT>(materials.size());
        materials.push_back(material);
        return localIndex;
    };
    for (size_t i = 0; i < snapshot.Groups.size(); ++i)
    {
        auto& group = frame.Groups[i];
        if (group.Reason != Rejection::None) continue;
        for (UINT casterIndex : snapshot.Groups[i])
        {
            const auto& caster = snapshot.Casters[casterIndex];
            if (caster.Kind == GeometryKind::ExpandedGrass)
            {
                if (caster.MaterialIndex != UINT_MAX) bindMaterial(caster.MaterialIndex);
                group.Grass.push_back(snapshot.GrassBatches[caster.Range]);
                continue;
            }
            auto& drawIndex = casterToDraw[casterIndex];
            if (drawIndex == UINT_MAX)
            {
                const UINT materialIndex = bindMaterial(caster.MaterialIndex);

                drawIndex = static_cast<UINT>(frame.Draws.size());
                const bool dynamic = caster.Dynamic || caster.Kind != GeometryKind::Static;
                Draw draw{ caster.SourceId, caster.Kind, caster.ShadowFlags, dynamic, *caster.Bounds, {}, {},
                    { drawIndex, 0, 1, 0, 0, 0 },
                    UINT64{drawIndex} * sizeof(GDX12IndirectDrawArgs) };
                if (caster.Kind == GeometryKind::Static)
                {
                    const UINT meshIndex = meshCanonical[caster.Range];
                    const auto& mesh = snapshot.Meshes[meshIndex];
                    auto& rangeIndex = frame.MeshToLocal[meshIndex];
                    if (mesh.Resident)
                    {
                        const auto& resident = *mesh.Resident;
                        draw.Vertices = Vertices(resident.Vertices, 0, resident.VertexCount);
                        draw.Indices = Indices(resident.Indices, 0, resident.IndexCount);
                        AddAccess(frame.RasterAccesses, resident.Vertices, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                        AddAccess(frame.RasterAccesses, resident.Indices, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                        if (rangeIndex == UINT_MAX)
                        {
                            rangeIndex = static_cast<UINT>(frame.PackedRanges.size());
                            frame.PackedRanges.push_back({ 0, resident.IndexCount, 0 });
                        }
                    }
                    else if (rangeIndex == UINT_MAX)
                    {
                        if (!Fits(vertices.size(), mesh.VertexCount, UINT_MAX / sizeof(Vertex)) ||
                            !Fits(indices.size(), mesh.IndexCount, UINT_MAX / sizeof(UINT)))
                            throw std::length_error("Packed scene replica geometry exceeds D3D12 IA limits");
                        rangeIndex = static_cast<UINT>(frame.PackedRanges.size());
                        frame.PackedRanges.push_back({ static_cast<UINT>(indices.size()), mesh.IndexCount, static_cast<INT>(vertices.size()) });
                        const auto* src = mesh.Vertices.data() + mesh.FirstVertex;
                        vertices.insert(vertices.end(), src, src + mesh.VertexCount);
                        const auto* idx = mesh.Indices.data() + mesh.FirstIndex;
                        for (UINT j = 0; j < mesh.IndexCount; ++j) indices.push_back(RelativeIndex(mesh, idx[j]));
                    }
                    const auto& range = frame.PackedRanges[rangeIndex];
                    frame.MeshToLocal[caster.Range] = rangeIndex;
                    draw.Arguments.IndexCountPerInstance = range.IndexCount;
                    draw.Arguments.StartIndexLocation = range.FirstIndex;
                    draw.Arguments.BaseVertexLocation = range.BaseVertex;
                }
                else
                {
                    const auto& range = snapshot.DynamicGeometry[caster.Range];
                    draw.Vertices = Vertices(range.Vertices, range.VertexByteOffset, range.VertexCount);
                    draw.Indices = Indices(range.Indices, range.IndexByteOffset, range.IndexCount);
                    draw.Arguments.IndexCountPerInstance = range.IndexCount;
                    AddAccess(frame.RasterAccesses, range.Vertices, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                    AddAccess(frame.RasterAccesses, range.Indices, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                    storage->ExternalOwners.push_back(range.Vertices.Owner);
                    storage->ExternalOwners.push_back(range.Indices.Owner);
                }
                GDX12InstanceData instance{};
                instance.TransformIndex = drawIndex;
                instance.MaterialIndex = materialIndex;
                instance.ReflectionDynamic = dynamic;
                instance.BoundingBoxCenter = caster.Bounds->Center;
                instance.BoundingBoxExtents = caster.Bounds->Extents;
                instances.push_back(instance);
                transforms.push_back(*caster.Transform);
                indirect.push_back(draw.Arguments);
                frame.Draws.push_back(draw);
            }
            group.Draws.push_back(drawIndex);
        }
    }

    if (!vertices.empty())
    {
        const auto vb = storage->UploadBuffer(vertices, newIdentity, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
        const auto ib = storage->UploadBuffer(indices, newIdentity, D3D12_RESOURCE_STATE_INDEX_BUFFER, false);
        for (auto& draw : frame.Draws)
            if (draw.Kind == GeometryKind::Static && !draw.Vertices.BufferLocation)
            {
                draw.Vertices = Vertices(vb.View, 0, static_cast<UINT>(vertices.size()));
                draw.Indices = Indices(ib.View, 0, static_cast<UINT>(indices.size()));
            }
    }
    if (!frame.Draws.empty())
    {
        frame.Instances = storage->UploadBuffer(instances, newIdentity, ShaderRead, true);
        frame.Transforms = storage->UploadBuffer(transforms, newIdentity, ShaderRead, true);
        frame.Indirect = storage->UploadBuffer(indirect, newIdentity, ShaderRead | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, true);
    }
    if (!materials.empty())
    {
        frame.Materials = storage->UploadBuffer(materials, newIdentity, ShaderRead | PixelRead, true);
        frame.AlphaTable = heap->GetGPUHandle(Texture2D_StartIndex);
    }
    slot->_storage = std::move(storage);
}

void RecordUploads(GDX12CommandList& commands, const Slot& slot)
{
    const auto& frame = slot.Get();
    auto* list = commands.GetCommandList().Get();
    ComPtr<ID3D12Device14> owner;
    ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&owner)));
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || owner.Get() != slot._storage->Device->GetDevice().Get())
        throw std::invalid_argument("Scene replica uploads require the secondary DIRECT list");
    for (const auto& access : frame.UploadAccesses)
        if (access.View.Native->D3DResource.Get() != access.Handle.Get())
            throw std::logic_error("Scene replica upload target changed after preparation");
    for (const auto& upload : slot._storage->Uploads)
        list->CopyBufferRegion(upload.Destination.Get(), 0, upload.Source.Get(), 0, upload.Bytes);
    for (const auto& upload : slot._storage->AlphaUploads) RecordAlphaUpload(commands, upload);
}
}
