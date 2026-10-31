#pragma once
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/System.h"
#include <cmath>

class ReflectionProbeUpdateSystem final : public System
{
public:
    void Tick(World& world,float) override
    {
        auto render=BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        auto& ecs=world.GetECS();
        if(ecs.Get<CameraComponent>(world.ActiveCamera)._CBufferIndex!=render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex) return;
        std::vector<GDX12ReflectionProbeDesc> probes;
        ecs.ForEach<TransformComponent,ReflectionProbeComponent>([&](Entity entity,TransformComponent& transform,ReflectionProbeComponent& probe)
        {
            if(!probe.Enabled) return;
            GDX12ReflectionProbeDesc desc; desc.Entity=entity; desc.Position=transform.Location;
            desc.Extents={std::abs(probe.Extents.x*transform.Scale.x),std::abs(probe.Extents.y*transform.Scale.y),std::abs(probe.Extents.z*transform.Scale.z)};
            desc.NearPlane=probe.NearPlane; desc.FarPlane=probe.FarPlane; desc.Mode=probe.Mode; desc.Revision=probe.Revision; probes.push_back(desc);
        });
        uint64_t staticRevision=1469598103934665603ull,shadingRevision=staticRevision;
        if(!probes.empty()) ecs.ForEach<TransformComponent,StaticMeshRenderComponent>([&](Entity entity,TransformComponent& transform,StaticMeshRenderComponent& mesh)
        {
            auto& hash=mesh.ReflectionDynamic?shadingRevision:staticRevision;
            Mix(hash,entity); Mix(hash,transform.Location); Mix(hash,transform.Rotation); Mix(hash,transform.Scale);
            Mix(hash,mesh.MeshHandler.GetValue()); Mix(hash,mesh.MeshHandler.GetGeneration());
            for(auto index:mesh.SubMeshIndices) Mix(hash,index);
            for(const auto* material:mesh.Materials)
            {
                Mix(hash,material->Metallic); Mix(hash,material->Roughness); Mix(hash,material->Opacity);
                Mix(hash,material->SpecularColor); Mix(hash,material->EmissiveColor); Mix(hash,material->Type);
                Mix(hash,material->HasNormalMap); Mix(hash,material->HasSpecularMap); Mix(hash,material->HasRoughnessMap); Mix(hash,material->HasEmissiveMap); Mix(hash,material->UseBakedLighting);
                for(const auto* texture:{material->Diffuse,material->Normal,material->Specular,material->RoughnessMap,material->Emissive,material->Displacement})
                { const auto* native=texture?texture->PrimaryDeviceTexture:nullptr; Mix(hash,native); }
            }
        });
        render->SubmitReflectionProbes(world.GetRuntimeId(),std::move(probes),staticRevision,shadingRevision);
    }
private:
    template<class T> static void Mix(uint64_t& hash,const T& value)
    {
        const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(size_t i=0;i<sizeof(value);++i) hash=(hash^bytes[i])*1099511628211ull;
    }
};
