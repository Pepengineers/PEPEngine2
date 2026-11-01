#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoader.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <stdexcept>

namespace
{
    static AutoConsoleVariable<bool> SkinningEnabled(
        L"Render.Skinning.Enabled", true, L"Freeze skeletal deformation when disabled.",
        EConsoleVariableFlags::Default);
    static AutoConsoleVariable<int> SkinningQuality(
        L"Render.Skinning.Quality", 2, L"0 Off, 1 Low (15 Hz), 2 Mid (30 Hz), 3 High (60 Hz).",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 3; });

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
    _primaryDevice->GetCommandQueue()->Flush();
    const GDX12IndirectDrawArgs empty = {};
    for (const auto& part : it->second.Parts)
    {
        _primaryResources.IndirectCommandsCache->CopyData(part.Instance, empty);
        _primaryResources.GeometryBuffer->ReleaseSkinnedVertices(part.GPU->OutputVertex, part.GPU->VertexCount);
        _freeSkeletalInstances.push_back(part.Instance);
    }
    _skeletalStates.erase(it);
    ++_primaryResources.ReflectionDynamicRevision;
    if (_skeletalStates.empty()) _skinning.reset();
}

void RenderModule::PrepareSkeletalMeshes()
{
    const bool enabled = SkinningEnabled.GetValue();
    const int quality = SkinningQuality.GetValue();
    const bool animate = enabled && quality != 0;
    const double presetRates[] = {0, 15, 30, 60};
    auto& assets = AssetManager::GetInstance();
    auto& meshes = assets.Meshes();
    auto& scenes = assets.Scenes();
    auto* frame = GetCurrentPrimaryFrameConstants();
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
            if (_dualGPUMode) throw std::runtime_error("Skeletal rendering requires the single-GPU pipeline");
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
                    for (const auto& node : scene->GetNodes())
                        for (const auto handle : node.Meshes) partCount += meshes.GetMesh(handle)->GetSubMeshCount();
                    if (partCount > UINT_MAX - _instanceCount) throw std::overflow_error("Too many skeletal attachments");
                    if (partCount && !_skinning) _skinning = std::make_unique<GDX12Skinning>(_primaryDevice.get());
                    EnsureInstanceCapacity(_instanceCount + static_cast<UINT>(partCount));
                    created.Parts.reserve(partCount);
                    std::map<std::pair<UINT, UINT>, std::vector<GDX12Material*>> importedMaterials;
                    for (UINT nodeIndex = 0; nodeIndex < scene->GetNodeCount(); ++nodeIndex)
                    {
                        for (const auto handle : scene->GetNode(nodeIndex).Meshes)
                        {
                            const auto* mesh = meshes.GetMesh(handle);
                            const auto revision = meshes.GetContentRevision(handle);
                            SubmitMesh(mesh, handle);
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
                                part.GPU = _skinning->CreateMesh(source, static_cast<UINT>(_primaryResources.FrameConstants.size()));
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
                                draw.BaseVertexLocation = static_cast<INT>(ready.GPU->OutputVertex);
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
            state.Seen = true;
            if (state.Parts.empty()) return;
            const bool edited = state.ComponentRevision != component._revision;
            const double dt = world->IsPaused() ? 0 : static_cast<double>(_timer->DeltaTime()) * world->GetTimeScale();
            if (!std::isfinite(dt)) throw std::invalid_argument("Invalid skeletal world delta time");
            if (animate && !component._frozen)
            {
                const double next = component._time + dt * component._speed;
                if (!std::isfinite(next)) throw std::overflow_error("Animation time overflow");
                component._time = next;
                state.SinceUpdate += std::abs(dt);
            }
            const double rate = component._rate == 0 ? presetRates[quality] : component._rate;
            const bool due = animate && !component._frozen && state.SinceUpdate >= 1.0 / rate;
            auto previousPose = state.Pose;
            const bool evaluate = state.Pose.empty() || (animate && (edited || due));
            if (evaluate)
            {
                state.Pose = scene->EvaluatePose(component._clip, component._time, component._loop, component._localPose);
                state.SinceUpdate = due ? std::fmod(state.SinceUpdate, 1.0 / rate) : 0;
            }
            const bool changed = previousPose != state.Pose;
            const bool reset = previousPose.empty() || edited || !animate || component._frozen ||
                world->IsPaused() || _RPcommonData.ResetHistory;
            if (reset) previousPose = state.Pose;
            state.Dispatch = changed || state.NeedsSettle;
            // Only the frame displaying a new pose carries deformation velocity.
            state.NeedsSettle = changed && !reset;
            if (animate || state.ComponentRevision == 0) state.ComponentRevision = component._revision;
            const auto& transform = ecs.Get<TransformComponent>(entity);
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
                instance.BoundingBoxCenter = part.Bounds.Center;
                instance.BoundingBoxExtents = part.Bounds.Extents;
                frame->InstanceCache->CopyData(part.Instance, instance);
            }
        });
    }
    for (auto it = _skeletalStates.begin(); it != _skeletalStates.end();)
    {
        if (it->second.Seen) { ++it; continue; }
        const auto key = it->first;
        ++it;
        ReleaseSkeletalState(*key.first, key.second);
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
