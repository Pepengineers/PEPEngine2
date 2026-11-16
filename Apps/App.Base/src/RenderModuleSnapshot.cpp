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
    hash(staticHash, resources.ShadowQuality.SunCascades.Count);
    hash(staticHash, resources.ShadowQuality.SunCascades.MaxDistance);
    hash(staticHash, resources.ShadowQuality.SunCascades.AutomaticSplits);
    hash(staticHash, resources.ShadowQuality.SunCascades.Splits);
    hash(staticHash, resources.ShadowQuality.LocalResolution);
    for (const auto& light : resources.LocalLights) hash(staticHash, light);
    resources.ReflectionProbes.clear();
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
                const bool castsShadows = instance.CastsShadows != 0 &&
                    material->Type != MaterialType::Transparent && !material->ReflectionProbePreview;
                const bool captured = !material->ReflectionProbePreview;
                cacheGroup &= instance.ReflectionDynamic == 0 &&
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
    }
    CollectReflectionProbes();
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
            Vector3(resources.Lighting.LightDirection), &bounds, resources.ShadowQuality.SunResolution,
            resources.ShadowQuality.SunCascades));
    }
    const auto geometryHash = ReflectionStaticGeometryHash();
    if (_reflectionStaticGeometryHash != geometryHash)
    {
        ++resources.ReflectionStaticGeometryRevision;
        _reflectionStaticGeometryHash = geometryHash;
    }
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

