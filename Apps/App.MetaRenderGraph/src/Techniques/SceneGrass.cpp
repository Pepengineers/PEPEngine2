#include "SceneGrass.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <fstream>

namespace Meta {
SceneGrass::SceneGrass(RenderPipelineContext& c):_resources(c.Primary),
    _scene(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(),c.Runtime) {
    for(UINT mode=0;mode<2;++mode) { _gpu[mode]=std::make_unique<GDX12GrassRenderer>(&_resources,105000); _frames[mode].resize(_resources.FrameConstants.size()); }
}
void SceneGrass::Prepare(RenderPipelineContext& c,float dt) {
    _scene.CollectScene(dt);
    for(auto& gpu:_gpu) gpu->BeginFrame(c.Common,_scene._grass,_scene._wind,_scene._controls,_scene._sources,_scene._deltaSeconds);
    _gpu[1]->PublishPrimitives(_resources.Primitives,&_scene);
}
const std::vector<ID3D12Resource*>& SceneGrass::Resources(UINT slot) const { return _frames[_mode].at(slot).Bindings; }
std::vector<ID3D12Resource*> SceneGrass::Append(Graph& g,UINT slot,const std::vector<ID3D12Resource*>& existing,GDX12Descriptor& materials,const std::vector<GeometryTarget>& targets,bool viewsOnly,const std::function<void(const GeometryTarget&,Graph&,std::vector<ID3D12Resource*>&)>& beforeView) {
    auto& gpu=*_gpu[_mode]; auto& frame=gpu.CurrentFrame(); auto& recorded=_frames[_mode].at(slot);
    if(!viewsOnly){recorded.First=Id(g.Operations.size());recorded.Bindings.clear();recorded.Commands.clear();}
    recorded.Version=gpu._interactionRevision; recorded.Patches=gpu.PatchCount();
    std::vector<ID3D12Resource*> all=existing;
    const auto resource=[&](ID3D12Resource* native) {
        const auto found=std::find(all.begin(),all.end(),native); if(found!=all.end()) return Id(found-all.begin());
        const Id id=Id(all.size()); all.push_back(native); recorded.Bindings.push_back(native);
        D3D12_HEAP_PROPERTIES heap{}; ThrowIfFailed(native->GetHeapProperties(&heap,nullptr));
        const State initial=heap.Type==D3D12_HEAP_TYPE_UPLOAD?State(D3D12_RESOURCE_STATE_GENERIC_READ):heap.Type==D3D12_HEAP_TYPE_READBACK?State(D3D12_RESOURCE_STATE_COPY_DEST):0;
        const bool decays=heap.Type==D3D12_HEAP_TYPE_DEFAULT && native->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;
        g.Resources.push_back({"grass resource "+std::to_string(id),initial,initial,(native->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)!=0,decays});
        return id;
    };
    const auto access=[&](auto* buffer,Mode mode,State state) { return Access{resource(buffer->GetResource().D3DResource.Get()),mode,state}; };
    const auto add=[&](std::string name,std::vector<Access> accesses,std::function<void(ID3D12GraphicsCommandList*)> command) {
        std::vector<Access> unique;
        for(auto item:accesses) {
            const auto found=std::find_if(unique.begin(),unique.end(),[&](const auto& a){return a.Resource==item.Resource;});
            if(found==unique.end()) unique.push_back(item);
            else if(found->Required!=item.Required || found->Usage!=item.Usage) throw std::logic_error("Conflicting frozen grass access");
        }
        const Id id=Id(g.Operations.size());g.Operations.push_back({std::move(name),std::move(unique)});recorded.Commands.emplace_back(id,std::move(command));
    };
    ID3D12DescriptorHeap* heap=_resources.SRV_UAV_Heap->GetHeap().Get();
    if(!viewsOnly) {
    for(const auto& [roots,upload]:frame.RootUploads) {
        const auto dst=roots->GetResource().D3DResource.Get(),src=upload->GetResource().D3DResource.Get(); const auto bytes=UINT64(roots->GetElementCount())*sizeof(XMFLOAT3);
        add("grass root upload",{access(roots,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST),access(upload,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ)},[=](auto* cmd){cmd->CopyBufferRegion(dst,0,src,0,bytes);});
    }
    {
        const auto root=gpu._interactionRoot->GetRootSignature(); const auto pipeline=gpu._interactionPSO;
        const auto cb=frame.InteractionConstants->GetElementAddress(0); const auto sources=frame.Sources->GetSRV()->GPUHandle;
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE,3> uavs={gpu._trail->GetUAV()->GPUHandle,gpu._previousTrail->GetUAV()->GPUHandle,gpu._presence->GetUAV()->GPUHandle};
        const UINT b=gpu._interactionRoot->GetBRootParamIndex(0),t=gpu._interactionRoot->GetTRootParamIndex(0),u=gpu._interactionRoot->GetURootParamIndex(0);
        add("grass interaction version "+std::to_string(recorded.Version),{access(frame.Sources.get(),Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ),access(frame.InteractionConstants.get(),Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ),
            access(gpu._trail.get(),Mode::ReadWrite,UnorderedAccess),access(gpu._previousTrail.get(),Mode::Write,UnorderedAccess),access(gpu._presence.get(),Mode::Write,UnorderedAccess)},
            [=](auto* cmd){cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());cmd->SetComputeRootConstantBufferView(b,cb);cmd->SetComputeRootDescriptorTable(t,sources);for(UINT i=0;i<3;++i)cmd->SetComputeRootDescriptorTable(u+i,uavs[i]);cmd->Dispatch(8,8,1);});
    }
    for(const auto& job:frame.WindJobs) {
        const auto root=gpu._windRoot->GetRootSignature(); const auto pipeline=gpu._windPSO.at(job.Stage);
        const auto cb=job.Constants->GetElementAddress(0); const auto output=job.Output->GetUAV()->GPUHandle;
        const auto input=(job.Input?job.Input:gpu._neutralWind.get())->GetSRV()->GPUHandle;
        const auto pressure=(job.Pressure?job.Pressure:gpu._neutralWind.get())->GetSRV()->GPUHandle;
        const UINT b=gpu._windRoot->GetBRootParamIndex(0),t=gpu._windRoot->GetTRootParamIndex(0),u=gpu._windRoot->GetURootParamIndex(0),groups=(job.Resolution+7)/8;
        std::vector<Access> accesses={access(job.Constants,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ),access(job.Output,Mode::ReadWrite,UnorderedAccess)};
        if(job.Input) accesses.push_back(access(job.Input,Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        if(job.Pressure) accesses.push_back(access(job.Pressure,Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        add("grass wind stage "+std::to_string(job.Stage),accesses,[=](auto* cmd){cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());cmd->SetComputeRootConstantBufferView(b,cb);cmd->SetComputeRootDescriptorTable(t,input);cmd->SetComputeRootDescriptorTable(t+1,pressure);cmd->SetComputeRootDescriptorTable(u,output);cmd->Dispatch(groups,groups,1);});
    }
    }
    auto views=targets;
    if(!viewsOnly) {
    constexpr UINT probeSize=128;
    constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
    if(!recorded.ProbeCameras) recorded.ProbeCameras=std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(_resources.Device,6);
    GDX12LocalLightConstants capture;capture.Position=targets.front().View.Eye;capture.Range=2000;capture.Type=1;BuildLocalLightShadowMatrices(capture);
    for(UINT face=0;face<6;++face) {
        auto& probe=recorded.Probes[face];
        if(!probe.Depth) {
            const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS,probeSize,probeSize,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;
            probe.Depth=_resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON,&clear);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            probe.Dsv.InitAsDSV(probe.Depth.Get(),&dsv,_resources.DSVHeap.get(),_resources.DSVHeap->GetAvailableIndex());
            dsv.Flags=D3D12_DSV_FLAG_READ_ONLY_DEPTH;probe.ReadDepth.InitAsDSV(probe.Depth.Get(),&dsv,_resources.DSVHeap.get(),_resources.DSVHeap->GetAvailableIndex());
            for(UINT color=0;color<5;++color){probe.Color[color]=std::make_unique<GDX12Texture>();GDX12RenderHelpers::InitializeRenderTarget(*probe.Color[color],_resources,probeSize,probeSize,formats[color],{0,0,0,0});}
        }
        Matrix reverse=Matrix::Identity;reverse._33=-1;reverse._43=1;
        const auto matrix=Matrix(capture.ShadowViewProj[face]).Transpose()*reverse;
        GDX12CameraConstants camera{};camera.ViewProj=camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=matrix.Transpose();camera.CameraLocation=capture.Position;
        recorded.ProbeCameras->CopyData(face,camera);
        GeometryTarget depth;
        depth.View={GDX12GeometryStage::Depth,matrix,capture.Position,recorded.ProbeCameras->GetElementAddress(face)};
        depth.View.Key=(3ull<<60)|face;depth.View.FrameSlot=slot;depth.View.DepthFormat=DXGI_FORMAT_D32_FLOAT;
        depth.Camera=resource(recorded.ProbeCameras->GetResource().D3DResource.Get());
        depth.Width=depth.Height=probeSize;depth.Depth=resource(probe.Depth.Get());depth.Dsv=probe.Dsv.CPUHandle;
        auto gbuffer=depth;gbuffer.View.Stage=GDX12GeometryStage::GBuffer;gbuffer.View.ColorTargetCount=5;gbuffer.Dsv=probe.ReadDepth.CPUHandle;
        std::vector<Access> clear={{depth.Depth,Mode::Write,D3D12_RESOURCE_STATE_DEPTH_WRITE}};
        for(UINT color=0;color<5;++color){const Id id=resource(probe.Color[color]->GetResource()->D3DResource.Get());gbuffer.Colors.push_back(id);gbuffer.Rtvs.push_back(probe.Color[color]->GetRTV()->CPUHandle);gbuffer.View.ColorFormats[color]=formats[color];clear.push_back({id,Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET});}
        const auto dsv=probe.Dsv.CPUHandle; const auto rtvs=gbuffer.Rtvs;
        add("grass probe clear face "+std::to_string(face),clear,[=](auto* cmd){cmd->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);const float zero[4]={};for(auto rtv:rtvs)cmd->ClearRenderTargetView(rtv,zero,0,nullptr);});
        views.push_back(depth);views.push_back(gbuffer);
    }
    }
    for(const auto& target:views) {
      if(beforeView)beforeView(target,g,all);
      for(UINT patch=0;patch<gpu.PatchCount();++patch) {
        if((target.View.Key>>60)==3 && !gpu._grass.at(gpu._drawOrder.at(patch)).Source.CaptureReflections)continue;
        gpu.PrepareView(target.View,patch,&materials); const auto* output=gpu.FindOutput(target.View,patch); if(!output) continue;
        const auto& state=gpu._grass.at(output->Owner);
        const auto root=gpu._computeRoot->GetRootSignature(); const auto clear=gpu._clearPSO,expand=gpu._expandPSO;
        const auto cb=output->Constants->GetElementAddress(output->ConstantIndex);
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE,5> srvs={state.Roots[slot]->GetSRV()->GPUHandle,output->CurrentWind->GetSRV()->GPUHandle,output->PreviousWind->GetSRV()->GPUHandle,gpu._trail->GetSRV()->GPUHandle,gpu._previousTrail->GetSRV()->GPUHandle};
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE,3> uavs={gpu._vertices->GetUAV()->GPUHandle,gpu._indices->GetUAV()->GPUHandle,gpu._arguments->GetUAV()->GPUHandle};
        const UINT b=gpu._computeRoot->GetBRootParamIndex(0),t=gpu._computeRoot->GetTRootParamIndex(0),u=gpu._computeRoot->GetURootParamIndex(0),groups=(GrassBladeCount(state.Source)+63)/64;
        const auto bind=[=](ID3D12GraphicsCommandList* cmd){cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(root.Get());cmd->SetComputeRootConstantBufferView(b,cb);for(UINT i=0;i<5;++i)cmd->SetComputeRootDescriptorTable(t+i,srvs[i]);for(UINT i=0;i<3;++i)cmd->SetComputeRootDescriptorTable(u+i,uavs[i]);};
        std::vector<Access> accesses={access(output->Constants,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ),access(state.Roots[slot].get(),Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            access(output->CurrentWind,Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),access(output->PreviousWind,Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            access(gpu._trail.get(),Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),access(gpu._previousTrail.get(),Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            access(gpu._vertices.get(),Mode::Write,UnorderedAccess),access(gpu._indices.get(),Mode::Write,UnorderedAccess),access(gpu._arguments.get(),Mode::ReadWrite,UnorderedAccess)};
        const std::string name="grass view "+std::to_string(target.View.Key)+" patch "+std::to_string(patch);
        add(name+" clear arguments",accesses,[=](auto* cmd){bind(cmd);cmd->SetPipelineState(clear.Get());cmd->Dispatch(1,1,1);});
        add(name+" expand",accesses,[=](auto* cmd){bind(cmd);cmd->SetPipelineState(expand.Get());cmd->Dispatch(groups,1,1);});
        const auto binding=output->Binding; const auto batch=output->Batch;
        std::vector<Access> draw={access(gpu._vertices.get(),Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),access(gpu._indices.get(),Mode::Read,D3D12_RESOURCE_STATE_INDEX_BUFFER),access(gpu._arguments.get(),Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            access(output->Constants,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ),{2,Mode::Read,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
            {target.Depth,target.View.Stage==GDX12GeometryStage::GBuffer?Mode::Read:Mode::Write,State(target.View.Stage==GDX12GeometryStage::GBuffer?D3D12_RESOURCE_STATE_DEPTH_READ:D3D12_RESOURCE_STATE_DEPTH_WRITE)}};
        draw.insert(draw.end(),target.AdditionalReads.begin(),target.AdditionalReads.end());
        for(auto color:target.Colors) draw.push_back({color,Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET});
        draw.push_back(access(_resources.FrameConstants[slot]->MainCB.get(),Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ));
        if(target.Camera!=Invalid){D3D12_HEAP_PROPERTIES heapType{};ThrowIfFailed(all.at(target.Camera)->GetHeapProperties(&heapType,nullptr));draw.push_back({target.Camera,Mode::Read,heapType.Type==D3D12_HEAP_TYPE_UPLOAD?State(D3D12_RESOURCE_STATE_GENERIC_READ):State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});}
        add(name+" ExecuteIndirect",draw,[=](auto* cmd){
            cmd->SetDescriptorHeaps(1,&heap);cmd->SetGraphicsRootSignature(binding.Root->GetRootSignature().Get());cmd->SetPipelineState(binding.Pipeline.Get());
            for(const auto& cbv:binding.CBVs)cmd->SetGraphicsRootConstantBufferView(binding.Root->GetBRootParamIndex(cbv.first),cbv.second);
            for(const auto& srv:binding.SRVs)cmd->SetGraphicsRootDescriptorTable(binding.Root->GetTRootParamIndex(srv.first),srv.second);
            D3D12_VIEWPORT vp{0,0,float(target.Width),float(target.Height),0,1};D3D12_RECT rect{0,0,LONG(target.Width),LONG(target.Height)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&rect);
            cmd->OMSetRenderTargets(UINT(target.Rtvs.size()),target.Rtvs.data(),FALSE,&target.Dsv);cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->IASetIndexBuffer(&batch.IndexBuffer);
            cmd->ExecuteIndirect(batch.Draw.Signature,1,batch.Draw.Arguments,0,nullptr,0);
        });
    }
    }
    if(viewsOnly)return all;
    std::vector<ID3D12Resource*> fields={gpu._trail->GetResource().D3DResource.Get(),gpu._previousTrail->GetResource().D3DResource.Get(),gpu._presence->GetResource().D3DResource.Get()};
    for(const auto& [id,wind]:gpu._wind) for(const auto& velocity:wind.Velocity) fields.push_back(velocity->GetResource().D3DResource.Get());
    if(gpu._arguments) fields.push_back(gpu._arguments->GetResource().D3DResource.Get());
    UINT64 total=0;std::vector<UINT64> sizes;for(auto* field:fields){sizes.push_back(field->GetDesc().Width);total+=sizes.back();}
    if(recorded.Bytes!=total){recorded.Bytes=total;recorded.Readback=_resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(total),D3D12_RESOURCE_STATE_COPY_DEST);}
    std::vector<Access> readback={{resource(recorded.Readback.Get()),Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}};
    for(auto* field:fields)readback.push_back({resource(field),Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE});
    const auto destination=recorded.Readback.Get();add("grass field/version readback",readback,[=](auto* cmd){UINT64 offset=0;for(UINT i=0;i<fields.size();++i){cmd->CopyBufferRegion(destination,offset,fields[i],0,sizes[i]);offset+=sizes[i];}});
    std::vector<ID3D12Resource*> images;
    for(const auto& probe:recorded.Probes){images.push_back(probe.Depth.Get());for(const auto& color:probe.Color)images.push_back(color->GetResource()->D3DResource.Get());}
    if(!recorded.ProbeReadback){UINT64 offset=0;for(auto* image:images){const auto desc=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;_resources.Device->GetDevice()->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);fp.Offset=offset;recorded.ProbeFootprints.push_back(fp);offset=(offset+bytes+511)&~UINT64(511);}recorded.ProbeBytes=offset;recorded.ProbeReadback=_resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(offset),D3D12_RESOURCE_STATE_COPY_DEST);}
    std::vector<Access> imageReads={{resource(recorded.ProbeReadback.Get()),Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}};for(auto* image:images)imageReads.push_back({resource(image),Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE});
    const auto probeReadback=recorded.ProbeReadback.Get();const auto footprints=recorded.ProbeFootprints;
    add("grass diagnostic probe GBuffer readback",imageReads,[=](auto* cmd){for(UINT i=0;i<images.size();++i){D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=probeReadback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[i];D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=images[i];src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}});
    return all;
}
bool SceneGrass::Record(UINT slot,Id id,ID3D12GraphicsCommandList* cmd) const {
    const auto& frame=_frames[_mode].at(slot);const auto found=std::find_if(frame.Commands.begin(),frame.Commands.end(),[id](const auto& entry){return entry.first==id;});if(found==frame.Commands.end())return false;found->second(cmd);return true;
}
std::vector<uint8_t> SceneGrass::Readback(UINT slot,const std::string& stem) const {
    const auto& frame=_frames[_mode].at(slot);void* mapped=nullptr;D3D12_RANGE read{0,SIZE_T(frame.Bytes)},none{};ThrowIfFailed(frame.Readback->Map(0,&read,&mapped));
    const auto* bytes=static_cast<const uint8_t*>(mapped);std::vector<uint8_t> result(bytes,bytes+frame.Bytes);
    if(!stem.empty()){std::ofstream out(stem+"-grass.bin",std::ios::binary);out.write(reinterpret_cast<const char*>(bytes),frame.Bytes);std::ofstream meta(stem+"-grass.txt");meta<<"version="<<frame.Version<<" patches="<<frame.Patches<<" bytes="<<frame.Bytes<<"\n";}
    frame.Readback->Unmap(0,&none);
    D3D12_RANGE probeRange{0,SIZE_T(frame.ProbeBytes)};ThrowIfFailed(frame.ProbeReadback->Map(0,&probeRange,&mapped));bytes=static_cast<const uint8_t*>(mapped);
    for(UINT i=0;i<frame.ProbeFootprints.size();++i){const auto& fp=frame.ProbeFootprints[i];const UINT pixelBytes=i%6<3?4:8;for(UINT y=0;y<128;++y){const auto* row=bytes+fp.Offset+UINT64(y)*fp.Footprint.RowPitch;result.insert(result.end(),row,row+128*pixelBytes);}}
    if(!stem.empty())for(UINT face=0;face<6;++face){const auto& fp=frame.ProbeFootprints[face*6];UINT covered=0;for(UINT y=0;y<128;++y)for(UINT x=0;x<128;++x){float depth;std::memcpy(&depth,bytes+fp.Offset+UINT64(y)*fp.Footprint.RowPitch+x*4,4);covered+=depth>0;}std::ofstream meta(stem+"-probe-"+std::to_string(face)+".txt");meta<<"covered="<<covered<<"\n";const auto& color=frame.ProbeFootprints[face*6+1];std::ofstream ppm(stem+"-probe-"+std::to_string(face)+".ppm",std::ios::binary);ppm<<"P6\n128 128\n255\n";for(UINT y=0;y<128;++y)for(UINT x=0;x<128;++x)ppm.write(reinterpret_cast<const char*>(bytes+color.Offset+UINT64(y)*color.Footprint.RowPitch+x*4),3);}
    frame.ProbeReadback->Unmap(0,&none);return result;
}
}
