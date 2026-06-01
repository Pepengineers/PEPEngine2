#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"

class RenderSubmitSystem final : public System
{
private:
    static Matrix BuildWorldMatrix(const TransformComponent& transform)
    {
        return Matrix::CreateScale(transform.Scale) *
            Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y),
                XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) *
            Matrix::CreateTranslation(transform.Location);
    }

    static BoundingFrustum BuildCameraFrustum(
        RenderModule& renderModule,
        const CameraComponent& camera,
        const TransformComponent& transform)
    {
        const Matrix rotation = Matrix::CreateFromYawPitchRoll(
            XMConvertToRadians(transform.Rotation.y),
            XMConvertToRadians(transform.Rotation.x),
            XMConvertToRadians(transform.Rotation.z));

        const Vector3 forward = rotation.Forward();
        const Matrix view = Matrix::CreateLookAt(transform.Location, transform.Location - forward, Vector3::Up);
        const Matrix projection = Matrix::CreatePerspectiveFieldOfView(
            camera.FOV,
            renderModule.GetAspectRatio(),
            camera.NearPlane,
            camera.FarPlane);

        BoundingFrustum viewFrustum = {};
        BoundingFrustum::CreateFromMatrix(viewFrustum, projection, true);

        BoundingFrustum worldFrustum = {};
        viewFrustum.Transform(worldFrustum, view.Invert());
        return worldFrustum;
    }

public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();
        auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

        CameraComponent* activeCamera = &ecs.Get<CameraComponent>(world.ActiveCamera);
        TransformComponent* activeCameraTransform = &ecs.Get<TransformComponent>(world.ActiveCamera);

        auto commandRecorder = renderModule->GetCommandRecorder();
        commandRecorder->ClearCommands();

        commandRecorder->DrawFromCamera(
            activeCamera->_CBufferIndex,
            BuildCameraFrustum(*renderModule, *activeCamera, *activeCameraTransform));

        // Since we don't have culling for now, we'll just grab all RenderComponents.
        // In the future, this should source the list of visible components from RenderCullingSystem
        ecs.ForEach<StaticMeshRenderComponent, TransformComponent>(
            [&ecs, &commandRecorder](Entity entity, StaticMeshRenderComponent& renderer, TransformComponent& transform)
            {
                commandRecorder->DrawMesh(
                    renderer.MeshHandler,
                    renderer.Materials,
                    transform._CBufferIndex,
                    RenderSubmitSystem::BuildWorldMatrix(transform));
            });
        
    }
};