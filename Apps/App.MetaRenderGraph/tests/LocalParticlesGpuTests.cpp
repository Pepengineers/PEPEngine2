#include "Graph/MetaGraphExecutor.h"
#include "Techniques/NativeWork.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
namespace {
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void Set(const wchar_t* name,const char* value) { Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value),"Particle fixture CVar write failed"); }
static std::vector<float> Read(GDX12Device& device, GDX12Texture& texture, UINT channel = 0)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands); queue->Flush();
    void* mapped = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool rgba32 = desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    const bool halfScalar = desc.Format == DXGI_FORMAT_R16_FLOAT;
    const UINT channels = hdr && channel != 3 ? 3 : 1;
    std::vector<float> result;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* pixel = static_cast<const unsigned char*>(mapped) + footprint.Offset +
                y * footprint.Footprint.RowPitch + x * (hdr ? 8 : rgba32 ? 16 : halfScalar ? 2 : 4);
            for (UINT c = 0; c < channels; ++c)
            {
                const float value = halfScalar ? PackedVector::XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(pixel)) : hdr ? PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(pixel)[channel == 3 ? 3 : c]) : reinterpret_cast<const float*>(pixel)[channel];
                Require(std::isfinite(value), "Nonfinite GPU output");
                result.push_back(value);
            }
        }
    static UINT sample=0;
    const auto path=Engine::Core::GetExecutableDirectory()/"Logs/local-particles";
    std::filesystem::create_directories(path);
    std::ofstream raw(path/(std::to_string(sample)+".bin"),std::ios::binary);
    raw.write(static_cast<const char*>(mapped),static_cast<std::streamsize>(bytes));
    Require(bool(raw),"Cannot save particle readback");
    std::ofstream metadata(path/(std::to_string(sample++)+".txt"));
    metadata << "width=" << desc.Width << " height=" << desc.Height << " format=" << desc.Format
        << " row_pitch=" << footprint.Footprint.RowPitch << " channel=" << channel << '\n';
    const D3D12_RANGE noWrite = {};
    readback->Unmap(0, &noWrite);
    return result;
}

#include "../../App/tests/ParticleSimulationControlsChecks.h"
}

void CaptureTechniqueBuffers(GDX12Device&,const Meta::Schedule&,const std::vector<ID3D12Resource*>&,
    const std::vector<std::pair<std::string,GDX12Resource*>>&,const std::string&);
