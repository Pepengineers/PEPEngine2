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
                        Matrix::CreateRotationX(transform.Rotation.x) * 
                        Matrix::CreateRotationY(transform.Rotation.y) *
                        Matrix::CreateRotationZ(transform.Rotation.z) * 
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