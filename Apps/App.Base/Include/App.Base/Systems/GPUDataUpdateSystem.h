#pragma once
#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"

class GPUDataUpdateSystem final : public System
{
public:
    SystemUpdatePhase GetUpdatePhase() const override { return SystemUpdatePhase::RenderPrep; }
    void Tick(World& world, float dt) override
    {
        Prepare(world, dt);
        TickRange(world, dt, 0, GetWorkCount(world));
    }
protected:
    void Prepare(World&, float) override
    {
        _render = BenchmarkEngine::GetLocator().GetModule<RenderModule>().get();
        _numFrames = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()
            ->FindConsoleVariable(L"Render.NumFrames").GetInt();
    }
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<TransformComponent>().Size(); }
    void TickRange(World& world, float, size_t begin, size_t end) override
    {
        auto& ecs = world.GetECS();
        ecs.ForEachRange<TransformComponent>(begin, end, [&](Entity entity, TransformComponent& transform)
        {
            UpdateCamera(ecs, entity, transform);
            auto& gpu = _render->GetTransformGPUData(entity);
            const bool changed = transform.DirtyFlag;
            // One final cycle replaces the last moving slot with zero object motion.
            if (changed) gpu.NumFramesDirty = static_cast<UINT>(_numFrames) + 1;
            if (gpu.NumFramesDirty > 0)
            {
                gpu.World = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                    XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                    XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
                GDX12TransformConstants constants;
                XMStoreFloat4x4(&constants.WorldMatrix, XMMatrixTranspose(gpu.World));
                XMStoreFloat4x4(&constants.PrevWorldMatrix,
                    XMMatrixTranspose(gpu.HasPreviousWorld ? gpu.PrevWorld : gpu.World));
                gpu.PrevWorld = gpu.World;
                gpu.HasPreviousWorld = true;
                if (_render->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    _render->GetCurrentPrimaryFrameConstants()->TransformCache->CopyData(gpu.CBufferIndex, constants);
                if (_render->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    _render->GetCurrentSecondaryFrameConstants()->TransformCache->CopyData(gpu.CBufferIndex, constants);
                --gpu.NumFramesDirty;
            }
            if (ecs.Has<StaticMeshRenderComponent>(entity))
            {
                auto& renderer = ecs.Get<StaticMeshRenderComponent>(entity);
                const bool dynamic = renderer.ReflectionDynamic || ecs.Has<VelocityComponent>(entity) ||
                    ecs.Has<CircleMovementComponent>(entity) || ecs.Has<SplineFollowComponent>(entity) ||
                    ecs.Has<LookAtTargetComponent>(entity) ||
                    (ecs.Has<RigidBodyComponent>(entity) && ecs.Get<RigidBodyComponent>(entity).MotionType != PhysicsMotionType::Static);
                renderer.DirtyFlag |= renderer._reflectionDynamic != dynamic;
                renderer._reflectionDynamic = dynamic;
                if (changed || renderer.DirtyFlag)
                {
                    renderer._numFramesDirty = _numFrames;
                    renderer.DirtyFlag = false;
                }
                if (renderer._numFramesDirty > 0)
                {
                    if (_render->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                        UpdateInstances(_render->GetPrimaryGPUMesh(renderer.MeshHandler),
                            _render->GetCurrentPrimaryFrameConstants(), gpu, renderer);
                    if (_render->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                        UpdateInstances(_render->GetSecondaryGPUMesh(renderer.MeshHandler),
                            _render->GetCurrentSecondaryFrameConstants(), gpu, renderer);
                    --renderer._numFramesDirty;
                }
            }
            // Both transform and instance consumers must see the change before it is cleared.
            transform.DirtyFlag = false;
        });
    }
private:
    static void UpdateInstances(const GPUMesh* mesh, GDX12FrameConstants* frame,
        const TransformCompGPUData& transform, StaticMeshRenderComponent& renderer)
    {
        const size_t count = renderer.GetSubMeshCount(mesh->SubMeshes.size());
        for (size_t i = 0; i < count; ++i)
        {
            const auto index = renderer.GetSubMeshIndex(i);
            const auto& submesh = mesh->SubMeshes.at(index);
            BoundingBox bounds;
            mesh->CPUMesh->GetSubMesh(index).Bounds.Transform(bounds, transform.World);
            if (i == 0) renderer.Bounds = bounds;
            else BoundingBox::CreateMerged(renderer.Bounds, renderer.Bounds, bounds);
            GDX12InstanceData instance = {};
            instance.ReflectionDynamic = renderer._reflectionDynamic ? 1u : 0u;
            instance.TransformIndex = transform.CBufferIndex;
            instance.MaterialIndex = renderer.Materials.at(submesh.MaterialIndex)->_CBufferIndex;
            instance.BoundingBoxCenter = bounds.Center;
            instance.BoundingBoxExtents = bounds.Extents;
            frame->InstanceCache->CopyData(renderer._CBufferIndices.at(i), instance);
        }
    }

    void UpdateCamera(WorldECS& ecs, Entity entity, TransformComponent& transform)
    {
        if (!ecs.Has<CameraComponent>(entity)) return;
        auto& camera = ecs.Get<CameraComponent>(entity);
        auto* renderModule = _render;
        const int numFrames = _numFrames;
        // Active camera requires constant update due to jittering
        auto pipelineCommonData = renderModule->GetRenderPipelineCommonData();
        bool isActiveCamera = pipelineCommonData->ActiveCameraCBufferIndex == camera._CBufferIndex;
        if (isActiveCamera)
        {
            camera.DirtyFlag = true;
        }

        if (camera.DirtyFlag || transform.DirtyFlag)
        {
            camera.DirtyFlag = false;
            camera._numFramesDirty = numFrames;
        }

        if (camera._numFramesDirty > 0)
        {
            GDX12CameraConstants objConstants;

            Vector3 CameraLocation = transform.Location;
            Vector3 CameraRotation = transform.Rotation;
            float FOV = DirectX::XMConvertToRadians(camera.FOV);

            Matrix rotMatrix = Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(CameraRotation.y),
                XMConvertToRadians(CameraRotation.x),
                XMConvertToRadians(CameraRotation.z));

            Vector3 forward = rotMatrix.Forward();
            Vector3 CameraTarget = CameraLocation - forward;


            Matrix view = Matrix::CreateLookAt(CameraLocation, CameraTarget, Vector3::Up);

            const auto sunShadowResolution = renderModule->GetShadowQuality().SunResolution;
            if (isActiveCamera && sunShadowResolution != 0)
            {
                const auto& lighting = renderModule->GetLightingSettings();
                const auto shadows = BuildShadowCascades(view.Invert(), FOV, renderModule->GetAspectRatio(),
                    camera.NearPlane, camera.FarPlane, Vector3(lighting.LightDirection), nullptr, sunShadowResolution);
                renderModule->SubmitCameraShadows(shadows);
            }

            //reserved-Z proj matrix
            Matrix proj = Matrix::CreatePerspectiveFieldOfView(FOV, renderModule->GetAspectRatio(),
                camera.NearPlane, camera.FarPlane);
            const float range = camera.NearPlane / (camera.FarPlane - camera.NearPlane);
            proj._33 = range;
            proj._43 = range * camera.FarPlane;

            Matrix unjitteredViewProj = view * proj;

            XMStoreFloat4x4(&objConstants.ViewProjNoJitter, XMMatrixTranspose(unjitteredViewProj));
            const bool reset = camera.ResetHistory || (isActiveCamera &&
                (pipelineCommonData->RequestHistoryReset || proj != pipelineCommonData->CameraViewToClip));
            objConstants.PrevViewProjNoJitter = reset ? objConstants.ViewProjNoJitter : camera.PrevViewProjNoJitter;
            camera.ResetHistory = false;

            if (isActiveCamera)
            {
                if (reset)
                {
                    ++pipelineCommonData->HistoryRevision;
                    _jitterFrameIndex = 0;
                }
                pipelineCommonData->ActiveCameraFOV = camera.FOV;
                pipelineCommonData->ActiveCameraNearPlane = camera.NearPlane;
                pipelineCommonData->ActiveCameraFarPlane = camera.FarPlane;
                pipelineCommonData->ResetHistory = reset;
                pipelineCommonData->RequestHistoryReset = false;
                pipelineCommonData->CameraViewToClip = proj;
                pipelineCommonData->ClipToCameraView = proj.Invert();

                // Streamline uses row-major matrices.
                pipelineCommonData->ClipToPrevClip = unjitteredViewProj.Invert() * Matrix(objConstants.PrevViewProjNoJitter).Transpose();
                pipelineCommonData->PrevClipToClip = pipelineCommonData->ClipToPrevClip.Invert();
            }

            XMStoreFloat4x4(&camera.PrevViewProjNoJitter, XMMatrixTranspose(unjitteredViewProj));

            if (isActiveCamera)
            {
                pipelineCommonData->ActiveCameraJitterOffsetX = 0;
                pipelineCommonData->ActiveCameraJitterOffsetY = 0;
            }
            // Jitter current camera proj matrix if it is needed
            if (isActiveCamera &&
                (renderModule->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_JITTER) ||
                    renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_JITTER)))
            {
                ++_jitterFrameIndex;

                float jitterX = HaltonSequence(_jitterFrameIndex, 2) - 0.5f;
                float jitterY = HaltonSequence(_jitterFrameIndex, 3) - 0.5f;
                float inputWidth = static_cast<float>(pipelineCommonData->DownscaledWidth);
                float inputHeight = static_cast<float>(pipelineCommonData->DownscaledHeight);
                // RH projection has w=-viewZ: these signs give a +jitter pixel displacement.
                proj._31 -= jitterX * 2.0f / inputWidth;
                proj._32 += jitterY * 2.0f / inputHeight;
                pipelineCommonData->ActiveCameraJitterOffsetX = jitterX;
                pipelineCommonData->ActiveCameraJitterOffsetY = jitterY;
            }

            camera.ViewProj = view * proj;
            if (isActiveCamera)
            {
                pipelineCommonData->CameraView = view;
                pipelineCommonData->CameraProjection = proj;
            }

            XMStoreFloat4x4(&objConstants.ViewProj, XMMatrixTranspose(view * proj));
            XMStoreFloat4x4(&objConstants.View, XMMatrixTranspose(view));
            XMStoreFloat4x4(&objConstants.InvViewProj, XMMatrixTranspose((view * proj).Invert()));
            objConstants.CameraLocation = transform.Location;
            objConstants.NearPlane = camera.NearPlane;
            objConstants.FarPlane = camera.FarPlane;

            if (renderModule->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS))
            {
                auto& CBuffer = renderModule->GetCurrentPrimaryFrameConstants()->CameraCB;
                CBuffer->CopyData(camera._CBufferIndex, objConstants);
            }

            if (renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS))
            {
                auto& CBuffer = renderModule->GetCurrentSecondaryFrameConstants()->CameraCB;
                CBuffer->CopyData(camera._CBufferIndex, objConstants);
            }

            camera._numFramesDirty--;
        }
    }

    static float HaltonSequence(uint32_t index, uint32_t base)
    {
        float f = 1.0f;
        float result = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index = static_cast<uint32_t>(floorf(static_cast<float>(index) / static_cast<float>(base)));
        }
        return result;
    }
    RenderModule* _render = nullptr;
    int _numFrames = 3;
    uint32_t _jitterFrameIndex = 0;
};
