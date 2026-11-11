#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/ECS/Components/StaticMeshRenderComponent.h"
#include "App.Base/ECS/Components/TransformComponent.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include <algorithm>
#include <cstring>

void RenderModule::CollectReflectionScene()
{
    GDX12_SCOPED_CPU_EVENT("Scene snapshot");
    uint64_t staticHash = 14695981039346656037ull;
    uint64_t dynamicHash = staticHash;
    const auto hash = [](uint64_t& result, const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    };
    auto& resources = _primaryResources;
    const auto& frame = *resources.FrameConstants[resources.CurrFrameConstantsIndex];
    const auto* instances = frame.InstanceCache->GetCPUData();
    const auto meshCacheRevision = resources.GeometryBuffer ? resources.GeometryBuffer->GetMeshCacheRevision() : 0;
    const auto materialInputs = [](const GDX12Material* material)
    {
        return SnapshotMaterial{material, material ? material->GetRevision() : 0,
            material ? material->Type : MaterialType{}, material && material->ReflectionProbePreview};
    };
    const auto* transforms = frame.TransformCache->GetCPUData();
    if (!resources.LocalShadowCasters) resources.LocalShadowCasters.emplace();
    resources.LocalShadowCasters->clear();
    resources.GeometryRevision = 14695981039346656037ull;
    hash(staticHash, resources.ReflectionEnvironmentRevision);
    hash(staticHash, resources.Lighting.LightDirection);
    hash(staticHash, resources.Lighting.LightRadiance);
    hash(staticHash, resources.Lighting.SunShadowFlags);
    hash(staticHash, resources.Lighting.SunShadowRevision);
    hash(staticHash, resources.ShadowQuality.SunResolution);
    hash(staticHash, resources.ShadowQuality.LocalResolution);
    for (const auto& light : resources.LocalLights) hash(staticHash, light);
    resources.ReflectionProbes.clear();
    resources.ReflectionPreviewDraws.clear();
    GDX12RenderBounds reflectionBounds, casterBounds;
    size_t staticMeshCount = 0;
    size_t reusedSubmeshes = 0;
    size_t visitedSubmeshes = 0;
    size_t publishedSubmeshes = 0;
    bool staticMeshesChanged = false;
    UINT lastTransform = UINT_MAX;
    uint64_t lastTransformSignature = 0;
    auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    for (size_t worldIndex = 0; worldIndex < scenes->GetWorldCount(); ++worldIndex)
    {
        GDX12_SCOPED_CPU_EVENT("Snapshot world collection");
        auto* world = scenes->GetWorld(worldIndex);
        auto& ecs = world->GetECS();
        hash(staticHash, world);
        ecs.ForEach<StaticMeshRenderComponent>([&](Entity entity, StaticMeshRenderComponent& renderer)
        {
            if (!ecs.Has<TransformComponent>(entity)) return;
            if (renderer._CBufferIndices.empty()) return;
            // Instances allocated for a static renderer occupy a contiguous range.
            const UINT first = renderer._CBufferIndices.front();
            if (_staticRendererSnapshots.size() <= first) _staticRendererSnapshots.resize(size_t(first) + 1);
            auto& group = _staticRendererSnapshots[first];
            const auto transformIndex = instances[first].TransformIndex;
            const auto& worldMatrix = transforms[transformIndex].WorldMatrix;
            if (group.Count && group.Mesh == renderer.MeshHandler && group.MeshCacheRevision == meshCacheRevision &&
                group.Count == renderer._CBufferIndices.size() && group.Visible == renderer.Visible &&
                group.Materials.size() == renderer.Materials.size() && group.SubMeshes == renderer.SubMeshIndices &&
                std::memcmp(_staticSnapshotInstances.data() + first, instances + first,
                    group.Count * sizeof(GDX12InstanceData)) == 0 &&
                std::memcmp(&_meshSnapshots[first].World, &worldMatrix, sizeof(worldMatrix)) == 0 &&
                std::equal(group.Materials.begin(), group.Materials.end(), renderer.Materials.begin(),
                    [&](const SnapshotMaterial& saved, const GDX12Material* material)
                    {
                        const auto current = materialInputs(material);
                        return saved.Material == current.Material && saved.Revision == current.Revision &&
                            saved.Type == current.Type && saved.Preview == current.Preview;
                    }))
            {
                size_t retained = group.Count;
                if (!resources.RetainKnownMeshPrimitives(first, group.Count, group.PrimitiveMembershipRevision))
                {
                    retained = 0;
                    while (retained < group.Count && resources.RetainMeshPrimitive(
                        first + static_cast<UINT>(retained), _meshSnapshots[first + retained].PrimitiveHandle))
                        ++retained;
                }
                if (retained == group.Count)
                {
                    group.PrimitiveMembershipRevision = resources.Primitives.GetMembershipRevision();
                    staticMeshCount += group.Count;
                    reusedSubmeshes += group.Count;
                    return;
                }
            }

            group.Count = 0;
            const auto* mesh = GetPrimaryGPUMesh(renderer.MeshHandler);
            const auto partCount = renderer.GetSubMeshCount(mesh->SubMeshes.size());
            visitedSubmeshes += partCount;
            bool cacheGroup = partCount == renderer._CBufferIndices.size();
            for (size_t part = 0; part < partCount; ++part)
            {
                const auto submeshIndex = renderer.GetSubMeshIndex(part);
                const auto& submesh = mesh->SubMeshes.at(submeshIndex);
                const auto* material = renderer.Materials.at(submesh.MaterialIndex);
                const UINT instanceIndex = renderer._CBufferIndices.at(part);
                const auto& instance = instances[instanceIndex];
                const auto& worldMatrix = transforms[instance.TransformIndex].WorldMatrix;
                if (_meshSnapshots.size() <= instanceIndex) _meshSnapshots.resize(size_t(instanceIndex) + 1);
                auto& cached = _meshSnapshots[instanceIndex];
                const bool castsShadows = material->Type != MaterialType::Transparent && !material->ReflectionProbePreview;
                const bool captured = !material->ReflectionProbePreview;
                cacheGroup &= instance.ReflectionDynamic == 0 && captured &&
                    instance.TransformIndex == transformIndex && instanceIndex == first + part;
                const bool sameGeometry = cached.Valid && cached.Handle == renderer.MeshHandler &&
                    cached.Mesh == mesh && cached.GeometryRevision == mesh->ContentRevision && cached.SubMesh == submeshIndex;
                const bool sameMaterial = cached.Valid && cached.Material == material && cached.MaterialRevision == material->_revision;
                const bool sameWorld = cached.Valid && std::memcmp(&cached.World, &worldMatrix, sizeof(worldMatrix)) == 0;
                const bool unchanged = sameGeometry && sameMaterial && sameWorld &&
                    cached.Primitive.Dynamic == (instance.ReflectionDynamic != 0) &&
                    cached.Primitive.Visible == renderer.Visible && cached.Primitive.CastsShadows == castsShadows &&
                    cached.Primitive.CapturedInReflections == captured &&
                    std::memcmp(&cached.Instance, &instance, sizeof(instance)) == 0;
                if (!unchanged)
                {
                    GDX12RenderPrimitive primitive = cached.Primitive;
                    primitive.Bounds = { instance.BoundingBoxCenter, instance.BoundingBoxExtents };
                    primitive.TransformIndex = instance.TransformIndex;
                    primitive.FirstInstance = instanceIndex;
                    primitive.MaterialIndex = instance.MaterialIndex;
                    primitive.Visible = renderer.Visible;
                    primitive.Dynamic = instance.ReflectionDynamic != 0;
                    primitive.CastsShadows = castsShadows;
                    primitive.CapturedInReflections = captured;
                    if (!sameGeometry)
                    {
                        primitive.GeometryRevision = 14695981039346656037ull;
                        hash(primitive.GeometryRevision, renderer.MeshHandler);
                        hash(primitive.GeometryRevision, mesh->ContentRevision);
                        hash(primitive.GeometryRevision, submeshIndex);
                    }
                    if (!sameMaterial)
                    {
                        primitive.MaterialRevision = 14695981039346656037ull;
                        hash(primitive.MaterialRevision, material);
                        hash(primitive.MaterialRevision, material->_revision);
                    }
                    if (!sameWorld)
                    {
                        if (lastTransform != instance.TransformIndex)
                        {
                            lastTransform = instance.TransformIndex;
                            lastTransformSignature = 14695981039346656037ull;
                            hash(lastTransformSignature, worldMatrix);
                        }
                        primitive.TransformRevision = lastTransformSignature;
                    }
                    cached.Handle = renderer.MeshHandler;
                    cached.Mesh = mesh;
                    cached.GeometryRevision = mesh->ContentRevision;
                    cached.SubMesh = submeshIndex;
                    cached.Material = material;
                    cached.MaterialRevision = material->_revision;
                    cached.Instance = instance;
                    cached.World = worldMatrix;
                    cached.Primitive = primitive;
                    cached.Valid = true;
                }
                const bool retained = unchanged && resources.RetainMeshPrimitive(instanceIndex, cached.PrimitiveHandle);
                publishedSubmeshes += !retained;
                const auto handle = retained
                    ? cached.PrimitiveHandle : resources.PublishMeshPrimitive(instanceIndex, cached.Primitive);
                if (!cached.Primitive.Dynamic)
                {
                    ++staticMeshCount;
                    staticMeshesChanged |= !unchanged || !(cached.PrimitiveHandle == handle);
                }
                if (cached.Valid && (!unchanged || !(cached.PrimitiveHandle == handle)))
                {
                    cached.Signature = cached.Primitive.Revision();
                    hash(cached.Signature, handle.Index);
                    hash(cached.Signature, handle.Generation);
                    cached.PrimitiveHandle = handle;
                }
                if (!renderer.Visible || !material->ReflectionProbePreview) continue;
                GDX12IndirectDrawArgs draw = {};
                draw.InstanceID = draw.StartInstanceLocation = instanceIndex;
                draw.InstanceCount = 1;
                draw.IndexCountPerInstance = submesh.IndexCount;
                draw.StartIndexLocation = submesh.StartIndexLocation;
                draw.BaseVertexLocation = submesh.StartVertexLocation;
                resources.ReflectionPreviewDraws.push_back(draw);
            }
            if (cacheGroup && partCount)
            {
                if (_staticSnapshotInstances.size() < first + partCount)
                    _staticSnapshotInstances.resize(first + partCount);
                std::memcpy(_staticSnapshotInstances.data() + first, instances + first,
                    partCount * sizeof(GDX12InstanceData));
                group.Mesh = renderer.MeshHandler;
                group.MeshCacheRevision = meshCacheRevision;
                group.PrimitiveMembershipRevision = resources.Primitives.GetMembershipRevision();
                group.Visible = renderer.Visible;
                group.SubMeshes = renderer.SubMeshIndices;
                group.Materials.clear();
                for (const auto* material : renderer.Materials) group.Materials.push_back(materialInputs(material));
                group.Count = partCount;
            }
        });
        ecs.ForEach<ReflectionProbeComponent>([&](Entity entity, ReflectionProbeComponent& component)
        {
            if (!component.Enabled || !ecs.Has<TransformComponent>(entity)) return;
            const auto& transform = ecs.Get<TransformComponent>(entity);
            GDX12ReflectionProbe probe;
            probe.Id = ((_primaryResources.ReflectionWorldRevision * 65599 + worldIndex) << 32) | entity;
            probe.Position = transform.Location + component.CaptureOffset;
            probe.BoxMin = transform.Location - component.Extents;
            probe.BoxMax = transform.Location + component.Extents;
            probe.BlendDistance = component.BlendDistance;
            probe.Intensity = component.Intensity;
            probe.NearPlane = component.NearPlane;
            probe.FarPlane = component.FarPlane;
            static_cast<GDX12ReflectionCaptureSettings&>(probe) = component;
            probe.Revision = component.Revision;
            if (!IsValidReflectionProbe(probe)) throw std::invalid_argument("Invalid ReflectionProbeComponent");
            resources.ReflectionProbes.push_back(probe);
        });
    }
    {
        GDX12_SCOPED_CPU_EVENT("Snapshot primitive retirement");
        resources.EndMeshPrimitiveUpdate();
    }
    const bool rebuildStatic = staticMeshesChanged || staticMeshCount != _staticMeshSnapshotCount;
    if (rebuildStatic)
    {
        _staticMeshSnapshotCount = staticMeshCount;
        _staticMeshSnapshotHash = 14695981039346656037ull;
        _staticMeshReflectionBounds = {};
        _staticMeshCasterBounds = {};
        resources.StaticMeshShadowCasters.clear();
        ++resources.StaticMeshShadowRevision;
    }
    {
        GDX12_SCOPED_CPU_EVENT("Snapshot primitive reduction");
        const auto findMeshSnapshot = [&](GDX12RenderPrimitiveHandle handle, const GDX12RenderPrimitive& primitive)
            -> const MeshSnapshot*
        {
            if (primitive.Source || primitive.InstanceCount != 1 || primitive.FirstInstance >= _meshSnapshots.size())
                return nullptr;
            const auto& snapshot = _meshSnapshots[primitive.FirstInstance];
            return snapshot.Valid && snapshot.PrimitiveHandle == handle ? &snapshot : nullptr;
        };
        const auto membershipRevision = resources.Primitives.GetMembershipRevision();
        if (rebuildStatic || _snapshotPrimitiveMembershipRevision != membershipRevision)
        {
            _nonStaticMeshPrimitives.clear();
            resources.Primitives.ForEach([&](GDX12RenderPrimitiveHandle handle, const GDX12RenderPrimitive& primitive)
            {
                const auto* snapshot = findMeshSnapshot(handle, primitive);
                if (primitive.Dynamic || !snapshot)
                {
                    _nonStaticMeshPrimitives.push_back(handle);
                    return;
                }
                if (!rebuildStatic || !primitive.Visible || (!primitive.CastsShadows && !primitive.CapturedInReflections))
                    return;
                hash(_staticMeshSnapshotHash, snapshot->Signature);
                if (primitive.CastsShadows)
                {
                    resources.StaticMeshShadowCasters.push_back({ primitive.Bounds, snapshot->Signature, false });
                    _staticMeshCasterBounds.Add(primitive.Bounds);
                }
                else _staticMeshReflectionBounds.Add(primitive.Bounds);
            });
            _snapshotPrimitiveMembershipRevision = membershipRevision;
        }
        for (const auto handle : _nonStaticMeshPrimitives)
        {
            const auto& primitive = *resources.Primitives.Get(handle);
            if (!primitive.Visible || (!primitive.CastsShadows && !primitive.CapturedInReflections)) continue;
            const auto* snapshot = findMeshSnapshot(handle, primitive);
            uint64_t signature = snapshot ? snapshot->Signature : primitive.Revision();
            if (!snapshot)
            {
                hash(signature, handle.Index);
                hash(signature, handle.Generation);
            }
            hash(resources.GeometryRevision, signature);
            // Shadow contributors can change probe lighting even when capture geometry is excluded.
            hash(primitive.Dynamic ? dynamicHash : staticHash, signature);
            if (primitive.CastsShadows)
            {
                resources.LocalShadowCasters->push_back({ primitive.Bounds, signature, primitive.Dynamic });
                casterBounds.Add(primitive.Bounds);
            }
            else reflectionBounds.Add(primitive.Bounds);
        }
    }
    // Rebuild the static reduction on membership changes so removing an extreme shrinks its bounds.
    hash(staticHash, _staticMeshSnapshotHash);
    hash(resources.GeometryRevision, _staticMeshSnapshotHash);
    casterBounds.Merge(_staticMeshCasterBounds);
    reflectionBounds.Merge(_staticMeshReflectionBounds);
    reflectionBounds.Merge(casterBounds);
    resources.ReflectionSceneBounds = reflectionBounds.Empty() ? BoundingBox{} : reflectionBounds.Get();
    if (resources.ShadowQuality.SunResolution && !casterBounds.Empty())
    {
        const auto bounds = casterBounds.Get();
        SubmitCameraShadows(BuildShadowCascades(_RPcommonData.CameraView.Invert(),
            XMConvertToRadians(_RPcommonData.ActiveCameraFOV), GetAspectRatio(),
            _RPcommonData.ActiveCameraNearPlane, _RPcommonData.ActiveCameraFarPlane,
            Vector3(resources.Lighting.LightDirection), &bounds, resources.ShadowQuality.SunResolution));
    }
    std::sort(resources.ReflectionProbes.begin(), resources.ReflectionProbes.end(),
        [](const auto& a, const auto& b) { return a.Id < b.Id; });
    if (_reflectionStaticHash != staticHash)
    {
        ++resources.ReflectionStaticRevision;
        _reflectionStaticHash = staticHash;
    }
    if (_reflectionDynamicHash != dynamicHash)
    {
        ++resources.ReflectionDynamicRevision;
        _reflectionDynamicHash = dynamicHash;
    }
#if defined(DEBUG) || defined(_DEBUG) || defined(PROFILE_BUILD)
    PIXSetMarker(0, "Snapshot submeshes reused=%llu visited=%llu published=%llu static=%llu rebuilt=%u",
        static_cast<unsigned long long>(reusedSubmeshes), static_cast<unsigned long long>(visitedSubmeshes),
        static_cast<unsigned long long>(publishedSubmeshes), static_cast<unsigned long long>(staticMeshCount),
        static_cast<unsigned>(rebuildStatic));
#endif
}
