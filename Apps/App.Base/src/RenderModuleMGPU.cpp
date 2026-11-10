#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Rendering/RenderShadowSceneCapture.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace
{
using namespace CrossAdapterSceneReplica;

using RigidKey = std::tuple<UINT, UINT, std::uint64_t, UINT>;

UINT AppendRigidRange(CapturedShadowScene& result, std::map<RigidKey, UINT>& indices, MeshHandle handle,
    std::uint64_t revision, UINT submeshIndex, const SubMesh& source)
{
    const RigidKey key{ handle.GetValue(), handle.GetGeneration(), revision, submeshIndex };
    if (const auto old = indices.find(key); old != indices.end()) return old->second;
    if (result.Frame.Meshes.size() >= UINT_MAX || !handle.IsValid() || !revision || source.Vertices.empty() || source.Indices.empty() ||
        source.Vertices.size() > UINT_MAX || source.Indices.size() > UINT_MAX)
        return UINT_MAX;
    const auto index = static_cast<UINT>(result.Frame.Meshes.size());
    MeshRange range;
    range.Mesh = handle;
    range.ContentRevision = revision;
    range.Submesh = submeshIndex;
    range.VertexCount = static_cast<UINT>(source.Vertices.size());
    range.IndexCount = static_cast<UINT>(source.Indices.size());
    range.Addressing = IndexAddressing::RelativeToRange;
    result.Frame.Meshes.push_back(std::move(range));
    result.Sources.Meshes.push_back(&source);
    indices.emplace(key, index);
    return index;
}
}

