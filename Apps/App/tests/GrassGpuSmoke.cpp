#define main LightingGpuSmokeMain
#include "LightingGpuSmoke.cpp"
#undef main
#include "Engine.RendererDX12/RenderPasses/GDX12GrassWindPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.Core/ConsoleModule.h"
#include <numeric>
#include <iomanip>

namespace
{
    template<class T> std::vector<T> ReadGpu(GDX12Device& device,GDX12Resource& resource,UINT64 byteOffset,UINT count)
    {
        const UINT64 bytes=UINT64(count)*sizeof(T); const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes); ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->ResourceBarrier({resource.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(),0,resource.D3DResource.Get(),byteOffset,bytes);
        commands->ResourceBarrier({resource.GetGenericReadBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped=nullptr; const D3D12_RANGE read={0,static_cast<SIZE_T>(bytes)}; ThrowIfFailed(readback->Map(0,&read,&mapped));
        const auto* begin=static_cast<const T*>(mapped); std::vector<T> result(begin,begin+count); const D3D12_RANGE noWrite={}; readback->Unmap(0,&noWrite); return result;
    }
    template<class T> T ReadUpload(GDX12UploadBuffer<T>& buffer,UINT index)
    {
        void* mapped=nullptr; const D3D12_RANGE read={index*buffer.GetElementSize(),(index+1)*buffer.GetElementSize()};
        ThrowIfFailed(buffer.GetResource().D3DResource->Map(0,&read,&mapped)); T value;
        memcpy(&value,static_cast<const BYTE*>(mapped)+index*buffer.GetElementSize(),sizeof(T)); const D3D12_RANGE noWrite={}; buffer.GetResource().D3DResource->Unmap(0,&noWrite); return value;
    }
    template<class T> void UploadGpu(GDX12Device& device,GDX12Resource& destination,const std::vector<T>& values,UINT64 byteOffset=0)
    {
        GDX12UploadBuffer<T> upload(&device,static_cast<UINT>(values.size()),EBufferType::Upload,false); upload.CopyData(0,values.data(),static_cast<UINT>(values.size()));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->ResourceBarrier({destination.GetCopyDestBarrier()});
        commands->GetCommandList()->CopyBufferRegion(destination.D3DResource.Get(),byteOffset,upload.GetResource().D3DResource.Get(),0,values.size()*sizeof(T));
        commands->ResourceBarrier({destination.GetGenericReadBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
    }
    template<class T> std::unique_ptr<GDX12UploadBuffer<T>> DefaultBuffer(GDX12DeviceResources& r,UINT count)
    {
        auto buffer=std::make_unique<GDX12UploadBuffer<T>>(r.Device,count,EBufferType::Default,false);
        buffer->CreateSRV(r.SRV_UAV_Heap.get(),r.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        buffer->CreateUAV(r.SRV_UAV_Heap.get(),r.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources)); return buffer;
    }
    void WindOracle(GDX12Device& device,GDX12DeviceResources& resources)
    {
        constexpr UINT grid=17,cells=grid*grid,canaries=7;
        struct Parameters { UINT Grid=grid,Pad[3]={}; float Dt=.1f,Cell=8,Decay=.2f,Force=120; Vector2 Direction={.8f,.6f},Padding={}; } parameters;
        static_assert(sizeof(parameters)==48);
        GDX12RootSignatureDesc desc; desc.Constants.push_back(12); desc.NumSingleSRVSlots=2; desc.NumSingleUAVSlots=1;
        GDX12RootSignature root(&device,desc);
        const char* names[]={"Clear","Inject","Divergence","Jacobi","Project"}; std::array<ComPtr<ID3D12PipelineState>,5> pipelines;
        for(UINT i=0;i<pipelines.size();++i)
        {
            auto shader=GDX12ShaderCompiler::GetInstance().CompileShader(&device,SHADERS_FOLDER "WindFluid.hlsl",nullptr,names[i],"cs");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso={}; pso.pRootSignature=root.GetRootSignature().Get(); pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            ThrowIfFailed(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipelines[i])));
        }
        std::array<std::unique_ptr<GDX12UploadBuffer<Vector2>>,6> buffers;
        for(auto& buffer:buffers)
        {
            buffer=DefaultBuffer<Vector2>(resources,cells+canaries);
            UploadGpu(device,buffer->GetResource(),std::vector<Vector2>(cells+canaries,Vector2(1234,5678)));
        }
        GDX12UploadBuffer<Vector2> neutral(&device,1,EBufferType::Upload,false); neutral.CopyData(0,Vector2::Zero);
        neutral.CreateSRV(resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        auto* queue=device.GetCommandQueue();
        const auto dispatch=[&](UINT pipeline,GDX12UploadBuffer<Vector2>& a,GDX12UploadBuffer<Vector2>& b,GDX12UploadBuffer<Vector2>& output)
        {
            auto* commands=queue->GetCommandList(); commands->SetDescriptorHeaps({resources.SRV_UAV_Heap.get()}); commands->SetComputeRootSignature(&root); commands->SetPipelineState(pipelines[pipeline]);
            commands->GetCommandList()->SetComputeRoot32BitConstants(0,12,&parameters,0);
            if(&a!=&neutral) commands->ResourceBarrier({a.GetResource().GetSRVBarrier()});
            if(&b!=&neutral&&&a!=&b) commands->ResourceBarrier({b.GetResource().GetSRVBarrier()});
            commands->ResourceBarrier({output.GetResource().GetUnorderedAccessBarrier()});
            commands->SetComputeSRV(0,a.GetSRV()->GPUHandle); commands->SetComputeSRV(1,b.GetSRV()->GPUHandle); commands->SetComputeUAV(0,output.GetUAV()->GPUHandle);
            commands->Dispatch((grid+7)/8,(grid+7)/8,1); commands->ResourceBarrier({output.GetResource().GetUAVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        };
        for(auto& buffer:buffers) dispatch(0,neutral,neutral,*buffer);
        const auto cleared=ReadGpu<Vector2>(device,buffers[0]->GetResource(),0,cells+canaries);
        for(UINT i=0;i<cells;++i) Require(cleared[i]==Vector2::Zero,"Odd-sized neutral field must be exactly zero");
        for(UINT i=cells;i<cells+canaries;++i) Require(cleared[i]==Vector2(1234,5678),"Odd-grid dispatch must preserve canaries");
        dispatch(1,*buffers[0],neutral,*buffers[1]); dispatch(2,*buffers[1],neutral,*buffers[2]);
        UINT input=3;
        for(UINT iteration=0;iteration<128;++iteration) { UINT output=input==3?4:3; dispatch(3,*buffers[2],*buffers[input],*buffers[output]); input=output; }
        dispatch(4,*buffers[1],*buffers[input],*buffers[5]);
        const auto injected=ReadGpu<Vector2>(device,buffers[1]->GetResource(),0,cells),projected=ReadGpu<Vector2>(device,buffers[5]->GetResource(),0,cells+canaries);
        const auto fluxError=[&](const auto& values)
        {
            double sum=0; for(UINT y=0;y<grid;++y) for(UINT x=0;x<grid;++x)
            { const auto v=values[y*grid+x]; Require(std::isfinite(v.x)&&std::isfinite(v.y),"Wind must stay finite"); const double flux=v.x-(x?values[y*grid+x-1].x:0)+v.y-(y?values[(y-1)*grid+x].y:0); sum+=flux*flux; } return sum;
        };
        Require(fluxError(projected)<fluxError(injected)*.5,"Pressure projection must reduce measured squared divergence");
        for(UINT i=cells;i<cells+canaries;++i) Require(projected[i]==Vector2(1234,5678),"Projection must preserve odd-grid canaries");
    }
    std::vector<float> ReadTexture(GDX12Device& device,GDX12Texture& texture,UINT channels,bool half=true,UINT subresource=0)
    {
        const auto desc=texture.GetResource()->D3DResource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={}; UINT64 bytes=0;
        device.GetDevice()->GetCopyableFootprints(&desc,subresource,1,0,&footprint,nullptr,nullptr,&bytes);
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK); const auto buffer=CD3DX12_RESOURCE_DESC::Buffer(bytes); ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->ResourceBarrier({texture.GetResource()->GetCopySourceBarrier()});
        const CD3DX12_TEXTURE_COPY_LOCATION src(texture.GetResource()->D3DResource.Get(),subresource),dst(readback.Get(),footprint); commands->GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        commands->ResourceBarrier({texture.GetResource()->GetSRVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped=nullptr; const D3D12_RANGE read={0,static_cast<SIZE_T>(bytes)}; ThrowIfFailed(readback->Map(0,&read,&mapped)); std::vector<float> result;
        for(UINT y=0;y<footprint.Footprint.Height;++y) for(UINT x=0;x<footprint.Footprint.Width*channels;++x)
        {
            const auto* row=static_cast<const BYTE*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch;
            const float value=half?PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(row)[x]):reinterpret_cast<const float*>(row)[x];
            Require(std::isfinite(value),"GPU texture must contain finite values"); result.push_back(value);
        }
        const D3D12_RANGE noWrite={}; readback->Unmap(0,&noWrite); return result;
    }
}

int main(int argc,char** argv)
{
    std::cout<<std::unitbuf; SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
    try
    {
        ConsoleModule console;
        const auto set=[&](const wchar_t* name,const char* value){Require(console.FindConsoleVariable(name).SetFromString(value),"Valid Grass/Wind CVar rejected");};
        Require(!console.FindConsoleVariable(L"Render.Grass.Quality").SetFromString("4"),"Grass quality enum must reject unknown values");
        Require(!console.FindConsoleVariable(L"Render.Wind.GridResolution").SetFromString("8"),"Unsafe wind grid must be rejected");
        Require(!console.FindConsoleVariable(L"Render.Grass.DensityScale").SetFromString("nan"),"Density must reject NaN");
        Require(!console.FindConsoleVariable(L"Render.Wind.PressureIterations").SetFromString("0"),"Pressure solver needs a positive iteration count");
        bool rejectedTinyDomain=false; try { GDX12WindSettings tiny; tiny.HalfExtent=1e-37f; ValidateWind(tiny); } catch(const std::invalid_argument&) { rejectedTinyDomain=true; }
        Require(rejectedTinyDomain,"Wind domains whose cell scale underflows must be rejected before GPU work");
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        auto adapter=GDX12DeviceFactory::GetMostPerformantAdapter(); GDX12Device device; ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources; resources.Initialize(&device); ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info)); info->ClearStoredMessages();
        RenderPipelineCommonData common; common.WindowWidth=common.DownscaledWidth=65; common.WindowHeight=common.DownscaledHeight=49; common.ActiveCameraCBufferIndex=0;
        GDX12GrassWindPass grass; grass.Setup(&resources,nullptr,&common); grass.Initialize();
        auto* queue=device.GetCommandQueue();
        const auto execute=[&] { auto* list=queue->GetCommandList(); grass.Execute(list); queue->ExecuteCommandList(list); queue->Flush(); };
        const auto* neutralCounts=resources.GeneratedIndexCounts->GetResource().D3DResource.Get();
        grass.PrepareFrame(); execute(); Require(resources.GeneratedIndexCounts->GetResource().D3DResource.Get()==neutralCounts,"Absent components must retain the neutral count buffer");
        Require(!resources.GeometryBuffer->Vertices().D3DResource&&!resources.GeometryBuffer->PreviousPositions().D3DResource,"An empty scene must allocate no generated geometry or previous-position array");
        WindOracle(device,resources);

        // A real static prefix makes every generated BaseVertexLocation nonzero.
        SubMesh prefix; prefix.Vertices.resize(3); prefix.Indices={0,1,2};
        for(auto& v:prefix.Vertices) v.Position={10000,10000,10000};
        Mesh prefixMesh({prefix},BoundingBox({10000,10000,10000},{1,1,1})); resources.GeometryBuffer->AddMesh(&prefixMesh,MeshHandle(0));
        resources.IndirectCommandsCache->Resize(2); resources.IndirectCommandsCache->CopyData(0,GDX12IndirectDrawArgs{});
        for(auto& frame:resources.FrameConstants) { frame->InstanceCache->Resize(2); frame->TransformCache->Resize(1); frame->MaterialCache->Resize(1); frame->CameraCB->Resize(1); }
        GDX12GrassWindDesc submission; submission.World=1; submission.DeltaTime=.1f;
        GDX12GrassDesc patch; patch.Entity=1; patch.Settings.Extents={50,50}; patch.Settings.ChunkSize=100; patch.Settings.Height=80; patch.Settings.Width=8; patch.Settings.TuftsPerChunk=17; patch.Instances={1}; submission.Grass.push_back(patch);
        GDX12WindDesc wind; wind.Entity=2; wind.Settings.GridResolution=17; wind.Settings.PressureIterations=64; wind.Settings.HalfExtent=200; wind.Settings.Force=2000; submission.Winds.push_back(wind);
        set(L"Render.Grass.Quality","3");
        const auto generate=[&](bool submit=true)
        {
            resources.CurrFrameConstantsIndex=(resources.CurrFrameConstantsIndex+1)%static_cast<UINT>(resources.FrameConstants.size());
            if(submit) grass.Submit(submission); grass.PrepareFrame(); execute();
        };
        const auto count=[&]{return ReadGpu<UINT>(device,resources.GeneratedIndexCounts->GetResource(),sizeof(UINT),1)[0];};
        const auto current=[&]
        {
            const auto command=ReadUpload(*resources.IndirectCommandsCache,1);
            return ReadGpu<Vertex>(device,resources.GeometryBuffer->Vertices(),UINT64(command.BaseVertexLocation)*sizeof(Vertex),count());
        };
        const auto previous=[&]
        {
            const auto instance=ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->InstanceCache,1);
            return ReadGpu<Vector3>(device,resources.GeometryBuffer->PreviousPositions(),UINT64(instance.PreviousPositionBase)*12,count());
        };
        generate(); Require(count()==17*4*6*12,"Near high-quality tuft output must contain only actual triangles");
        Require(ReadUpload(*resources.IndirectCommandsCache,1).BaseVertexLocation!=0,"Motion test needs nonzero generated BaseVertexLocation");
        auto vertices=current(); auto old=previous();
        {
            float maximum=0; size_t first=vertices.size();
            for(size_t i=0;i<vertices.size();++i) { const float error=(vertices[i].Position-old[i]).Length(); maximum=(std::max)(maximum,error); if(first==vertices.size()&&vertices[i].Position!=old[i]) first=i; }
            const auto command=ReadUpload(*resources.IndirectCommandsCache,1); const auto instance=ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->InstanceCache,1);
            if(first<vertices.size())
            {
            std::cout<<std::setprecision(12)<<"RESET_DIAGNOSTIC count="<<vertices.size()<<" base="<<command.BaseVertexLocation<<" previousBase="<<instance.PreviousPositionBase<<" flags="<<instance.Flags<<" maxDelta="<<maximum;
            if(first<vertices.size()) std::cout<<" first="<<first<<" current=("<<vertices[first].Position.x<<","<<vertices[first].Position.y<<","<<vertices[first].Position.z<<") previous=("<<old[first].x<<","<<old[first].y<<","<<old[first].z<<")";
            std::cout<<'\n';
            }
        }
        for(size_t i=0;i<vertices.size();++i)
        { Require(vertices[i].Position==old[i],"New allocation must reset previous pose to current"); Require(std::isfinite(vertices[i].Normal.x)&&std::abs(vertices[i].Normal.Length()-1)<.001f,"Generated surface normals must be finite and unit length"); }
        Mesh suffixMesh({prefix},BoundingBox({10000,10000,10000},{1,1,1})); resources.GeometryBuffer->AddMesh(&suffixMesh,MeshHandle(1));
        auto preserved=current(); Require(preserved.size()==vertices.size()&&memcmp(preserved.data(),vertices.data(),vertices.size()*sizeof(Vertex))==0,"A later static AddMesh must preserve GPU-written grass");
        generate(); vertices=current(); old=previous(); bool moves=false;
        for(size_t i=0;i<vertices.size();++i) moves|=(vertices[i].Position-old[i]).LengthSquared()>1e-8f;
        Require(moves,"Changing wind must produce a distinct previous local position");
        // No Submit is made while the application skips a paused world.
        generate(false); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require((vertices[i].Position-old[i]).LengthSquared()<1e-8f,"A render without a new submission must freeze field/time and catch up previous positions");
        auto paused=vertices; generate(false); vertices=current();
        const auto vertexLess=[](const Vertex& a,const Vertex& b){const auto& p=a.Position; const auto& q=b.Position; return p.x<q.x||(p.x==q.x&&(p.y<q.y||(p.y==q.y&&p.z<q.z)));};
        std::sort(paused.begin(),paused.end(),vertexLess); std::sort(vertices.begin(),vertices.end(),vertexLess);
        for(size_t i=0;i<vertices.size();++i) Require((paused[i].Position-vertices[i].Position).LengthSquared()<1e-8f,"Repeated paused rendering must not consume the old delta again");
        const auto suffixStart=resources.GeometryBuffer->GetGPUMeshByHandle(MeshHandle(1))->SubMeshes[0].StartVertexLocation;
        const auto canary=ReadGpu<Vertex>(device,resources.GeometryBuffer->Vertices(),UINT64(suffixStart)*sizeof(Vertex),3);
        for(const auto& v:canary) Require(v.Position==Vector3(10000,10000,10000),"Compact generation must preserve a neighboring static canary range");

        // Zero, single, odd and maximum authored tuft counts, quality changes and Off.
        for(UINT tufts:{0u,1u,17u,1024u})
        {
            submission.Grass[0].Settings.TuftsPerChunk=tufts; generate(); Require(count()==tufts*4*6*12,"GPU compact count must match zero/single/odd/maximum tuft workloads");
        }
        submission.Grass[0].Settings.TuftsPerChunk=17;
        for(UINT quality=1;quality<=3;++quality)
        {
            set(L"Render.Grass.Quality",std::to_string(quality).c_str()); generate(); const UINT expected[]={0,17*1*2*12,17*2*4*12,17*4*6*12}; Require(count()==expected[quality],"Grass quality must change actual vertex work");
        }
        set(L"Render.Grass.Enabled","false"); generate(); Require(ReadUpload(*resources.IndirectCommandsCache,1).IndexCountPerInstance==0&&count()==0,"Grass Off must produce zero template and generated counts");
        const auto offRevision=resources.DeformationRevision; generate();
        Require(resources.DeformationRevision==offRevision&&count()==0,"Wind advancing behind Grass Off must keep deformation/shadow revision stable");
        set(L"Render.Grass.Enabled","true"); set(L"Render.Wind.Enabled","false"); generate(); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"Wind Off must publish neutral wind and reset motion");
        set(L"Render.Wind.Enabled","true"); generate();

        set(L"Render.Wind.Quality","0"); generate(); Require(count()>0,"Wind quality Off must leave grass visible"); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"Wind quality Off must publish neutral pose");
        submission.Winds[0].Settings.GridResolution=0; submission.Winds[0].Settings.PressureIterations=0;
        for(UINT quality=1;quality<=3;++quality)
        {
            set(L"Render.Wind.Quality",std::to_string(quality).c_str()); generate(); vertices=current(); old=previous();
            for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"Wind grid preset reallocation must reset previous pose");
        }
        set(L"Render.Wind.GridResolution","33"); set(L"Render.Wind.PressureIterations","5"); generate(); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"Odd runtime grid override must reallocate/reset safely");
        set(L"Render.Wind.Quality","2"); generate();
        Require(RenderSettings::GetWind().GridResolution==33&&RenderSettings::GetWind().PressureIterations==5,"Manual wind overrides must survive a quality change");
        Require(submission.Winds[0].Settings.GridResolution==0&&submission.Winds[0].Settings.PressureIterations==0,"Global solver controls must not mutate component authoring values");
        set(L"Render.Wind.GridResolution","-1"); set(L"Render.Wind.PressureIterations","-1"); submission.Winds[0].Settings.GridResolution=17; submission.Winds[0].Settings.PressureIterations=64;
        ++submission.Winds[0].Settings.ResetRevision; generate(); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"Explicit wind reset must reset the generated temporal pose");
        set(L"Render.Grass.DensityScale","0.5"); set(L"Render.Grass.HeightScale","1.5"); set(L"Render.Grass.BendScale","2"); set(L"Render.Grass.LodScale","1.25"); generate();
        Require(count()==9*4*6*12,"Density scale must change the compact GPU workload");
        Require(submission.Grass[0].Settings.TuftsPerChunk==17&&submission.Grass[0].Settings.Height==80&&submission.Grass[0].Settings.Bend==1,"Global grass controls must preserve component authoring values");
        set(L"Render.Grass.DensityScale","1"); set(L"Render.Grass.HeightScale","1"); set(L"Render.Grass.BendScale","1"); set(L"Render.Grass.LodScale","1");
        common.ActiveCameraView=Matrix::CreateTranslation(0,0,-1500); generate(); Require(count()==17*2*2*12,"Middle LOD must reduce blade and segment work");
        common.ActiveCameraView=Matrix::CreateTranslation(0,0,-4000); generate(); Require(count()==17*12,"Far LOD must emit one actual two-sided quad per tuft");
        common.ActiveCameraView=Matrix::CreateTranslation(0,0,-7000); generate(); Require(count()==0,"Maximum distance must emit zero geometry");
        common.ActiveCameraView=Matrix::Identity; generate();

        // Raster integration: actual GPU culling and ExecuteIndirect consume generated counts.
        GDX12Texture white; GDX12TextureDesc texture; texture.Width=texture.Height=1;
        texture.Format=texture.SRVDesc.Format=texture.RTVDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.SRV_UAV_Heap=resources.SRV_UAV_Heap.get(); texture.SRVHeapIndex=resources.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex,Texture2D_RangeLength);
        texture.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; texture.SRVDesc.Texture2D.MipLevels=1; texture.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.CreateRTV=true; texture.RTVHeap=resources.RTVHeap.get(); texture.RTVHeapIndex=texture.RTVHeap->GetAvailableIndex(); texture.RTVDesc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D; texture.ClearValue={1,.4f,.2f,1}; white.Initialize(texture);
        GDX12OpaquePass opaque; opaque.Setup(&resources,nullptr,&common); opaque.Initialize();
        GDX12GPUCullingPass culling; culling.Setup(&resources,nullptr,&common);
        GDX12CascadedShadowPass shadows; shadows.Setup(&resources,nullptr,&common); shadows.Initialize();
        GDX12LocalShadowPass local; local.Setup(&resources,nullptr,&common); local.Initialize();
        const Matrix view=Matrix::CreateLookAt(Vector3(0,100,-220),Vector3(0,40,0),Vector3::UnitY); common.ActiveCameraView=view;
        Matrix projection=Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f),65.0f/49,.1f,10000); projection._33=.1f/(10000-.1f); projection._43=10000*projection._33;
        common.ActiveCameraViewProjection=common.ActiveCameraPreviousViewProjection=view*projection; common.CameraViewToClip=projection; common.ClipToCameraView=projection.Invert();
        GDX12CameraConstants camera={}; camera.View=view.Transpose(); camera.ViewProj=camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=(view*projection).Transpose(); camera.InvViewProj=(view*projection).Invert().Transpose(); camera.CameraLocation={0,100,-220}; camera.NearPlane=.1f; camera.FarPlane=10000;
        resources.Lighting.SunShadowFlags=1; resources.Lighting.RenderTargetSize={65,49};
        GDX12LocalLightConstants point; point.Position={0,100,-100}; point.Range=1000; point.EntityId=17; point.ShadowFirstSlice=0; point.ShadowFlags=1; BuildLocalLightShadowMatrices(point); resources.LocalLights={point};
        GDX12MaterialConstants material; material.DiffuseIndex=white.GetSRV()->HeapIndex-Texture2D_StartIndex; material.Metallic=.3f; material.Roughness=.4f;
        const auto visibilityBuffer=[&](auto& pointer,UINT size)
        {
            using T=typename std::remove_reference_t<decltype(pointer)>::element_type;
            pointer=std::make_unique<T>(&device,size,EBufferType::Default,false); pointer->CreateUAV(resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        };
        for(auto& frame:resources.FrameConstants)
        {
            frame->CameraCB->CopyData(0,camera); frame->TransformCache->CopyData(0,GDX12TransformConstants{}); frame->MaterialCache->CopyData(0,material); frame->MainCB->CopyData(0,resources.Lighting);
            frame->ShadowCB->CopyData(0,BuildShadowCascades(view.Invert(),XMConvertToRadians(60.0f),65.0f/49,.1f,10000,Vector3(.35f,-1,.25f)));
            frame->LocalLightCache->Resize(1); frame->LocalLightCache->CopyData(0,point); frame->CameraVisibilityCommands.emplace_back(); auto& v=frame->CameraVisibilityCommands[0];
            visibilityBuffer(v.VisibleOpaqueCommandsCache,2); visibilityBuffer(v.VisibleTransparentCommandsCache,2); visibilityBuffer(v.OpaqueDrawCounter,1); visibilityBuffer(v.TransparentDrawCounter,1);
        }
        const auto raster=[&](bool submit=true)
        {
            if(submit) grass.Submit(submission); grass.PrepareFrame();
            auto* commands=queue->GetCommandList(); commands->ResourceBarrier({white.GetResource()->GetRenderTargetBarrier()}); commands->ClearRenderTargetView(&white); commands->ResourceBarrier({white.GetResource()->GetSRVBarrier()});
            grass.Execute(commands); shadows.Execute(commands); local.Execute(commands); culling.Execute(commands); opaque.Execute(commands); queue->ExecuteCommandList(commands); queue->Flush();
        };
        submission.Grass[0].Settings.TuftsPerChunk=128; submission.DeltaTime=0;
        raster();
        const auto depth=ReadTexture(device,*opaque.GetOutput(2)->GetTexture(),1,false);
        Require(std::any_of(depth.begin(),depth.end(),[](float v){return v>0;}),"Generated indirect geometry must rasterize into the main G-buffer");
        const auto normal=ReadTexture(device,*opaque.GetOutput(3)->GetTexture(),4);
        bool hasMaterial=false; for(size_t i=0;i<normal.size();i+=4) if(std::abs(normal[i+3]-.4f)<.001f) hasMaterial=true;
        Require(hasMaterial,"Generated vertices must use the PBR material roughness");
        const auto velocity=ReadTexture(device,*opaque.GetOutput(1)->GetTexture(),2);
        for(float v:velocity) Require(std::abs(v)<.0001f,"Stationary generated geometry with nonzero BaseVertex must have zero motion");
        CheckReadback(device,*shadows.GetOutput(0)->GetTexture(),0,false,true);
        Require(local.GetLastFrameRenderedViews()==6,"Generated geometry must populate all point-shadow views initially");
        raster(false); Require(local.GetLastFrameRenderedViews()==0,"Paused grass must preserve cached local shadows");
        submission.DeltaTime=.1f; raster(); Require(local.GetLastFrameRenderedViews()==6,"Wind deformation must invalidate cached local shadows");
        const auto movingVelocity=ReadTexture(device,*opaque.GetOutput(1)->GetTexture(),2);
        Require(std::any_of(movingVelocity.begin(),movingVelocity.end(),[](float v){return std::abs(v)>.0001f;}),"Previous wind must produce rasterized vertex motion");
        raster(false); const auto frozenVelocity=ReadTexture(device,*opaque.GetOutput(1)->GetTexture(),2);
        for(float v:frozenVelocity) Require(std::abs(v)<.0001f,"A paused field must produce zero rasterized wind motion");
        for(auto& frame:resources.FrameConstants) { auto masked=material; masked.RenderLayer=1; frame->MaterialCache->CopyData(0,masked); }
        texture.ClearValue={1,.4f,.2f,0}; white.Initialize(texture); raster(false);
        const auto maskedDepth=ReadTexture(device,*opaque.GetOutput(2)->GetTexture(),1,false);
        for(float v:maskedDepth) Require(v==0,"Masked grass material alpha must clip generated geometry");
        texture.ClearValue={1,.4f,.2f,1}; white.Initialize(texture); for(auto& frame:resources.FrameConstants) frame->MaterialCache->CopyData(0,material);

        // Moving across a frozen spatial field must retain the prior local curve.
        submission.DeltaTime=0; raster(false); const auto beforeMove=current();
        const auto sortedPositions=[](const auto& input)
        {
            std::vector<Vector3> points; for(const auto& vertex:input) points.push_back(vertex.Position);
            std::sort(points.begin(),points.end(),[](const Vector3& a,const Vector3& b){return a.x<b.x||(a.x==b.x&&(a.y<b.y||(a.y==b.y&&a.z<b.z)));}); return points;
        };
        const auto oldPoints=sortedPositions(beforeMove);
        submission.Grass[0].World=Matrix::CreateTranslation(30,0,0);
        for(auto& frame:resources.FrameConstants) { GDX12TransformConstants transform; transform.WorldMatrix=submission.Grass[0].World.Transpose(); frame->TransformCache->CopyData(0,transform); }
        raster(); old=previous();
        std::sort(old.begin(),old.end(),[](const Vector3& a,const Vector3& b){return a.x<b.x||(a.x==b.x&&(a.y<b.y||(a.y==b.y&&a.z<b.z)));});
        Require(old.size()==oldPoints.size(),"Moving patch oracle must retain stable topology");
        for(size_t i=0;i<old.size();++i) Require((old[i]-oldPoints[i]).LengthSquared()<1e-6f,"Previous wind lookup must use the previous world root");
        submission.Grass[0].Entity=99; ++submission.Grass[0].Settings.Seed; raster(); vertices=current(); old=previous();
        for(size_t i=0;i<vertices.size();++i) Require(vertices[i].Position==old[i],"An instance slot reused by a new entity must reset its previous pose");
        submission.Grass[0].World=Matrix::Identity;
        for(auto& frame:resources.FrameConstants) frame->TransformCache->CopyData(0,GDX12TransformConstants{});

        // Draw split and coalesced identity ranges through the actual indexed pipeline.
        auto sceneArena=std::move(resources.GeometryBuffer); resources.GeometryBuffer=std::make_unique<GDX12GeometryBuffer>(&device); resources.GeometryBuffer->InitializeViews(resources.SRV_UAV_Heap.get());
        const auto whole=resources.GeometryBuffer->AllocateGenerated(30); resources.GeometryBuffer->ReleaseGenerated(whole);
        const auto head=resources.GeometryBuffer->AllocateGenerated(6),tail=resources.GeometryBuffer->AllocateGenerated(24);
        Require(tail.VertexBase==head.VertexBase+6,"Reusable arena must split a free range");
        const auto drawRange=[&](GDX12GeneratedGeometryRange range,UINT localIndex=0)
        {
            std::vector<Vertex> triangle(range.Capacity); for(auto& vertex:triangle) vertex.Position={10000,10000,10000};
            const Vector3 points[]={Vector3(-30,0,0),Vector3(30,0,0),Vector3(0,80,0)};
            for(UINT i=0;i<6;++i) { triangle[localIndex+i].Position=points[i<3?i:5-i]; triangle[localIndex+i].Normal={0,0,-1}; }
            UploadGpu(device,resources.GeometryBuffer->Vertices(),triangle,UINT64(range.VertexBase)*sizeof(Vertex));
            GDX12IndirectDrawArgs command={}; command.InstanceID=command.StartInstanceLocation=1; command.IndexCountPerInstance=6; command.InstanceCount=1; command.StartIndexLocation=range.IndexBase+localIndex; command.BaseVertexLocation=range.VertexBase; resources.IndirectCommandsCache->CopyData(1,command);
            GDX12InstanceData instance={}; instance.BoundingBoxCenter={0,40,0}; instance.BoundingBoxExtents={30,40,1}; resources.FrameConstants[resources.CurrFrameConstantsIndex]->InstanceCache->CopyData(1,instance);
            auto* list=queue->GetCommandList(); list->ResourceBarrier({resources.GeometryBuffer->PreviousPositions().GetSRVBarrier()}); culling.Execute(list); opaque.Execute(list); queue->ExecuteCommandList(list); queue->Flush();
            const auto drawn=ReadTexture(device,*opaque.GetOutput(2)->GetTexture(),1,false); Require(std::any_of(drawn.begin(),drawn.end(),[](float v){return v>0;}),"Split/coalesced range indices must start at zero for indexed draws");
        };
        drawRange(tail); resources.GeometryBuffer->ReleaseGenerated(head); resources.GeometryBuffer->ReleaseGenerated(tail);
        const auto merged=resources.GeometryBuffer->AllocateGenerated(30); Require(merged.VertexBase==whole.VertexBase,"Adjacent free ranges must coalesce"); drawRange(merged,24); resources.GeometryBuffer->ReleaseGenerated(merged);
        resources.GeometryBuffer=std::move(sceneArena);
        // The production command cache keeps its template until it changes; restore the fixture edit explicitly.
        submission.Grass[0].Settings.ResetRevision++; set(L"Render.Grass.Enabled","false"); raster(); set(L"Render.Grass.Enabled","true"); raster();

        GDX12EnvironmentIBLPass environment; environment.Setup(&resources,nullptr,&common); environment.Initialize();
        GDX12SkyDesc sky; sky.World=1; sky.Boxes.push_back({1,{}}); environment.Submit(sky); environment.PrepareFrame();
        GDX12ReflectionProbePass probes; probes.Setup(&resources,nullptr,&common); probes.SetInputs({environment.GetOutput(0),environment.GetOutput(1),environment.GetOutput(2),environment.GetOutput(3),local.GetOutput(0)}); probes.Initialize();
        set(L"Render.Reflection.Resolution","32"); set(L"Render.Reflection.FacesPerFrame","6");
        GDX12ReflectionProbeDesc probe; probe.Entity=12; probe.Position={0,40,-140}; probe.Extents=Vector3(500); probe.NearPlane=.1f; probe.FarPlane=1000; probe.Mode=GDX12ReflectionProbeMode::BakedStatic;
        material.EmissiveColor={6,0,0};
        for(auto& frame:resources.FrameConstants) frame->MaterialCache->CopyData(0,material);
        const auto capture=[&]
        {
            grass.Submit(submission); grass.PrepareFrame(); environment.PrepareFrame(); probes.Submit(1,{probe},1,1); probes.PrepareFrame();
            auto* list=queue->GetCommandList(); grass.Execute(list); environment.Execute(list); local.Execute(list); probes.Execute(list); queue->ExecuteCommandList(list); queue->Flush();
        };
        capture(); Require(probes.GetPublished().size()==1,"Grass capture must publish a complete cube");
        bool sawGrass=false;
        for(UINT face=0;face<6;++face)
        {
            const auto radiance=ReadTexture(device,*probes.GetPublished()[0].Texture,4,true,face*probes.GetPublished()[0].MipCount);
            for(size_t i=0;i<radiance.size();i+=4) if(radiance[i]>5&&radiance[i]>radiance[i+1]+4) sawGrass=true;
        }
        Require(sawGrass,"Generated grass must contribute PBR/emissive radiance to dynamic probe capture");
        set(L"Render.Reflection.FacesPerFrame","1"); const auto publication=probes.GetPublished()[0].Revision;
        submission.DeltaTime=.05f;
        for(UINT frame=0;frame<6;++frame) capture();
        Require(probes.GetPublished()[0].Revision>publication,"Changing wind must not repeatedly rewind the probe face cursor");

        // Looking away must cull main draws while retaining generated geometry and shadow casters.
        common.ActiveCameraView=Matrix::CreateLookAt(Vector3(0,100,-220),Vector3(0,100,-440),Vector3::UnitY);
        camera.View=common.ActiveCameraView.Transpose(); camera.ViewProj=camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=(common.ActiveCameraView*projection).Transpose();
        for(auto& frame:resources.FrameConstants) frame->CameraCB->CopyData(0,camera);
        raster(); const auto behindDepth=ReadTexture(device,*opaque.GetOutput(2)->GetTexture(),1,false);
        for(float v:behindDepth) Require(v==0,"Behind-camera chunks must be culled from the main view");
        Require(count()>0,"Main frustum must not suppress off-camera geometry generation");
        CheckReadback(device,*shadows.GetOutput(0)->GetTexture(),0,false,true);
        capture(); Require(probes.GetPublished().size()==1,"Off-camera casters must remain available to probe capture");
        common.ActiveCameraView=view; camera.View=view.Transpose(); camera.ViewProj=camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=(view*projection).Transpose();
        for(auto& frame:resources.FrameConstants) frame->CameraCB->CopyData(0,camera);
        probes.ClearDenendencies(); environment.ClearDenendencies();

        set(L"Render.Grass.Enabled","false"); raster(); const auto offDepth=ReadTexture(device,*opaque.GetOutput(2)->GetTexture(),1,false);
        for(float v:offDepth) Require(v==0,"Grass Off must remove actual indirect draws");
        set(L"Render.Grass.Enabled","true");
        if(argc==2&&std::string(argv[1])=="--benchmark")
        {
            constexpr UINT warmup=4,samples=16; GDX12GPUTimer timer(&device); ComPtr<IDXGIAdapter3> memoryAdapter; ThrowIfFailed(adapter.As(&memoryAdapter));
            submission.DeltaTime=1.0f/60; submission.Grass[0].Settings.TuftsPerChunk=1024;
            for(UINT quality=1;quality<=3;++quality)
            {
                set(L"Render.Grass.Quality",std::to_string(quality).c_str()); set(L"Render.Wind.Quality",std::to_string(quality).c_str());
                submission.Winds[0].Settings.GridResolution=0; submission.Winds[0].Settings.PressureIterations=0;
                std::vector<double> times;
                for(UINT sample=0;sample<warmup+samples;++sample)
                {
                    resources.CurrFrameConstantsIndex=(resources.CurrFrameConstantsIndex+1)%static_cast<UINT>(resources.FrameConstants.size()); grass.Submit(submission); grass.PrepareFrame();
                    auto* list=queue->GetCommandList(); timer.Start(list); grass.Execute(list); timer.Stop(list); queue->ExecuteCommandList(list); timer.OnCommandListExecuted(list->FenceValue); queue->Flush();
                    const double ms=timer.GetTimeMS(); Require(std::isfinite(ms)&&ms>=0,"Grass GPU timestamp must resolve"); if(sample>=warmup) times.push_back(ms);
                }
                DXGI_QUERY_VIDEO_MEMORY_INFO memory={}; ThrowIfFailed(memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory));
                const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size(); std::sort(times.begin(),times.end());
                std::cout<<std::setprecision(9)<<"GRASS_GPU_BENCHMARK {\"quality\":"<<quality<<",\"tufts\":1024,\"warmupFrames\":"<<warmup<<",\"samples\":"<<samples
                    <<",\"workload\":\"production wind solver plus compact generation, fixed one-chunk patch, per-frame fence flush, debug layer\""
                    <<",\"gpuMeanMs\":"<<mean<<",\"gpuMedianMs\":"<<(times[7]+times[8])*.5<<",\"gpuMinMs\":"<<times.front()<<",\"gpuMaxMs\":"<<times.back()
                    <<",\"memoryScope\":\"DXGI whole-process local usage, includes all fixture render/arena resources, not isolated grass allocation\",\"processLocalCurrentUsageBytes\":"<<memory.CurrentUsage<<",\"processLocalBudgetBytes\":"<<memory.Budget<<"}\n";
            }
        }
        grass.ClearDenendencies(); local.ClearDenendencies(); shadows.ClearDenendencies(); culling.ClearDenendencies(); opaque.ClearDenendencies();
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        {
            SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<BYTE> storage(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,message,&size));
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        GDX12ShaderCompiler::Shutdown(); std::cout<<"Grass/Wind GPU assertions passed.\n";
        return 0;
    }
    catch(const DxException& e) { std::wcerr<<e.ToString()<<'\n'; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; }
    return 1;
}
