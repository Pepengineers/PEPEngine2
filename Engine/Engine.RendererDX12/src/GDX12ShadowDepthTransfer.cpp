#include "Engine.RendererDX12/GDX12ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <bitset>
#include <map>
#include <stdexcept>
#include <utility>

namespace
{
    using Key = std::pair<UINT, uint64_t>;
    using GroupIndex = std::map<Key, const GDX12ShadowOutput*>;

    Key GroupKey(const GDX12ShadowOutput& group)
    {
        return { group.Light.EntityId, group.Light.Generation };
    }

    bool SameRegion(const GDX12ShadowRegion& a, const GDX12ShadowRegion& b)
    {
        return a.Slice == b.Slice && a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
    }

    std::optional<GroupIndex> IndexGroups(GDX12ShadowAtlas atlas, UINT extent, const std::vector<GDX12ShadowOutput>& groups)
    {
        using Kind = GDX12ShadowOutput::Kind;
        GroupIndex index;
        std::bitset<D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION> usedSlices;
        for (const auto& group : groups)
        {
            if (group.StaticOnly || !index.emplace(GroupKey(group), &group).second) return std::nullopt;
            if (atlas == GDX12ShadowAtlas::Sun)
            {
                if (group.Type != Kind::Sun || !(group.Light == GDX12LocalShadowKey{})) return std::nullopt;
                for (UINT view = 0; view < group.ViewCount(); ++view)
                    if (!SameRegion(group.Views[view].Region, SunShadowRegion(view, extent))) return std::nullopt;
                continue;
            }
            if (atlas != GDX12ShadowAtlas::Local || (group.Type != Kind::Spot && group.Type != Kind::Point) ||
                group.Light.EntityId == 0 || group.Light.Generation == 0)
                return std::nullopt;
            const UINT firstSlice = group.Views[0].Region.Slice;
            if (firstSlice > usedSlices.size() - group.ViewCount()) return std::nullopt;
            for (UINT face = 0; face < group.ViewCount(); ++face)
            {
                const UINT slice = firstSlice + face;
                const GDX12ShadowRegion expected{ slice, 0, 0, extent, extent };
                if (usedSlices[slice] || !SameRegion(group.Views[face].Region, expected)) return std::nullopt;
                usedSlices.set(slice);
            }
        }
        return index;
    }

    struct RegionConstants
    {
        UINT X, Y, Width, Height, AtlasSlice, PayloadLayer;
    };
    constexpr UINT RegionConstantCount = 6;
    static_assert(sizeof(RegionConstants) == RegionConstantCount * sizeof(UINT));

    RegionConstants ConstantsFor(const GDX12ShadowTransferView& view)
    {
        const auto& region = view.Region;
        return { region.X, region.Y, region.Width, region.Height, region.Slice, view.PayloadLayer };
    }

