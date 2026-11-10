#include "App.Base/Rendering/CrossAdapterShadowScene.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace
{
    namespace S2R = CrossAdapterSceneReplica;
    using namespace GDX12Submission;
    using Engine::Core::SubMesh;
    using Engine::Core::Vertex;
    using Mode = GDX12RenderGraph::Mode;
    constexpr auto ShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    enum class AssetKind { Rigid, Skin, Alpha };
    using AssetKey = std::tuple<AssetKind, UINT, UINT, std::uint64_t, UINT>;

    struct Asset
    {
        S2R::ResidentGeometry Geometry;
        S2R::ResourceView Ranges, Influences;
        UINT BoneCount = 0;
        S2R::AlphaTexture Alpha;
        std::shared_ptr<GDX12DescriptorHeap> Heap;
        std::shared_ptr<GDX12Descriptor> AlphaDescriptor;
    };

    struct BufferUpload
    {
        S2R::ResourceView Target;
        GDX12ResourceHandle Staging;
        UINT64 Bytes;
    };

    struct PendingAsset
    {
        std::shared_ptr<Asset> Value;
        std::vector<BufferUpload> Buffers;
        std::optional<S2R::AlphaUpload> Alpha;
    };

    struct SkinDispatch
    {
        std::shared_ptr<const Asset> Source;
        S2R::ResourceView Output;
        BufferUpload Palette;
    };

    S2R::ResourceView Own(S2R::ResourceView view, const std::shared_ptr<const Asset>& asset)
    {
        view.Owner = asset;
        return view;
    }

    void Access(std::vector<S2R::Access>& accesses, const S2R::ResourceView& view, Mode mode, D3D12_RESOURCE_STATES state)
    {
        if (std::none_of(accesses.begin(), accesses.end(), [&](const auto& access)
            { return access.View.Identity.Id == view.Identity.Id; }))
            accesses.push_back({ view, view.Native->D3DResource, mode, state });
    }

    S2R::ResourceView Allocate(GDX12Device& device, const S2R::NewIdentity& identity, UINT64 bytes,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        S2R::ResourceView view{ std::make_shared<GDX12Resource>(), identity() };
        const auto& id = view.Identity;
        if (id.Id == ResourceId::Invalid || id.Adapter != device.GetSubmissionAdapterId() ||
            id.ResourceEpoch == Epoch::Invalid || id.Transfer != TransferId::Invalid || id.Allocation == AllocationId::Invalid)
            throw std::invalid_argument("Invalid secondary shadow buffer identity");
        view.Native->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Buffer(bytes, flags), D3D12_RESOURCE_STATE_COMMON);
        return view;
    }

    BufferUpload StageBuffer(GDX12Device& device, const S2R::NewIdentity& identity, const void* data, UINT64 bytes)
    {
        auto view = Allocate(device, identity, bytes);
        auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
            D3D12_RESOURCE_STATE_GENERIC_READ);
        const D3D12_RANGE noRead{ 0, 0 };
        void* mapped = nullptr;
        ThrowIfFailed(upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        upload->Unmap(0, nullptr);
        return { std::move(view), std::move(upload), bytes };
    }

    template<class T>
    bool Fits(const std::vector<T>& data, UINT first, UINT count)
    { return first <= data.size() && count <= data.size() - first; }

    std::optional<PendingAsset> StageGeometry(GDX12Device& device, const S2R::NewIdentity& identity,
        const Vertex* vertices, size_t vertexCount, const UINT* indices, size_t indexCount, UINT base)
    {
        if (!vertexCount || vertexCount > UINT_MAX / sizeof(Vertex) ||
            !indexCount || indexCount > UINT_MAX / sizeof(UINT) || indexCount % 3)
            return {};
        UINT maxIndex = 0;
        for (size_t i = 0; i < indexCount; ++i)
        {
            const UINT index = indices[i];
            if (index < base || index - base >= vertexCount) return {};
            maxIndex = (std::max)(maxIndex, index - base);
        }
        for (size_t i = 0; i < vertexCount; ++i)
        {
            const auto& v = vertices[i];
            if (!std::isfinite(v.Position.x) || !std::isfinite(v.Position.y) || !std::isfinite(v.Position.z) ||
                !std::isfinite(v.TexCoord.x) || !std::isfinite(v.TexCoord.y))
                return {};
        }

        std::vector<UINT> relative;
        if (base)
        {
            relative.reserve(indexCount);
            for (size_t i = 0; i < indexCount; ++i) relative.push_back(indices[i] - base);
            indices = relative.data();
        }
        PendingAsset staged;
        staged.Value = std::make_shared<Asset>();
        auto& geometry = staged.Value->Geometry;
        geometry.VertexCount = static_cast<UINT>(vertexCount);
        geometry.IndexCount = static_cast<UINT>(indexCount);
        geometry.MaxRelativeIndex = maxIndex;
        staged.Buffers.push_back(StageBuffer(device, identity, vertices, vertexCount * sizeof(Vertex)));
        geometry.Vertices = staged.Buffers.back().Target;
        staged.Buffers.push_back(StageBuffer(device, identity, indices, indexCount * sizeof(UINT)));
        geometry.Indices = staged.Buffers.back().Target;
        return staged;
    }

    bool ValidMatrix(const DirectX::SimpleMath::Matrix& matrix)
    {
        if (matrix._14 != 0 || matrix._24 != 0 || matrix._34 != 0 || matrix._44 != 1) return false;
        for (const auto& row : matrix.m)
            for (const float value : row)
                if (!std::isfinite(value)) return false;
        return true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS Address(const S2R::ResourceView& view)
    { return view.Native->D3DResource->GetGPUVirtualAddress(); }

    template<class T>
    auto Borrow(const std::vector<const T*>& source, UINT index)
    { return index < source.size() ? source[index] : nullptr; }

    S2R::AlphaTexture AlphaMetadata(const S2R::AlphaTexture& alpha)
    {
        S2R::AlphaTexture result;
        result.Texture = alpha.Texture;
        result.ContentRevision = alpha.ContentRevision;
        result.Format = alpha.Format;
        result.Width = alpha.Width;
        result.Height = alpha.Height;
        result.MipLevels = alpha.MipLevels;
        result.SecondaryView = alpha.SecondaryView;
        result.SecondarySRVIndex = alpha.SecondarySRVIndex;
        result.SecondarySRVHeap = alpha.SecondarySRVHeap;
        return result;
    }

    bool Matches(const S2R::AlphaTexture& a, const S2R::AlphaTexture& b)
    {
        return a.Texture == b.Texture && a.ContentRevision == b.ContentRevision && a.Format == b.Format &&
            a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels;
    }

    struct FrameWork
    {
        std::shared_ptr<S2R::Slot> Slot;
        std::vector<BufferUpload> Buffers;
        std::vector<S2R::AlphaUpload> Alpha;
        std::vector<SkinDispatch> Skin;
        std::shared_ptr<GDX12Skinning> Pipeline;
        bool RecordedUploads = false;
    };
}

