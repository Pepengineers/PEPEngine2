#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Common/ConsoleVariables.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace
{
    bool Quality(int value) { return value >= 0 && value <= 3; }
    bool Mode(int value) { return value == 0 || value == 1; }
    bool Resolution(int value) { return value == -1 || (value >= 16 && value <= 1024 && (value & (value-1)) == 0); }
    bool Faces(int value) { return value == -1 || (value >= 1 && value <= 6); }
    static AutoConsoleVariable<bool> enabled(L"Render.Reflection.Enabled",true,L"Enable reflection probe capture");
    static AutoConsoleVariable<int> quality(L"Render.Reflection.Quality",2,L"0 Off, 1 Low, 2 Mid, 3 High",EConsoleVariableFlags::Default,Quality);
    static AutoConsoleVariable<int> mode(L"Render.Reflection.Implementation",0,L"0 dynamic, 1 baked static color/depth plus dynamic geometry",EConsoleVariableFlags::Default,Mode);
    static AutoConsoleVariable<int> resolution(L"Render.Reflection.Resolution",-1,L"-1 quality preset, or power of two 16..1024",EConsoleVariableFlags::Default,Resolution);
    static AutoConsoleVariable<int> faces(L"Render.Reflection.FacesPerFrame",-1,L"-1 quality preset, or 1..6 capture faces per probe/frame",EConsoleVariableFlags::Default,Faces);
    struct ShadowParameters { XMFLOAT4X4 ViewProjection; float TexelWorld; UINT Size; XMFLOAT2 Padding = {}; };
    static_assert(sizeof(ShadowParameters)==80);
    uint64_t Hash(uint64_t hash,const void* data,size_t bytes)
    {
        const auto* source=static_cast<const unsigned char*>(data);
        for(size_t i=0;i<bytes;++i) hash=(hash^source[i])*1099511628211ull;
        return hash;
    }
    template<class T> void HashValue(uint64_t& hash,const T& value) { hash=Hash(hash,&value,sizeof(value)); }
    uint64_t LightingHash(GDX12DeviceResources& resources)
    {
        uint64_t result=1469598103934665603ull;
        HashValue(result,resources.Lighting.LightDirection); HashValue(result,resources.Lighting.LightRadiance);
        const UINT flags=resources.Lighting.SunShadowFlags&1u;
        HashValue(result,flags); HashValue(result,resources.Lighting.SunShadowRevision);
        for(const auto& light:resources.LocalLights)
        {
            HashValue(result,light.EntityId); HashValue(result,light.Position); HashValue(result,light.Range);
            HashValue(result,light.Direction); HashValue(result,light.InnerCos); HashValue(result,light.OuterCos);
            HashValue(result,light.Radiance); HashValue(result,light.Type); HashValue(result,light.ShadowFlags);
            HashValue(result,light.ShadowRevision); HashValue(result,light.ShadowFirstSlice);
        }
        return result;
    }
    struct Cube
    {
        GDX12Texture Texture;
        std::array<GDX12Descriptor,6> Views;
        std::vector<std::unique_ptr<GDX12Descriptor>> Mips;
    };
    std::unique_ptr<Cube> CreateCube(GDX12DeviceResources& resources,UINT size,bool depth,UINT mips=1,bool filtered=false)
    {
        auto cube=std::make_unique<Cube>();
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const DXGI_FORMAT format=depth?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R16G16B16A16_FLOAT;
        const D3D12_RESOURCE_FLAGS flags=depth?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:
            (filtered?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto resource=CD3DX12_RESOURCE_DESC::Tex2D(format,size,size,6,static_cast<UINT16>(mips),1,0,flags);
        D3D12_CLEAR_VALUE clear={}; clear.Format=depth?DXGI_FORMAT_D32_FLOAT:format; if(!depth) clear.Color[3]=1;
        GDX12TextureDesc desc; desc.Width=desc.Height=size; desc.Format=depth?DXGI_FORMAT_D32_FLOAT:format;
        ThrowIfFailed(resources.Device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,
            D3D12_RESOURCE_STATE_COMMON,filtered?nullptr:&clear,IID_PPV_ARGS(&desc.ExternalResource)));
        desc.CreateSRV=!depth;
        if(!depth)
        {
            desc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get(); desc.SRVHeapIndex=desc.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex,TextureCube_RangeLength);
            desc.SRVDesc.Format=format; desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.SRVDesc.TextureCube.MipLevels=mips;
        }
        cube->Texture.Initialize(desc);
        if(filtered)
            for(UINT mip=0;mip<mips;++mip)
            {
                auto view=std::make_unique<GDX12Descriptor>(); D3D12_UNORDERED_ACCESS_VIEW_DESC uav={};
                uav.Format=format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uav.Texture2DArray.ArraySize=6; uav.Texture2DArray.MipSlice=mip;
                view->InitAsUAV(desc.ExternalResource.Get(),nullptr,&uav,resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
                cube->Mips.push_back(std::move(view));
            }
        else for(UINT face=0;face<6;++face)
        {
            if(depth)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC view={}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.ArraySize=1; view.Texture2DArray.FirstArraySlice=face;
                cube->Views[face].InitAsDSV(desc.ExternalResource.Get(),&view,resources.DSVHeap.get(),resources.DSVHeap->GetAvailableIndex());
            }
            else
            {
                D3D12_RENDER_TARGET_VIEW_DESC view={}; view.Format=format; view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.ArraySize=1; view.Texture2DArray.FirstArraySlice=face;
                cube->Views[face].InitAsRTV(desc.ExternalResource.Get(),&view,resources.RTVHeap.get(),resources.RTVHeap->GetAvailableIndex());
            }
        }
        return cube;
    }
    std::unique_ptr<GDX12Texture> CreateShadow(GDX12DeviceResources& resources,UINT size)
    {
        auto texture=std::make_unique<GDX12Texture>();
        GDX12TextureDesc desc; desc.Width=desc.Height=size; desc.Format=desc.DSVDesc.Format=DXGI_FORMAT_D32_FLOAT;
        desc.CreateDSV=true; desc.DSVHeap=resources.DSVHeap.get(); desc.DSVHeapIndex=desc.DSVHeap->GetAvailableIndex(); desc.DSVDesc.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        desc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get(); desc.SRVHeapIndex=desc.SRV_UAV_Heap->GetAvailableIndex(ShadowMaps_StartIndex,ShadowMaps_RangeLength);
        desc.SRVDesc.Format=DXGI_FORMAT_R32_FLOAT; desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Texture2D.MipLevels=1; desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.ClearValue={1,0,0,0};
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto resource=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS,size,size,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear={}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=1;
        ThrowIfFailed(resources.Device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COMMON,&clear,IID_PPV_ARGS(&desc.ExternalResource)));
        texture->Initialize(desc); return texture;
    }
    template<class T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(GDX12DeviceResources& resources,UINT count)
    {
        auto buffer=std::make_unique<GDX12UploadBuffer<T>>(resources.Device,count,EBufferType::Default,false);
        buffer->CreateUAV(resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources)); return buffer;
    }
    struct Frame
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> Cameras;
        std::unique_ptr<GDX12UploadBuffer<ShadowParameters>> Shadow;
        GDX12VisibilityBuffers Visibility;
        UINT Capacity=0;
    };
    struct Entry
    {
        GDX12ReflectionProbeDesc Description,PublishedDescription;
        GDX12ReflectionProbeMode Mode;
        UINT Size=0,MipCount=0,NextFace=0,PublishedIndex=0;
        uint64_t StaticRevision=0,ShadeRevision=0,LightHash=0,EnvironmentRevision=0,Publication=0;
        bool Published=false;
        std::array<bool,6> StaticValid={};
        std::unique_ptr<Cube> WorkColor,WorkDepth,StaticColor,StaticDepth,Fallback;
        std::array<std::unique_ptr<Cube>,2> Filtered;
        std::unique_ptr<GDX12Texture> Shadow;
        std::vector<Frame> Frames;
    };
    bool SameDescription(const GDX12ReflectionProbeDesc& a,const GDX12ReflectionProbeDesc& b)
    {
        return a.Entity==b.Entity && a.Position==b.Position && a.Extents==b.Extents && a.NearPlane==b.NearPlane && a.FarPlane==b.FarPlane && a.Mode==b.Mode && a.Revision==b.Revision;
    }
}

