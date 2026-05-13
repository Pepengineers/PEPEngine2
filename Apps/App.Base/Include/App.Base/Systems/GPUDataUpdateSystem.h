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
                    float FOV = camera.FOV;

                    Matrix rotMatrix = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
                        XMConvertToRadians(transform.Rotation.x), XMConvertToRadians(transform.Rotation.z));

                    Vector3 forward = Vector3::Transform(Vector3::Forward, rotMatrix);
                    Vector3 up = Vector3::Transform(Vector3::Up, rotMatrix);
                    Vector3 cameraTarget = CameraLocation + forward;

                    Matrix view = Matrix::CreateLookAt(CameraLocation, cameraTarget, up);
                    Matrix proj = Matrix::CreatePerspectiveFieldOfView(camera.FOV, 
                        renderModule->GetAspectRatio(), camera.NearPlane, camera.FarPlane);

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
                        Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
                            XMConvertToRadians(transform.Rotation.x), XMConvertToRadians(transform.Rotation.z)) *
                        Matrix::CreateTranslation(transform.Location);

					GDX12TransformConstants objConstants;
					XMStoreFloat4x4(&objConstants.WorldMatrix, XMMatrixTranspose(world));

                    auto& CBuffer = renderModule->GetCurrentFrameConstants()->TransformCB;
                    CBuffer->CopyData(transform._CBufferIndex, objConstants);

                    transform._numFramesDirty--;
				}
            });

    }
};