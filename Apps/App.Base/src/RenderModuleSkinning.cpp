#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoader.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <stdexcept>

namespace
{
    static AutoConsoleVariable<bool> SkinningEnabled(
        L"Render.Skinning.Enabled", true, L"Freeze skeletal deformation when disabled.",
        EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> SkinningQuality(
        L"Render.Skinning.Quality", GDX12Quality::Mid, L"0 Off, 1 Low (15 Hz), 2 Mid (30 Hz), 3 High and 4 Epic (60 Hz).",
        EConsoleVariableFlags::Default);
    static AutoConsoleVariable<int> SkinningPoseRate(
        L"Render.Skinning.PoseRateHz", 0, L"Global pose rate override (1..240 Hz). 0 honors component and quality settings.",
        EConsoleVariableFlags::Default, [](const int value) { return value >= 0 && value <= 240; });

    Matrix EntityWorld(const TransformComponent& transform)
    {
        return Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
            XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
            XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
    }
}

void RenderModule::ReleaseSkeletalState(World& world, Entity entity)
{
    const auto it = _skeletalStates.find({&world, entity});
    if (it == _skeletalStates.end()) return;
    // Uploads/descriptors and draw templates are shared with queued raster command lists.
    if (!_batchWorldRelease) _primaryDevice->GetCommandQueue()->Flush();
    const GDX12IndirectDrawArgs empty = {};
    for (const auto& part : it->second.Parts)
    {
        _primaryResources.RemoveMeshPrimitive(part.Instance);
        _primaryResources.IndirectCommandsCache->CopyData(part.Instance, empty);
        _primaryResources.GeometryBuffer->ReleaseSkinnedVertices(part.GPU->OutputVertex, part.GPU->VertexCount);
        _freeSkeletalInstances.push_back(part.Instance);
    }
    _skeletalStates.erase(it);
    _skeletalBatchDirty = true;
    ++_primaryResources.ReflectionDynamicRevision;
    if (_skeletalStates.empty() && !_batchWorldRelease) _skinning.reset();
}

void RenderModule::PrepareSkeletalMeshes()
{
    const bool instancing = GDX12SkeletalInstancingEnabled();
    _skeletalBatchDirty |= instancing != _skeletalInstancing;
    _skeletalInstancing = instancing;
    const bool enabled = SkinningEnabled.GetValue();
    const int quality = SkinningQuality.GetInt();
    const bool animate = enabled && quality != 0;
    constexpr double presetRates[] = {0, 15, 30, 60, 60};
    auto& assets = AssetManager::GetInstance();
    auto& meshes = assets.Meshes();
    auto& scenes = assets.Scenes();
    if (_skeletalStates.empty())
    {
        UINT64 vertices = 0, attachments = 0;
        auto worlds = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
        for (size_t index = 0; index < worlds->GetWorldCount(); ++index)
            worlds->GetWorld(index)->GetECS().ForEach<SkeletalMeshRenderComponent>(
                [&](Entity entity, const SkeletalMeshRenderComponent& component)
                {
                    if (!worlds->GetWorld(index)->GetECS().Has<TransformComponent>(entity)) return;
                    const auto* scene = scenes.GetScene(component._scene);
                    if (!scene) return;
                    for (const auto& node : scene->GetNodes())
                        for (const auto handle : node.Meshes)
                            if (const auto* mesh = meshes.GetMesh(handle))
                            {
                                SubmitMesh(mesh, handle);
                                for (const auto& part : mesh->GetSubMeshes())
                                {
                                    if (part.Vertices.empty() || part.Indices.empty()) continue;
                                    vertices += part.Vertices.size();
                                    ++attachments;
                                }
                            }
                });
        const auto extra = attachments > _freeSkeletalInstances.size() ? attachments - _freeSkeletalInstances.size() : 0;
        if (vertices > UINT_MAX || extra > UINT_MAX - _instanceCount)
            throw std::overflow_error("Initial skeletal crowd exceeds buffer addressing");
        EnsureInstanceCapacity(_instanceCount + static_cast<UINT>(extra));
        if (vertices)
        {
            const auto offset = _primaryResources.GeometryBuffer->AllocateSkinnedVertices(static_cast<UINT>(vertices));
            _primaryResources.GeometryBuffer->ReleaseSkinnedVertices(offset, static_cast<UINT>(vertices));
        }
    }
    struct PoseJob final : ParallelJob
    {
        struct Task
        {
            World* WorldPtr;
            Entity Id;
            SkeletalMeshRenderComponent* Component;
            SkeletalState* State;
            const SceneAsset* Scene;
        };
        PoseJob(RenderModule& owner, bool animate, double rate, int rateOverride) :
            Owner(owner), Animate(animate), Rate(rate), RateOverride(rateOverride) {}
        void Execute(size_t index) override
        {
            auto& task = Tasks[index];
            const double componentRate = task.Component->GetUpdateRateHz();
            const double rate = RateOverride ? RateOverride : (componentRate != 0 ? componentRate : Rate);
            Owner.UpdateSkeletalPose(*task.WorldPtr, task.Id, *task.Component, *task.State, *task.Scene, Animate, rate);
        }
        RenderModule& Owner;
        bool Animate;
        double Rate;
        int RateOverride;
        std::vector<Task> Tasks;
    } poses(*this, animate, presetRates[quality], SkinningPoseRate.GetValue());
    poses.Tasks.reserve(_skeletalStates.size());
    for (auto& entry : _skeletalStates) entry.second.Seen = false;
    auto worlds = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    for (size_t worldIndex = 0; worldIndex < worlds->GetWorldCount(); ++worldIndex)
    {
        auto* world = worlds->GetWorld(worldIndex);
        auto& ecs = world->GetECS();
        ecs.ForEach<SkeletalMeshRenderComponent>([&](Entity entity, SkeletalMeshRenderComponent& component)
        {
            if (!component._scene.IsValid() || !ecs.Has<TransformComponent>(entity))
            {
                ReleaseSkeletalState(*world, entity);
                return;
            }
            const auto* scene = scenes.GetScene(component._scene);
            if (!scene)
            {
                ReleaseSkeletalState(*world, entity);
                return;
            }
            const auto sceneRevision = scenes.GetContentRevision(component._scene);
            const auto key = std::make_pair(world, entity);
            auto found = _skeletalStates.find(key);
            bool rebuild = found == _skeletalStates.end();
            if (!rebuild)
            {
                const auto& state = found->second;
                rebuild = state.Scene != component._scene || state.SceneRevision != sceneRevision;
                for (const auto& part : state.Parts)
                    rebuild |= !meshes.GetMesh(part.Mesh) || meshes.GetContentRevision(part.Mesh) != part.MeshRevision;
            }
            for (const auto& node : scene->GetNodes())
                for (const auto handle : node.Meshes)
                    if (!meshes.GetMesh(handle))
                    {
                        ReleaseSkeletalState(*world, entity);
                        return;
                    }
            if (rebuild)
            {
                ReleaseSkeletalState(*world, entity);
                SkeletalState state;
                state.Scene = component._scene;
                state.SceneRevision = sceneRevision;
                _skeletalStates.emplace(key, std::move(state));
                try
                {
                    _primaryDevice->GetCommandQueue()->Flush();
                    auto& created = _skeletalStates.at(key);
                    size_t partCount = 0;
                    UINT skinVertices = 0;
                    for (const auto& node : scene->GetNodes())
                        for (const auto handle : node.Meshes)
                        {
                            const auto* mesh = meshes.GetMesh(handle);
                            SubmitMesh(mesh, handle);
                            partCount += mesh->GetSubMeshCount();
                            for (const auto& source : mesh->GetSubMeshes())
                            {
                                if (source.Vertices.empty() || source.Indices.empty()) continue;
                                if (source.Vertices.size() > UINT_MAX / sizeof(Vertex) - skinVertices)
                                    throw std::overflow_error("Skeletal attachments exceed vertex buffer addressing");
                                skinVertices += static_cast<UINT>(source.Vertices.size());
                            }
                        }
                    const auto extra = partCount > _freeSkeletalInstances.size() ? partCount - _freeSkeletalInstances.size() : 0;
                    if (extra > UINT_MAX - _instanceCount) throw std::overflow_error("Too many skeletal attachments");
                    if (partCount && !_skinning) _skinning = std::make_unique<GDX12Skinning>(_primaryDevice.get());
                    EnsureInstanceCapacity(_instanceCount + static_cast<UINT>(extra));
                    // Upload source meshes first so they cannot consume the reserved output range.
                    _primaryResources.GeometryBuffer->ReserveSkinnedVertices(skinVertices);
                    created.Parts.reserve(partCount);
                    std::map<std::pair<UINT, UINT>, std::vector<GDX12Material*>> importedMaterials;
                    for (UINT nodeIndex = 0; nodeIndex < scene->GetNodeCount(); ++nodeIndex)
                    {
                        for (const auto handle : scene->GetNode(nodeIndex).Meshes)
                        {
                            const auto* mesh = meshes.GetMesh(handle);
                            const auto revision = meshes.GetContentRevision(handle);
                            const auto materialKey = std::make_pair(handle.GetValue(), handle.GetGeneration());
                            auto materialIt = importedMaterials.find(materialKey);
                            if (materialIt == importedMaterials.end())
                            {
                                const auto name = "Skeletal_" + std::to_string(handle.GetValue()) + "_" +
                                    std::to_string(handle.GetGeneration()) + "_" + std::to_string(revision);
                                materialIt = importedMaterials.emplace(materialKey,
                                    WorldLoader::BuildMaterialSlots(*this, *mesh, name)).first;
                            }
                            const auto* gpuMesh = GetPrimaryGPUMesh(handle);
                            for (UINT subMesh = 0; subMesh < mesh->GetSubMeshCount(); ++subMesh)
                            {
                                const auto& source = mesh->GetSubMesh(subMesh);
                                if (source.Vertices.empty() || source.Indices.empty()) continue;
                                SkeletalPart part;
                                part.Mesh = handle;
                                part.MeshRevision = revision;
                                part.Node = nodeIndex;
                                part.SubMesh = subMesh;
                                part.SourceBounds = source.Bounds;
                                part.SourceMaterial = source.MaterialIndex;
                                part.ImportedMaterial = materialIt->second.at(source.MaterialIndex);
                                for (const auto& bone : source.Bones)
                                {
                                    UINT index = UINT_MAX;
                                    for (UINT i = 0; i < scene->GetNodeCount(); ++i)
                                        if (scene->GetNode(i).Name == bone.NodeName)
                                        {
                                            if (index != UINT_MAX) throw std::invalid_argument("Ambiguous skeleton node name");
                                            index = i;
                                        }
                                    if (index == UINT_MAX) throw std::invalid_argument("Missing skeleton node");
                                    part.BoneNodes.push_back(index);
                                    part.InverseBinds.push_back(bone.InverseBind);
                                }
                                part.GPU = _skinning->CreateMesh(source, static_cast<UINT>(_primaryResources.FrameConstants.size()), handle, revision);
                                part.Palette.resize(part.BoneNodes.size() + 1);
                                part.GPU->OutputVertex = _primaryResources.GeometryBuffer->AllocateSkinnedVertices(part.GPU->VertexCount);
                                if (_freeSkeletalInstances.empty()) part.Instance = _instanceCount++;
                                else
                                {
                                    part.Instance = _freeSkeletalInstances.back();
                                    _freeSkeletalInstances.pop_back();
                                }
                                created.Parts.push_back(std::move(part));
                                const auto& ready = created.Parts.back();
                                GDX12IndirectDrawArgs draw = {};
                                draw.InstanceID = draw.StartInstanceLocation = ready.Instance;
                                draw.IndexCountPerInstance = gpuMesh->SubMeshes[subMesh].IndexCount;
                                draw.InstanceCount = 1;
                                draw.StartIndexLocation = gpuMesh->SubMeshes[subMesh].StartIndexLocation;
                                draw.BaseVertexLocation = 0;
                                _primaryResources.IndirectCommandsCache->CopyData(ready.Instance, draw);
                            }
                        }
                    }
                }
                catch (...)
                {
                    ReleaseSkeletalState(*world, entity);
                    throw;
                }
            }
            auto& state = _skeletalStates.at(key);
            _skeletalBatchDirty |= state.ComponentRevision != component._revision;
            state.Seen = true;
            if (state.Parts.empty()) return;
            poses.Tasks.push_back({world, entity, &component, &state, scene});
        });
    }
    for (auto it = _skeletalStates.begin(); it != _skeletalStates.end();)
    {
        if (it->second.Seen) { ++it; continue; }
        const auto key = it->first;
        ++it;
        ReleaseSkeletalState(*key.first, key.second);
    }
    if (_skeletalBatchDirty)
    {
        std::map<std::tuple<UINT, UINT, std::uint64_t, UINT, UINT>, std::vector<SkeletalPart*>> batches;
        for (const auto& task : poses.Tasks)
            for (auto& part : task.State->Parts)
            {
                part.Batch = UINT_MAX;
                part.BatchStart = 0;
                if (!instancing) continue;
                auto* material = task.Component->_materials.empty() ? part.ImportedMaterial :
                    task.Component->_materials.at(part.SourceMaterial);
                batches[{part.Mesh.GetValue(), part.Mesh.GetGeneration(), part.MeshRevision, part.SubMesh,
                    material->_CBufferIndex}].push_back(&part);
            }
        UINT batch = 0, first = 0;
        for (const auto& [key, parts] : batches)
        {
            for (auto* part : parts) { part->Batch = batch; part->BatchStart = first; }
            first += static_cast<UINT>(parts.size());
            ++batch;
        }
        _primaryResources.SkeletalBatchCount = batch;
        _primaryResources.SkeletalInstanceCount = first;
        _skeletalBatchDirty = false;
    }
    // Resource creation/release stays on the owner thread; poses and disjoint instance slots run in jobs.
    poses.Schedule(*_RPcommonData.Jobs, poses.Tasks.size(), 1).Complete();
    const auto hash = [](uint64_t& result, const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    };
    const auto* frame = GetCurrentPrimaryFrameConstants();
    for (const auto& [owner, state] : _skeletalStates)
    {
        for (const auto& part : state.Parts)
        {
            const auto& instance = frame->InstanceCache->GetCPUData()[part.Instance];
            GDX12RenderPrimitive primitive;
            primitive.Bounds = part.Bounds;
            primitive.TransformIndex = instance.TransformIndex;
            primitive.FirstInstance = part.Instance;
            primitive.MaterialIndex = part.Material;
            primitive.Dynamic = true;
            primitive.CastsShadows = instance.CastsShadows != 0 &&
                part.ActiveMaterial->Type != MaterialType::Transparent && !part.ActiveMaterial->ReflectionProbePreview;
            primitive.CapturedInReflections = !part.ActiveMaterial->ReflectionProbePreview;
            primitive.GeometryRevision = 14695981039346656037ull;
            hash(primitive.GeometryRevision, state.SceneRevision);
            hash(primitive.GeometryRevision, part.Mesh);
            hash(primitive.GeometryRevision, part.MeshRevision);
            hash(primitive.GeometryRevision, part.SubMesh);
            primitive.MaterialRevision = 14695981039346656037ull;
            hash(primitive.MaterialRevision, part.ActiveMaterial);
            hash(primitive.MaterialRevision, part.ActiveMaterial->_revision);
            primitive.TransformRevision = 14695981039346656037ull;
            hash(primitive.TransformRevision, frame->TransformCache->GetCPUData()[instance.TransformIndex].WorldMatrix);
            primitive.DeformationRevision = state.DeformationRevision;
            _primaryResources.PublishMeshPrimitive(part.Instance, primitive);
        }
    }

}

void RenderModule::UpdateSkeletalPose(World& world, Entity entity, SkeletalMeshRenderComponent& component,
    SkeletalState& state, const SceneAsset& scene, bool animate, double rate)
{
    auto* frame = GetCurrentPrimaryFrameConstants();
    const bool edited = state.ComponentRevision != component._revision;
    const double dt = world.IsPaused() ? 0 : static_cast<double>(_timer->DeltaTime()) * world.GetTimeScale();
    if (!std::isfinite(dt)) throw std::invalid_argument("Invalid skeletal world delta time");
    if (animate && !component._frozen)
    {
        const double next = component._time + dt * component._speed;
        if (!std::isfinite(next)) throw std::overflow_error("Animation time overflow");
        component._time = next;
        state.SinceUpdate += std::abs(dt);
    }
    const bool due = animate && !component._frozen && state.SinceUpdate >= 1.0 / rate;
    auto previousPose = state.Pose;
    const bool evaluate = state.Pose.empty() || (animate && (edited || due));
    if (evaluate)
    {
        state.Pose = scene.EvaluatePose(component._clip, component._time, component._loop, component._localPose);
        state.SinceUpdate = due ? std::fmod(state.SinceUpdate, 1.0 / rate) : 0;
    }
    const bool changed = previousPose != state.Pose;
    if (changed || state.DeformationRevision == 0)
    {
        // Pose jobs already own this state; keep the byte-identical signature off the recording thread.
        uint64_t deformation = 14695981039346656037ull;
        for (const auto& matrix : state.Pose)
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&matrix);
            for (size_t i = 0; i < sizeof(matrix); ++i) deformation = (deformation ^ bytes[i]) * 1099511628211ull;
        }
        state.DeformationRevision = deformation;
    }
    const bool reset = previousPose.empty() || edited || !animate || component._frozen ||
        world.IsPaused() || _RPcommonData.ResetHistory;
    if (reset) previousPose = state.Pose;
    state.Dispatch = changed || state.NeedsSettle;
    // Only the frame displaying a new pose carries deformation velocity.
    state.NeedsSettle = changed && !reset;
    if (animate || state.ComponentRevision == 0) state.ComponentRevision = component._revision;
    const auto& transform = world.GetECS().Get<TransformComponent>(entity);
    const Matrix entityWorld = EntityWorld(transform);
    const auto transformIt = _transformGPUData.find(entity);
    if (transformIt == _transformGPUData.end()) throw std::logic_error("Missing skeletal transform registration");
    for (auto& part : state.Parts)
    {
        for (size_t bone = 0; bone < part.BoneNodes.size(); ++bone)
        {
            part.Palette[bone].Current = part.InverseBinds[bone] * state.Pose.at(part.BoneNodes[bone]);
            part.Palette[bone].Previous = part.InverseBinds[bone] * previousPose.at(part.BoneNodes[bone]);
        }
        part.Palette.back() = {state.Pose.at(part.Node), previousPose.at(part.Node)};
        bool hasBounds = false;
        BoundingBox localBounds;
        // Positive normalized LBS lies inside the union of each transformed source box.
        for (const auto& palette : part.Palette)
        {
            BoundingBox bounds;
            part.SourceBounds.Transform(bounds, palette.Current);
            if (hasBounds) BoundingBox::CreateMerged(localBounds, localBounds, bounds);
            else localBounds = bounds;
            hasBounds = true;
        }
        localBounds.Extents.x += std::abs(localBounds.Extents.x) * 0.00001f + 0.01f;
        localBounds.Extents.y += std::abs(localBounds.Extents.y) * 0.00001f + 0.01f;
        localBounds.Extents.z += std::abs(localBounds.Extents.z) * 0.00001f + 0.01f;
        localBounds.Transform(part.Bounds, entityWorld);
        const auto& center = part.Bounds.Center;
        const auto& extents = part.Bounds.Extents;
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
            !std::isfinite(extents.x) || !std::isfinite(extents.y) || !std::isfinite(extents.z))
            throw std::overflow_error("Non-finite skeletal bounds");
        auto* material = component._materials.empty() ? part.ImportedMaterial :
            component._materials.at(part.SourceMaterial);
        part.ActiveMaterial = material;
        part.Material = material->_CBufferIndex;
        GDX12InstanceData instance = {};
        instance.TransformIndex = transformIt->second.CBufferIndex;
        instance.MaterialIndex = part.Material;
        instance.ReflectionDynamic = 1;
        instance.CastsShadows = component.GetCastsShadows() ? 1u : 0u;
        instance.SkinOutputVertex = part.GPU->OutputVertex;
        instance.SkinBatch = part.Batch;
        instance.SkinBatchStart = part.BatchStart;
        instance.BoundingBoxCenter = part.Bounds.Center;
        instance.BoundingBoxExtents = part.Bounds.Extents;
        frame->InstanceCache->CopyData(part.Instance, instance);
    }
}

void RenderModule::DispatchSkeletalMeshes(GDX12CommandList* commands)
{
    for (auto& entry : _skeletalStates)
    {
        auto& state = entry.second;
        if (!state.Dispatch) continue;
        for (auto& part : state.Parts)
            _skinning->Dispatch(commands, *_primaryResources.GeometryBuffer, *part.GPU,
                _primaryResources.CurrFrameConstantsIndex, part.Palette);
    }
    _primaryResources.GeometryBuffer->EndSkinning(commands);
}

std::vector<GDX12SkinningWork> RenderModule::FreezeSkeletalMeshes() {
    std::vector<GDX12SkinningWork> result;
    for(auto& [owner,state]:_skeletalStates) {
        if(!state.Dispatch) continue;
        for(auto& part:state.Parts) result.push_back(_skinning->Freeze(*part.GPU,_primaryResources.CurrFrameConstantsIndex,part.Palette));
    }
    return result;
}
