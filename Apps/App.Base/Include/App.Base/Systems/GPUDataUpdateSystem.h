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
        auto consoleModule = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();

        IConsoleVariable* ICVNumframes;
        consoleModule->TryFindConsoleVariable(L"Render.NumFrames", ICVNumframes);
        int numFrames = ICVNumframes->GetInt();

        ecs.ForEach<TransformComponent, CameraComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform, CameraComponent& camera)
            {
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
                    XMStoreFloat4x4(&camera.PrevViewProjNoJitter, XMMatrixTranspose(unjitteredViewProj));


                    pipelineCommonData->CameraViewToClip = unjitteredViewProj;
                    pipelineCommonData->ClipToCameraView = pipelineCommonData->CameraViewToClip.Invert();

                    pipelineCommonData->ClipToPrevClip = unjitteredViewProj.Invert() * camera.PrevViewProjNoJitter;
                    pipelineCommonData->PrevClipToClip = pipelineCommonData->ClipToPrevClip.Invert();

                    // Jitter current camera proj matrix if it is needed
                    if (isActiveCamera &&
                        (renderModule->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_JITTER) ||
                            renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_JITTER)))
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

                    XMStoreFloat4x4(&objConstants.ViewProj, XMMatrixTranspose(view * proj));
                    XMStoreFloat4x4(&objConstants.View, XMMatrixTranspose(view));
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
            });

        ecs.ForEach<TransformComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform)
            {
                auto& GPUData = renderModule->GetTransformGPUData(entity);
                if (transform.DirtyFlag)
                {
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

                    if (renderModule->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    {
                        auto& CBuffer = renderModule->GetCurrentPrimaryFrameConstants()->TransformCache;
                        CBuffer->CopyData(GPUData.CBufferIndex, objConstants);
                    }
                    if (renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    {
                        auto& CBuffer = renderModule->GetCurrentSecondaryFrameConstants()->TransformCache;
                        CBuffer->CopyData(GPUData.CBufferIndex, objConstants);
                    }

                    GPUData.NumFramesDirty--;
				}
            });

        ecs.ForEach<TransformComponent, StaticMeshRenderComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform, StaticMeshRenderComponent& renderer)
            {
                auto& transformGPUData = renderModule->GetTransformGPUData(entity);

                if (renderer.DirtyFlag || transform.DirtyFlag)
                {
                    renderer.DirtyFlag = false;
                    renderer._numFramesDirty = numFrames;

                    if (renderModule->GetPrimaryPipelineFlags() & RENDER_PASS_FLAG_USE_INSTANCES)
                    {
                        auto gpuMesh = renderModule->GetPrimaryGPUMesh(renderer.MeshHandler);
                        gpuMesh->CPUMesh->GetBounds().Transform(renderer.Bounds, XMLoadFloat4x4(&transformGPUData.World));
                        auto indirectCommandsCache = renderModule->GetPrimaryIndirectCommandsCache();
                        for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                        {
                            const auto& subMesh = gpuMesh->SubMeshes[i];

                            GDX12IndirectDrawArgs drawCommand;
                            drawCommand.IndexCountPerInstance = subMesh.IndexCount;
                            drawCommand.InstanceCount = 1;
                            drawCommand.StartIndexLocation = subMesh.StartIndexLocation;
                            drawCommand.BaseVertexLocation = subMesh.StartVertexLocation;
                            drawCommand.StartInstanceLocation = renderer._CBufferIndices[i];
                            drawCommand.InstanceID = renderer._CBufferIndices[i];

                            indirectCommandsCache->CopyData(renderer._CBufferIndices[i], drawCommand);
                        }
                    }

                    if (renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    {
                        auto gpuMesh = renderModule->GetSecondaryGPUMesh(renderer.MeshHandler);
                        gpuMesh->CPUMesh->GetBounds().Transform(renderer.Bounds, XMLoadFloat4x4(&transformGPUData.World));
                        auto indirectCommandsCache = renderModule->GetSecondaryIndirectCommandsCache();
                        for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                        {
                            const auto& subMesh = gpuMesh->SubMeshes[i];

                            GDX12IndirectDrawArgs drawCommand;
                            drawCommand.IndexCountPerInstance = subMesh.IndexCount;
                            drawCommand.InstanceCount = 1;
                            drawCommand.StartIndexLocation = subMesh.StartIndexLocation;
                            drawCommand.BaseVertexLocation = subMesh.StartVertexLocation;
                            drawCommand.StartInstanceLocation = renderer._CBufferIndices[i];
                            drawCommand.InstanceID = renderer._CBufferIndices[i];

                            indirectCommandsCache->CopyData(renderer._CBufferIndices[i], drawCommand);
                        }
                    }
                }

                if (renderer._numFramesDirty > 0)
                {
                    if (renderModule->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    {
                        auto& instanceCache = renderModule->GetCurrentPrimaryFrameConstants()->InstanceCache;
                        auto gpuMesh = renderModule->GetPrimaryGPUMesh(renderer.MeshHandler);
                        for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                        {
                            const auto& subMesh = gpuMesh->SubMeshes[i];
                            const auto& subMeshBounds = gpuMesh->CPUMesh->GetSubMesh(i).Bounds;

                            BoundingBox bounds;
                            subMeshBounds.Transform(bounds, XMLoadFloat4x4(&transformGPUData.World));

                            GDX12InstanceData instanceData;
                            instanceData.TransformIndex = transformGPUData.CBufferIndex;
                            instanceData.MaterialIndex = renderer.Materials[subMesh.MaterialIndex]->_CBufferIndex;
                            instanceData.BoundingBoxCenter = bounds.Center;
                            instanceData.BoundingBoxExtents = bounds.Extents;

                            instanceCache->CopyData(renderer._CBufferIndices[i], instanceData);
                        }
                    }
                    if (renderModule->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    {
                        auto& instanceCache = renderModule->GetCurrentSecondaryFrameConstants()->InstanceCache;
                        auto gpuMesh = renderModule->GetSecondaryGPUMesh(renderer.MeshHandler);
                        for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                        {
                            const auto& subMesh = gpuMesh->SubMeshes[i];
                            const auto& subMeshBounds = gpuMesh->CPUMesh->GetSubMesh(i).Bounds;

                            BoundingBox bounds;
                            subMeshBounds.Transform(bounds, XMLoadFloat4x4(&transformGPUData.World));

                            GDX12InstanceData instanceData;
                            instanceData.TransformIndex = transformGPUData.CBufferIndex;
                            instanceData.MaterialIndex = renderer.Materials[subMesh.MaterialIndex]->_CBufferIndex;
                            instanceData.BoundingBoxCenter = bounds.Center;
                            instanceData.BoundingBoxExtents = bounds.Extents;

                            instanceCache->CopyData(renderer._CBufferIndices[i], instanceData);
                        }
                    }

                    renderer._numFramesDirty--;
                }
            });

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
