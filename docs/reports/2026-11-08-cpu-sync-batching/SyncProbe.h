#pragma once
#include "App.Base/Systems/DynamicBodyUpdateSystem.h"
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
namespace SyncProbe {
inline unsigned Tick=0;
inline void Run(DynamicBodyUpdateSystem& sync,JobSystem& jobs,World& world,float step){
    if(++Tick%60){sync.Schedule(jobs,world,step).Complete();return;}
    auto& bodies=world.GetECS().GetPool<RigidBodyComponent>().GetDense();
    auto& transforms=world.GetECS().GetPool<TransformComponent>().GetDense();
    const auto originalBodies=bodies;
    const auto originalTransforms=transforms;
    std::vector<TransformComponent> reference;
    std::array<double,3> times{};
    const size_t batches[]={64,128,256};
    unsigned changed=0;
    for(unsigned iteration=0;iteration<3;++iteration){
        bodies=originalBodies;transforms=originalTransforms;
        const unsigned variant=(iteration+Tick/60)%3;
        const auto start=std::chrono::steady_clock::now();
        sync.Schedule(jobs,world,step,batches[variant]).Complete();
        times[variant]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(iteration==0){reference=transforms;for(size_t i=0;i<transforms.size();++i)if(transforms[i].Location!=originalTransforms[i].Location||transforms[i].Rotation!=originalTransforms[i].Rotation)++changed;}
        else for(size_t i=0;i<transforms.size();++i)
            if(transforms[i].Location!=reference[i].Location||transforms[i].Rotation!=reference[i].Rotation||transforms[i].Scale!=reference[i].Scale||transforms[i].DirtyFlag!=reference[i].DirtyFlag)
                throw std::runtime_error("Batch sizes published different transforms");
    }
    static std::ofstream csv("D:/Project/PEPEngine2/Build/CpuSyncOptimize-20261108/moving-probe.csv");
    if(csv.tellp()==0)csv<<"tick,changed_transforms,bodies,first_variant,batch64_ms,batch128_ms,batch256_ms\n";
    csv<<Tick<<','<<changed<<','<<bodies.size()<<','<<Tick/60%3<<','<<times[0]<<','<<times[1]<<','<<times[2]<<'\n';
}
}
