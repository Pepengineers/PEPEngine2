#define main FidelityFXGpuSmokeMain
#include "FidelityFXGpuSmoke.cpp"
#undef main
#include "Engine.RendererDX12/RenderPasses/GDX12SkyAerialPass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include <iomanip>
#include <numeric>

namespace
{
    std::vector<float> ReadFace(GDX12Device& device,GDX12DeviceResources& resources,GDX12Texture& cube,UINT face)
    {
        GDX12Texture slice; MakeInput(resources,slice,cube.GetWidth(),cube.GetHeight(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList();
        commands->ResourceBarrier({cube.GetResource()->GetCopySourceBarrier(),slice.GetResource()->GetCopyDestBarrier()});
        const CD3DX12_TEXTURE_COPY_LOCATION src(cube.GetResource()->D3DResource.Get(),face*cube.GetResource()->D3DResource->GetDesc().MipLevels),dst(slice.GetResource()->D3DResource.Get(),0);
        commands->GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        commands->ResourceBarrier({cube.GetResource()->GetSRVBarrier(),slice.GetResource()->GetSRVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        return ReadColor(device,slice);
    }
    void CheckVolume(GDX12Device& device,GDX12Texture& texture,bool expectAttenuation=true)
    {
        const auto description=texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={}; UINT rows=0; UINT64 bytes=0;
        device.GetDevice()->GetCopyableFootprints(&description,0,1,0,&footprint,&rows,nullptr,&bytes);
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto buffer=CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> readback; ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->ResourceBarrier({texture.GetResource()->GetCopySourceBarrier()});
        const CD3DX12_TEXTURE_COPY_LOCATION src(texture.GetResource()->D3DResource.Get(),0),dst(readback.Get(),footprint); commands->GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        commands->ResourceBarrier({texture.GetResource()->GetSRVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped=nullptr; const D3D12_RANGE range={0,static_cast<SIZE_T>(bytes)}; ThrowIfFailed(readback->Map(0,&range,&mapped));
        bool attenuates=false;
        for(UINT z=0;z<footprint.Footprint.Depth;++z) for(UINT y=0;y<rows;++y)
        {
            const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped)+footprint.Offset+(static_cast<size_t>(z)*rows+y)*footprint.Footprint.RowPitch);
            for(UINT x=0;x<texture.GetWidth()*4;++x) { float v=PackedVector::XMConvertHalfToFloat(row[x]); Require(std::isfinite(v)&&v>=0&&v<=1,"RGB aerial transmittance must be finite in [0,1]"); if(x%4!=3&&v<.99f) attenuates=true; }
        }
        const D3D12_RANGE noWrite={}; readback->Unmap(0,&noWrite); if(expectAttenuation) Require(attenuates,"Aerial volume must attenuate nonzero optical depth");
    }
}

int main(int argc,char** argv)
{
    std::cout << std::unitbuf;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ConsoleModule console;
        const auto set=[&](const wchar_t* name,const char* value) { Require(console.FindConsoleVariable(name).SetFromString(value),"Invalid Sky CVar"); };
        Require(!console.FindConsoleVariable(L"Render.Sky.Source").SetFromString("3"),"Unknown sky source must be rejected");
        Require(!console.FindConsoleVariable(L"Render.SkyBox.Intensity").SetFromString("-1"),"Negative sky intensity must be rejected");
        Require(!console.FindConsoleVariable(L"Render.SkyAtmosphere.Quality").SetFromString("4"),"Unknown sky quality must be rejected");
        Require(!console.FindConsoleVariable(L"Render.SkyAtmosphere.AerialDistanceKm").SetFromString("0"),"Zero aerial range must be rejected");
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        auto adapter=GDX12DeviceFactory::GetMostPerformantAdapter();
        GDX12Device device; ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources; resources.Initialize(&device);
        RenderPipelineCommonData common; common.WindowWidth=common.DownscaledWidth=65; common.WindowHeight=common.DownscaledHeight=49;
        common.ActiveCameraCBufferIndex=0; common.ActiveCameraNearPlane=.1f; common.ActiveCameraFarPlane=1e7f;
        Matrix projection=Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f),65.0f/49,.1f,1e7f);
        projection._33=.1f/(1e7f-.1f); projection._43=1e7f*projection._33;
        common.CameraViewToClip=projection; common.ClipToCameraView=projection.Invert(); common.ActiveCameraViewProjection=common.ActiveCameraPreviousViewProjection=projection;
        GDX12EnvironmentIBLPass environment; environment.Setup(&resources,nullptr,&common); environment.Initialize();
        GDX12SkyDesc sky; sky.World=17; sky.Boxes.push_back({7,{}}); environment.Submit(sky);
        UINT frame=0;
        const auto render=[&] { resources.CurrFrameConstantsIndex=frame++%static_cast<UINT>(resources.FrameConstants.size()); common.ActiveCameraViewProjection=common.ActiveCameraView*projection; environment.PrepareFrame(); auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); environment.Execute(commands); queue->ExecuteCommandList(commands); queue->Flush(); };
        set(L"Render.Sky.Source","1"); render();
        const auto box=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        Require(std::any_of(box.begin(),box.end(),[](float v){return v>.01f;}),"Explicit SkyBox must produce sky radiance");
        const auto revision=common.EnvironmentRevision;
        set(L"Render.SkyBox.Intensity","2"); render();
        const auto twice=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        for(size_t i=0;i<box.size();++i) if(i%4!=3) Require(std::abs(twice[i]-box[i]*2)<.01f,"SkyBox intensity must apply once");
        Require(common.EnvironmentRevision==revision,"Intensity must not reset identity/history");
        set(L"Render.SkyBox.Intensity","1"); render();
        const auto minusZ=ReadFace(device,resources,*environment.GetOutput(3)->GetTexture(),5);
        set(L"Render.SkyBox.Rotation","90"); render();
        const auto rotated=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        double rotationDelta=0; for(size_t i=0;i<rotated.size();++i) if(i%4!=3) rotationDelta+=std::abs(rotated[i]-box[i]);
        Require(rotationDelta>1,"SkyBox rotation must change the sampled cube axes");
        for(size_t i=0;i<rotated.size();++i) if(i%4!=3) Require(std::abs(rotated[i]-minusZ[i])<.01f,"A 90-degree SkyBox rotation must map +X onto the original -Z face");
        set(L"Render.SkyBox.Rotation","0");
        GDX12Texture color,depth,motion,normal,material;
        MakeInput(resources,color,65,49,DXGI_FORMAT_R16G16B16A16_FLOAT); MakeInput(resources,depth,65,49,DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources,motion,65,49,DXGI_FORMAT_R16G16_FLOAT); MakeInput(resources,normal,65,49,DXGI_FORMAT_R16G16B16A16_FLOAT); MakeInput(resources,material,65,49,DXGI_FORMAT_R16G16B16A16_FLOAT);
        std::vector<uint16_t> colors(65*49*4,PackedVector::XMConvertFloatToHalf(4)),normals(65*49*4,0),materials(65*49*4,PackedVector::XMConvertFloatToHalf(-1)),velocities(65*49*2,0);
        std::vector<float> depths(65*49,projection._43/100000-projection._33);
        for(size_t i=0;i<65*49;++i) { colors[i*4+3]=PackedVector::XMConvertFloatToHalf(1); normals[i*4+1]=PackedVector::XMConvertFloatToHalf(1); normals[i*4+3]=materials[i*4+3]=PackedVector::XMConvertFloatToHalf(.3f); }
        Upload(device,color,colors.data(),8); Upload(device,depth,depths.data(),4); Upload(device,motion,velocities.data(),4); Upload(device,normal,normals.data(),8); Upload(device,material,materials.data(),8);
        GDX12SkyAerialPass aerial(&environment); aerial.Setup(&resources,nullptr,&common); aerial.SetInputs({&color,&depth}); aerial.Initialize();
        const auto apply=[&] { auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); aerial.Execute(commands); queue->ExecuteCommandList(commands); queue->Flush(); return ReadColor(device,*aerial.GetOutput(0)->GetTexture()); };
        Require(apply()==ReadColor(device,color),"SkyBox aerial path must be an exact HDR bypass");
        sky.Boxes.push_back({3,{}}); sky.Boxes.back().Settings.Intensity=0; environment.Submit(sky); render();
        const auto duplicate=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        for(size_t i=0;i<duplicate.size();++i) if(i%4!=3) Require(duplicate[i]==0,"Duplicate source selection must deterministically use the lowest entity ID");
        sky.Boxes.clear(); environment.Submit(sky); render();
        const auto missing=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        for(size_t i=0;i<missing.size();++i) if(i%4!=3) Require(missing[i]==0,"Missing selected component must produce neutral sky");
        sky.Atmospheres.push_back({8,{}}); environment.Submit(sky); set(L"Render.Sky.Source","2");
        for(int quality=1;quality<=3;++quality)
        {
            set(L"Render.SkyAtmosphere.Quality",std::to_string(quality).c_str()); render();
            const auto trans=ReadColor(device,*environment.GetTransmittanceLUT());
            for(size_t i=0;i<trans.size();++i) Require(std::isfinite(trans[i])&&trans[i]>=0&&trans[i]<=1,"Atmosphere transmittance must be finite in [0,1]");
            const auto radiance=ReadColor(device,*environment.GetOutput(3)->GetTexture());
            Require(std::any_of(radiance.begin(),radiance.end(),[](float v){return v>.001f;}),"LUT atmosphere must produce radiance");
            for(float v:radiance) Require(std::isfinite(v)&&v>=0,"Atmosphere cube must be finite");
            for(UINT face=1;face<6;++face) for(float v:ReadFace(device,resources,*environment.GetOutput(3)->GetTexture(),face)) Require(std::isfinite(v)&&v>=0,"Zenith/horizon/nadir cube faces must remain finite");
            CheckVolume(device,*environment.GetAerialTransmittance());
            const auto integrated=apply(); double difference=0; for(size_t i=0;i<integrated.size();++i) if(i%4!=3) difference+=std::abs(integrated[i]-4);
            Require(difference>1,"Atmosphere aerial integration must affect geometry at one kilometre");
        }
        set(L"Render.SkyAtmosphere.Quality","1"); render();
        sky.Atmospheres[0].Settings.PlanetRadiusKm=100000;
        sky.Atmospheres[0].Settings.AtmosphereHeightKm=.001f; environment.Submit(sky);
        bool rejectedThinShell=false;
        try { environment.PrepareFrame(); } catch(const std::invalid_argument&) { rejectedThinShell=true; }
        Require(rejectedThinShell,"An unrepresentably thin atmosphere shell must be rejected before GPU dispatch");
        sky.Atmospheres[0].Settings.AtmosphereHeightKm=100; environment.Submit(sky); render();
        for(float value:ReadColor(device,*environment.GetTransmittanceLUT())) Require(std::isfinite(value)&&value>=0&&value<=1,"Maximum supported planet radius must retain finite transmittance");
        for(UINT face=0;face<6;++face) for(float value:ReadFace(device,resources,*environment.GetOutput(3)->GetTexture(),face)) Require(std::isfinite(value)&&value>=0,"Maximum supported planet radius must retain finite sky radiance");
        CheckVolume(device,*environment.GetAerialTransmittance());
        sky.Atmospheres[0].Settings.PlanetRadiusKm=6360; environment.Submit(sky); render();
        // A horizontal 1 km ray at peak ozone altitude has a known Beer-Lambert transmittance.
        set(L"Render.SkyAtmosphere.RayleighScale","0"); set(L"Render.SkyAtmosphere.MieScale","0"); set(L"Render.SkyAtmosphere.MultipleScattering","0");
        common.ActiveCameraView=Matrix::CreateTranslation(0,-2500000.0f,0); render();
        const auto absorptionOnly=apply(); const float ozoneCoefficients[]={.00065f,.001881f,.000085f};
        for(UINT c=0;c<3;++c) Require(std::abs(absorptionOnly[(24*65+32)*4+c]-4*std::exp(-ozoneCoefficients[c]))<.004f,"Aerial matrix/depth units must reconstruct a one-kilometre horizontal ray");
        set(L"Render.SkyAtmosphere.RayleighScale","1"); set(L"Render.SkyAtmosphere.MieScale","1"); set(L"Render.SkyAtmosphere.MultipleScattering","1"); common.ActiveCameraView=Matrix::Identity; render();
        GDX12SssrPass ssr; ssr.Setup(&resources,nullptr,&common); ssr.SetInputs({&color,&depth,&motion,&normal,&material,environment.GetOutput(1),environment.GetOutput(2)}); ssr.Initialize();
        const auto reflect=[&] { ssr.PrepareFrame(); auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); ssr.Execute(commands); queue->ExecuteCommandList(commands); queue->Flush(); };
        set(L"Render.SSR.Enabled","true"); set(L"Render.SSR.Quality","1");
        reflect(); render(); reflect(); Require(ssr.HasValidHistory(),"SSSR must warm under the selected atmosphere");
        const auto identity=common.EnvironmentRevision,history=common.HistoryRevision;
        for(int i=0;i<12;++i) { resources.Lighting.LightDirection={.2f+.01f*i,-1,.3f}; common.ActiveCameraView=Matrix::CreateTranslation(0,-100.0f*i,0); render(); reflect(); Require(common.EnvironmentRevision==identity&&common.HistoryRevision==history&&ssr.HasValidHistory(),"Continuous sun/camera changes must preserve warm SSSR history"); }
        resources.Lighting.LightDirection={0,-1,0}; common.ActiveCameraView=Matrix::Identity; render();
        for(UINT face=0;face<6;++face) for(float v:ReadFace(device,resources,*environment.GetOutput(3)->GetTexture(),face)) Require(std::isfinite(v)&&v>=0,"Vertical sun/zenith must remain finite");
        common.ActiveCameraView=Matrix::CreateTranslation(0,-20000000.0f,0); render();
        for(UINT face=0;face<6;++face) for(float v:ReadFace(device,resources,*environment.GetOutput(3)->GetTexture(),face)) Require(std::isfinite(v)&&v>=0,"Camera outside atmosphere must remain finite");
        CheckVolume(device,*environment.GetAerialTransmittance(),false); apply();
        set(L"Render.Sky.Source","0"); render();
        const auto off=ReadColor(device,*environment.GetOutput(3)->GetTexture());
        for(size_t i=0;i<off.size();++i) if(i%4!=3) Require(off[i]==0,"Sky Off must be black");
        ssr.PrepareFrame(); Require(!ssr.HasValidHistory(),"A source identity switch must reset SSSR history");
        const auto offRevision=common.EnvironmentRevision; render(); Require(common.EnvironmentRevision==offRevision,"A stable Off source must not reset identity every frame");
        for(UINT output=0;output<2;++output) { const auto black=ReadColor(device,*environment.GetOutput(output)->GetTexture()); for(size_t i=0;i<black.size();++i) if(i%4!=3) Require(black[i]==0,"Sky Off must also make irradiance/prefiltered IBL black"); }
        if(argc==2&&std::string(argv[1])=="--benchmark")
        {
            constexpr UINT warmup=4,samples=16;
            ComPtr<IDXGIAdapter3> memoryAdapter; ThrowIfFailed(adapter.As(&memoryAdapter));
            GDX12GPUTimer timer(&device);
            set(L"Render.Sky.Source","2"); common.ActiveCameraView=Matrix::Identity; common.ActiveCameraViewProjection=projection;
            for(int quality=1;quality<=3;++quality)
            {
                set(L"Render.SkyAtmosphere.Quality",std::to_string(quality).c_str());
                std::vector<double> times;
                for(UINT sample=0;sample<warmup+samples;++sample)
                {
                    resources.CurrFrameConstantsIndex=frame++%static_cast<UINT>(resources.FrameConstants.size());
                    resources.Lighting.LightDirection={.2f+.001f*sample,-1,.3f}; environment.PrepareFrame();
                    auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); timer.Start(commands);
                    environment.Execute(commands); aerial.Execute(commands); timer.Stop(commands); queue->ExecuteCommandList(commands); timer.OnCommandListExecuted(commands->FenceValue); queue->Flush();
                    const double ms=timer.GetTimeMS(); Require(std::isfinite(ms)&&ms>=0,"Sky GPU timestamp did not resolve"); if(sample>=warmup) times.push_back(ms);
                }
                DXGI_QUERY_VIDEO_MEMORY_INFO memory={}; ThrowIfFailed(memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory));
                const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size(); std::sort(times.begin(),times.end()); const double median=(times[7]+times[8])*.5;
                std::cout<<std::setprecision(9)<<"SKY_GPU_BENCHMARK {\"quality\":"<<quality<<",\"width\":65,\"height\":49,\"warmupFrames\":"<<warmup<<",\"sampleCount\":"<<samples
                    <<",\"workload\":\"fixed 65x49 HDR/depth at 1km; changing sun each frame; steady transmittance/multiscattering; sky/IBL/camera-volume refresh plus aerial integration; D3D12 debug layer; per-frame flush\""
                    <<",\"gpuMeanMs\":"<<mean<<",\"gpuMedianMs\":"<<median<<",\"gpuMinMs\":"<<times.front()<<",\"gpuMaxMs\":"<<times.back()
                    <<",\"memoryScope\":\"DXGI process-local usage includes fixture inputs, live SSSR and sky resources; not isolated sky allocation\",\"processLocalCurrentUsageBytes\":"<<memory.CurrentUsage<<",\"processLocalBudgetBytes\":"<<memory.Budget<<"}\n";
            }
        }
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)));
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i) { SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<unsigned char> storage(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,message,&size)); if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription); }
        ssr.ClearDenendencies(); aerial.ClearDenendencies(); environment.ClearDenendencies(); GDX12ShaderCompiler::Shutdown();
        std::cout<<"Sky GPU: explicit sources, neutral missing/Off, LUT quality, radiance and history epochs passed.\n";
        return 0;
    }
    catch(const DxException& e) { std::wcerr<<e.ToString()<<'\n'; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; }
    return 1;
}