void RunLocalParticlesGpuTests(GDX12Device& device,bool interactionOnly)
{
    const wchar_t* controls[]={L"Render.Particles.Enabled",L"Render.Particles.DrawEnabled",L"Render.Particles.SimulationEnabled"};
    struct Restore {
        std::vector<std::pair<IConsoleVariable*,bool>> Values;
        ~Restore() { for(auto [variable,value]:Values) variable->SetFromString(value?"true":"false"); }
    } restore;
    for(auto name:controls) { auto& cvar=ConsoleModule{}.FindConsoleVariable(name); restore.Values.emplace_back(&cvar,cvar.GetBool()); Set(name,"true"); }
    GDX12DeviceResources resources; resources.Initialize(&device);
    RenderPipelineCommonData common;
    common.WindowWidth=common.DownscaledWidth=129; common.WindowHeight=common.DownscaledHeight=97;
    common.ActiveCameraCBufferIndex=0;
    const Vector3 eye(0,30,-150);
    Vector3 forward(0,-10,150); forward.Normalize();
    const Matrix view=Matrix::CreateLookAt(eye,eye+forward,Vector3::UnitY);
    Matrix projection=Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2,129.f/97,1,10000);
    projection._33=1.f/9999; projection._43=10000.f/9999;
    const Matrix vp=view*projection; common.CameraView=view;
    for(auto& frame:resources.FrameConstants) {
        frame->CameraCB->Resize(1);
        GDX12CameraConstants camera{};
        XMStoreFloat4x4(&camera.View,view.Transpose()); XMStoreFloat4x4(&camera.ViewProj,vp.Transpose());
        XMStoreFloat4x4(&camera.InvViewProj,vp.Invert().Transpose());
        camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=camera.ViewProj;
        camera.CameraLocation=eye; camera.NearPlane=1; camera.FarPlane=10000;
        frame->CameraCB->CopyData(0,camera);
    }
    GDX12OpaquePass opaque; opaque.Setup(&resources,&common); opaque.Initialize();
    auto* depth=opaque.GetOutput(2)->GetTexture();
    GDX12Texture accum,reveal,hdr;
    GDX12RenderHelpers::InitializeRenderTarget(accum,resources,129,97,DXGI_FORMAT_R16G16B16A16_FLOAT,{0,0,0,0});
    GDX12RenderHelpers::InitializeRenderTarget(reveal,resources,129,97,DXGI_FORMAT_R16_FLOAT,{1,1,1,1});
    GDX12RenderHelpers::InitializeRenderTarget(hdr,resources,129,97,DXGI_FORMAT_R16G16B16A16_FLOAT,{0,0,0,0});
    auto* initialize=device.GetCommandQueue()->GetCommandList();
    initialize->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
    initialize->GetCommandList()->ClearDepthStencilView(depth->GetDSV()->CPUHandle,D3D12_CLEAR_FLAG_DEPTH,(10000.f/150-1)/9999,0,0,nullptr);
    device.GetCommandQueue()->ExecuteCommandList(initialize); device.GetCommandQueue()->Flush();
    resources.ParticleSimulation=std::make_unique<GDX12ParticleSimulation>();
    auto& simulation=*resources.ParticleSimulation;
    GDX12ParticlesPass particles; particles.Setup(&resources,&common); particles.SetLocalExecution(true);
    particles.SetInputs({depth,&accum,&reveal}); particles.Initialize();
    Meta::Executor executor(device,1,128);
    UINT frame=0; bool asyncPlacement=true;
    GDX12UploadBuffer<XMFLOAT4> fieldUpload(&device,16,EBufferType::Upload,false),field(&device,16,EBufferType::Default,false);
    if(interactionOnly) {
        field.CreateSRV(resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength));
        for(UINT i=0;i<16;++i) fieldUpload.CopyData(i,XMFLOAT4(.25f,.1f,.75f,30));
        simulation.SetInteractionField({&field.GetResource(),field.GetSRV()->GPUHandle,{0,0,100,100},4,-10});
    }
    const auto root=Engine::Core::GetExecutableDirectory()/"Logs/local-particles";
    std::filesystem::create_directories(root);
    const auto execute=[&](GDX12ParticlesPass& pass) {
        Require(&pass==&particles,"Unexpected particle pass");
        executor.Begin(0);
        simulation.BeginFrame(&resources,common,true); particles.BeginFrame();
        Meta::Graph graph;
        std::vector<ID3D12Resource*> bindings;
        std::vector<GDX12Resource*> wrappers;
        std::vector<std::function<void(GDX12CommandList*)>> callbacks;
        const auto add=[&](const char* name,std::vector<GDX12Resource*> inputs,Meta::Queue queue,std::function<void(GDX12CommandList*)> callback) {
            std::vector<Meta::Access> accesses;
            for(auto* resource:inputs) {
                if(!resource) continue;
                auto* native=resource->D3DResource.Get();
                const auto id=Meta::NativeWork::ResourceFor(graph,bindings,native,"particle fixture resource");
                if(std::any_of(accesses.begin(),accesses.end(),[&](auto a){return a.Resource==id;})) continue;
                if(std::find(wrappers.begin(),wrappers.end(),resource)==wrappers.end()) wrappers.push_back(resource);
                const auto desc=native->GetDesc();
                if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER) graph.Resources[id].Initial=graph.Resources[id].Final=resource->GetCurrentState();
                const bool upload=graph.Resources[id].Memory==Meta::Heap::Upload;
                auto state=upload || desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                if(queue==Meta::Queue::Compute && !upload) state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                const bool fieldRead=interactionOnly && resource==&field.GetResource() && std::string(name)!="produce interaction field";
                accesses.push_back({id,upload || fieldRead?Meta::Mode::Read:Meta::Mode::ReadWrite,Meta::State(state)});
            }
            graph.Operations.push_back({name,accesses,{},false,1,true,queue}); callbacks.push_back(std::move(callback));
        };
        add("one shared accumulation clear",{accum.GetResource(),reveal.GetResource(),hdr.GetResource()},Meta::Queue::Direct,[&](auto* cmd) {
            cmd->ResourceBarrier({accum.GetResource()->GetRenderTargetBarrier(),reveal.GetResource()->GetRenderTargetBarrier(),hdr.GetResource()->GetRenderTargetBarrier()});
            cmd->ClearRenderTargetView(&accum); cmd->ClearRenderTargetView(&reveal); cmd->ClearRenderTargetView(&hdr);
        });
        if(interactionOnly) add("produce interaction field",{&fieldUpload.GetResource(),&field.GetResource()},Meta::Queue::Direct,[&](auto* cmd) {
            cmd->ResourceBarrier({field.GetResource().GetCopyDestBarrier()});
            cmd->GetCommandList()->CopyBufferRegion(field.GetResource().D3DResource.Get(),0,fieldUpload.GetResource().D3DResource.Get(),0,16*sizeof(XMFLOAT4));
        });
        std::array<std::vector<uint64_t>,2> groups;
        for(const auto& emitter:resources.ParticleEmitters) groups[emitter.DepthCollision?1:0].push_back(emitter.Id);
        auto* camera=&resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraCB->GetResource();
        for(size_t group=0;group<2;++group) {
            const auto ids=groups[group];
            auto inputs=simulation.GetPoolResources(&ids);
            const auto constants=simulation.GetConstantResources(resources.CurrFrameConstantsIndex,&ids);
            inputs.insert(inputs.end(),constants.begin(),constants.end()); inputs.push_back(camera);
            if(group) inputs.push_back(depth->GetResource());
            if(interactionOnly) inputs.push_back(&field.GetResource());
            const auto queue=(group || interactionOnly) && asyncPlacement?Meta::Queue::Compute:Meta::Queue::Direct;
            add("initialize local particle group",inputs,queue,[&,ids](auto* cmd){simulation.InitializePools(cmd,&resources,&ids);});
            add("simulate local particle group",inputs,queue,[&,ids](auto* cmd){simulation.Execute(cmd,&resources,common,depth,&ids);});
        }
        for(size_t group=0;group<2;++group) {
            const auto ids=groups[group];
            auto inputs=particles.NativeResources();
            const auto pools=simulation.GetPoolResources(&ids); inputs.insert(inputs.end(),pools.begin(),pools.end()); inputs.push_back(camera);
            add(group?"append B and global SortedAlpha":"clear/cull A before B",inputs,Meta::Queue::Direct,
                [&,ids,group](auto* cmd){particles.PrepareRenderGroup(cmd,ids,group==0,group==1);});
        }
        auto draws=particles.NativeResources(); const auto pools=simulation.GetPoolResources(); draws.insert(draws.end(),pools.begin(),pools.end());
        draws.insert(draws.end(),{camera,depth->GetResource(),accum.GetResource(),reveal.GetResource(),hdr.GetResource()});
        add("shared weighted/motion and global sorted/additive",draws,Meta::Queue::Direct,[&](auto* cmd){particles.DrawWeightedAndMotion(cmd);particles.Composite(cmd,&hdr);});
        const auto schedule=Meta::Compile(graph);
        std::ofstream plan(root/("frame-"+std::to_string(frame)+"-schedule.txt")); schedule.Write(plan);
        executor.Execute(schedule,bindings,[&](Meta::Id op,uint32_t,ID3D12GraphicsCommandList* native) {
            std::vector<GDX12RecordingStates::Binding> boundary;
            for(auto a:graph.Operations[op].Accesses) boundary.emplace_back(bindings[a.Resource],D3D12_RESOURCE_STATES(a.Required));
            GDX12RecordingStates tracker(boundary); GDX12CommandList cmd(native);
            callbacks.at(op)(&cmd); Require(!cmd.HasSubmissionSignals(),"Particle fixture hid a private submission"); tracker.Finish(native);
        },nullptr,nullptr,frame%2==0);
        executor.Drain();
        for(auto* resource:wrappers) {
            const auto found=std::find(bindings.begin(),bindings.end(),resource->D3DResource.Get());
            resource->SetCurrentState(D3D12_RESOURCE_STATES(schedule.FinalStates.at(found-bindings.begin())));
        }
        if(interactionOnly) {
            const auto pool=simulation.GetPoolResources();Require(!pool.empty(),"Missing interaction pool");
            CaptureTechniqueBuffers(device,schedule,bindings,{{"particles",pool[0]}},(root/("interaction-"+std::to_string(frame))).string());
        }
        ++frame;
    };
    if(!interactionOnly) CheckParticleSimulationControls(device,resources,particles,execute,*depth,eye,forward);
    // Four depth-interleaved emitters exercise one global sort and one shared accumulation.
    if(!interactionOnly) for(auto blend:{ParticleBlend::SortedAlpha,ParticleBlend::WeightedAlpha}) {
        std::vector<std::vector<float>> reference;
        for(bool mixed:{false,true}) {
            resources.ParticleEmitters.clear(); asyncPlacement=mixed;
            const Vector4 colors[]={Vector4(1,0,0,.5f),Vector4(0,1,0,.5f),Vector4(0,0,1,.5f),Vector4(1,1,0,.5f)};
            for(UINT i=0;i<4;++i) {
                GDX12ParticleEmitter emitter; emitter.Id=500+i; emitter.ResetRevision=mixed?2:1; emitter.Seed=12345+i;
                emitter.MaxParticles=4; emitter.BurstTotal=1; emitter.EmissionRate=0; emitter.DeltaTime=0;
                emitter.World=Matrix::CreateTranslation(eye+forward*float(70+i*20));
                emitter.SpeedMin=emitter.SpeedMax=0; emitter.Acceleration=Vector3::Zero; emitter.SizeStart=emitter.SizeEnd=30;
                emitter.SizeVariation=0; emitter.ColorStart=emitter.ColorEnd=colors[i]; emitter.SoftDistance=0;
                emitter.Blend=blend; emitter.DepthCollision=i%2!=0; resources.ParticleEmitters.push_back(emitter);
            }
            execute(particles);
            std::vector<std::vector<float>> outputs{Read(device,hdr),Read(device,accum),Read(device,reveal),Read(device,*resources.ParticleMotion,3)};
            if(!mixed) reference=outputs;
            else for(size_t i=0;i<outputs.size();++i) {
                Require(outputs[i].size()==reference[i].size(),"Mixed particle output grid changed");
                for(size_t j=0;j<outputs[i].size();++j) Require(std::abs(outputs[i][j]-reference[i][j])<=.003f,"Mixed particle groups changed shared transparent output");
            }
            const size_t center=48*129+64;
            if(blend==ParticleBlend::SortedAlpha) {
                const float expected[]={.5625f,.3125f,.125f};
                for(UINT c=0;c<3;++c) Require(std::abs(outputs[0][center*3+c]-expected[c])<.01f,"Global SortedAlpha depth order is wrong");
            } else Require(std::abs(outputs[2][center]-.0625f)<.003f,"WBOIT producers cleared the shared revealage or lost a group");
        }
    }
    if(interactionOnly) {
        std::ofstream checks(root/"interaction-checks.csv");
        checks<<"compute,enabled,position_x,position_y,position_z,velocity_x,velocity_y,velocity_z,max_error\n";
        std::vector<float> reference;
        for(bool compute:{false,true}) for(bool enabled:{false,true}) {
            asyncPlacement=compute;
            GDX12ParticleEmitter emitter;emitter.Id=1701;emitter.ResetRevision=100+frame;
            emitter.MaxParticles=4;emitter.BurstTotal=1;emitter.EmissionRate=0;
            emitter.SpeedMin=emitter.SpeedMax=0;emitter.Acceleration=Vector3::Zero;emitter.Drag=0;
            emitter.SizeStart=emitter.SizeEnd=10;emitter.LifetimeMin=emitter.LifetimeMax=10;
            emitter.Substeps=1;emitter.Simulation=ParticleSimulation::SemiImplicitEuler;emitter.InteractionEnabled=enabled;
            emitter.DeltaTime=0;resources.ParticleEmitters={emitter};execute(particles);
            resources.ParticleEmitters[0].DeltaTime=.1f;execute(particles);
            const auto file=root/("interaction-"+std::to_string(frame-1)+"-particles.bin");
            std::ifstream raw(file,std::ios::binary);std::vector<float> state(4*18);raw.read(reinterpret_cast<char*>(state.data()),state.size()*4);Require(bool(raw),"Missing interaction state");
            size_t alive=4;for(size_t i=0;i<4;++i) if(state[i*18+7]>0) {Require(alive==4,"Unexpected interaction birth count");alive=i;}
            Require(alive<4,"Interaction particle died");
            const float blend=enabled?float((1-std::exp(-1.))*.75*.75):0.f;
            const float velocity[]={60*blend,220*blend,24*blend};double error=0;
            for(size_t i=0;i<3;++i) {
                error=(std::max)(error,double(std::abs(state[alive*18+i]-.1f*velocity[i])));
                error=(std::max)(error,double(std::abs(state[alive*18+8+i]-velocity[i])));
            }
            Require(error<2e-4,"Interaction displacement/velocity differs from CPU oracle");
            std::vector<float> measured;for(size_t i:{size_t(0),size_t(1),size_t(2),size_t(8),size_t(9),size_t(10)}) measured.push_back(state[alive*18+i]);
            if(!compute && enabled) reference=measured;
            if(compute && enabled) for(size_t i=0;i<measured.size();++i) Require(std::abs(measured[i]-reference[i])<1e-6f,"Interaction DIRECT/COMPUTE mismatch");
            checks<<compute<<','<<enabled;for(float value:measured) checks<<','<<value;checks<<','<<error<<'\n';
        }
        Require(bool(checks),"Cannot save interaction checks");
        Log::Info("MetaRenderGraph","PASS particle interaction: CPU displacement/velocity oracle, enabled/disabled controls, declared field producer, DIRECT/COMPUTE; frames=",frame);
    }
    executor.Drain(); particles.ClearDenendencies(); opaque.ClearDenendencies();
    Log::Info("MetaRenderGraph","PASS LocalParticlesGpuTests: numeric displacement tolerance=.03cm; controls/burst/reset/death/volume/depth/box; mixed DIRECT/COMPUTE SortedAlpha/WBOIT image tolerance=.003; frames=",frame);
}

void RunParticleInteractionGpuTests(GDX12Device& device) { RunLocalParticlesGpuTests(device,true); }
