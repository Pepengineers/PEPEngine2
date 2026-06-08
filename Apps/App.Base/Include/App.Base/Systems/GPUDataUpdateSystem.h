#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

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
                    objConstants.CameraLocation = transform.Location;

                    auto& CBuffer = renderModule->GetCurrentFrameConstants()->CameraCB;
                    CBuffer->CopyData(camera._CBufferIndex, objConstants);

                    camera._numFramesDirty--;
                }
            });

        ecs.ForEach<TransformComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform)
            {
                if (transform.DirtyFlag)
                {
                    transform.DirtyFlag = false;
                    transform._numFramesDirty = numFrames;
                }

				if (transform._numFramesDirty > 0)
				{
					Matrix world = Matrix::CreateScale(transform.Scale) * 
                        Matrix::CreateFromYawPitchRoll(
                            XMConvertToRadians(transform.Rotation.y),
                            XMConvertToRadians(transform.Rotation.x), 
                            XMConvertToRadians(transform.Rotation.z)) *
                        Matrix::CreateTranslation(transform.Location);

					GDX12TransformConstants objConstants;
					XMStoreFloat4x4(&objConstants.WorldMatrix, XMMatrixTranspose(world));

                    auto& CBuffer = renderModule->GetCurrentFrameConstants()->TransformCache;
                    CBuffer->CopyData(transform._CBufferIndex, objConstants);

                    transform._numFramesDirty--;
				}
            });

        ecs.ForEach<TransformComponent, StaticMeshRenderComponent>(
            [&ecs, &renderModule, &numFrames](Entity entity, TransformComponent& transform, StaticMeshRenderComponent& renderer)
            {
                if (!renderer.DirtyFlag) { return; }
                renderer.DirtyFlag = false;

                auto instanceCache = renderModule->GetInstanceCache();
                auto indirectCommandsCache = renderModule->GetIndirectCommandsCache();
                auto gpuMesh = renderModule->GetGPUMesh(renderer.MeshHandler);

                for (int i = 0; i < gpuMesh->SubMeshes.size(); i++)
                {
                    const auto& subMesh = gpuMesh->SubMeshes[i];

                    GDX12InstanceData instanceData;
                    instanceData.TransformIndex = transform._CBufferIndex;
                    instanceData.MaterialIndex = renderer.Materials[subMesh.MaterialIndex]->_CBufferIndex;
                    GDX12IndirectDrawArgs drawCommand;
                    drawCommand.IndexCountPerInstance = subMesh.IndexCount;
                    drawCommand.InstanceCount = 1;
                    drawCommand.StartIndexLocation = subMesh.StartIndexLocation;
                    drawCommand.BaseVertexLocation = subMesh.StartVertexLocation;
                    drawCommand.StartInstanceLocation = renderer._CBufferIndices[i];

                    instanceCache->CopyData(renderer._CBufferIndices[i], instanceData);
                    indirectCommandsCache->CopyData(renderer._CBufferIndices[i], drawCommand);
                }
            });

    }
};