struct CrossAdapterShadowScene::State
{
    State(GDX12Device& device, S2R::NewIdentity identity) : Device(device), Identity(std::move(identity)),
        Heap(std::make_shared<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            Texture2D_StartIndex + Texture2D_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
    {
        if (!Identity) throw std::invalid_argument("Shadow scene requires a resource identity allocator");
        Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        for (auto& slot : Slots) slot = std::make_shared<S2R::Slot>();
    }

    GDX12Device& Device;
    S2R::NewIdentity Identity;
    std::shared_ptr<GDX12DescriptorHeap> Heap;
    std::array<std::shared_ptr<S2R::Slot>, 3> Slots;
    std::map<AssetKey, std::shared_ptr<const Asset>> Cache;
    std::shared_ptr<GDX12Skinning> Skinning;
    std::uint64_t CacheRevision = 0;
};

CrossAdapterShadowScene::CrossAdapterShadowScene(RenderMGPURuntime& runtime, GDX12Device& secondary)
    : CrossAdapterShadowScene(secondary, [&runtime, &secondary]
        { return ResourceIdentity{ runtime.NewResourceId(), secondary.GetSubmissionAdapterId(),
            runtime.NewEpoch(), TransferId::Invalid, runtime.NewAllocationId() }; })
{}

CrossAdapterShadowScene::CrossAdapterShadowScene(GDX12Device& secondary, S2R::NewIdentity newIdentity)
    : _state(std::make_shared<State>(secondary, std::move(newIdentity)))
{}

PreparedShadowReplica CrossAdapterShadowScene::Prepare(const S2R::Snapshot& source, const ShadowSceneSources& sources)
{
    if (!source.Stamp.IsValid()) throw std::invalid_argument("Invalid shadow scene frame stamp");
    auto& state = *_state;
    const auto slot = std::find_if(state.Slots.begin(), state.Slots.end(), [](const auto& entry)
        { return entry.use_count() == 1; });
    if (slot == state.Slots.end()) return {};
    auto* queue = state.Device.GetCommandQueue();
    const auto timeline = queue->GetTimelineIdentity();
    const auto baseline = queue->GetLastCompletionPoint().Value();
    const auto revision = state.CacheRevision;

    for (const auto count : { source.Casters.size(), source.Meshes.size(), source.AlphaTextures.size(),
        source.DynamicGeometry.size(), sources.Skin.size() })
        if (count >= UINT_MAX) throw std::length_error("Shadow scene exceeds the shader index range");

    S2R::Snapshot frame;
    frame.Stamp = source.Stamp;
    frame.Casters = source.Casters;
    frame.Materials = source.Materials;
    frame.Groups = source.Groups;
    frame.GrassBatches = source.GrassBatches;
    frame.DynamicGeometry = source.DynamicGeometry;
    frame.Meshes.resize(source.Meshes.size());
    frame.AlphaTextures.resize(source.AlphaTextures.size());

    std::map<AssetKey, PendingAsset> pending;
    const auto asset = [&](const AssetKey& key, const auto& stage) -> std::shared_ptr<const Asset>
    {
        if (const auto it = state.Cache.find(key); it != state.Cache.end()) return it->second;
        if (const auto it = pending.find(key); it != pending.end()) return it->second.Value;
        auto value = stage();
        if (!value) return {};
        return pending.emplace(key, std::move(*value)).first->second.Value;
    };

    std::vector<bool> selected(frame.Casters.size());
    for (const auto& group : frame.Groups)
        for (const UINT index : group)
            if (index < selected.size()) selected[index] = true;

    std::vector<bool> meshSeen(frame.Meshes.size()), alphaSeen(frame.AlphaTextures.size());
    std::vector<const FrozenShadowSkin*> skins(frame.Casters.size());
    std::vector<bool> duplicateSkin(frame.Casters.size());
    for (const auto& skin : sources.Skin)
        if (skin.CasterIndex < skins.size())
        {
            duplicateSkin[skin.CasterIndex] = skins[skin.CasterIndex] != nullptr;
            skins[skin.CasterIndex] = &skin;
        }
    std::vector<SkinDispatch> skinDispatches;
    for (UINT i = 0; i < frame.Casters.size(); ++i)
    {
        if (!selected[i]) continue;
        auto& caster = frame.Casters[i];
        if (caster.Kind == S2R::GeometryKind::Static && caster.Range < frame.Meshes.size() && !meshSeen[caster.Range])
        {
            meshSeen[caster.Range] = true;
            const auto& input = source.Meshes[caster.Range];
            auto& mesh = frame.Meshes[caster.Range];
            mesh.Mesh = input.Mesh;
            mesh.ContentRevision = input.ContentRevision;
            mesh.Submesh = input.Submesh;
            if (input.Resident)
            {
                mesh.VertexCount = input.VertexCount;
                mesh.IndexCount = input.IndexCount;
                mesh.Resident = input.Resident;
            }
            else if (input.Mesh.IsValid() && input.ContentRevision)
            {
                const AssetKey key{ AssetKind::Rigid, input.Mesh.GetValue(), input.Mesh.GetGeneration(),
                    input.ContentRevision, input.Submesh };
                const auto value = asset(key, [&]() -> std::optional<PendingAsset>
                {
                    if (const auto* borrowed = Borrow(sources.Meshes, caster.Range))
                        return StageGeometry(state.Device, state.Identity, borrowed->Vertices.data(), borrowed->Vertices.size(),
                            borrowed->Indices.data(), borrowed->Indices.size(), 0);
                    if (!input.VertexCount || !input.IndexCount || !Fits(input.Vertices, input.FirstVertex, input.VertexCount) ||
                        !Fits(input.Indices, input.FirstIndex, input.IndexCount) ||
                        (input.Addressing != S2R::IndexAddressing::RelativeToRange &&
                            input.Addressing != S2R::IndexAddressing::AbsoluteInPayload))
                        return {};
                    return StageGeometry(state.Device, state.Identity,
                        input.Vertices.data() + input.FirstVertex, input.VertexCount,
                        input.Indices.data() + input.FirstIndex, input.IndexCount,
                        input.Addressing == S2R::IndexAddressing::AbsoluteInPayload ? input.FirstVertex : 0);
                });
                if (value)
                {
                    mesh.Resident = value->Geometry;
                    mesh.VertexCount = mesh.Resident->VertexCount;
                    mesh.IndexCount = mesh.Resident->IndexCount;
                    mesh.Resident->Vertices = Own(mesh.Resident->Vertices, value);
                    mesh.Resident->Indices = Own(mesh.Resident->Indices, value);
                }
            }
        }
        if (caster.Kind == S2R::GeometryKind::Skinned && skins[i])
        {
            const auto& skin = *skins[i];
            caster.Range = UINT_MAX;
            if (!duplicateSkin[i] && skin.Mesh.IsValid() && skin.ContentRevision &&
                caster.GeometryRevision == skin.ContentRevision && !skin.Palette.empty() &&
                std::all_of(skin.Palette.begin(), skin.Palette.end(), [](const auto& bone)
                    { return ValidMatrix(bone.Current) && ValidMatrix(bone.Previous); }))
            {
                const AssetKey key{ AssetKind::Skin, skin.Mesh.GetValue(), skin.Mesh.GetGeneration(),
                    skin.ContentRevision, skin.Submesh };
                const auto value = asset(key, [&]() -> std::optional<PendingAsset>
                {
                    if (!skin.Source || skin.Palette.size() != skin.Source->Bones.size() + 1) return {};
                    std::vector<Engine::Core::SkinVertexRange> ranges;
                    try { ranges = GDX12Skinning::ValidateSource(*skin.Source); }
                    catch (const std::invalid_argument&) { return {}; }
                    auto result = StageGeometry(state.Device, state.Identity, skin.Source->Vertices.data(), skin.Source->Vertices.size(),
                        skin.Source->Indices.data(), skin.Source->Indices.size(), 0);
                    if (!result) return {};
                    result->Value->BoneCount = static_cast<UINT>(skin.Palette.size());
                    result->Buffers.push_back(StageBuffer(state.Device, state.Identity, ranges.data(),
                        ranges.size() * sizeof(ranges.front())));
                    result->Value->Ranges = result->Buffers.back().Target;
                    const Engine::Core::SkinInfluence dummy{};
                    const auto& influences = skin.Source->Influences;
                    result->Buffers.push_back(StageBuffer(state.Device, state.Identity,
                        influences.empty() ? &dummy : influences.data(), (std::max)(size_t{1}, influences.size()) * sizeof(dummy)));
                    result->Value->Influences = result->Buffers.back().Target;
                    return result;
                });
                if (value && value->BoneCount == skin.Palette.size())
                {
                    const auto& geo = value->Geometry;
                    auto output = Allocate(state.Device, state.Identity, UINT64{geo.VertexCount} * sizeof(Vertex),
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    output.Owner = output.Native;
                    caster.Range = static_cast<UINT>(frame.DynamicGeometry.size());
                    frame.DynamicGeometry.push_back({ frame.Stamp, output, Own(geo.Indices, value),
                        0, 0, geo.VertexCount, geo.IndexCount, geo.MaxRelativeIndex });
                    skinDispatches.push_back({ value, std::move(output),
                        StageBuffer(state.Device, state.Identity, skin.Palette.data(), skin.Palette.size() * sizeof(GDX12SkinPalette)) });
                }
            }
        }
        if (caster.MaterialIndex >= frame.Materials.size()) continue;
        const auto& material = frame.Materials[caster.MaterialIndex];
        const auto index = material.AlphaTextureIndex;
        if (material.Value.RenderLayer != 1 || index >= frame.AlphaTextures.size() || alphaSeen[index]) continue;
        alphaSeen[index] = true;
        const auto& input = source.AlphaTextures[index];
        auto& alpha = frame.AlphaTextures[index];
        alpha = AlphaMetadata(input);
        if (!input.Texture.IsValid() || !input.ContentRevision) continue;
        if (alpha.SecondaryView.Native && (!input.Mips.empty() || alpha.SecondarySRVIndex != UINT_MAX ||
            alpha.SecondarySRVHeap || alpha.SecondaryView.Identity.Transfer != TransferId::Invalid))
        {
            if (!input.Mips.empty()) alpha.SecondaryView = {};
            continue;
        }
        const AssetKey key{ AssetKind::Alpha, input.Texture.GetValue(), input.Texture.GetGeneration(),
            input.ContentRevision, 0 };
        const auto value = asset(key, [&]() -> std::optional<PendingAsset>
        {
            const auto* payload = input.SecondaryView.Native ? &input : Borrow(sources.Alpha, index);
            if (!payload) payload = &input;
            if (!Matches(input, *payload) || payload->SecondaryView.Identity.Transfer != TransferId::Invalid)
                return {};
            auto staged = S2R::StageAlpha(state.Device, *state.Heap, *payload, state.Identity);
            if (!staged) return {};
            PendingAsset result;
            result.Value = std::make_shared<Asset>();
            result.Value->Alpha = AlphaMetadata(staged->Texture);
            result.Value->AlphaDescriptor = std::move(staged->Descriptor);
            result.Value->Heap = state.Heap;
            result.Alpha = std::move(staged->Upload);
            return result;
        });
        if (value && Matches(input, value->Alpha) && (!input.SecondaryView.Native ||
            value->Alpha.SecondaryView.Native == input.SecondaryView.Native))
        {
            alpha = AlphaMetadata(value->Alpha);
            alpha.SecondaryView = Own(alpha.SecondaryView, value);
        }
        else if (value) alpha.SecondaryView = {};
    }

    S2R::Prepare(*slot, state.Device, state.Heap, frame.Stamp, frame, state.Identity);
    auto work = std::make_shared<FrameWork>();
    work->Slot = *slot;
    const auto& replica = (*slot)->Get();
    const auto used = [&](const S2R::ResourceView& view)
    {
        return view.Native && std::any_of(replica.RasterAccesses.begin(), replica.RasterAccesses.end(),
            [&](const auto& access) { return access.View.Identity.Id == view.Identity.Id; });
    };

    std::vector<S2R::Access> uploads, skinAccesses;
    for (auto& skin : skinDispatches)
        if (used(skin.Output))
        {
            const auto& sourceAsset = *skin.Source;
            for (const auto* view : { &sourceAsset.Geometry.Vertices, &sourceAsset.Ranges, &sourceAsset.Influences })
                Access(skinAccesses, Own(*view, skin.Source), Mode::Read, ShaderRead);
            Access(skinAccesses, skin.Palette.Target, Mode::Read, ShaderRead);
            Access(skinAccesses, skin.Output, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Access(uploads, skin.Palette.Target, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST);
            work->Buffers.push_back({ skin.Palette.Target, std::move(skin.Palette.Staging), skin.Palette.Bytes });
            work->Skin.push_back(std::move(skin));
        }

    std::map<AssetKey, std::shared_ptr<const Asset>> publish;
    for (auto& [key, staged] : pending)
    {
        const auto& value = staged.Value;
        // A skin's index resource is read only by draws using that immutable source.
        if (!used(value->Geometry.Indices) && !used(value->Alpha.SecondaryView)) continue;
        for (auto& buffer : staged.Buffers)
        {
            Access(uploads, Own(buffer.Target, value), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST);
            work->Buffers.push_back(std::move(buffer));
        }
        if (staged.Alpha)
        {
            Access(uploads, Own(value->Alpha.SecondaryView, value), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST);
            work->Alpha.push_back(std::move(*staged.Alpha));
        }
        publish.emplace(key, value);
    }

    PreparedShadowReplica result;
    result.Replica = { work, &replica };
    result.Upload = { work, slot->get() };
    result.MaterialHeap = state.Heap;
    const bool needsUpload = !uploads.empty();
    if (needsUpload)
        result.Preparation.push_back({ "ShadowSceneAssets", std::move(uploads), [work](GDX12CommandList& commands)
        {
            auto* list = commands.GetCommandList().Get();
            for (const auto& buffer : work->Buffers)
                list->CopyBufferRegion(buffer.Target.Native->D3DResource.Get(), 0, buffer.Staging.Get(), 0, buffer.Bytes);
            for (const auto& alpha : work->Alpha) S2R::RecordAlphaUpload(commands, alpha);
            work->RecordedUploads = true;
        } });
    if (!work->Skin.empty())
    {
        if (!state.Skinning) state.Skinning = std::make_shared<GDX12Skinning>(&state.Device);
        work->Pipeline = state.Skinning;
        result.Preparation.push_back({ "ShadowSceneSkin", std::move(skinAccesses),
            [work](GDX12CommandList& commands)
        {
            for (const auto& skin : work->Skin)
            {
                const auto& inputs = *skin.Source;
                work->Pipeline->RecordDispatch(commands, inputs.Geometry.VertexCount, 0, inputs.BoneCount,
                    Address(inputs.Geometry.Vertices), Address(inputs.Ranges), Address(inputs.Influences),
                    Address(skin.Palette.Target), Address(skin.Output));
            }
        } });
    }
    if (!publish.empty())
    {
        result.Commit = [weak = std::weak_ptr<State>(_state), work, queue, timeline, baseline, revision, needsUpload,
            publish = std::move(publish)]() mutable
        {
            const auto state = weak.lock();
            if (!state || state->CacheRevision != revision) return;
            const auto point = queue->GetLastCompletionPoint();
            if (!point.IsValid() || !(point.Timeline() == timeline) || point.Value() <= baseline)
            {
                if (needsUpload) throw std::logic_error("Shadow scene publication requires its secondary DIRECT submission");
                return;
            }
            if (needsUpload && !work->RecordedUploads)
                throw std::logic_error("Shadow scene publication requires its asset recorder");
            for (const auto& [key, value] : publish)
            {
                const auto [kind, handle, generation, content, submesh] = key;
                for (auto entry = state->Cache.begin(); entry != state->Cache.end();)
                {
                    const auto& [oldKind, oldHandle, oldGeneration, oldContent, oldSubmesh] = entry->first;
                    if (kind == oldKind && handle == oldHandle && submesh == oldSubmesh)
                        entry = state->Cache.erase(entry);
                    else ++entry;
                }
            }
            state->Cache.merge(publish);
            ++state->CacheRevision;
        };
    }
    return result;
}