    D3D12_RESOURCE_DESC TextureDesc(GDX12Device& device, const std::shared_ptr<GDX12Resource>& resource,
        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
    {
        if (!resource || !resource->D3DResource) throw std::invalid_argument("Missing shadow transfer resource");
        const auto desc = resource->D3DResource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            desc.Format != format || (desc.Flags & flags) != flags ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("Incompatible shadow transfer texture");
        ComPtr<ID3D12Device14> owner;
        ThrowIfFailed(resource->D3DResource->GetDevice(IID_PPV_ARGS(&owner)));
        if (owner.Get() != device.GetDevice().Get()) throw std::invalid_argument("Shadow transfer texture is on another device");
        return desc;
    }

    void CheckTextureShapes(GDX12Device& device, const GDX12ShadowTransferLayout& layout,
        const std::vector<GDX12ShadowTransferView>& views, const std::shared_ptr<GDX12Resource>& depth,
        const std::shared_ptr<GDX12Resource>& payload, bool pack)
    {
        const auto depthDesc = TextureDesc(device, depth, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        const auto payloadDesc = TextureDesc(device, payload, DXGI_FORMAT_R32_FLOAT,
            pack ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        const UINT extent = layout.Extent();
        const bool sun = layout.Atlas() == GDX12ShadowAtlas::Sun;
        const UINT atlasExtent = sun ? 2 * extent : extent;
        if (views.empty() || views.size() != layout.Layers() ||
            depthDesc.Width != atlasExtent || depthDesc.Height != atlasExtent || (sun && depthDesc.DepthOrArraySize != 1) ||
            payloadDesc.Width != extent || payloadDesc.Height != extent || payloadDesc.DepthOrArraySize != layout.Layers())
            throw std::invalid_argument("Shadow transfer textures do not fit the prepared layout");
        for (const auto& view : views)
            if (view.Region.Slice >= depthDesc.DepthOrArraySize)
                throw std::invalid_argument("Shadow transfer region references a missing atlas slice");
    }

    void CreateArraySRV(GDX12Device& device, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = resource->GetDesc().DepthOrArraySize;
        device.GetDevice()->CreateShaderResourceView(resource, &srv, descriptor);
    }

    struct RetainedViews
    {
        RetainedViews(std::shared_ptr<GDX12Resource> depth, std::shared_ptr<GDX12Resource> payload)
            : Depth(std::move(depth)), Payload(std::move(payload)), DepthHandle(Depth->D3DResource),
              PayloadHandle(Payload->D3DResource) {}

        void Check(const ID3D12Resource* depth, const ID3D12Resource* payload) const
        {
            if (depth != DepthHandle.Get() || payload != PayloadHandle.Get() ||
                Depth->D3DResource.Get() != depth || Payload->D3DResource.Get() != payload)
                throw std::logic_error("Bound shadow views changed after descriptor preparation");
        }

        std::shared_ptr<GDX12Resource> Depth, Payload;
        GDX12ResourceHandle DepthHandle, PayloadHandle;
    };
}

std::optional<GDX12ShadowTransferLayout> GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas atlas,
    std::vector<GDX12ShadowOutput> selected)
{
    if (selected.empty()) return std::nullopt;
    const UINT extent = selected.front().Views[0].Region.Width;
    if (!(atlas == GDX12ShadowAtlas::Sun ? IsSunShadowResolution(extent) : IsLocalShadowResolution(extent)) ||
        !IndexGroups(atlas, extent, selected)) return std::nullopt;
    GDX12ShadowTransferLayout layout(atlas, extent);
    for (const auto& group : selected)
        for (UINT face = 0; face < group.ViewCount(); ++face)
            layout._sourceViews.push_back({ group.Views[face].Region, layout.Layers() });
    layout._groups = std::move(selected);
    return layout;
}

std::optional<std::vector<GDX12ShadowTransferView>> GDX12ShadowTransferLayout::ResolveDestination(
    const std::vector<GDX12ShadowOutput>& prepared) const
{
    const auto index = IndexGroups(_atlas, _extent, prepared);
    if (!index) return std::nullopt;
    std::vector<GDX12ShadowTransferView> views;
    views.reserve(Layers());
    for (const auto& group : _groups)
    {
        const auto match = index->find(GroupKey(group));
        if (match == index->end() || !group.MatchesDepthInputs(*match->second)) return std::nullopt;
        for (UINT face = 0; face < group.ViewCount(); ++face)
            views.push_back({ match->second->Views[face].Region, static_cast<UINT>(views.size()) });
    }
    return views;
}

struct GDX12ShadowDepthTransfer::Pack
{
    Pack(GDX12Device& device, std::vector<GDX12ShadowTransferView> regions,
        std::shared_ptr<GDX12Resource> depth, std::shared_ptr<GDX12Resource> payload)
        : Bindings(std::move(depth), std::move(payload)), Regions(std::move(regions)),
          Heap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
    {
        CreateArraySRV(device, Bindings.DepthHandle.Get(), Heap.GetCPUHandle(0));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.ArraySize = static_cast<UINT>(Regions.size());
        device.GetDevice()->CreateUnorderedAccessView(Bindings.PayloadHandle.Get(), nullptr, &uav, Heap.GetCPUHandle(1));
    }

    RetainedViews Bindings;
    std::vector<GDX12ShadowTransferView> Regions;
    GDX12DescriptorHeap Heap;
};

struct GDX12ShadowDepthTransfer::Restore
{
    Restore(GDX12Device& device, GDX12ShadowAtlas atlas, std::vector<GDX12ShadowTransferView> regions,
        std::shared_ptr<GDX12Resource> depth, std::shared_ptr<GDX12Resource> payload, const InitializedSlices& initialized)
        : Bindings(std::move(depth), std::move(payload)), Regions(std::move(regions)),
          SRVs(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
          DSVs(&device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, atlas == GDX12ShadowAtlas::Sun ? 1 : static_cast<UINT>(Regions.size()),
              D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
    {
        CreateArraySRV(device, Bindings.PayloadHandle.Get(), SRVs.GetCPUHandle(0));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        if (atlas == GDX12ShadowAtlas::Sun)
        {
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device.GetDevice()->CreateDepthStencilView(Bindings.DepthHandle.Get(), &dsv, DSVs.GetCPUHandle(0));
            if (!initialized[0]) FirstUseClears.push_back(DSVs.GetCPUHandle(0));
        }
        else
        {
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv.Texture2DArray.ArraySize = 1;
            for (const auto& view : Regions)
            {
                dsv.Texture2DArray.FirstArraySlice = view.Region.Slice;
                const auto handle = DSVs.GetCPUHandle(view.PayloadLayer);
                device.GetDevice()->CreateDepthStencilView(Bindings.DepthHandle.Get(), &dsv, handle);
                if (!initialized[view.Region.Slice]) FirstUseClears.push_back(handle);
            }
        }
    }

    RetainedViews Bindings;
    std::vector<GDX12ShadowTransferView> Regions;
    GDX12DescriptorHeap SRVs, DSVs;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> FirstUseClears;
};

GDX12ShadowDepthTransfer::GDX12ShadowDepthTransfer(GDX12Device& device) : _device(device)
{
    GDX12RootSignatureDesc root;
    root.Constants.push_back(RegionConstantCount);
    root.NumSingleSRVSlots = 1;
    root.NumSingleUAVSlots = 1;
    _packRoot = std::make_unique<GDX12RootSignature>(&device, root);
    root.NumSingleUAVSlots = 0;
    _restoreRoot = std::make_unique<GDX12RootSignature>(&device, root);

    constexpr auto shader = SHADERS_FOLDER "ShadowDepthTransfer.hlsl";
    _packPipeline = GDX12RenderHelpers::CreateComputePipeline(&device, _packRoot.get(), shader, "PackCS");
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto vs = compiler.CompileShader(&device, shader, nullptr, "RestoreVS", "vs");
    const auto ps = compiler.CompileShader(&device, shader, nullptr, "RestorePS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _restoreRoot->GetRootSignature().Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_restorePipeline)));
}

GDX12ShadowDepthTransfer::~GDX12ShadowDepthTransfer() = default;

std::shared_ptr<GDX12ShadowDepthTransfer::Pack> GDX12ShadowDepthTransfer::PreparePack(
    const GDX12ShadowTransferLayout& layout, std::shared_ptr<GDX12Resource> depth,
    std::shared_ptr<GDX12Resource> payload) const
{
    const auto& views = layout.SourceViews();
    CheckTextureShapes(_device, layout, views, depth, payload, true);
    return std::make_shared<Pack>(_device, views, std::move(depth), std::move(payload));
}

std::shared_ptr<GDX12ShadowDepthTransfer::Restore> GDX12ShadowDepthTransfer::PrepareRestore(
    const GDX12ShadowTransferLayout& layout, const std::vector<GDX12ShadowOutput>& prepared,
    std::shared_ptr<GDX12Resource> depth, std::shared_ptr<GDX12Resource> payload, const InitializedSlices& initialized) const
{
    auto views = layout.ResolveDestination(prepared);
    if (!views) return {};
    CheckTextureShapes(_device, layout, *views, depth, payload, false);
    return std::make_shared<Restore>(_device, layout.Atlas(), std::move(*views), std::move(depth), std::move(payload), initialized);
}

void GDX12ShadowDepthTransfer::RecordPack(GDX12CommandList& commands, Pack& pack,
    const ID3D12Resource* depth, const ID3D12Resource* payload) const
{
    pack.Bindings.Check(depth, payload);
    commands.SetDescriptorHeaps({ &pack.Heap });
    commands.SetComputeRootSignature(_packRoot.get());
    commands.SetPipelineState(_packPipeline);
    commands.SetComputeSRV(0, pack.Heap.GetGPUHandle(0));
    commands.SetComputeUAV(0, pack.Heap.GetGPUHandle(1));
    for (const auto& view : pack.Regions)
    {
        const auto constants = ConstantsFor(view);
        commands.GetCommandList()->SetComputeRoot32BitConstants(0, RegionConstantCount, &constants, 0);
        commands.Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);
    }
}

void GDX12ShadowDepthTransfer::RecordRestore(GDX12CommandList& commands, Restore& restore,
    const ID3D12Resource* depth, const ID3D12Resource* payload) const
{
    restore.Bindings.Check(depth, payload);
    commands.SetDescriptorHeaps({ &restore.SRVs });
    commands.SetGraphicsRootSignature(_restoreRoot.get());
    commands.SetPipelineState(_restorePipeline);
    commands.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.SetGraphicsSRV(0, restore.SRVs.GetGPUHandle(0));
    auto* list = commands.GetCommandList().Get();
    for (const auto dsv : restore.FirstUseClears)
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    for (const auto& view : restore.Regions)
    {
        const auto constants = ConstantsFor(view);
        list->SetGraphicsRoot32BitConstants(0, RegionConstantCount, &constants, 0);
        const auto& r = view.Region;
        const auto dsv = restore.DSVs.GetCPUHandle(restore.DSVs.GetNumDescriptors() == 1 ? 0 : view.PayloadLayer);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        commands.SetViewport({ static_cast<float>(r.X), static_cast<float>(r.Y),
            static_cast<float>(r.Width), static_cast<float>(r.Height), 0.0f, 1.0f });
        commands.SetScissorRect({ static_cast<LONG>(r.X), static_cast<LONG>(r.Y),
            static_cast<LONG>(r.X + r.Width), static_cast<LONG>(r.Y + r.Height) });
        commands.DrawInstanced(3, 1, 0, 0);
    }
}
