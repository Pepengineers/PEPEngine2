#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"
#include "Common/ConsoleVariables.h"

class GPUDataUpdateSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        //the system will go through each component with constant buffers in them and update them

        WorldECS& ecs = world.GetECS();
        auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        int numFrames = static_cast<int>(renderModule->GetFrameSlotCount());
        const auto primaryFlags = renderModule->GetDevicePassFlags(DEVICE_ROLE_PRIMARY);
        const auto secondaryFlags = renderModule->GetDevicePassFlags(DEVICE_ROLE_SECONDARY);
        const auto devices = {
            std::pair{DEVICE_ROLE_PRIMARY, primaryFlags},
            std::pair{DEVICE_ROLE_SECONDARY, secondaryFlags}};
        const auto pipelineFlags = primaryFlags | secondaryFlags;

        const CameraComponent* lightingCamera = nullptr;
        Matrix lightingView, lightingProjection;
        ecs.ForEach<CameraComponent>(
            [&](Entity entity, CameraComponent& camera)
            {
                const auto& transform = ecs.Has<TransformComponent>(entity)
                    ? ecs.Get<TransformComponent>(entity) : DefaultTransform;
                // Active camera requires constant update due to jittering
                auto pipelineCommonData = renderModule->GetRenderPipelineCommonData();
                bool isActiveCamera = pipelineCommonData->ActiveCameraCBufferIndex == camera._CBufferIndex;
                if (isActiveCamera) { camera.DirtyFlag = true; }

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

                    //reserved-Z proj matrix
                    Matrix proj = Matrix::CreatePerspectiveFieldOfView(FOV, renderModule->GetAspectRatio(),
                        camera.NearPlane, camera.FarPlane);
                    const float range = camera.NearPlane / (camera.FarPlane - camera.NearPlane);
                    proj._33 = range;
                    proj._43 = range * camera.FarPlane;

                    Matrix unjitteredViewProj = view * proj;

                    XMStoreFloat4x4(&objConstants.ViewProjNoJitter, XMMatrixTranspose(unjitteredViewProj));
                    objConstants.PrevViewProjNoJitter = camera.PrevViewProjNoJitter;

                    if (isActiveCamera)
                    {
                        pipelineCommonData->CameraViewToClip = proj;
                        pipelineCommonData->ClipToCameraView = proj.Invert();

                        // Streamline uses row-major matrices.
                        pipelineCommonData->ClipToPrevClip = unjitteredViewProj.Invert() * camera.PrevViewProjNoJitter.Transpose();
                        pipelineCommonData->PrevClipToClip = pipelineCommonData->ClipToPrevClip.Invert();
                    }

                    XMStoreFloat4x4(&camera.PrevViewProjNoJitter, XMMatrixTranspose(unjitteredViewProj));

                    // Jitter current camera proj matrix if it is needed
                    if (isActiveCamera && (pipelineFlags & RENDER_PASS_FLAG_USE_JITTER))
                    {
                        static uint32_t frameIndex = 0;
                        frameIndex++;

                        float jitterX = HaltonSequence(frameIndex, 2) - 0.5f;
                        float jitterY = HaltonSequence(frameIndex, 3) - 0.5f;
                        float inputWidth = static_cast<float>(pipelineCommonData->DownscaledWidth);
                        float inputHeight = static_cast<float>(pipelineCommonData->DownscaledHeight);
                        proj._31 += jitterX * 2.0f / inputWidth;
                        proj._32 -= jitterY * 2.0f / inputHeight;
                        pipelineCommonData->ActiveCameraJitterOffsetX = jitterX;
                        pipelineCommonData->ActiveCameraJitterOffsetY = jitterY;
                    }

                    camera.ViewProj = view * proj;
                    if (isActiveCamera && (pipelineFlags & RENDER_PASS_FLAG_USE_LIGHTING))
                    {
                        lightingCamera = &camera;
                        lightingView = view;
                        lightingProjection = proj;
                        XMStoreFloat4x4(&objConstants.InvViewProj, XMMatrixTranspose(camera.ViewProj.Invert()));
                    }

                    XMStoreFloat4x4(&objConstants.ViewProj, XMMatrixTranspose(view * proj));
                    XMStoreFloat4x4(&objConstants.View, XMMatrixTranspose(view));
                    objConstants.CameraLocation = transform.Location;
                    objConstants.NearPlane = camera.NearPlane;
                    objConstants.FarPlane = camera.FarPlane;

                    for (const auto& [role, flags] : devices)
                    {
                        if (!(flags & RENDER_PASS_FLAG_USE_CAMERAS)) { continue; }
                        auto& CBuffer = renderModule->GetCurrentFrameConstants(role)->CameraCB;
                        CBuffer->CopyData(camera._CBufferIndex, objConstants);
                    }

                    camera._numFramesDirty--;
                }
            });

        ecs.ForEach<TransformComponent>(
            [&ecs, &renderModule, &numFrames, &devices](Entity entity, TransformComponent& transform)
            {
                auto& GPUData = renderModule->GetTransformGPUData(entity);
                if (transform.DirtyFlag)
                {
                    if (ecs.Has<LightComponent>(entity))
                        ecs.Get<LightComponent>(entity).DirtyFlag = true;
                    if (ecs.Has<StaticMeshRenderComponent>(entity))
                        ecs.Get<StaticMeshRenderComponent>(entity).DirtyFlag = true;
                    transform.DirtyFlag = false;
                    GPUData.NumFramesDirty = numFrames;
                }

				if (GPUData.NumFramesDirty > 0)
				{
					Matrix world = Matrix::CreateScale(transform.Scale) * 
                        Matrix::CreateFromYawPitchRoll(
                            XMConvertToRadians(transform.Rotation.y),
                            XMConvertToRadians(transform.Rotation.x), 
                            XMConvertToRadians(transform.Rotation.z)) *
                        Matrix::CreateTranslation(transform.Location);

                    GPUData.World = world;

					GDX12TransformConstants objConstants;
					XMStoreFloat4x4(&objConstants.WorldMatrix, XMMatrixTranspose(world));
                    XMStoreFloat4x4(&objConstants.PrevWorldMatrix, XMMatrixTranspose(GPUData.PrevWorld));

                    XMStoreFloat4x4(&GPUData.PrevWorld, world);

                    for (const auto& [role, flags] : devices)
                    {
                        if (!(flags & RENDER_PASS_FLAG_USE_INSTANCES)) { continue; }
                        auto& CBuffer = renderModule->GetCurrentFrameConstants(role)->TransformCache;
                        CBuffer->CopyData(GPUData.CBufferIndex, objConstants);
                    }

                    GPUData.NumFramesDirty--;
				}
            });

        ecs.ForEach<StaticMeshRenderComponent>(
            [&ecs, &renderModule, &numFrames, &devices](Entity entity, StaticMeshRenderComponent& renderer)
            {
                if (renderer.DirtyFlag)
                {
                    renderer.DirtyFlag = false;
                    renderer._numFramesDirty = numFrames;
                }
                if (renderer._numFramesDirty > 0)
                {
                    const auto* transform = ecs.Has<TransformComponent>(entity)
                        ? &renderModule->GetTransformGPUData(entity) : nullptr;
                    const Matrix worldMatrix = transform ? transform->World :
                        Matrix::CreateScale(DefaultTransform.Scale) *
                        Matrix::CreateFromYawPitchRoll(XMConvertToRadians(DefaultTransform.Rotation.y),
                            XMConvertToRadians(DefaultTransform.Rotation.x), XMConvertToRadians(DefaultTransform.Rotation.z)) *
                        Matrix::CreateTranslation(DefaultTransform.Location);
                    const UINT transformIndex = transform ? transform->CBufferIndex : 0;
                    for (const auto& [role, flags] : devices)
                    {
                        if (!(flags & RENDER_PASS_FLAG_USE_INSTANCES)) { continue; }
                        auto& instanceCache = renderModule->GetCurrentFrameConstants(role)->InstanceCache;
                        auto gpuMesh = renderModule->GetGPUMesh(role, renderer._boundMeshHandler);
                        gpuMesh->CPUMesh->GetBounds().Transform(renderer.Bounds, worldMatrix);
                        for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                        {
                            const auto& subMesh = gpuMesh->SubMeshes[i];
                            const auto& subMeshBounds = gpuMesh->CPUMesh->GetSubMesh(i).Bounds;

                            BoundingBox bounds;
                            subMeshBounds.Transform(bounds, worldMatrix);

                            GDX12InstanceData instanceData;
                            instanceData.TransformIndex = transformIndex;
                            instanceData.MaterialIndex = renderer.Materials[subMesh.MaterialIndex]->_CBufferIndex;
                            instanceData.BoundingBoxCenter = bounds.Center;
                            instanceData.BoundingBoxExtents = bounds.Extents;

                            instanceCache->CopyData(renderer._CBufferIndices[i], instanceData);
                        }
                    }

                    renderer._numFramesDirty--;
                }
            });

        if (!lightingCamera) { return; }

        auto& grid = renderModule->GetLightingGrid();
        const auto& lights = ecs.GetPool<LightComponent>();
        grid.Lights.resize(lights.Size());

        grid.ActiveLightIndices.clear();
        for (const auto& [role, flags] : devices)
        {
            if (flags & RENDER_PASS_FLAG_USE_LIGHTING)
                renderModule->GetCurrentFrameConstants(role)->LightsCache->Resize(static_cast<UINT>(lights.Size()));
        }

        UINT nextLightIndex = 0;
        ecs.ForEach<LightComponent>([&](Entity entity, LightComponent& light)
        {
            const UINT index = nextLightIndex++;
            const bool hasTransform = ecs.Has<TransformComponent>(entity);
            if (light.Enabled && hasTransform) { grid.ActiveLightIndices.push_back(index); }

            auto& data = grid.Lights[index];
            // Removing a light can move the last component into its buffer slot.
            if (light.DirtyFlag || light._CBufferIndex != index)
            {
                data = {};
                data.Type = light.Type;
                data.Color = light.Color;
                data.Intensity = light.Intensity;
                data.Range = light.Range;
                const float outer = std::clamp(light.OuterAngle, 1.0f, 89.0f);
                data.OuterConeCos = std::cos(XMConvertToRadians(outer));
                data.InnerConeCos = std::cos(XMConvertToRadians(std::clamp(light.InnerAngle, 0.0f, outer)));
                if (hasTransform)
                {
                    const auto& transform = ecs.Get<TransformComponent>(entity);
                    data.Position = transform.Location;
                    const auto rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
                        XMConvertToRadians(transform.Rotation.x), XMConvertToRadians(transform.Rotation.z));
                    data.Direction = -rotation.Forward();
                }
                light._CBufferIndex = index;
                light._numFramesDirty = numFrames;
                light.DirtyFlag = false;
            }

            if (light._numFramesDirty > 0)
            {
                for (const auto& [role, flags] : devices)
                {
                    if (flags & RENDER_PASS_FLAG_USE_LIGHTING)
                        renderModule->GetCurrentFrameConstants(role)->LightsCache->CopyData(index, data);
                }
                --light._numFramesDirty;
            }
        });

        const auto* pipelineCommonData = renderModule->GetRenderPipelineCommonData();
        grid.BuildClusterLightMasks(lightingView, lightingProjection, lightingCamera->NearPlane, lightingCamera->FarPlane,
                                   pipelineCommonData->DownscaledWidth, pipelineCommonData->DownscaledHeight);
    }

private:
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
};