struct GDX12ReflectionProbePass::Data
{
    std::map<uint32_t,std::unique_ptr<Entry>> Entries;
    std::vector<GDX12ReflectionProbeDesc> Submitted;
    std::vector<GDX12PublishedReflectionProbe> Published;
    uint64_t World=0,SubmittedWorld=0,StaticRevision=0,ShadeRevision=0;
    bool Active=false,Initialized=false;
    UINT FaceBudget=6,StaticFaces=0;
    GDX12GPUCullingPass Culling;
    std::unique_ptr<GDX12RootSignature> Root,FilterRoot;
    ComPtr<ID3D12PipelineState> CapturePipeline,SkyPipeline,ShadowPipeline,FilterPipeline;
    ComPtr<ID3D12CommandSignature> Signature;

    void Initialize(GDX12DeviceResources& resources,RenderPipelineCommonData* common)
    {
        Culling.Setup(&resources,nullptr,common);
        GDX12RootSignatureDesc root; root.Constants.push_back(1); root.NumSingleCBVSlots=3; root.NumSingleSRVSlots=10;
        root.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength)); root.StaticSamplers=GetStaticSamplers();
        CD3DX12_STATIC_SAMPLER_DESC sampler(6,D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL; sampler.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE; root.StaticSamplers.push_back(sampler);
        Root=std::make_unique<GDX12RootSignature>(resources.Device,root);
        auto& compiler=GDX12ShaderCompiler::GetInstance();
        auto vs=compiler.CompileShader(resources.Device,SHADERS_FOLDER "ReflectionCapture.hlsl",nullptr,"VS","vs");
        auto ps=compiler.CompileShader(resources.Device,SHADERS_FOLDER "ReflectionCapture.hlsl",nullptr,"PS","ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline={}; pipeline.pRootSignature=Root->GetRootSignature().Get();
        pipeline.InputLayout={resources.InputLayouts["Default"].data(),static_cast<UINT>(resources.InputLayouts["Default"].size())};
        pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); pipeline.RasterizerState.FrontCounterClockwise=FALSE;
        pipeline.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT); pipeline.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL; pipeline.SampleMask=UINT_MAX;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pipeline.NumRenderTargets=1; pipeline.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipeline.DSVFormat=DXGI_FORMAT_D32_FLOAT; pipeline.SampleDesc.Count=1;
        ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&CapturePipeline)));
        auto shadowPS=compiler.CompileShader(resources.Device,SHADERS_FOLDER "ReflectionCapture.hlsl",nullptr,"ShadowPS","ps");
        pipeline.PS={shadowPS->GetBufferPointer(),shadowPS->GetBufferSize()}; pipeline.NumRenderTargets=0; pipeline.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;
        pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL; pipeline.RasterizerState.FrontCounterClockwise=TRUE;
        pipeline.RasterizerState.DepthBias=250; pipeline.RasterizerState.SlopeScaledDepthBias=1.5f;
        ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&ShadowPipeline)));
        auto skyVS=compiler.CompileShader(resources.Device,SHADERS_FOLDER "ReflectionCapture.hlsl",nullptr,"SkyVS","vs");
        auto skyPS=compiler.CompileShader(resources.Device,SHADERS_FOLDER "ReflectionCapture.hlsl",nullptr,"SkyPS","ps");
        pipeline.VS={skyVS->GetBufferPointer(),skyVS->GetBufferSize()}; pipeline.PS={skyPS->GetBufferPointer(),skyPS->GetBufferSize()};
        pipeline.InputLayout={}; pipeline.NumRenderTargets=1; pipeline.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        pipeline.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.DepthStencilState.DepthEnable=FALSE; pipeline.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&SkyPipeline)));
        D3D12_INDIRECT_ARGUMENT_DESC args[2]={}; args[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT; args[0].Constant.Num32BitValuesToSet=1; args[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC signature={}; signature.ByteStride=sizeof(GDX12IndirectDrawArgs); signature.NumArgumentDescs=2; signature.pArgumentDescs=args;
        ThrowIfFailed(resources.Device->GetDevice()->CreateCommandSignature(&signature,Root->GetRootSignature().Get(),IID_PPV_ARGS(&Signature)));
        GDX12RootSignatureDesc filter; filter.Constants.push_back(2); filter.NumSingleSRVSlots=1; filter.NumSingleUAVSlots=1;
        filter.StaticSamplers.emplace_back(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        FilterRoot=std::make_unique<GDX12RootSignature>(resources.Device,filter);
        D3D_SHADER_MACRO macros[]={{"PROBE_BASE_MIP_ONLY","1"},{nullptr,nullptr}};
        auto cs=compiler.CompileShader(resources.Device,SHADERS_FOLDER "EnvironmentIBL.hlsl",macros,"PrefilterCS","cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute={}; compute.pRootSignature=FilterRoot->GetRootSignature().Get(); compute.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        ThrowIfFailed(resources.Device->GetDevice()->CreateComputePipelineState(&compute,IID_PPV_ARGS(&FilterPipeline)));
        Initialized=true;
    }
    std::unique_ptr<Entry> Allocate(GDX12DeviceResources& resources,const GDX12ReflectionProbeDesc& desc,GDX12ReflectionProbeMode captureMode,UINT size)
    {
        auto entry=std::make_unique<Entry>(); entry->Description=desc; entry->Mode=captureMode; entry->Size=size; entry->MipCount=1+static_cast<UINT>(std::log2(size));
        entry->WorkColor=CreateCube(resources,size,false); entry->WorkDepth=CreateCube(resources,size,true);
        for(auto& texture:entry->Filtered) texture=CreateCube(resources,size,false,entry->MipCount,true);
        if(captureMode==GDX12ReflectionProbeMode::BakedStatic) { entry->StaticColor=CreateCube(resources,size,false); entry->StaticDepth=CreateCube(resources,size,true); }
        entry->Shadow=CreateShadow(resources,std::clamp(size*4u,256u,2048u));
        entry->Frames.resize(resources.FrameConstants.size());
        for(auto& frame:entry->Frames)
        {
            frame.Cameras=std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(resources.Device,7);
            frame.Shadow=std::make_unique<GDX12UploadBuffer<ShadowParameters>>(resources.Device,1);
        }
        return entry;
    }
    void EnsureVisibility(GDX12DeviceResources& resources,Frame& frame)
    {
        const UINT count=(std::max)(resources.IndirectCommandsCache->GetElementCount(),1u);
        if(frame.Capacity>=count) return;
        frame.Visibility.VisibleOpaqueCommandsCache=Buffer<GDX12IndirectDrawArgs>(resources,count);
        frame.Visibility.VisibleTransparentCommandsCache=Buffer<GDX12IndirectDrawArgs>(resources,count);
        if(!frame.Visibility.OpaqueDrawCounter) frame.Visibility.OpaqueDrawCounter=Buffer<UINT>(resources,1);
        if(!frame.Visibility.TransparentDrawCounter) frame.Visibility.TransparentDrawCounter=Buffer<UINT>(resources,1);
        frame.Capacity=count;
    }
};

GDX12ReflectionProbePass::GDX12ReflectionProbePass():_data(std::make_unique<Data>())
{ _numInputs=5; _flags=RENDER_PASS_FLAG_USE_GEOMETRY|RENDER_PASS_FLAG_USE_MATERIALS|RENDER_PASS_FLAG_USE_INSTANCES; }
GDX12ReflectionProbePass::~GDX12ReflectionProbePass()
{ if(_data && !_data->Entries.empty() && _resources) _resources->Device->GetCommandQueue()->Flush(); }
void GDX12ReflectionProbePass::Initialize() { if(_inputs.size()!=5) throw std::invalid_argument("Probe capture requires global IBL, sky and local shadows"); }
void GDX12ReflectionProbePass::Submit(uint64_t world,std::vector<GDX12ReflectionProbeDesc> probes,uint64_t staticRevision,uint64_t shadingRevision)
{ _data->SubmittedWorld=world; _data->Submitted=std::move(probes); _data->StaticRevision=staticRevision; _data->ShadeRevision=shadingRevision; }
const std::vector<GDX12PublishedReflectionProbe>& GDX12ReflectionProbePass::GetPublished() const { return _data->Published; }
bool ReflectionProbeLightingEnabled() { return enabled.GetValue() && quality.GetValue() != 0; }
UINT GDX12ReflectionProbePass::GetSubmittedCount() const { return static_cast<UINT>(_data->Submitted.size()); }
UINT GDX12ReflectionProbePass::GetLastFrameStaticFaces() const { return _data->StaticFaces; }
void GDX12ReflectionProbePass::PrepareFrame()
{
    auto& d=*_data; d.Published.clear(); d.StaticFaces=0;
    bool flushed=false;
    const auto flush=[&] { if(!flushed) { _resources->Device->GetCommandQueue()->Flush(); flushed=true; } };
    if(d.World!=d.SubmittedWorld) { flush(); d.Entries.clear(); d.World=d.SubmittedWorld; }
    for(auto it=d.Entries.begin();it!=d.Entries.end();)
        if(std::none_of(d.Submitted.begin(),d.Submitted.end(),[&](const auto& probe) {return probe.Entity==it->first;})) { flush(); it=d.Entries.erase(it); } else ++it;
    d.Active=ReflectionProbeLightingEnabled();
    if(!d.Active || d.Submitted.empty()) return;
    if(!d.Initialized) d.Initialize(*_resources,_commonData);
    constexpr UINT sizes[]={0,64,128,256},budgets[]={0,1,2,6};
    const UINT size=resolution.GetValue()<0?sizes[quality.GetValue()]:static_cast<UINT>(resolution.GetValue());
    d.FaceBudget=faces.GetValue()<0?budgets[quality.GetValue()]:static_cast<UINT>(faces.GetValue());
    uint64_t lightHash=LightingHash(*_resources);
    HashValue(lightHash,_commonData->EnvironmentRevision);
    HashValue(lightHash,_commonData->EnvironmentLightingRevision);
    const bool dynamicShadows=(_resources->Lighting.SunShadowFlags&1u)!=0 || std::any_of(_resources->LocalLights.begin(),_resources->LocalLights.end(),[](const auto& light) {return (light.ShadowFlags&1u)!=0;});
    for(const auto& desc:d.Submitted)
    {
        if(!std::isfinite(desc.NearPlane)||!std::isfinite(desc.FarPlane)||desc.NearPlane<=0||desc.FarPlane<=desc.NearPlane||desc.FarPlane>1000000 ||
            !std::isfinite(desc.Position.x)||!std::isfinite(desc.Position.y)||!std::isfinite(desc.Position.z)||
            !std::isfinite(desc.Extents.x)||!std::isfinite(desc.Extents.y)||!std::isfinite(desc.Extents.z)||desc.Extents.x<=0||desc.Extents.y<=0||desc.Extents.z<=0 ||
            desc.Mode<GDX12ReflectionProbeMode::Inherit||desc.Mode>GDX12ReflectionProbeMode::BakedStatic) throw std::invalid_argument("Invalid reflection probe component");
        auto captureMode=desc.Mode==GDX12ReflectionProbeMode::Inherit?static_cast<GDX12ReflectionProbeMode>(mode.GetValue()):desc.Mode;
        auto& pointer=d.Entries[desc.Entity];
        if(!pointer || pointer->Size!=size || pointer->Mode!=captureMode)
        {
            flush(); auto replacement=d.Allocate(*_resources,desc,captureMode,size);
            if(pointer)
            {
                replacement->Publication=pointer->Publication; replacement->PublishedDescription=pointer->PublishedDescription;
                replacement->Fallback=pointer->Published?std::move(pointer->Filtered[pointer->PublishedIndex]):std::move(pointer->Fallback);
            }
            pointer=std::move(replacement);
        }
        auto& entry=*pointer;
        if(entry.Published && entry.Fallback) { flush(); entry.Fallback.reset(); }
        if(!SameDescription(entry.Description,desc)||entry.StaticRevision!=d.StaticRevision || entry.EnvironmentRevision!=_commonData->EnvironmentRevision)
        { entry.StaticValid.fill(false); entry.NextFace=0; }
        if(entry.LightHash!=lightHash || (dynamicShadows&&entry.ShadeRevision!=d.ShadeRevision)) entry.StaticValid.fill(false);
        entry.Description=desc; entry.StaticRevision=d.StaticRevision; entry.ShadeRevision=d.ShadeRevision; entry.LightHash=lightHash; entry.EnvironmentRevision=_commonData->EnvironmentRevision;
        auto& frame=entry.Frames[_resources->CurrFrameConstantsIndex];
        if(frame.Capacity<(std::max)(_resources->IndirectCommandsCache->GetElementCount(),1u)) { flush(); d.EnsureVisibility(*_resources,frame); }
    }
}

void GDX12ReflectionProbePass::Execute(GDX12CommandList* commands)
{
    auto& d=*_data; if(!d.Active||d.Submitted.empty()) return;
    auto& scene=*_resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    const UINT drawCount=_resources->IndirectCommandsCache->GetElementCount();
    commands->BeginPixEvent("Reflection probe capture",Colors::CadetBlue);
    for(auto& [id,pointer]:d.Entries)
    {
        auto& entry=*pointer; auto& frame=entry.Frames[_resources->CurrFrameConstantsIndex];
        const auto& desc=entry.Description;
        const Vector3 directions[]={Vector3::UnitX,-Vector3::UnitX,Vector3::UnitY,-Vector3::UnitY,Vector3::UnitZ,-Vector3::UnitZ};
        const Vector3 ups[]={Vector3::UnitY,Vector3::UnitY,-Vector3::UnitZ,Vector3::UnitZ,Vector3::UnitY,Vector3::UnitY};
        Matrix projection=Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2,1,desc.NearPlane,desc.FarPlane);
        // DX cubemap faces are left-handed; mirror RH projection X and reverse capture winding.
        projection._11=-projection._11;
        projection._33=desc.NearPlane/(desc.FarPlane-desc.NearPlane); projection._43=projection._33*desc.FarPlane;
        for(UINT face=entry.NextFace;face<(std::min)(entry.NextFace+d.FaceBudget,6u);++face)
        {
            const Matrix view=Matrix::CreateLookAt(desc.Position,desc.Position+directions[face],ups[face]);
            GDX12CameraConstants camera={}; camera.View=view.Transpose(); camera.ViewProj=camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=(view*projection).Transpose();
            camera.InvViewProj=(view*projection).Invert().Transpose(); camera.CameraLocation=desc.Position; camera.NearPlane=desc.NearPlane; camera.FarPlane=desc.FarPlane;
            frame.Cameras->CopyData(face,camera);
        }
        Vector3 lightDirection(_resources->Lighting.LightDirection); if(lightDirection.LengthSquared()<1e-8f) lightDirection=Vector3(0,-1,0); lightDirection.Normalize();
        const float radius=desc.FarPlane*std::sqrt(3.0f);
        const Vector3 lightPosition=desc.Position-lightDirection*(2*radius);
        const Matrix shadowView=Matrix::CreateLookAt(lightPosition,desc.Position,std::abs(lightDirection.y)>.99f?Vector3::UnitZ:Vector3::UnitY);
        const Matrix shadowProjection=Matrix::CreateOrthographic(2*radius,2*radius,.1f,4*radius);
        GDX12CameraConstants shadowCamera={}; shadowCamera.View=shadowView.Transpose(); shadowCamera.ViewProj=(shadowView*shadowProjection).Transpose();
        shadowCamera.CameraLocation=lightPosition; shadowCamera.NearPlane=.1f; shadowCamera.FarPlane=4*radius; frame.Cameras->CopyData(6,shadowCamera);
        ShadowParameters shadowParameters={}; shadowParameters.ViewProjection=shadowCamera.ViewProj; shadowParameters.TexelWorld=2*radius/entry.Shadow->GetWidth(); shadowParameters.Size=entry.Size;
        frame.Shadow->CopyData(0,shadowParameters);
        const auto bind=[&](UINT camera)
        {
            commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()}); commands->SetGraphicsRootSignature(d.Root.get());
            commands->SetGraphicsRootConstantBufferView(1,scene.MainCB->GetElementAddress(0)); commands->SetGraphicsRootConstantBufferView(2,frame.Cameras->GetElementAddress(camera));
            commands->SetGraphicsRootConstantBufferView(3,frame.Shadow->GetElementAddress(0));
            commands->SetGraphicsSRV(0,scene.MaterialCache->GetSRV()->GPUHandle); commands->SetGraphicsSRV(1,scene.TransformCache->GetSRV()->GPUHandle); commands->SetGraphicsSRV(2,scene.InstanceCache->GetSRV()->GPUHandle);
            if(camera!=6) commands->SetGraphicsSRV(3,entry.Shadow->GetSRV()->GPUHandle);
            for(UINT i=0;i<3;++i) { auto* texture=_inputs[i]->GetTexture(); commands->ResourceBarrier({texture->GetResource()->GetSRVBarrier()}); commands->SetGraphicsSRV(i+4,texture->GetSRV()->GPUHandle); }
            commands->SetGraphicsSRV(7,scene.LocalLightCache->GetSRV()->GPUHandle);
            commands->ResourceBarrier({_inputs[4]->GetTexture()->GetResource()->GetSRVBarrier(),_inputs[3]->GetTexture()->GetResource()->GetSRVBarrier()});
            commands->SetGraphicsSRV(8,_inputs[4]->GetTexture()->GetSRV()->GPUHandle); commands->SetGraphicsSRV(9,_inputs[3]->GetTexture()->GetSRV()->GPUHandle);
            commands->SetGraphicsSRV(10,_resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex)); commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commands->SetGeometryBuffer(_resources->GeometryBuffer.get());
        };
        const auto draw=[&](UINT camera,UINT mask,ID3D12PipelineState* pipeline)
        {
            d.Culling.ExecuteView(commands,frame.Cameras->GetElementAddress(camera),frame.Visibility,mask); bind(camera); commands->SetPipelineState(pipeline);
            commands->ResourceBarrier({frame.Visibility.VisibleOpaqueCommandsCache->GetResource().GetIndirectArgsBarrier(),frame.Visibility.OpaqueDrawCounter->GetResource().GetIndirectArgsBarrier()});
            if(drawCount) commands->ExecuteIndirect(d.Signature.Get(),drawCount,frame.Visibility.VisibleOpaqueCommandsCache->GetResource().D3DResource.Get(),0,frame.Visibility.OpaqueDrawCounter->GetResource().D3DResource.Get(),0);
        };
        if((_resources->Lighting.SunShadowFlags&1u)!=0)
        {
            commands->ResourceBarrier({entry.Shadow->GetResource()->GetDepthWriteBarrier()}); commands->SetRenderTargets({},entry.Shadow.get()); commands->SetViewport(entry.Shadow->GetViewport()); commands->SetScissorRect(entry.Shadow->GetScissorRect());
            commands->GetCommandList()->ClearDepthStencilView(entry.Shadow->GetDSV()->CPUHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr); draw(6,0,d.ShadowPipeline.Get());
        }
        commands->ResourceBarrier({entry.Shadow->GetResource()->GetSRVBarrier()});
        const auto capture=[&](Cube& color,Cube& depth,UINT face,UINT mask,bool clear)
        {
            commands->ResourceBarrier({color.Texture.GetResource()->GetRenderTargetBarrier(),depth.Texture.GetResource()->GetDepthWriteBarrier()});
            commands->GetCommandList()->OMSetRenderTargets(1,&color.Views[face].CPUHandle,FALSE,&depth.Views[face].CPUHandle);
            commands->SetViewport(color.Texture.GetViewport()); commands->SetScissorRect(color.Texture.GetScissorRect());
            if(clear)
            {
                const float black[]={0,0,0,1}; commands->GetCommandList()->ClearRenderTargetView(color.Views[face].CPUHandle,black,0,nullptr);
                commands->GetCommandList()->ClearDepthStencilView(depth.Views[face].CPUHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                bind(face); commands->SetPipelineState(d.SkyPipeline); commands->DrawInstanced(3,1,0,0);
            }
            draw(face,mask,d.CapturePipeline.Get());
        };
        for(UINT count=0;count<d.FaceBudget && entry.NextFace<6;++count,++entry.NextFace)
        {
            const UINT face=entry.NextFace;
            if(entry.Mode==GDX12ReflectionProbeMode::BakedStatic)
            {
                if(!entry.StaticValid[face]) { capture(*entry.StaticColor,*entry.StaticDepth,face,1,true); entry.StaticValid[face]=true; ++d.StaticFaces; }
                commands->ResourceBarrier({entry.StaticColor->Texture.GetResource()->GetCopySourceBarrier(),entry.StaticDepth->Texture.GetResource()->GetCopySourceBarrier(),entry.WorkColor->Texture.GetResource()->GetCopyDestBarrier(),entry.WorkDepth->Texture.GetResource()->GetCopyDestBarrier()});
                for(auto pair:{std::pair<Cube*,Cube*>(entry.StaticColor.get(),entry.WorkColor.get()),{entry.StaticDepth.get(),entry.WorkDepth.get()}})
                {
                    const CD3DX12_TEXTURE_COPY_LOCATION source(pair.first->Texture.GetResource()->D3DResource.Get(),face),target(pair.second->Texture.GetResource()->D3DResource.Get(),face);
                    commands->GetCommandList()->CopyTextureRegion(&target,0,0,0,&source,nullptr);
                }
                capture(*entry.WorkColor,*entry.WorkDepth,face,2,false);
            }
            else capture(*entry.WorkColor,*entry.WorkDepth,face,0,true);
        }
        if(entry.NextFace==6)
        {
            const UINT destination=entry.Published?1-entry.PublishedIndex:0; auto& output=*entry.Filtered[destination];
            commands->ResourceBarrier({entry.WorkColor->Texture.GetResource()->GetSRVBarrier(),output.Texture.GetResource()->GetUnorderedAccessBarrier()});
            commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()}); commands->SetComputeRootSignature(d.FilterRoot.get()); commands->SetPipelineState(d.FilterPipeline);
            commands->SetComputeSRV(0,entry.WorkColor->Texture.GetSRV()->GPUHandle);
            for(UINT mip=0;mip<entry.MipCount;++mip)
            {
                struct Filter { UINT Size; float Roughness; } filter={entry.Size>>mip,static_cast<float>(mip)/(entry.MipCount-1)};
                commands->GetCommandList()->SetComputeRoot32BitConstants(0,2,&filter,0); commands->SetComputeUAV(0,output.Mips[mip]->GPUHandle);
                commands->Dispatch((filter.Size+7)/8,(filter.Size+7)/8,6);
            }
            commands->ResourceBarrier({output.Texture.GetResource()->GetSRVBarrier()});
            entry.Published=true; entry.PublishedIndex=destination; entry.PublishedDescription=desc; entry.PublishedDescription.Mode=entry.Mode; ++entry.Publication; entry.NextFace=0;
        }
        Cube* published=entry.Published?entry.Filtered[entry.PublishedIndex].get():entry.Fallback.get();
        if(published) d.Published.push_back({d.World,entry.PublishedDescription,&published->Texture,entry.Publication,published->Texture.GetResource()->D3DResource->GetDesc().MipLevels});
    }
    commands->EndPixEvent();
}
void GDX12ReflectionProbePass::ClearDenendencies()
{ if(_resources) _resources->Device->GetCommandQueue()->Flush(); GDX12RenderPass::ClearDenendencies(); _data.reset(); }