void RenderModule::CollectReflectionProbes()
{
    auto& resources = _primaryResources;
    resources.ReflectionProbes.clear();
    auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    for (size_t worldIndex = 0; worldIndex < scenes->GetWorldCount(); ++worldIndex)
    {
        auto* world = scenes->GetWorld(worldIndex);
        auto& ecs = world->GetECS();
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
    std::sort(resources.ReflectionProbes.begin(), resources.ReflectionProbes.end(),
        [](const auto& a, const auto& b) { return a.Id < b.Id; });
}

uint64_t RenderModule::ReflectionStaticGeometryHash() const
{
    uint64_t result = 14695981039346656037ull;
    const auto hash = [&](const auto& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    };
    hash(_primaryResources.ReflectionWorldRevision);
    _primaryResources.Primitives.ForEach([&](auto handle, const GDX12RenderPrimitive& primitive) {
        if (primitive.Dynamic || !primitive.Visible ||
            (!primitive.CapturedInReflections && !primitive.CastsShadows)) return;
        hash(handle.Index); hash(handle.Generation);
        hash(primitive.GeometryRevision); hash(primitive.MaterialRevision); hash(primitive.TransformRevision);
        hash(primitive.MaterialIndex); hash(primitive.Bounds.Center); hash(primitive.Bounds.Extents);
        hash(primitive.CapturedInReflections); hash(primitive.CastsShadows);
    });
    return result;
}

void RenderModule::PrepareReflectionScene()
{
    auto& resources = _primaryResources;
    auto& frame = *resources.FrameConstants.at(resources.CurrFrameConstantsIndex);
    const auto meshRevision = resources.GeometryBuffer->GetMeshCacheRevision();
    const auto* instances = frame.InstanceCache->GetCPUData();
    const auto* transforms = frame.TransformCache->GetCPUData();
    const auto* commands = resources.IndirectCommandsCache->GetCPUData();
    const auto hash = [](uint64_t& result, const auto& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    };
    auto changed = resources.IndirectCommandsCache->ChangedElements();
    const auto changedMaterials = frame.MaterialCache->ChangedElements();
    const auto append = [&](const auto& indices, const auto& references) {
        for (UINT index : indices)
            if (const auto found = references.find(index); found != references.end())
                changed.insert(changed.end(), found->second.begin(), found->second.end());
    };
    const auto changedInstances = frame.InstanceCache->ChangedElements();
    changed.insert(changed.end(), changedInstances.begin(), changedInstances.end());
    append(frame.TransformCache->ChangedElements(), _reflectionTransformInstances);
    append(changedMaterials, _reflectionMaterialInstances);
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    bool topology = _reflectionSnapshotWorld != resources.ReflectionWorldRevision ||
        _reflectionSnapshotMesh != meshRevision || _reflectionSnapshotInstances != _instanceCount;
    for (UINT index : changed)
    {
        if (topology) break;
        if (index >= _meshSnapshots.size() || !_meshSnapshots[index].Valid) continue;
        if (index >= _reflectionCommands.size()) { topology = true; break; }
        const auto& command = commands[index];
        const auto& old = _reflectionCommands.at(index);
        topology |= command.InstanceCount && (command.IndexCountPerInstance != old.IndexCountPerInstance ||
            command.StartIndexLocation != old.StartIndexLocation || command.BaseVertexLocation != old.BaseVertexLocation ||
            command.InstanceID != old.InstanceID);
        topology |= instances[index].MaterialIndex >= _reflectionMaterials.size() ||
            !_reflectionMaterials[instances[index].MaterialIndex];
    }
    if (topology)
    {
        CollectReflectionScene();
        _reflectionSnapshotWorld = resources.ReflectionWorldRevision;
        _reflectionSnapshotMesh = meshRevision;
        _reflectionSnapshotInstances = _instanceCount;
        _reflectionTransformInstances.clear();
        _reflectionMaterialInstances.clear();
        _reflectionMaterials.assign(frame.MaterialCache->GetElementCount(), nullptr);
        for (const auto& [name, material] : _materials)
            if (material->_CBufferIndex < _reflectionMaterials.size())
                _reflectionMaterials[material->_CBufferIndex] = material.get();
        _reflectionCommands.assign(commands, commands + resources.IndirectCommandsCache->GetElementCount());
        for (UINT index = 0; index < _meshSnapshots.size(); ++index)
        {
            const auto& snapshot = _meshSnapshots[index];
            if (!snapshot.Valid || !resources.GetMeshPrimitive(index)) continue;
            _reflectionTransformInstances[snapshot.Instance.TransformIndex].push_back(index);
            _reflectionMaterialInstances[snapshot.Instance.MaterialIndex].push_back(index);
        }
    }
    else
    {
        // Destruction removes primitives directly; only a topology rebuild needs epoch retirement.
        bool staticChanged = false;
        for (UINT index : changed)
        {
            if (index >= _meshSnapshots.size() || !_meshSnapshots[index].Valid) continue;
            auto& snapshot = _meshSnapshots[index];
            auto* current = resources.GetMeshPrimitive(index);
            if (!current)
            {
                staticChanged |= snapshot.Primitive.Visible && !snapshot.Primitive.Dynamic;
                snapshot.Valid = false;
                continue;
            }
            const auto& instance = instances[index];
            const auto* material = _reflectionMaterials.at(instance.MaterialIndex);
            const auto& world = transforms[instance.TransformIndex].WorldMatrix;
            auto primitive = snapshot.Primitive;
            primitive.Bounds = {instance.BoundingBoxCenter, instance.BoundingBoxExtents};
            primitive.TransformIndex = instance.TransformIndex;
            primitive.MaterialIndex = instance.MaterialIndex;
            primitive.Visible = commands[index].InstanceCount != 0;
            primitive.Dynamic = instance.ReflectionDynamic != 0;
            primitive.CastsShadows = instance.CastsShadows && material->Type != MaterialType::Transparent &&
                !material->ReflectionProbePreview;
            primitive.CapturedInReflections = !material->ReflectionProbePreview;
            primitive.MaterialRevision = 14695981039346656037ull;
            hash(primitive.MaterialRevision, material); hash(primitive.MaterialRevision, material->_revision);
            primitive.TransformRevision = 14695981039346656037ull;
            hash(primitive.TransformRevision, world);
            _reflectionCommands[index] = commands[index];
            if (primitive.Revision() == snapshot.Primitive.Revision()) continue;
            const auto rebind = [index](auto& references, UINT old, UINT next) {
                if (old == next) return;
                auto& previous = references[old];
                previous.erase(std::remove(previous.begin(), previous.end(), index), previous.end());
                references[next].push_back(index);
            };
            rebind(_reflectionTransformInstances, snapshot.Instance.TransformIndex, instance.TransformIndex);
            rebind(_reflectionMaterialInstances, snapshot.Instance.MaterialIndex, instance.MaterialIndex);
            staticChanged |= !snapshot.Primitive.Dynamic || !primitive.Dynamic;
            snapshot.Primitive = primitive;
            snapshot.Instance = instance;
            snapshot.World = world;
            snapshot.Material = material;
            snapshot.MaterialRevision = material->_revision;
            snapshot.PrimitiveHandle = resources.PublishMeshPrimitive(index, primitive);
            snapshot.Signature = primitive.Revision();
            hash(snapshot.Signature, snapshot.PrimitiveHandle.Index);
            hash(snapshot.Signature, snapshot.PrimitiveHandle.Generation);
        }
        const auto membership = resources.Primitives.GetMembershipRevision();
        if (staticChanged || membership != _snapshotPrimitiveMembershipRevision)
        {
            _staticMeshSnapshotHash = 14695981039346656037ull;
            _staticMeshReflectionBounds = {};
            _staticMeshCasterBounds = {};
            _nonStaticMeshPrimitives.clear();
            resources.StaticMeshShadowCasters.clear();
            ++resources.StaticMeshShadowRevision;
            resources.Primitives.ForEach([&](auto handle, const GDX12RenderPrimitive& primitive) {
                if (primitive.Dynamic || primitive.Source)
                {
                    _nonStaticMeshPrimitives.push_back(handle);
                    return;
                }
                if (!primitive.Visible || (!primitive.CastsShadows && !primitive.CapturedInReflections)) return;
                auto signature = primitive.Revision();
                hash(signature, handle.Index); hash(signature, handle.Generation);
                hash(_staticMeshSnapshotHash, signature);
                if (primitive.CastsShadows)
                {
                    resources.StaticMeshShadowCasters.push_back({primitive.Bounds, signature, false});
                    _staticMeshCasterBounds.Add(primitive.Bounds);
                }
                else _staticMeshReflectionBounds.Add(primitive.Bounds);
            });
            _snapshotPrimitiveMembershipRevision = membership;
            const auto geometryHash = ReflectionStaticGeometryHash();
            if (geometryHash != _reflectionStaticGeometryHash)
            {
                _reflectionStaticGeometryHash = geometryHash;
                ++resources.ReflectionStaticGeometryRevision;
            }
        }
        CollectReflectionProbes();
        uint64_t staticHash = 14695981039346656037ull, dynamicHash = staticHash;
        hash(staticHash, resources.ReflectionEnvironmentRevision);
        hash(staticHash, resources.Lighting.LightDirection); hash(staticHash, resources.Lighting.LightRadiance);
        hash(staticHash, resources.Lighting.SunShadowFlags); hash(staticHash, resources.Lighting.SunShadowRevision);
        hash(staticHash, resources.ShadowQuality.SunResolution);
        hash(staticHash, resources.ShadowQuality.SunCascades.Count);
        hash(staticHash, resources.ShadowQuality.SunCascades.MaxDistance);
        hash(staticHash, resources.ShadowQuality.SunCascades.AutomaticSplits);
        hash(staticHash, resources.ShadowQuality.SunCascades.Splits);
        hash(staticHash, resources.ShadowQuality.LocalResolution);
        for (const auto& light : resources.LocalLights) hash(staticHash, light);
        hash(staticHash, _staticMeshSnapshotHash);
        GDX12RenderBounds bounds = _staticMeshReflectionBounds;
        bounds.Merge(_staticMeshCasterBounds);
        if (!resources.LocalShadowCasters) resources.LocalShadowCasters.emplace();
        resources.LocalShadowCasters->clear();
        for (const auto handle : _nonStaticMeshPrimitives)
        {
            const auto* primitive = resources.Primitives.Get(handle);
            if (!primitive || !primitive->Visible || (!primitive->CastsShadows && !primitive->CapturedInReflections)) continue;
            auto signature = primitive->Revision();
            hash(signature, handle.Index); hash(signature, handle.Generation);
            hash(primitive->Dynamic ? dynamicHash : staticHash, signature);
            bounds.Add(primitive->Bounds);
            if (primitive->CastsShadows)
                resources.LocalShadowCasters->push_back({primitive->Bounds, signature, primitive->Dynamic});
        }
        resources.ReflectionSceneBounds = bounds.Empty() ? BoundingBox{} : bounds.Get();
        if (_reflectionStaticHash != staticHash)
        {
            _reflectionStaticHash = staticHash;
            ++resources.ReflectionStaticRevision;
        }
        if (_reflectionDynamicHash != dynamicHash)
        {
            _reflectionDynamicHash = dynamicHash;
            ++resources.ReflectionDynamicRevision;
        }
    }
    // Local shadow trees use this as topology, not a hash of moving transforms.
    uint64_t geometry = 14695981039346656037ull;
    hash(geometry, resources.ReflectionWorldRevision); hash(geometry, meshRevision);
    hash(geometry, _instanceCount); hash(geometry, resources.Primitives.GetMembershipRevision());
    resources.GeometryRevision = geometry;
}
