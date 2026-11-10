#pragma once

#include "Engine.Core/AssetHandles.h"
#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12ResourceHandle.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class GDX12CommandList;
class GDX12Descriptor;
class GDX12DescriptorHeap;
class GDX12Device;

namespace CrossAdapterSceneReplica
{
    struct FrameStamp
    {
        GDX12Submission::FrameSubmissionId Frame = GDX12Submission::FrameSubmissionId::Invalid;
        GDX12Submission::ProducerInvocationId Invocation = GDX12Submission::ProducerInvocationId::Invalid;
        std::uint64_t SceneRevision = 0;

        bool IsValid() const;
        friend bool operator==(const FrameStamp& a, const FrameStamp& b)
        { return a.Frame == b.Frame && a.Invocation == b.Invocation && a.SceneRevision == b.SceneRevision; }
    };

    struct ResourceView
    {
        std::shared_ptr<GDX12Resource> Native;
        GDX12Submission::ResourceIdentity Identity{};
        GDX12Submission::TransferVersion Version{};
        std::shared_ptr<const void> Owner;
    };
    struct Access
    {
        ResourceView View;
        GDX12ResourceHandle Handle;
        GDX12RenderGraph::Mode Mode;
        D3D12_RESOURCE_STATES State;
    };

    enum class IndexAddressing { RelativeToRange, AbsoluteInPayload };
    struct ResidentGeometry
    {
        ResourceView Vertices, Indices;
        UINT VertexCount = 0, IndexCount = 0, MaxRelativeIndex = 0;
        UINT64 VertexByteOffset = 0, IndexByteOffset = 0;
    };
    struct MeshRange
    {
        Engine::Core::MeshHandle Mesh;
        std::uint64_t ContentRevision = 0;
        UINT Submesh = 0;
        std::vector<Engine::Core::Vertex> Vertices;
        std::vector<std::uint32_t> Indices;
        UINT FirstVertex = 0, VertexCount = 0, FirstIndex = 0, IndexCount = 0;
        IndexAddressing Addressing = IndexAddressing::RelativeToRange;
        std::optional<ResidentGeometry> Resident;
    };

    enum class GeometryKind { Static, Skinned, ExpandedGrass };
    struct DynamicRange
    {
        FrameStamp Stamp;
        ResourceView Vertices, Indices;
        UINT64 VertexByteOffset = 0, IndexByteOffset = 0;
        UINT VertexCount = 0, IndexCount = 0, MaxRelativeIndex = 0;
    };
    struct GrassBatch
    {
        FrameStamp Stamp;
        UINT GroupIndex = UINT_MAX;
        std::uint64_t SourceId = UINT64_MAX;
        const GDX12Device* Device = nullptr;
        std::shared_ptr<const void> Owner;
        // A same-plan dependency is declared by S2. A separate producer must supply its issued point.
        bool SamePlan = false;
        GDX12Submission::CompletionPoint Ready;
        std::vector<Access> Reads;
    };

    struct AlphaMip
    {
        std::vector<std::byte> Bytes;
        UINT64 RowPitch = 0;
    };
    struct AlphaTexture
    {
        Engine::Core::TextureHandle Texture;
        std::uint64_t ContentRevision = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        UINT Width = 0, Height = 0, MipLevels = 0;
        std::vector<AlphaMip> Mips;
        ResourceView SecondaryView;
        UINT SecondarySRVIndex = UINT_MAX;
        const GDX12DescriptorHeap* SecondarySRVHeap = nullptr;
    };

    struct Material
    {
        GDX12MaterialConstants Value{};
        UINT AlphaTextureIndex = UINT_MAX;
        std::uint64_t AlphaRevision = 0;
    };

    struct Caster
    {
        std::uint64_t SourceId = UINT64_MAX;
        GeometryKind Kind = GeometryKind::Static;
        UINT Range = UINT_MAX, MaterialIndex = UINT_MAX;
        std::uint64_t GeometryRevision = 0;
        std::optional<GDX12TransformConstants> Transform;
        std::optional<DirectX::BoundingBox> Bounds;
        UINT ShadowFlags = 0;
        bool Dynamic = false;
    };

    struct Snapshot
    {
        FrameStamp Stamp;
        std::vector<MeshRange> Meshes;
        std::vector<DynamicRange> DynamicGeometry;
        std::vector<GrassBatch> GrassBatches;
        std::vector<AlphaTexture> AlphaTextures;
        std::vector<Material> Materials;
        std::vector<Caster> Casters;
        // Each entry describes one complete selected sun, point or spot group.
        std::vector<std::vector<UINT>> Groups;
    };

    enum class Rejection { None, MissingCaster, MissingTransform, MissingGeometry, StaleGeometry, MissingMaterial, MissingAlpha };
    struct Group
    {
        Rejection Reason = Rejection::None;
        std::vector<UINT> Draws;
        std::vector<GrassBatch> Grass;
    };

    struct PackedRange
    {
        UINT FirstIndex, IndexCount;
        INT BaseVertex;
    };
    struct Draw
    {
        std::uint64_t SourceId;
        GeometryKind Kind;
        UINT ShadowFlags;
        bool Dynamic;
        DirectX::BoundingBox Bounds;
        D3D12_VERTEX_BUFFER_VIEW Vertices;
        D3D12_INDEX_BUFFER_VIEW Indices;
        GDX12IndirectDrawArgs Arguments;
        UINT64 IndirectByteOffset;
    };
    struct Buffer
    {
        ResourceView View;
        D3D12_GPU_DESCRIPTOR_HANDLE SRV{};
    };
    struct PreparedFrame
    {
        FrameStamp Stamp;
        std::vector<Group> Groups;
        std::vector<PackedRange> PackedRanges;
        std::vector<UINT> MeshToLocal, MaterialToLocal;
        std::vector<Draw> Draws;
        Buffer Instances, Transforms, Materials, Indirect;
        D3D12_GPU_DESCRIPTOR_HANDLE AlphaTable{};
        std::vector<Access> UploadAccesses, RasterAccesses;
    };

    using NewIdentity = std::function<GDX12Submission::ResourceIdentity()>;

    struct AlphaUpload
    {
        GDX12ResourceHandle Source, Destination;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
    };
    struct StagedAlpha
    {
        AlphaTexture Texture;
        std::shared_ptr<GDX12Descriptor> Descriptor;
        std::optional<AlphaUpload> Upload;
    };
    std::optional<StagedAlpha> StageAlpha(GDX12Device& device, GDX12DescriptorHeap& heap,
        const AlphaTexture& source, const NewIdentity& newIdentity);
    void RecordAlphaUpload(GDX12CommandList& commands, const AlphaUpload& upload);

    class Slot
    {
    public:
        Slot();
        ~Slot();
        const PreparedFrame& Get() const;

    private:
        friend void Prepare(const std::shared_ptr<Slot>&, GDX12Device&, const std::shared_ptr<GDX12DescriptorHeap>&,
            FrameStamp, const Snapshot&, const NewIdentity&);
        friend void RecordUploads(GDX12CommandList&, const Slot&);

        struct Storage;
        std::unique_ptr<Storage> _storage;
    };

    // The pool owns the sole reference when preparing. KeepAlive retains the slot through the last reader.
    // External resources must already have a same-plan producer edge or exact issued readiness.
    void Prepare(const std::shared_ptr<Slot>& slot, GDX12Device& secondary,
        const std::shared_ptr<GDX12DescriptorHeap>& heap, FrameStamp expected, const Snapshot& snapshot,
        const NewIdentity& newIdentity);
    void RecordUploads(GDX12CommandList& commands, const Slot& slot);
}