CapturedShadowScene RenderModule::CaptureShadowScene(CrossAdapterSceneReplica::FrameStamp stamp,
    const std::vector<GDX12ShadowOutput>& groups,
    const std::vector<CrossAdapterSceneReplica::GrassBatch>& grass, const IRenderGeometrySource* grassSource,
    const std::vector<GDX12RenderPrimitiveHandle>& grassPrimitives)
{
    using namespace CrossAdapterSceneReplica;
    if (!stamp.IsValid() || stamp.Frame != GetFrameSubmissionId() || stamp.SceneRevision != _primaryResources.GeometryRevision)
        throw std::invalid_argument("Shadow replica capture needs the live prepared scene revision");

    const auto& alphaAssets = _gpu->ShadowAlpha;
    const auto& frame = *GetCurrentPrimaryFrameConstants();
    const auto* instances = frame.InstanceCache->GetCPUData();
    const auto* transforms = frame.TransformCache->GetCPUData();
    const auto* materials = frame.MaterialCache->GetCPUData();
    const UINT instanceCount = frame.InstanceCache->GetElementCount();
    const UINT transformCount = frame.TransformCache->GetElementCount();
    const UINT materialCount = frame.MaterialCache->GetElementCount();
    if (materialCount && !materials) throw std::logic_error("Shadow replica requires the primary material CPU mirror");
    CapturedShadowScene result;
    auto& snapshot = result.Frame;
    snapshot.Stamp = stamp;
    snapshot.Groups.resize(groups.size());

    std::unordered_map<UINT, const SkeletalPart*> skinByInstance;
    for (const auto& state : _skeletalStates)
        if (state.second.Seen)
            for (const auto& part : state.second.Parts)
                if (part.GPU && !part.Palette.empty()) skinByInstance.emplace(part.Instance, &part);
    std::map<RigidKey, UINT> rigidIndices;
    std::unordered_map<UINT, UINT> casterIndices, materialIndices;
    std::unordered_map<const GPUTexture*, UINT> alphaIndices;
    std::unordered_map<UINT, const GDX12Material*> applicationMaterials;
    if (grassSource)
        for (const auto& entry : _materials)
            applicationMaterials.emplace(entry.second->_CBufferIndex, entry.second.get());

    const auto captureMaterial = [&](UINT slot, const GDX12Material* material)
    {
        if (!material || slot >= materialCount || slot != material->_CBufferIndex) return UINT_MAX;
        if (const auto old = materialIndices.find(slot); old != materialIndices.end()) return old->second;
        Material replica;
        replica.Value = materials[slot];
        if (material->Type == MaterialType::Masked && material->Diffuse)
        {
            if (const auto retained = alphaAssets.find(material->Diffuse); retained != alphaAssets.end())
            {
                const auto& source = retained->second;
                const auto [entry, inserted] = alphaIndices.try_emplace(material->Diffuse,
                    static_cast<UINT>(snapshot.AlphaTextures.size()));
                if (inserted)
                {
                    AlphaTexture alpha;
                    alpha.Texture = source.Texture;
                    alpha.ContentRevision = source.ContentRevision;
                    alpha.Format = source.Format;
                    alpha.Width = source.Width;
                    alpha.Height = source.Height;
                    alpha.MipLevels = source.MipLevels;
                    snapshot.AlphaTextures.push_back(std::move(alpha));
                    result.Sources.Alpha.push_back(&source);
                }
                replica.AlphaTextureIndex = entry->second;
                replica.AlphaRevision = source.ContentRevision;
            }
        }
        const auto index = static_cast<UINT>(snapshot.Materials.size());
        snapshot.Materials.push_back(std::move(replica));
        result.PrimaryMaterialSlots.push_back(slot);
        materialIndices.emplace(slot, index);
        return index;
    };

    const auto captureCaster = [&](GDX12RenderPrimitiveHandle handle)
    {
        const auto* primitive = _primaryResources.Primitives.Get(handle);
        if (!primitive || !primitive->Visible || !primitive->CastsShadows || primitive->Source ||
            primitive->InstanceCount != 1 || primitive->FirstInstance >= instanceCount ||
            primitive->TransformIndex >= transformCount) return UINT_MAX;
        if (const auto old = casterIndices.find(handle.Index); old != casterIndices.end()) return old->second;

        const auto instance = primitive->FirstInstance;
        const UINT casterIndex = static_cast<UINT>(snapshot.Casters.size());
        Caster caster;
        CapturedShadowScene::PrimaryDraw draw;
        const auto captureDraw = [&](const GPUSubMesh& submesh, UINT vertexStart, UINT vertexCount, const GDX12Material* material)
        {
            if (!vertexCount || !submesh.IndexCount || vertexCount > UINT_MAX / sizeof(Vertex) ||
                submesh.IndexCount > UINT_MAX / sizeof(UINT)) return;
            draw.Instance = instance;
            draw.VertexStart = vertexStart;
            draw.VertexCount = vertexCount;
            draw.IndexStart = submesh.StartIndexLocation;
            draw.IndexCount = submesh.IndexCount;
            if (material && material->Type == MaterialType::Masked) draw.Mask = material->Diffuse;
        };
        caster.SourceId = handle.Index;
        caster.Transform = transforms[primitive->TransformIndex];
        caster.Bounds = primitive->Bounds;
        caster.Dynamic = primitive->Dynamic;

        if (instances[instance].SkinOutputVertex != UINT_MAX)
        {
            caster.Kind = GeometryKind::Skinned;
            const auto found = skinByInstance.find(instance);
            if (found != skinByInstance.end())
            {
                const auto& part = *found->second;
                const auto* mesh = GetPrimaryGPUMesh(part.Mesh);
                if (mesh && mesh->ContentRevision == part.MeshRevision && part.SubMesh < mesh->SubMeshes.size())
                {
                    const auto& submesh = mesh->CPUMesh->GetSubMesh(part.SubMesh);
                    caster.MaterialIndex = captureMaterial(primitive->MaterialIndex, part.ActiveMaterial);
                    caster.GeometryRevision = part.MeshRevision;
                    captureDraw(mesh->SubMeshes[part.SubMesh], part.GPU->OutputVertex,
                        part.GPU->VertexCount, part.ActiveMaterial);
                    result.Sources.Skin.push_back({ casterIndex, part.Mesh, part.MeshRevision, part.SubMesh,
                        &submesh, part.Palette });
                }
            }
        }
        else if (instance < _meshSnapshots.size())
        {
            const auto& cached = _meshSnapshots[instance];
            if (cached.Valid && cached.PrimitiveHandle == handle && cached.Mesh &&
                cached.Mesh->ContentRevision == cached.GeometryRevision && cached.SubMesh < cached.Mesh->SubMeshes.size())
            {
                const auto& submesh = cached.Mesh->CPUMesh->GetSubMesh(cached.SubMesh);
                caster.GeometryRevision = cached.GeometryRevision;
                caster.MaterialIndex = captureMaterial(primitive->MaterialIndex, cached.Material);
                const auto& native = cached.Mesh->SubMeshes[cached.SubMesh];
                captureDraw(native, static_cast<UINT>(native.StartVertexLocation),
                    static_cast<UINT>(submesh.GetVertexCount()), cached.Material);
                caster.Range = AppendRigidRange(result, rigidIndices, cached.Handle, cached.GeometryRevision,
                    cached.SubMesh, submesh);
            }
        }
        snapshot.Casters.push_back(std::move(caster));
        result.Primary.push_back(draw);
        casterIndices.emplace(handle.Index, casterIndex);
        return casterIndex;
    };

    for (size_t i = 0; i < groups.size(); ++i)
    {
        const auto& output = groups[i];
        std::vector<GDX12LocalShadowFrustum> views;
        views.reserve(output.ViewCount());
        for (UINT face = 0; face < output.ViewCount(); ++face)
            views.emplace_back(output.Views[face].ViewProj);

        _primaryResources.Primitives.ForEach([&](auto handle, const auto& primitive)
        {
            if (!primitive.Visible || !primitive.CastsShadows) return;
            if (std::none_of(views.begin(), views.end(), [&](const auto& view)
                { return view.Intersects(primitive.Bounds); })) return;
            if (grassSource && primitive.Source == grassSource &&
                std::find(grassPrimitives.begin(), grassPrimitives.end(), handle) != grassPrimitives.end())
            {
                const auto batch = std::find_if(grass.begin(), grass.end(), [&](const auto& entry)
                    { return entry.GroupIndex == i && entry.SourceId == handle.Index && entry.Stamp == stamp &&
                        (entry.Device == _secondaryDevice.get() || entry.Device == _primaryDevice.get()) && entry.Owner; });
                const auto material = applicationMaterials.find(primitive.MaterialIndex);
                const UINT materialIndex = batch != grass.end() && material != applicationMaterials.end()
                    ? captureMaterial(primitive.MaterialIndex, material->second) : UINT_MAX;
                if (materialIndex != UINT_MAX)
                {
                    Caster caster;
                    caster.Kind = GeometryKind::ExpandedGrass;
                    caster.SourceId = handle.Index;
                    caster.Range = static_cast<UINT>(snapshot.GrassBatches.size());
                    caster.GeometryRevision = primitive.GeometryRevision;
                    caster.MaterialIndex = materialIndex;
                    caster.Transform.emplace(); // Grass expansion emits world-space vertices.
                    caster.Bounds = primitive.Bounds;
                    caster.Dynamic = true;
                    snapshot.Groups[i].push_back(static_cast<UINT>(snapshot.Casters.size()));
                    snapshot.GrassBatches.push_back(*batch);
                    snapshot.Casters.push_back(std::move(caster));
                    CapturedShadowScene::PrimaryDraw draw;
                    if (material->second->Type == MaterialType::Masked) draw.Mask = material->second->Diffuse;
                    result.Primary.push_back(draw);
                    return;
                }
            }
            snapshot.Groups[i].push_back(captureCaster(handle));
        });
    }
    return result;
}
