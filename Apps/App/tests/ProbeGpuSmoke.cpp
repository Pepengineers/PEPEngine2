#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.Core/ConsoleModule.h"
#include "App.Base/ECS/World.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <new>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <string>

namespace
{
    void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    std::array<float, 4> ReadCenter(GDX12Device& device, GDX12Texture& texture, UINT face, UINT mip = 0)
    {
        const auto desc = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        const UINT subresource = face * desc.MipLevels + mip;
        device.GetDevice()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), subresource), target(readback.Get(), footprint);
        commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped;
        const D3D12_RANGE read = { 0, static_cast<SIZE_T>(bytes) };
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped) + footprint.Offset +
            (footprint.Footprint.Height / 2) * footprint.Footprint.RowPitch + (footprint.Footprint.Width / 2) * 8);
        std::array<float, 4> result;
        for (UINT i = 0; i < 4; ++i) { result[i] = PackedVector::XMConvertHalfToFloat(pixel[i]); Require(std::isfinite(result[i]), "Probe mip contains non-finite radiance"); }
        const D3D12_RANGE noWrite = {}; readback->Unmap(0, &noWrite);
        return result;
    }
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        alignas(World) unsigned char worldStorage[sizeof(World)];
        auto* firstWorld = new (worldStorage) World(WorldDesc{});
        const uint64_t firstWorldId = firstWorld->GetRuntimeId();
        firstWorld->~World();
        auto* replacementWorld = new (worldStorage) World(WorldDesc{});
        const uint64_t replacementWorldId = replacementWorld->GetRuntimeId();
        replacementWorld->~World();
        Require(firstWorldId != replacementWorldId, "World reconstruction at the same address must create a new runtime identity");
        ConsoleModule console;
        const auto set = [&](const wchar_t* name, const char* value) { Require(console.FindConsoleVariable(name).SetFromString(value), "Invalid probe test CVar"); };
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        auto adapter=GDX12DeviceFactory::GetMostPerformantAdapter();
        GDX12Device device; ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info)); info->ClearStoredMessages();
        RenderPipelineCommonData common;
        GDX12EnvironmentIBLPass environment; environment.Setup(&resources, nullptr, &common); environment.Initialize();
        environment.Submit({0,{{1,{}}},{}}); environment.PrepareFrame();
        GDX12LocalShadowPass local; local.Setup(&resources, nullptr, &common); local.Initialize();
        GDX12ReflectionProbePass probes; probes.Setup(&resources, nullptr, &common);
        probes.SetInputs({ environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2), environment.GetOutput(3), local.GetOutput(0) }); probes.Initialize();
        set(L"Render.Reflection.Resolution", "32"); set(L"Render.Reflection.FacesPerFrame", "6");
        SubMesh quad;
        for (const auto p : { Vector3(-8,-8,10), Vector3(-8,8,10), Vector3(8,8,10), Vector3(8,-8,10) })
        { Vertex v{}; v.Position=p; v.Normal=Vector3(0,0,-1); v.Tangent=Vector3::UnitX; quad.Vertices.push_back(v); }
        quad.Indices = { 0,1,2,0,2,3 };
        BoundingBox bounds(XMFLOAT3(0,0,10), XMFLOAT3(8,8,0.1f));
        Mesh mesh({std::move(quad)},bounds); resources.GeometryBuffer->AddMesh(&mesh, Engine::Core::MeshHandle(0));
        std::array<GDX12Texture,2> colors;
        for (UINT i=0;i<2;++i)
        {
            GDX12TextureDesc desc; desc.Width=desc.Height=1;
            desc.Format=desc.SRVDesc.Format=desc.RTVDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get(); desc.SRVHeapIndex=desc.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex,Texture2D_RangeLength);
            desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.SRVDesc.Texture2D.MipLevels=1;
            desc.CreateRTV=true; desc.RTVHeap=resources.RTVHeap.get(); desc.RTVHeapIndex=desc.RTVHeap->GetAvailableIndex(); desc.RTVDesc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            desc.ClearValue=i==0?XMFLOAT4(4,0,0,1):XMFLOAT4(0,3,0,1); colors[i].Initialize(desc);
        }
        resources.IndirectCommandsCache->Resize(2);
        for (UINT i=0;i<2;++i) { GDX12IndirectDrawArgs draw{}; draw.InstanceID=draw.StartInstanceLocation=i; draw.IndexCountPerInstance=6; draw.InstanceCount=1; resources.IndirectCommandsCache->CopyData(i,draw); }
        for (auto& frame : resources.FrameConstants)
        {
            frame->TransformCache->Resize(2); frame->MaterialCache->Resize(2); frame->InstanceCache->Resize(2);
            for (UINT i=0;i<2;++i)
            {
                GDX12MaterialConstants material; material.DiffuseIndex=colors[i].GetSRV()->HeapIndex-Texture2D_StartIndex; material.Flags=16;
                frame->MaterialCache->CopyData(i,material);
            }
        }
        resources.Lighting.SunShadowFlags=0;
        GDX12ReflectionProbeDesc probe; probe.Entity=7; probe.Position=Vector3::Zero; probe.Extents=Vector3(20); probe.NearPlane=.1f; probe.FarPlane=100; probe.Mode=GDX12ReflectionProbeMode::BakedStatic;
        UINT frameIndex=0;
        uint64_t worldId=firstWorldId;
        bool shadowFixture=false;
        GDX12GPUTimer* captureTimer=nullptr;
        const auto render = [&](float dynamicZ, uint64_t staticRevision=1, uint64_t shadeRevision=1)
        {
            resources.CurrFrameConstantsIndex=frameIndex++%resources.FrameConstants.size();
            auto& frame=resources.FrameConstants[resources.CurrFrameConstantsIndex];
            for(UINT i=0;i<2;++i)
            {
                GDX12TransformConstants transform; Matrix world=i?Matrix::CreateTranslation(0,0,dynamicZ):Matrix::Identity;
                if(i && shadowFixture) world=Matrix::CreateScale(.25f)*Matrix::CreateTranslation(6,0,2.5f);
                transform.WorldMatrix=transform.PrevWorldMatrix=world.Transpose(); frame->TransformCache->CopyData(i,transform);
                GDX12InstanceData instance{}; instance.TransformIndex=instance.MaterialIndex=i; instance.Flags=i?1:0; instance.BoundingBoxCenter={0,0,10+(i?dynamicZ:0)}; instance.BoundingBoxExtents={8,8,.1f}; frame->InstanceCache->CopyData(i,instance);
                if(i && shadowFixture) { instance.BoundingBoxCenter={6,0,5}; instance.BoundingBoxExtents={2,2,.025f}; frame->InstanceCache->CopyData(i,instance); }
            }
            frame->MainCB->CopyData(0,resources.Lighting);
            environment.PrepareFrame();
            probes.Submit(worldId,{probe},staticRevision,shadeRevision); probes.PrepareFrame();
            auto* queue=device.GetCommandQueue(); auto* list=queue->GetCommandList();
            environment.Execute(list); local.Execute(list);
            for(auto& color:colors) { list->ResourceBarrier({color.GetResource()->GetRenderTargetBarrier()}); list->ClearRenderTargetView(&color); list->ResourceBarrier({color.GetResource()->GetSRVBarrier()}); }
            if(captureTimer) captureTimer->Start(list);
            probes.Execute(list);
            if(captureTimer) captureTimer->Stop(list);
            queue->ExecuteCommandList(list);
            if(captureTimer) captureTimer->OnCommandListExecuted(list->FenceValue);
            queue->Flush();
        };
        render(10);
        Require(probes.GetPublished().size()==1,"Completed six-face probe must publish");
        auto pixel=ReadCenter(device,*probes.GetPublished()[0].Texture,4);
        Require(pixel[0]>3.9f && pixel[1]<.01f,"Cached static depth must occlude the dynamic plane behind it");
        const auto baked=probes.GetLastFrameStaticFaces(); Require(baked==6,"First bake must rasterize six static faces");
        render(-5);
        Require(probes.GetLastFrameStaticFaces()==0,"Unchanged static scene must reuse cached static faces");
        pixel=ReadCenter(device,*probes.GetPublished()[0].Texture,4);
        Require(pixel[1]>2.9f && pixel[0]<.01f,"A dynamic plane in front of cached depth must be visible");
        render(-5,2); Require(probes.GetLastFrameStaticFaces()==6,"Static scene edits must invalidate all cached faces");
        ++common.EnvironmentRevision; render(-5,2); Require(probes.GetLastFrameStaticFaces()==6,"A new sky/IBL source revision must invalidate cached shading");
        for(UINT face=0;face<6;++face) ReadCenter(device,*probes.GetPublished()[0].Texture,face,probes.GetPublished()[0].MipCount-1);
        const auto publication=probes.GetPublished()[0].Revision;
        set(L"Render.Reflection.FacesPerFrame","1"); ++probe.Revision; probe.Position.x=1;
        for(UINT i=0;i<5;++i) { render(-5); Require(probes.GetPublished()[0].Revision==publication,"Partial capture must retain its previous publication"); Require(probes.GetPublished()[0].Description.Position.x==0,"Partial capture must retain the matching published capture position"); }
        render(-5); Require(probes.GetPublished()[0].Revision>publication && probes.GetPublished()[0].Description.Position.x==1,"Six completed faces and their metadata must publish together");
        ++probe.Revision; render(-5); ++probe.Revision; render(-5);
        const auto before=probes.GetPublished()[0].Revision;
        for(UINT i=0;i<4;++i) render(-5);
        Require(probes.GetPublished()[0].Revision==before,"Invalidation must restart a partial capture generation");
        render(-5); Require(probes.GetPublished()[0].Revision>before,"Restarted capture must publish after all six new faces");
        set(L"Render.Reflection.FacesPerFrame","6"); probe.Mode=GDX12ReflectionProbeMode::Dynamic; render(10);
        Require(probes.GetLastFrameStaticFaces()==0,"Dynamic mode must not bake a static cache");
        pixel=ReadCenter(device,*probes.GetPublished()[0].Texture,4); Require(pixel[0]>3.9f,"Dynamic capture must include static scene geometry");
        for(auto& frame:resources.FrameConstants) for(UINT i=0;i<2;++i)
        {
            GDX12MaterialConstants material; material.DiffuseIndex=colors[i].GetSRV()->HeapIndex-Texture2D_StartIndex;
            material.Metallic=0; material.Roughness=1; material.SpecularColor={0,0,0}; frame->MaterialCache->CopyData(i,material);
        }
        shadowFixture=true; resources.Lighting.LightDirection={-.7682213f,0,.6401844f}; resources.Lighting.LightRadiance={10,10,10};
        render(-5); const auto unshadowed=ReadCenter(device,*probes.GetPublished()[0].Texture,4);
        resources.Lighting.SunShadowFlags=1; render(-5); const auto shadowed=ReadCenter(device,*probes.GetPublished()[0].Texture,4);
        std::cout<<"Probe own-shadow receiver red "<<unshadowed[0]<<" / "<<shadowed[0]<<'\n';
        Require(unshadowed[0]>shadowed[0]+.2f,"An off-axis caster must shadow probe receivers independently of the active camera CSM");
        worldId=replacementWorldId; render(-5); Require(probes.GetPublished()[0].World==replacementWorldId,"Reused entity IDs in a new world must not retain old probe resources");
        const auto originalSun=resources.Lighting.LightDirection;
        GDX12SkyDesc atmosphere; atmosphere.World=worldId; atmosphere.Atmospheres.push_back({19,{}}); environment.Submit(atmosphere);
        set(L"Render.Sky.Source","2"); set(L"Render.SkyAtmosphere.Quality","1"); probe.Mode=GDX12ReflectionProbeMode::BakedStatic;
        for(UINT faceBudget:{1u,2u})
        {
            set(L"Render.Reflection.FacesPerFrame",std::to_string(faceBudget).c_str()); render(-5);
            const auto identity=common.EnvironmentRevision,history=common.HistoryRevision;
            const auto previousPublication=probes.GetPublished()[0].Revision;
            for(UINT i=0;i<12;++i)
            {
                resources.Lighting.LightDirection={.2f+.01f*i,-1,.3f}; common.ActiveCameraView=Matrix::CreateTranslation(0,-100.0f*i,0); render(-5);
                Require(common.EnvironmentRevision==identity&&common.HistoryRevision==history,"Continuous sky lighting must not change source identity");
            }
            Require(probes.GetPublished()[0].Revision>previousPublication,"Continuous atmosphere changes must not starve one/two-face probe publication");
        }
        environment.Submit({0,{{1,{}}},{}}); set(L"Render.Sky.Source","1"); set(L"Render.Reflection.FacesPerFrame","6"); probe.Mode=GDX12ReflectionProbeMode::Dynamic;
        resources.Lighting.LightDirection=originalSun; common.ActiveCameraView=Matrix::Identity; render(-5);
        if(argc==2 && std::string(argv[1])=="--benchmark")
        {
            // Use the same lit two-plane fixture and own-sun-shadow policy as the strict smoke above.
            constexpr UINT warmup=4, samples=16;
            ComPtr<IDXGIAdapter3> memoryAdapter; ThrowIfFailed(adapter.As(&memoryAdapter));
            DXGI_ADAPTER_DESC3 adapterDescription{}; ThrowIfFailed(adapter->GetDesc3(&adapterDescription));
            std::wcout<<L"PROBE_GPU_ADAPTER "<<adapterDescription.Description<<L'\n';
            GDX12GPUTimer timer(&device); captureTimer=&timer;
            set(L"Render.Reflection.Enabled","true"); set(L"Render.Reflection.Resolution","-1"); set(L"Render.Reflection.FacesPerFrame","6");
            for(int implementation=0;implementation<2;++implementation)
                for(int quality=1;quality<=3;++quality)
                {
                    set(L"Render.Reflection.Implementation",std::to_string(implementation).c_str());
                    set(L"Render.Reflection.Quality",std::to_string(quality).c_str());
                    probe.Mode=GDX12ReflectionProbeMode::Inherit;
                    std::vector<double> times;
                    for(UINT frame=0;frame<warmup+samples;++frame)
                    {
                        render(-5);
                        const double ms=timer.GetTimeMS();
                        Require(std::isfinite(ms) && ms>=0,"Probe benchmark timestamp did not resolve");
                        Require(probes.GetPublished().size()==1,"Benchmark probe did not publish all six faces");
                        if(implementation==1 && frame>=warmup) Require(probes.GetLastFrameStaticFaces()==0,"Steady baked benchmark must reuse the static cache");
                        if(frame>=warmup) times.push_back(ms);
                    }
                    // Readback/finite-radiance validation is deliberately outside capture timestamps.
                    for(UINT face=0;face<6;++face) ReadCenter(device,*probes.GetPublished()[0].Texture,face);
                    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
                    ThrowIfFailed(memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory));
                    const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
                    std::sort(times.begin(),times.end());
                    const double median=(times[times.size()/2-1]+times[times.size()/2])*.5;
                    std::cout<<std::setprecision(9)<<"PROBE_GPU_BENCHMARK {\"implementation\":"<<implementation<<",\"quality\":"<<quality
                        <<",\"faceResolution\":"<<(32u<<quality)<<",\"facesPerFrame\":6,\"warmupFrames\":"<<warmup<<",\"sampleCount\":"<<samples
                        <<",\"ownSunShadow\":true,\"fixture\":\"two planes, fixed geometry/light; baked static cache steady\""
                        <<",\"timing\":\"GPU timestamps around probes.Execute only; D3D12 debug layer enabled; flush each frame\""
                        <<",\"gpuMeanMs\":"<<mean<<",\"gpuMedianMs\":"<<median<<",\"gpuMinMs\":"<<times.front()<<",\"gpuMaxMs\":"<<times.back()
                        <<",\"memoryScope\":\"process local usage, includes fixture inputs and live outputs, not isolated capture allocation\""
                        <<",\"processLocalCurrentUsageBytes\":"<<memory.CurrentUsage<<",\"processLocalBudgetBytes\":"<<memory.Budget<<"}\n";
                }
            captureTimer=nullptr;
        }
        set(L"Render.Reflection.Quality","0"); render(-5); Require(probes.GetPublished().empty(),"Reflection Off must expose no active probes");
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        { SIZE_T bytes; info->GetMessage(i,nullptr,&bytes); std::vector<unsigned char> storage(bytes); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,message,&bytes)); if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription); }
        probes.ClearDenendencies(); local.ClearDenendencies(); environment.ClearDenendencies(); GDX12ShaderCompiler::Shutdown();
        std::cout<<"Probe GPU: color/depth cache, dynamic overlay, atomic publication, invalidation, mips and frame slots passed.\n";
        return 0;
    }
    catch(const DxException& error) {std::wcerr<<error.ToString()<<'\n';}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';}
    return 1;
}
