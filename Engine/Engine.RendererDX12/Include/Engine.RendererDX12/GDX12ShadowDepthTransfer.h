#pragma once

#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <bitset>
#include <memory>
#include <optional>

class GDX12CommandList;
class GDX12Device;
class GDX12Resource;
class GDX12RootSignature;

enum class GDX12ShadowAtlas { Sun, Local };

struct GDX12ShadowTransferView
{
    GDX12ShadowRegion Region;
    UINT PayloadLayer;
};

class GDX12ShadowTransferLayout
{
public:
    static std::optional<GDX12ShadowTransferLayout> Build(GDX12ShadowAtlas atlas,
        std::vector<GDX12ShadowOutput> selected);

    GDX12ShadowAtlas Atlas() const { return _atlas; }
    UINT Extent() const { return _extent; }
    UINT Layers() const { return static_cast<UINT>(_sourceViews.size()); }
    const std::vector<GDX12ShadowTransferView>& SourceViews() const { return _sourceViews; }
    const std::vector<GDX12ShadowOutput>& Outputs() const { return _groups; }
    std::optional<std::vector<GDX12ShadowOutput>> ResolveSnapshot(
        const std::vector<GDX12ShadowOutput>& prepared) const;
    std::optional<std::vector<GDX12ShadowTransferView>> ResolveDestination(
        const std::vector<GDX12ShadowOutput>& prepared) const;

private:
    GDX12ShadowTransferLayout(GDX12ShadowAtlas atlas, UINT extent) : _atlas(atlas), _extent(extent) {}

    GDX12ShadowAtlas _atlas;
    UINT _extent;
    std::vector<GDX12ShadowOutput> _groups;
    std::vector<GDX12ShadowTransferView> _sourceViews;
};

class GDX12ShadowDepthTransfer
{
public:
    explicit GDX12ShadowDepthTransfer(GDX12Device& device);
    ~GDX12ShadowDepthTransfer();

    struct Pack;
    struct Restore;
    using InitializedSlices = std::bitset<D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION>;

    std::shared_ptr<Pack> PreparePack(const GDX12ShadowTransferLayout& layout,
        std::shared_ptr<GDX12Resource> depth, std::shared_ptr<GDX12Resource> payload) const;
    std::shared_ptr<Restore> PrepareRestore(const GDX12ShadowTransferLayout& layout,
        const std::vector<GDX12ShadowOutput>& prepared, std::shared_ptr<GDX12Resource> depth,
        std::shared_ptr<GDX12Resource> payload, const InitializedSlices& initialized = {}) const;

    // Pass the resources resolved by the executor and retain the binding and pipeline owners.
    // The executor owns all barriers on these bound resources.
    void RecordPack(GDX12CommandList& commands, Pack& pack,
        const ID3D12Resource* depth, const ID3D12Resource* payload) const;
    void RecordRestore(GDX12CommandList& commands, Restore& restore,
        const ID3D12Resource* depth, const ID3D12Resource* payload) const;

private:
    GDX12Device& _device;
    std::unique_ptr<GDX12RootSignature> _packRoot, _restoreRoot;
    ComPtr<ID3D12PipelineState> _packPipeline, _restorePipeline;
};
