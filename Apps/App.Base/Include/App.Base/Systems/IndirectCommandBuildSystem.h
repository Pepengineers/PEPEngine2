#pragma once
#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"

class IndirectCommandBuildSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        Prepare(world, dt);
        TickRange(world, dt, 0, GetWorkCount(world));
    }
protected:
    void Prepare(World&, float) override { _render = BenchmarkEngine::GetLocator().GetModule<RenderModule>().get(); }
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<StaticMeshRenderComponent>().Size(); }
    void TickRange(World& world, float, size_t begin, size_t end) override
    {
        world.GetECS().ForEachRange<StaticMeshRenderComponent>(begin, end,
            [&](Entity, StaticMeshRenderComponent& renderer)
            {
                if (_render->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    Build(_render->GetPrimaryGPUMesh(renderer.MeshHandler), _render->GetPrimaryIndirectCommandsCache(), renderer);
                if (_render->SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES))
                    Build(_render->GetSecondaryGPUMesh(renderer.MeshHandler), _render->GetSecondaryIndirectCommandsCache(), renderer);
            });
    }
private:
    static void Build(const GPUMesh* mesh, GDX12UploadBuffer<GDX12IndirectDrawArgs>* buffer,
        const StaticMeshRenderComponent& renderer)
    {
        for (size_t i = 0; i < renderer.GetSubMeshCount(mesh->SubMeshes.size()); ++i)
        {
            const auto& part = mesh->SubMeshes.at(renderer.GetSubMeshIndex(i));
            GDX12IndirectDrawArgs draw = {};
            draw.InstanceID = draw.StartInstanceLocation = renderer._CBufferIndices.at(i);
            draw.IndexCountPerInstance = part.IndexCount;
            draw.InstanceCount = renderer.Visible ? 1 : 0;
            draw.StartIndexLocation = part.StartIndexLocation;
            draw.BaseVertexLocation = part.StartVertexLocation;
            buffer->CopyData(draw.InstanceID, draw);
        }
    }
    RenderModule* _render = nullptr;
};
