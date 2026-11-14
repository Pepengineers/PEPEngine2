#include "Graph/MetaGraphExecutor.h"
#include "Techniques/NativeWork.h"
#include "Rendering/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace
{
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
#include "../../App/tests/FluidSimulationChecks.h"
}

void RunLocalWaterGpuTests(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
{
    auto& device=*resources.Device;
    auto& async=ConsoleModule{}.FindConsoleVariable(L"Render.Fluid.AsyncCompute");
    struct Restore { IConsoleVariable& Variable; bool Value; ~Restore() { Variable.SetFromString(Value?"true":"false"); } } restore{async,async.GetBool()};
    GDX12Texture depth, motion;
    GDX12RenderHelpers::InitializeRenderTarget(depth,resources,16,16,DXGI_FORMAT_R32_FLOAT,{0,0,0,0});
    GDX12RenderHelpers::InitializeRenderTarget(motion,resources,16,16,DXGI_FORMAT_R16G16_FLOAT,{0,0,0,0});
    GDX12FluidSurfacePass water;
    water.Setup(&resources,&common); water.SetLocalExecution(true); water.SetInputs({&depth,&motion}); water.Initialize();
    Meta::Executor executor(device,1,64);
    const auto root=Engine::Core::GetExecutableDirectory()/"Logs/local-water";
    std::filesystem::create_directories(root);
    UINT frame=0;
    const auto execute=[&] {
        executor.Begin(0);
        water.PrepareFrame();
        auto wrappers=water.NativeResources();
        wrappers.push_back(depth.GetResource()); wrappers.push_back(motion.GetResource());
        std::vector<GDX12Resource*> unique;
        Meta::Graph graph;
        std::vector<ID3D12Resource*> bindings;
        std::vector<Meta::Access> accesses;
        for(auto* resource:wrappers) {
            if(std::find(unique.begin(),unique.end(),resource)!=unique.end()) continue;
            unique.push_back(resource);
            auto* native=resource->D3DResource.Get();
            const auto id=Meta::NativeWork::ResourceFor(graph,bindings,native,"local water resource");
            const auto desc=native->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)
                graph.Resources[id].Initial=graph.Resources[id].Final=resource->GetCurrentState();
            const bool upload=graph.Resources[id].Memory==Meta::Heap::Upload;
            const auto state=upload || desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_RESOURCE_STATE_GENERIC_READ :
                desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            accesses.push_back({id,upload?Meta::Mode::Read:Meta::Mode::ReadWrite,Meta::State(state)});
        }
        const auto depthId=Meta::Id(std::find(bindings.begin(),bindings.end(),depth.GetResource()->D3DResource.Get())-bindings.begin());
        const auto motionId=Meta::Id(std::find(bindings.begin(),bindings.end(),motion.GetResource()->D3DResource.Get())-bindings.begin());
        graph.Operations.push_back({"initialize fixture inputs",{{depthId,Meta::Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET},
            {motionId,Meta::Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET}}});
        auto simulation=accesses;
        const bool compute=async.GetBool();
        if(compute) for(auto& a:simulation)
            if(graph.Resources[a.Resource].Memory==Meta::Heap::Default && a.Required!=D3D12_RESOURCE_STATE_COMMON)
                a.Required=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        graph.Operations.push_back({"original local water solver",simulation,{},false,1,true,
            compute?Meta::Queue::Compute:Meta::Queue::Direct});
        graph.Operations.push_back({"original water surface/depth/motion",accesses,{},false,1,true});
        std::vector<GDX12ResourceHandle> readbacks;
        std::vector<ID3D12Resource*> particleSources;
        for(const auto& access:water.GetSimulationBufferAccesses()) {
            auto* source=access.Resource->D3DResource.Get();
            const auto bytes=source->GetDesc().Width;
            if(access.Usage!=GDX12RenderGraph::Mode::ReadWrite || (bytes!=36 && bytes!=72)) continue;
            particleSources.push_back(source);
            readbacks.push_back(device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(bytes),D3D12_RESOURCE_STATE_COPY_DEST));
            const auto src=Meta::NativeWork::ResourceFor(graph,bindings,source,"water particles");
            const auto dst=Meta::NativeWork::ResourceFor(graph,bindings,readbacks.back().Get(),"water state readback");
            graph.Operations.push_back({"capture water particle state",{{src,Meta::Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
                {dst,Meta::Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}});
        }
        const auto schedule=Meta::Compile(graph);
        std::ofstream plan(root/(std::to_string(frame)+"-schedule.txt")); schedule.Write(plan);
        executor.Execute(schedule,bindings,[&](Meta::Id op,uint32_t,ID3D12GraphicsCommandList* native) {
            if(op==0) {
                const float zero[4]{};
                native->ClearRenderTargetView(depth.GetRTV()->CPUHandle,zero,0,nullptr);
                native->ClearRenderTargetView(motion.GetRTV()->CPUHandle,zero,0,nullptr);
            } else if(op<=2) {
                std::vector<GDX12RecordingStates::Binding> boundary;
                for(auto a:graph.Operations[op].Accesses) boundary.emplace_back(bindings[a.Resource],D3D12_RESOURCE_STATES(a.Required));
                GDX12RecordingStates tracker(boundary);
                GDX12CommandList commands(native);
                if(op==1) water.ExecuteSimulation(&commands); else water.Execute(&commands);
                Require(!commands.HasSubmissionSignals(),"Water fixture hid a private submission");
                tracker.Finish(native);
            } else {
                const auto index=op-3;
                native->CopyResource(readbacks.at(index).Get(),particleSources.at(index));
            }
        },nullptr,nullptr,frame%2==0);
        executor.Drain();
        for(auto* wrapper:unique) {
            const auto found=std::find(bindings.begin(),bindings.end(),wrapper->D3DResource.Get());
            wrapper->SetCurrentState(D3D12_RESOURCE_STATES(schedule.FinalStates.at(found-bindings.begin())));
        }
        for(size_t i=0;i<readbacks.size();++i) {
            void* mapped=nullptr; ThrowIfFailed(readbacks[i]->Map(0,nullptr,&mapped));
            std::ofstream data(root/(std::to_string(frame)+"-particles-"+std::to_string(i)+".bin"),std::ios::binary);
            data.write(static_cast<const char*>(mapped),readbacks[i]->GetDesc().Width);
            readbacks[i]->Unmap(0,nullptr); Require(bool(data),"Cannot save local water state");
        }
        ++frame;
    };
    CheckFluidSimulation(device,water,execute);
    executor.Drain(); water.ClearDenendencies();
    Log::Info("MetaRenderGraph","PASS LocalWaterGpuTests: original numeric oracles through Meta executor; states=",frame,
        "; DIRECT/COMPUTE switches, serial/parallel, pause/timeScale, reset, pressure/prediction/collision/waves; no secondary device");
}
