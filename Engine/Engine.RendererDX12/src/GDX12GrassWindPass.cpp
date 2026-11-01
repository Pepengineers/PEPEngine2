#include "Engine.RendererDX12/RenderPasses/GDX12GrassWindPass.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace
{
    struct WindCB
    {
        UINT Grid=0,Pad0=0,Pad1=0,Pad2=0;
        float Dt=0,CellSize=1,Decay=0,Force=0;
        Vector2 Direction=Vector2::Zero,Pad3=Vector2::Zero;
    };
    struct GrassCB
    {
        Matrix World={},InverseWorld={},PreviousWorld={},PreviousInverseWorld={};
        Vector4 Patch={},Shape={},PreviousShape={},Lod={},Eye={},WindDomain={},PreviousWindDomain={};
        UINT VertexBase=0,PreviousBase=0,InstanceID=0,TuftCount=0;
        UINT Seed=0,BladeCount=0,SegmentCount=0,ResetMotion=0;
        UINT WindGrid=1,PreviousWindGrid=1,WindEnabled=0,Reserved=0;
    };
    static_assert(sizeof(WindCB)==48 && sizeof(GrassCB)==416);
    template<class T> void Hash(uint64_t& hash,const T& value)
    {
        const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(size_t i=0;i<sizeof(value);++i) hash=(hash^bytes[i])*1099511628211ull;
    }
    template<class T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(GDX12DeviceResources& r,UINT count,EBufferType type)
    {
        auto buffer=std::make_unique<GDX12UploadBuffer<T>>(r.Device,count,type,false);
        buffer->CreateSRV(r.SRV_UAV_Heap.get(),r.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        if(type==EBufferType::Default) buffer->CreateUAV(r.SRV_UAV_Heap.get(),r.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        return buffer;
    }
}
struct GDX12GrassWindPass::Data
{
    struct Field
    {
        GDX12WindDesc Description;
        UINT Grid=0,Iterations=0;
        std::unique_ptr<GDX12UploadBuffer<Vector2>> Current,Injected,Previous,Divergence,Pressure[2];
        WindCB Constants;
        bool Reset=true,WasEnabled=false;
        Vector4 PreviousDomain={};
        uint64_t Signature=0;
    };
    struct Chunk
    {
        GDX12GeneratedGeometryRange Range;
        GrassCB Constants={},Last={};
        GDX12IndirectDrawArgs Command={};
        UINT ConstantIndex=0;
        uint64_t Owner=0,WindEntity=0,ResetRevision=0,HistoryRevision=0;
        bool Reset=true,Active=false;
    };
    GDX12GrassWindDesc Submission;
    uint64_t World=0,Signature=0;
    float Time=0,DeltaTime=0;
    bool NewSubmission=false;
    RenderSettings::Grass Grass={};
    RenderSettings::Wind Wind={};
    std::unordered_map<uint64_t,Field> Fields;
    std::unordered_map<UINT,Chunk> Chunks;
    std::vector<std::unique_ptr<GDX12UploadBuffer<GrassCB>>> Constants;
    std::unique_ptr<GDX12UploadBuffer<Vector2>> Neutral;
    std::unique_ptr<GDX12RootSignature> WindRoot,GrassRoot;
    ComPtr<ID3D12PipelineState> Clear,Inject,Divergence,Jacobi,Project,ClearCount,Generate;
    bool CountsGpu=false;

    void InitializePipelines(GDX12DeviceResources& r)
    {
        if(GrassRoot) return;
        GDX12RootSignatureDesc wind; wind.Constants.push_back(12); wind.NumSingleSRVSlots=2; wind.NumSingleUAVSlots=1;
        WindRoot=std::make_unique<GDX12RootSignature>(r.Device,wind);
        GDX12RootSignatureDesc grass; grass.NumSingleCBVSlots=1; grass.NumSingleSRVSlots=2; grass.NumSingleUAVSlots=3;
        GrassRoot=std::make_unique<GDX12RootSignature>(r.Device,grass);
        const auto compile=[&](const std::wstring& file,const char* entry,GDX12RootSignature& root,ComPtr<ID3D12PipelineState>& target)
        {
            auto code=GDX12ShaderCompiler::GetInstance().CompileShader(r.Device,file,nullptr,entry,"cs");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso={}; pso.pRootSignature=root.GetRootSignature().Get(); pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
            ThrowIfFailed(r.Device->GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&target)));
        };
        compile(SHADERS_FOLDER "WindFluid.hlsl","Clear",*WindRoot,Clear);
        compile(SHADERS_FOLDER "WindFluid.hlsl","Inject",*WindRoot,Inject);
        compile(SHADERS_FOLDER "WindFluid.hlsl","Divergence",*WindRoot,Divergence);
        compile(SHADERS_FOLDER "WindFluid.hlsl","Jacobi",*WindRoot,Jacobi);
        compile(SHADERS_FOLDER "WindFluid.hlsl","Project",*WindRoot,Project);
        compile(SHADERS_FOLDER "Grass.hlsl","ClearCount",*GrassRoot,ClearCount);
        compile(SHADERS_FOLDER "Grass.hlsl","Generate",*GrassRoot,Generate);
    }
};
GDX12GrassWindPass::GDX12GrassWindPass() : _data(std::make_unique<Data>())
{ _flags=RENDER_PASS_FLAG_USE_GEOMETRY|RENDER_PASS_FLAG_USE_INSTANCES|RENDER_PASS_FLAG_USE_MATERIALS; }
GDX12GrassWindPass::~GDX12GrassWindPass() = default;
void GDX12GrassWindPass::Initialize()
{
    _data->Neutral=Buffer<Vector2>(*_resources,1,EBufferType::Upload);
    _data->Neutral->CopyData(0,Vector2::Zero);
    _data->Constants.resize(_resources->FrameConstants.size());
}
void GDX12GrassWindPass::Submit(GDX12GrassWindDesc submission)
{
    if(!std::isfinite(submission.DeltaTime)||submission.DeltaTime<0) throw std::invalid_argument("Grass/Wind needs finite nonnegative world time");
    _data->Submission=std::move(submission); _data->NewSubmission=true;
}
void GDX12GrassWindPass::PrepareFrame()
{
    auto& d=*_data; auto& r=*_resources;
    d.DeltaTime=d.NewSubmission?d.Submission.DeltaTime:0; d.NewSubmission=false;
    d.Grass=RenderSettings::GetGrass(); d.Wind=RenderSettings::GetWind();
    auto* queue=r.Device->GetCommandQueue();
    const bool worldChanged=d.World!=d.Submission.World;
    std::unordered_set<UINT> alive;
    for(const auto& grass:d.Submission.Grass) for(UINT instance:grass.Instances) alive.insert(instance);
    bool removed=false;
    for(auto it=d.Chunks.begin();it!=d.Chunks.end();)
        if(worldChanged||alive.find(it->first)==alive.end())
        { queue->Flush(); r.IndirectCommandsCache->CopyData(it->first,GDX12IndirectDrawArgs{}); r.GeometryBuffer->ReleaseGenerated(it->second.Range); it=d.Chunks.erase(it); removed=true; }
        else ++it;
    for(auto it=d.Fields.begin();it!=d.Fields.end();)
        if(worldChanged||std::none_of(d.Submission.Winds.begin(),d.Submission.Winds.end(),[&](const auto& field){return field.Entity==it->first;}))
        { queue->Flush(); it=d.Fields.erase(it); removed=true; } else ++it;
    if(worldChanged) { d.World=d.Submission.World; d.Time=0; }
    if(d.Wind.Enabled) d.Time=std::fmod(d.Time+d.DeltaTime,6.28318530718f/1.7f);
    constexpr UINT grids[]={0,64,128,256},iterations[]={0,12,24,40};
    for(const auto& desc:d.Submission.Winds)
    {
        ValidateWind(desc.Settings);
        if(!std::isfinite(desc.Center.x)||!std::isfinite(desc.Center.y)||!std::isfinite(desc.Center.z)) throw std::invalid_argument("Non-finite wind center");
        auto& field=d.Fields[desc.Entity]; field.Description=desc;
        if(!d.Wind.Enabled) { field.WasEnabled=false; field.Reset=true; continue; }
        UINT grid=d.Wind.GridResolution>=0?static_cast<UINT>(d.Wind.GridResolution):desc.Settings.GridResolution;
        if(!grid) grid=grids[static_cast<int>(d.Wind.Level)];
        UINT pressure=d.Wind.PressureIterations>=0?static_cast<UINT>(d.Wind.PressureIterations):desc.Settings.PressureIterations;
        if(!pressure) pressure=iterations[static_cast<int>(d.Wind.Level)];
        d.InitializePipelines(r);
        if(grid!=field.Grid)
        {
            queue->Flush(); field.Current=Buffer<Vector2>(r,grid*grid,EBufferType::Default); field.Injected=Buffer<Vector2>(r,grid*grid,EBufferType::Default);
            field.Previous=Buffer<Vector2>(r,grid*grid,EBufferType::Default); field.Divergence=Buffer<Vector2>(r,grid*grid,EBufferType::Default);
            for(auto& p:field.Pressure) p=Buffer<Vector2>(r,grid*grid,EBufferType::Default);
            field.Grid=grid; field.Reset=true;
        }
        uint64_t signature=1469598103934665603ull;
        Hash(signature,desc.Settings.ResetRevision); Hash(signature,desc.Center); Hash(signature,desc.Settings.HalfExtent);
        if(field.Signature!=signature||!field.WasEnabled) field.Reset=true;
        field.Signature=signature; field.Iterations=pressure; field.WasEnabled=true;
        field.Constants.Grid=grid; field.Constants.Dt=d.DeltaTime;
        field.Constants.CellSize=2*desc.Settings.HalfExtent/grid; field.Constants.Decay=desc.Settings.Dissipation;
        field.Constants.Force=desc.Settings.Force*d.Wind.ForceScale;
        const float angle=XMConvertToRadians(desc.Settings.DirectionDegrees+d.Wind.DirectionOffset);
        field.Constants.Direction={std::cos(angle),std::sin(angle)};
    }
    UINT requiredCounts=1;
    for(const auto& grass:d.Submission.Grass) for(UINT instance:grass.Instances) requiredCounts=(std::max)(requiredCounts,instance+1);
    if(d.Grass.Enabled&&!d.Submission.Grass.empty()&&(!d.CountsGpu||r.GeneratedIndexCounts->GetElementCount()<requiredCounts))
    {
        queue->Flush(); r.GeneratedIndexCounts=Buffer<UINT>(r,requiredCounts,EBufferType::Default); d.CountsGpu=true; d.InitializePipelines(r);
    }
    UINT constantCount=0;
    for(const auto& grass:d.Submission.Grass) constantCount+=static_cast<UINT>(grass.Instances.size());
    auto& constants=d.Constants[r.CurrFrameConstantsIndex];
    if(constantCount&&(!constants||constants->GetElementCount()<constantCount))
    { queue->Flush(); constants=std::make_unique<GDX12UploadBuffer<GrassCB>>(r.Device,constantCount); }
    UINT index=0; uint64_t signature=1469598103934665603ull;
    const Vector3 eye=Vector3::Transform(Vector3::Zero,_commonData->ActiveCameraView.Invert());
    for(const auto& desc:d.Submission.Grass)
    {
        const auto& s=desc.Settings; ValidateGrass(s);
        const UINT nx=static_cast<UINT>(std::ceil(2*s.Extents.x/s.ChunkSize)),nz=static_cast<UINT>(std::ceil(2*s.Extents.y/s.ChunkSize));
        if(desc.Instances.size()!=size_t(nx)*nz) throw std::invalid_argument("Grass instance count must match bounded chunks");
        const float determinant=desc.World.Determinant();
        if(!std::isfinite(determinant)||std::abs(determinant)<1e-12f) throw std::invalid_argument("Grass requires a finite invertible world transform");
        const UINT tufts=static_cast<UINT>(std::ceil(s.TuftsPerChunk*d.Grass.DensityScale));
        const bool enabled=d.Grass.Enabled&&tufts&&s.Height*d.Grass.HeightScale>0;
        Data::Field* field=nullptr;
        for(auto& [entity,candidate]:d.Fields)
            if((!s.WindEntity||entity==s.WindEntity)&&candidate.WasEnabled&&(!field||entity<field->Description.Entity)) field=&candidate;
        for(UINT z=0;z<nz;++z) for(UINT x=0;x<nx;++x)
        {
            const UINT instance=desc.Instances[z*nx+x]; auto& chunk=d.Chunks[instance]; if(chunk.Active!=enabled||chunk.Owner!=desc.Entity) chunk.Reset=true; chunk.Owner=desc.Entity; chunk.Active=enabled; chunk.ConstantIndex=index++;
            const UINT capacity=enabled?tufts*d.Grass.Blades*d.Grass.Segments*12:0;
            if(capacity&&chunk.Range.Capacity!=capacity)
            {
                r.GeometryBuffer->ReleaseGenerated(chunk.Range); chunk.Range=r.GeometryBuffer->AllocateGenerated(capacity); chunk.Reset=true;
            }
            GrassCB cb={}; cb.World=desc.World.Transpose(); cb.InverseWorld=desc.World.Invert().Transpose();
            const float left=-s.Extents.x+x*s.ChunkSize,right=(std::min)(left+s.ChunkSize,s.Extents.x);
            const float bottom=-s.Extents.y+z*s.ChunkSize,top=(std::min)(bottom+s.ChunkSize,s.Extents.y);
            cb.Patch={(left+right)*.5f,(bottom+top)*.5f,(right-left)*.5f,(top-bottom)*.5f};
            cb.Shape={s.Height*d.Grass.HeightScale,s.Width,s.Bend*d.Grass.BendScale,field?d.Time:0};
            cb.PreviousShape=chunk.Last.Shape; cb.PreviousWorld=chunk.Last.World; cb.PreviousInverseWorld=chunk.Last.InverseWorld;
            cb.Lod={s.LodNear*d.Grass.LodScale,s.LodFar*d.Grass.LodScale,s.MaxDistance*d.Grass.LodScale,0}; cb.Eye={eye.x,eye.y,eye.z,0};
            cb.VertexBase=chunk.Range.VertexBase; cb.PreviousBase=chunk.Range.PreviousBase; cb.InstanceID=instance; cb.TuftCount=tufts;
            cb.Seed=s.Seed+z*nx+x; cb.BladeCount=d.Grass.Blades; cb.SegmentCount=d.Grass.Segments;
            if(field)
            {
                cb.WindEnabled=1; cb.WindGrid=cb.PreviousWindGrid=field->Grid;
                cb.WindDomain={field->Description.Center.x,field->Description.Center.z,field->Description.Settings.HalfExtent,0}; cb.PreviousWindDomain=field->PreviousDomain;
            }
            const uint64_t windEntity=field?field->Description.Entity:0;
            const bool shapeChanged=std::memcmp(&cb.Patch,&chunk.Last.Patch,sizeof(cb.Patch))!=0||cb.Shape.x!=chunk.Last.Shape.x||cb.Shape.y!=chunk.Last.Shape.y||cb.Shape.z!=chunk.Last.Shape.z||cb.TuftCount!=chunk.Last.TuftCount||cb.Seed!=chunk.Last.Seed||cb.BladeCount!=chunk.Last.BladeCount||cb.SegmentCount!=chunk.Last.SegmentCount;
            const Vector3 movement(cb.World._14-chunk.Last.World._14,cb.World._24-chunk.Last.World._24,cb.World._34-chunk.Last.World._34);
            cb.ResetMotion=movement.Length()>s.MaxDistance||chunk.Reset||shapeChanged||chunk.ResetRevision!=s.ResetRevision||chunk.HistoryRevision!=_commonData->HistoryRevision||chunk.WindEntity!=windEntity||(field&&field->Reset);
            if(cb.ResetMotion) { cb.PreviousShape=cb.Shape; cb.PreviousWindDomain=cb.WindDomain; cb.PreviousWorld=cb.World; cb.PreviousInverseWorld=cb.InverseWorld; }
            chunk.Constants=cb; chunk.WindEntity=windEntity; chunk.ResetRevision=s.ResetRevision; chunk.HistoryRevision=_commonData->HistoryRevision;
            // Do not hash previous-frame bookkeeping: a paused steady field must keep cached shadows warm.
            Hash(signature,instance); Hash(signature,enabled);
            if(enabled)
            {
                Hash(signature,cb.World); Hash(signature,cb.Patch); Hash(signature,cb.Shape); Hash(signature,cb.Lod); Hash(signature,cb.Eye); Hash(signature,tufts); Hash(signature,cb.BladeCount); Hash(signature,cb.SegmentCount); Hash(signature,s.Seed); Hash(signature,windEntity);
                if(field) Hash(signature,field->Signature);
            }
            GDX12IndirectDrawArgs command={}; command.InstanceID=command.StartInstanceLocation=instance; command.InstanceCount=1;
            command.IndexCountPerInstance=enabled?chunk.Range.Capacity:0; command.StartIndexLocation=chunk.Range.IndexBase; command.BaseVertexLocation=static_cast<INT>(chunk.Range.VertexBase);
            if(std::memcmp(&chunk.Command,&command,sizeof(command))!=0)
            { queue->Flush(); r.IndirectCommandsCache->CopyData(instance,command); chunk.Command=command; }
            GDX12InstanceData data={}; data.TransformIndex=desc.TransformIndex; data.MaterialIndex=desc.MaterialIndex; data.Flags=7|(cb.ResetMotion?8u:0u);
            data.PreviousPositionBase=chunk.Range.PreviousBase; data.VertexBase=chunk.Range.VertexBase;
            BoundingBox local({cb.Patch.x,cb.Shape.x*.5f,cb.Patch.y},{cb.Patch.z+cb.Shape.x+cb.Shape.y,cb.Shape.x*.5f,cb.Patch.w+cb.Shape.x+cb.Shape.y}),bounds;
            local.Transform(bounds,desc.World); data.BoundingBoxCenter=bounds.Center; data.BoundingBoxExtents=bounds.Extents;
            r.FrameConstants[r.CurrFrameConstantsIndex]->InstanceCache->CopyData(instance,data);
            if(enabled) d.InitializePipelines(r);
            constants->CopyData(chunk.ConstantIndex,cb);
        }
    }
    if(signature!=d.Signature||removed) { ++r.DeformationRevision; d.Signature=signature; }
}
void GDX12GrassWindPass::Execute(GDX12CommandList* commands)
{
    auto& d=*_data; auto& r=*_resources;
    if(!d.GrassRoot) return;
    commands->BeginPixEvent("Wind field and compact grass",Colors::ForestGreen);
    commands->SetDescriptorHeaps({r.SRV_UAV_Heap.get()});
    const auto dispatch=[&](Data::Field& f,ID3D12PipelineState* pso,GDX12UploadBuffer<Vector2>& a,GDX12UploadBuffer<Vector2>& b,GDX12UploadBuffer<Vector2>& out)
    {
        commands->SetComputeRootSignature(d.WindRoot.get()); commands->SetPipelineState(pso);
        commands->GetCommandList()->SetComputeRoot32BitConstants(0,12,&f.Constants,0);
        if(&a!=d.Neutral.get()) commands->ResourceBarrier({a.GetResource().GetSRVBarrier()});
        if(&b!=d.Neutral.get()&&&a!=&b) commands->ResourceBarrier({b.GetResource().GetSRVBarrier()});
        commands->ResourceBarrier({out.GetResource().GetUnorderedAccessBarrier()});
        commands->SetComputeSRV(0,a.GetSRV()->GPUHandle); commands->SetComputeSRV(1,b.GetSRV()->GPUHandle); commands->SetComputeUAV(0,out.GetUAV()->GPUHandle);
        commands->Dispatch((f.Grid+7)/8,(f.Grid+7)/8,1); commands->ResourceBarrier({out.GetResource().GetUAVBarrier()});
    };
    for(auto& [entity,f]:d.Fields)
    {
        if(!f.Current||!d.Wind.Enabled) continue;
        if(f.Reset)
        {
            for(auto* target:{f.Current.get(),f.Injected.get(),f.Previous.get(),f.Divergence.get(),f.Pressure[0].get(),f.Pressure[1].get()}) dispatch(f,d.Clear.Get(),*d.Neutral,*d.Neutral,*target);
        }
        if(d.Wind.Enabled)
        {
            commands->ResourceBarrier({f.Current->GetResource().GetCopySourceBarrier(),f.Previous->GetResource().GetCopyDestBarrier()});
            commands->GetCommandList()->CopyResource(f.Previous->GetResource().D3DResource.Get(),f.Current->GetResource().D3DResource.Get());
            if(f.Constants.Dt>0)
            {
                dispatch(f,d.Inject.Get(),*f.Current,*d.Neutral,*f.Injected);
                dispatch(f,d.Divergence.Get(),*f.Injected,*d.Neutral,*f.Divergence);
                dispatch(f,d.Clear.Get(),*d.Neutral,*d.Neutral,*f.Pressure[0]);
                UINT input=0;
                for(UINT iteration=0;iteration<f.Iterations;++iteration)
                { dispatch(f,d.Jacobi.Get(),*f.Divergence,*f.Pressure[input],*f.Pressure[1-input]); input=1-input; }
                dispatch(f,d.Project.Get(),*f.Injected,*f.Pressure[input],*f.Current);
            }
        }
        commands->ResourceBarrier({f.Current->GetResource().GetSRVBarrier(),f.Previous->GetResource().GetSRVBarrier()});
    }
    bool countsWritten=false;
    if(d.CountsGpu) for(auto& [instance,chunk]:d.Chunks)
    {
        if(chunk.Active||instance>=r.GeneratedIndexCounts->GetElementCount()) continue;
        commands->SetComputeRootSignature(d.GrassRoot.get()); commands->SetComputeRootConstantBufferView(0,d.Constants[r.CurrFrameConstantsIndex]->GetElementAddress(chunk.ConstantIndex));
        commands->SetComputeSRV(0,d.Neutral->GetSRV()->GPUHandle); commands->SetComputeSRV(1,d.Neutral->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0,r.GeometryBuffer->VertexUAV()->GPUHandle); commands->SetComputeUAV(1,r.GeometryBuffer->PreviousUAV()->GPUHandle);
        commands->ResourceBarrier({r.GeneratedIndexCounts->GetResource().GetUnorderedAccessBarrier()}); commands->SetComputeUAV(2,r.GeneratedIndexCounts->GetUAV()->GPUHandle);
        commands->SetPipelineState(d.ClearCount.Get()); commands->Dispatch(1,1,1); commands->ResourceBarrier({r.GeneratedIndexCounts->GetResource().GetUAVBarrier()}); countsWritten=true;
    }
    bool wrote=false;
    for(auto& [instance,chunk]:d.Chunks)
    {
        if(!chunk.Active) continue;
        if(!wrote)
        {
            commands->ResourceBarrier({r.GeometryBuffer->Vertices().GetUnorderedAccessBarrier(),r.GeometryBuffer->PreviousPositions().GetUnorderedAccessBarrier(),r.GeneratedIndexCounts->GetResource().GetUnorderedAccessBarrier()}); wrote=true;
        }
        commands->SetComputeRootSignature(d.GrassRoot.get()); commands->SetComputeRootConstantBufferView(0,d.Constants[r.CurrFrameConstantsIndex]->GetElementAddress(chunk.ConstantIndex));
        auto found=d.Fields.find(chunk.WindEntity); auto* field=chunk.Constants.WindEnabled&&found!=d.Fields.end()?&found->second:nullptr;
        commands->SetComputeSRV(0,field?field->Current->GetSRV()->GPUHandle:d.Neutral->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1,field?field->Previous->GetSRV()->GPUHandle:d.Neutral->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0,r.GeometryBuffer->VertexUAV()->GPUHandle); commands->SetComputeUAV(1,r.GeometryBuffer->PreviousUAV()->GPUHandle); commands->SetComputeUAV(2,r.GeneratedIndexCounts->GetUAV()->GPUHandle);
        commands->SetPipelineState(d.ClearCount.Get()); commands->Dispatch(1,1,1); commands->ResourceBarrier({r.GeneratedIndexCounts->GetResource().GetUAVBarrier()});
        commands->SetPipelineState(d.Generate.Get()); commands->Dispatch((chunk.Constants.TuftCount+63)/64,1,1);
        commands->ResourceBarrier({r.GeneratedIndexCounts->GetResource().GetUAVBarrier()}); chunk.Last=chunk.Constants; chunk.Reset=false;
    }
    if(wrote) commands->ResourceBarrier({r.GeometryBuffer->Vertices().GetGenericReadBarrier(),r.GeometryBuffer->PreviousPositions().GetSRVBarrier(),r.GeneratedIndexCounts->GetResource().GetSRVBarrier()});
    if(countsWritten&&!wrote) commands->ResourceBarrier({r.GeneratedIndexCounts->GetResource().GetSRVBarrier()});
    for(auto& [entity,f]:d.Fields)
    {
        if(!f.Current||!d.Wind.Enabled) continue;
        f.PreviousDomain={f.Description.Center.x,f.Description.Center.z,f.Description.Settings.HalfExtent,0}; f.Reset=false;
    }
    commands->EndPixEvent();
}
void GDX12GrassWindPass::ClearDenendencies()
{
    if(_resources)
    {
        _resources->Device->GetCommandQueue()->Flush();
        for(auto& [instance,chunk]:_data->Chunks) { _resources->IndirectCommandsCache->CopyData(instance,GDX12IndirectDrawArgs{}); _resources->GeometryBuffer->ReleaseGenerated(chunk.Range); }
    }
    _data=std::make_unique<Data>(); GDX12RenderPass::ClearDenendencies();
}
