#pragma once

#include "App.Base/Rendering/CrossAdapterShadowRaster.h"
#include "Rendering/Grass/BistroGrassGeometry.h"

// G2's field transfer is a separate boundary before either S2 atlas replacement.
// The primary raster may continue once its direct staging copy is issued.
class BistroGrassShadowReplica
{
public:
    using Layout = GDX12GrassFieldProducer::SnapshotLayout;
    using Stamp = CrossAdapterSceneReplica::FrameStamp;
    using CompletionPoint = GDX12Submission::CompletionPoint;

    struct Field
    {
        Stamp Frame;
        Layout Manifest;
        GDX12Snapshot::PhysicalView Primary;
        GDX12Submission::TransferVersion SourceVersion;
        std::shared_ptr<const void> Owner;
    };

    struct ImportedField
    {
        Stamp Frame;
        Layout Manifest;
        GDX12Snapshot::PhysicalView Secondary;
        GDX12Submission::TransferVersion Version;
        CompletionPoint PrimaryStaged;
        CompletionPoint ImportReady;
    };

    struct Reader
    {
        std::shared_ptr<const ImportedField> Field;
        std::uint64_t Id = 0;
    };

    struct FrozenScene
    {
        Stamp Frame;
        BistroGrassGeometry::ShadowScene Scene;
    };
    struct LocalField
    {
        Stamp Frame;
        Layout Manifest;
        GDX12Snapshot::PhysicalView View;
        GDX12Submission::TransferVersion Version;
        CompletionPoint Ready;
        std::shared_ptr<const void> Owner;
    };
    struct MaterialBinding
    {
        CrossAdapterSceneReplica::Buffer Table;
        // Indexed by the patch's original primary material slot. UINT_MAX rejects the group.
        std::vector<UINT> ToLocal;
        std::vector<CrossAdapterShadows::Access> AlphaReads;
        std::shared_ptr<const void> Owner;
    };
    struct GroupRequest
    {
        GDX12ShadowOutput Output;
        UINT ReplicaGroup = UINT_MAX;
        Vector3 Eye = Vector3::Zero;
        std::vector<GDX12RenderPrimitiveHandle> Patches;
    };
    using GroupBatches = std::optional<std::vector<CrossAdapterSceneReplica::GrassBatch>>;
    using GroupViews = std::optional<std::array<std::optional<CrossAdapterShadowRaster::ApplicationView>, 6>>;
    struct PreparedAtlas
    {
        // Indexed in request order. A missing recipe leaves the complete group primary.
        std::vector<GroupViews> Groups;
        std::vector<CrossAdapterShadows::RasterStage> Preparation;
    };

    // Supply these batches when capturing the S2R scene. The raster uses their exact readiness.
    std::vector<GroupBatches> MakeBatches(GDX12Device& device, const FrozenScene& capture,
        const LocalField& field, const std::vector<GroupRequest>& groups);
    // Supply the lane's material binding after the capture/remap, then attach these per-face views.
    PreparedAtlas Prepare(GDX12Device& device, std::shared_ptr<GDX12DescriptorHeap> heap,
        const FrozenScene& capture, const LocalField& field,
        const std::vector<GroupRequest>& groups, const MaterialBinding& materials);

    BistroGrassShadowReplica(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary);
    ~BistroGrassShadowReplica();
    BistroGrassShadowReplica(const BistroGrassShadowReplica&) = delete;
    BistroGrassShadowReplica& operator=(const BistroGrassShadowReplica&) = delete;

    // After the shared runtime polls, release idle raster aliases before either field bank can resize.
    void CollectRetired();

    // Prefix already covers the primary grass prepare/import and releases the source to COMMON.
    // An empty result means no native work for this request was submitted.
    std::shared_ptr<const ImportedField> SubmitField(const Field& field, CompletionPoint prefix);
    // One lease for each submitted atlas using the field. Bind the actual secondary DIRECT tail.
    std::optional<Reader> AcquireReader(const std::shared_ptr<const ImportedField>& field);
    void BindReader(const Reader& reader, CompletionPoint secondaryTail);
    void CancelReader(const Reader& reader);
    // Close the demand after both atlas admission outcomes; submitted reader fences remain protected.
    void FinishField();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
