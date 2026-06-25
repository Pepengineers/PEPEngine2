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
                if (camera.DirtyFlag || transform.DirtyFlag)
                {
                    camera.DirtyFlag = false;
                    camera._numFramesDirty = numFrames;
                }

                if (camera._numFramesDirty > 0)
                {
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

                    GDX12CameraConstants objConstants;
                    XMStoreFloat4x4(&objConstants.ViewProj, XMMatrixTranspose(view * proj));
                    XMStoreFloat4x4(&objConstants.View, XMMatrixTranspose(view));
                    objConstants.CameraLocation = transform.Location;
                    objConstants.NearPlane = camera.NearPlane;
                    objConstants.FarPlane = camera.FarPlane;

                    auto& CBuffer = renderModule->GetCurrentPrimaryFrameConstants()->CameraCB;
                    CBuffer->CopyData(camera._CBufferIndex, objConstants);

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

                    auto& CBuffer = renderModule->GetCurrentPrimaryFrameConstants()->TransformCache;
                    CBuffer->CopyData(GPUData.CBufferIndex, objConstants);

                    GPUData.NumFramesDirty--;
				}
            });

        ecs.ForEach<TransformComponent, StaticMeshRenderComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform, StaticMeshRenderComponent& renderer)
            {
                auto& instanceCache = renderModule->GetCurrentPrimaryFrameConstants()->InstanceCache;
                auto indirectCommandsCache = renderModule->GetPrimaryIndirectCommandsCache();
                auto gpuMesh = renderModule->GetPrimaryGPUMesh(renderer.MeshHandler);
                auto& transformGPUData = renderModule->GetTransformGPUData(entity);

                if (renderer.DirtyFlag || transform.DirtyFlag)
                {
                    renderer.DirtyFlag = false;
                    renderer._numFramesDirty = numFrames;

                    gpuMesh->CPUMesh->GetBounds().Transform(renderer.Bounds, XMLoadFloat4x4(&transformGPUData.World));

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

                if (renderer._numFramesDirty > 0)
                {
                    for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                    {
                        const auto& subMesh = gpuMesh->SubMeshes[i];
                        const auto& subMeshBounds = gpuMesh->CPUMesh->GetSubMesh(i).Bounds;

                        BoundingBox bounds;
                        subMeshBounds.Transform(bounds, XMLoadFloat4x4(&transformGPUData.World));

                        GDX12InstanceData instanceData;
                        instanceData.TransformIndex = transformGPUData.CBufferIndex;
                        instanceData.MaterialIndex = renderer.Materials[subMesh.MaterialIndex]->_PrimaryCBufferIndex;
                        instanceData.BoundingBoxCenter = bounds.Center;
                        instanceData.BoundingBoxExtents = bounds.Extents;

                        instanceCache->CopyData(renderer._CBufferIndices[i], instanceData);
                    }
                    renderer._numFramesDirty--;
                }
            });

    }
};
