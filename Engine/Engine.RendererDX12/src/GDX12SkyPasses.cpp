#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SkyAerialPass.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
    bool Quality(int value) { return value>=0&&value<=3; }
    bool Source(int value) { return value>=0&&value<=2; }
    bool Scale(float value) { return std::isfinite(value)&&value>=0&&value<=100; }
    bool Rotation(float value) { return std::isfinite(value)&&std::abs(value)<=36000; }
    bool Anisotropy(float value) { return std::isfinite(value)&&value>=-.95f&&value<=.95f; }
    bool Distance(float value) { return std::isfinite(value)&&value>=.001f&&value<=1000; }
    AutoConsoleVariable<int> source(L"Render.Sky.Source",1,L"0 Off, 1 SkyBox, 2 SkyAtmosphere; requires matching enabled component",EConsoleVariableFlags::Default,Source);
    AutoConsoleVariable<bool> boxEnabled(L"Render.SkyBox.Enabled",true,L"Enable selected SkyBox");
    AutoConsoleVariable<int> boxQuality(L"Render.SkyBox.Quality",2,L"0 Off, 1 Low, 2 Mid, 3 High",EConsoleVariableFlags::Default,Quality);
    AutoConsoleVariable<float> boxIntensity(L"Render.SkyBox.Intensity",1,L"Multiplier for component SkyBox intensity",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> boxRotation(L"Render.SkyBox.Rotation",0,L"Additional SkyBox Y rotation in degrees",EConsoleVariableFlags::Default,Rotation);
    AutoConsoleVariable<bool> atmosphereEnabled(L"Render.SkyAtmosphere.Enabled",true,L"Enable selected atmosphere component");
    AutoConsoleVariable<int> atmosphereQuality(L"Render.SkyAtmosphere.Quality",2,L"0 Off, 1 Low, 2 Mid, 3 High",EConsoleVariableFlags::Default,Quality);
    AutoConsoleVariable<float> rayleigh(L"Render.SkyAtmosphere.RayleighScale",1,L"Rayleigh scattering coefficient multiplier",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> mie(L"Render.SkyAtmosphere.MieScale",1,L"Mie scattering/extinction coefficient multiplier",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> ozone(L"Render.SkyAtmosphere.OzoneScale",1,L"Ozone absorption coefficient multiplier",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> multi(L"Render.SkyAtmosphere.MultipleScattering",1,L"Multiple scattering factor multiplier",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> sunScale(L"Render.SkyAtmosphere.SunIntensity",1,L"Atmosphere solar irradiance multiplier; direction/color follow the active directional light",EConsoleVariableFlags::Default,Scale);
    AutoConsoleVariable<float> mieG(L"Render.SkyAtmosphere.MieAnisotropy",.8f,L"Mie phase eccentricity; adds value minus 0.8 to component eccentricity",EConsoleVariableFlags::Default,Anisotropy);
    AutoConsoleVariable<bool> aerialEnabled(L"Render.SkyAtmosphere.AerialPerspective",true,L"Apply RGB atmospheric transmittance/scattering to scene color");
    AutoConsoleVariable<float> aerialRange(L"Render.SkyAtmosphere.AerialDistanceKm",32,L"Aerial LUT far range in km; scales component range relative to default 32 km",EConsoleVariableFlags::Default,Distance);

    struct SkyParameters
    {
        XMFLOAT4X4 InverseViewProjection;
        XMFLOAT4 CameraKm,SunDirectionRadius,SunRadiance,RayleighHeight,MieHeight,MieExtinctionG,OzoneMulti,GroundRadius,Options;
        XMUINT4 Resolutions,Resolutions2;
        XMFLOAT4 CameraWorld;
    };
    static_assert(sizeof(SkyParameters)==256);
    static_assert(offsetof(SkyParameters,CameraKm)==64&&offsetof(SkyParameters,Resolutions)==208&&offsetof(SkyParameters,CameraWorld)==240);
    uint64_t Mix(uint64_t hash,const void* data,size_t size)
    { const auto* bytes=static_cast<const unsigned char*>(data); for(size_t i=0;i<size;++i) hash=(hash^bytes[i])*1099511628211ull; return hash; }
    template<class T> void Hash(uint64_t& hash,const T& value) { hash=Mix(hash,&value,sizeof(value)); }
    void CreateTexture(GDX12DeviceResources& resources,GDX12Texture& texture,UINT width,UINT height,UINT depth=1,UINT mips=1,bool cube=false)
    {
        auto* descriptors=resources.SRV_UAV_Heap.get();
        GDX12TextureDesc desc; desc.Width=width; desc.Height=height;
        desc.Format=desc.SRVDesc.Format=desc.UAVDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SRV_UAV_Heap=descriptors;
        desc.SRVHeapIndex=texture.IsInitialized()?texture.GetSRV()->HeapIndex:descriptors->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength);
        desc.UAVHeapIndex=texture.IsInitialized()?texture.GetUAV()->HeapIndex:descriptors->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.CreateUAV=true;
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC resource;
        if(cube)
        {
            resource=CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,width,height,6,static_cast<UINT16>(mips),1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE; desc.SRVDesc.TextureCube.MipLevels=mips;
            desc.UAVDesc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY; desc.UAVDesc.Texture2DArray.ArraySize=6;
        }
        else if(depth>1)
        {
            resource=CD3DX12_RESOURCE_DESC::Tex3D(desc.Format,width,height,static_cast<UINT16>(depth),1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D; desc.SRVDesc.Texture3D.MipLevels=1;
            desc.UAVDesc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D; desc.UAVDesc.Texture3D.WSize=depth;
        }
        else
        {
            resource=CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,width,height,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Texture2D.MipLevels=1;
            desc.UAVDesc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        ThrowIfFailed(resources.Device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&desc.ExternalResource)));
        texture.Initialize(desc);
    }
    void CreateMipViews(GDX12DeviceResources& resources,GDX12Texture& texture,UINT count,std::vector<std::unique_ptr<GDX12Descriptor>>& views)
    {
        for(UINT mip=0;mip<count;++mip)
        {
            if(mip>=views.size()) views.push_back(std::make_unique<GDX12Descriptor>());
            auto& view=*views[mip];
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc={}; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY; desc.Texture2DArray.ArraySize=6; desc.Texture2DArray.MipSlice=mip;
            view.InitAsUAV(texture.GetResource()->D3DResource.Get(),nullptr,&desc,resources.SRV_UAV_Heap.get(),view.HeapIndex!=UINT(-1)?view.HeapIndex:resources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength));
        }
    }
}

struct GDX12EnvironmentIBLPass::Data
{
    std::array<GDX12Texture,4> Outputs;
    GDX12Texture Box,IblSource,Transmittance,MultipleScattering,SkyView,AerialL,AerialT;
    std::unique_ptr<GDX12RootSignature> SkyRoot,FilterRoot;
    std::array<ComPtr<ID3D12PipelineState>,6> SkyPipelines;
    std::array<ComPtr<ID3D12PipelineState>,3> FilterPipelines;
    std::unique_ptr<GDX12UploadBuffer<SkyParameters>> Parameters;
    std::vector<std::unique_ptr<GDX12Descriptor>> SkyMipViews,PrefilterViews,IblMipViews;
    GDX12SkyDesc Submitted;
    SkyParameters Constants={};
    uint64_t Identity=0,Lighting=0,MediumHash=0,RenderedLighting=0,RenderedMedium=0,Diagnostic=0;
    std::filesystem::path LoadedPath;
    uint64_t LoadedRevision=UINT64_MAX;
    UINT EffectiveSource=0,PrefilterSize=128,PrefilterMips=8,SkySize=1,SkyMips=1;
    bool BrdfReady=false,Aerial=false;
    void LoadBox(GDX12DeviceResources&,const std::filesystem::path&);
    void BindSky(GDX12CommandList* commands,GDX12DeviceResources& resources)
    {
        commands->SetDescriptorHeaps({resources.SRV_UAV_Heap.get()}); commands->SetComputeRootSignature(SkyRoot.get());
        commands->SetComputeRootConstantBufferView(1,Parameters->GetElementAddress(resources.CurrFrameConstantsIndex));
    }
    void BindLuts(GDX12CommandList* commands)
    {
        GDX12Texture* inputs[]={&Box,&Transmittance,&MultipleScattering,&SkyView,&AerialL,&AerialT};
        for(UINT i=0;i<6;++i) { commands->ResourceBarrier({inputs[i]->GetResource()->GetSRVBarrier()}); commands->SetComputeSRV(i,inputs[i]->GetSRV()->GPUHandle); }
    }
    void DispatchSky(GDX12CommandList* commands,UINT pipeline,UINT x,UINT y,UINT z=1)
    { commands->SetPipelineState(SkyPipelines[pipeline]); commands->Dispatch(x,y,z); }
    void Filter(GDX12CommandList* commands,UINT pipeline,UINT size,float roughness,D3D12_GPU_DESCRIPTOR_HANDLE output,UINT slot,UINT faces)
    {
        struct { UINT Size; float Roughness; } constants={size,roughness};
        commands->SetPipelineState(FilterPipelines[pipeline]); commands->GetCommandList()->SetComputeRoot32BitConstants(0,2,&constants,0);
        commands->SetComputeUAV(slot,output); commands->Dispatch((size+7)/8,(size+7)/8,faces);
    }
};

GDX12EnvironmentIBLPass::GDX12EnvironmentIBLPass():_data(std::make_unique<Data>())
{ _flags=0; _numInputs=0; _numOutputs=4; for(auto& texture:_data->Outputs) _outputs.push_back(&texture); }
GDX12EnvironmentIBLPass::~GDX12EnvironmentIBLPass()=default;
void GDX12EnvironmentIBLPass::Submit(GDX12SkyDesc sky) { _data->Submitted=std::move(sky); }
UINT GDX12EnvironmentIBLPass::GetSource() const { return _data->EffectiveSource; }
GDX12Texture* GDX12EnvironmentIBLPass::GetTransmittanceLUT() { return &_data->Transmittance; }
GDX12Texture* GDX12EnvironmentIBLPass::GetMultiScatteringLUT() { return &_data->MultipleScattering; }
GDX12Texture* GDX12EnvironmentIBLPass::GetSkyViewLUT() { return &_data->SkyView; }
GDX12Texture* GDX12EnvironmentIBLPass::GetAerialTransmittance() { return &_data->AerialT; }

void GDX12EnvironmentIBLPass::Initialize()
{
    auto& d=*_data;
    CreateTexture(*_resources,d.Outputs[0],32,32,1,1,true); CreateTexture(*_resources,d.Outputs[1],128,128,1,8,true);
    CreateTexture(*_resources,d.Outputs[2],128,128); CreateTexture(*_resources,d.Outputs[3],1,1,1,1,true);
    CreateTexture(*_resources,d.IblSource,1,1,1,1,true); CreateTexture(*_resources,d.Box,1,1,1,1,true);
    CreateTexture(*_resources,d.Transmittance,1,1); CreateTexture(*_resources,d.MultipleScattering,1,1); CreateTexture(*_resources,d.SkyView,1,1);
    CreateTexture(*_resources,d.AerialL,2,2,2); CreateTexture(*_resources,d.AerialT,2,2,2);
    CreateMipViews(*_resources,d.Outputs[3],1,d.SkyMipViews); CreateMipViews(*_resources,d.Outputs[1],8,d.PrefilterViews);
    CreateMipViews(*_resources,d.IblSource,1,d.IblMipViews);
    d.Parameters=std::make_unique<GDX12UploadBuffer<SkyParameters>>(_resources->Device,static_cast<UINT>(_resources->FrameConstants.size()));
    GDX12RootSignatureDesc sky; sky.Constants.push_back(4); sky.NumSingleCBVSlots=1; sky.NumSingleSRVSlots=8; sky.NumSingleUAVSlots=4;
    sky.StaticSamplers.emplace_back(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    d.SkyRoot=std::make_unique<GDX12RootSignature>(_resources->Device,sky);
    const char* stages[]={"TransmittanceCS","MultiScatteringCS","SkyViewCS","CubeCS","AerialVolumeCS","ApplyAerialCS"};
    for(UINT i=0;i<6;++i) { auto shader=GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,SHADERS_FOLDER "SkyAtmosphere.hlsl",nullptr,stages[i],"cs"); D3D12_COMPUTE_PIPELINE_STATE_DESC desc={}; desc.pRootSignature=d.SkyRoot->GetRootSignature().Get(); desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()}; ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc,IID_PPV_ARGS(&d.SkyPipelines[i]))); }
    GDX12RootSignatureDesc filter; filter.Constants.push_back(2); filter.NumSingleSRVSlots=1; filter.NumSingleUAVSlots=2; filter.StaticSamplers=sky.StaticSamplers;
    d.FilterRoot=std::make_unique<GDX12RootSignature>(_resources->Device,filter);
    const char* filters[]={"IrradianceCS","PrefilterCS","IntegrateBRDFCS"};
    for(UINT i=0;i<3;++i) { auto shader=GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,SHADERS_FOLDER "EnvironmentIBL.hlsl",nullptr,filters[i],"cs"); D3D12_COMPUTE_PIPELINE_STATE_DESC desc={}; desc.pRootSignature=d.FilterRoot->GetRootSignature().Get(); desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()}; ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc,IID_PPV_ARGS(&d.FilterPipelines[i]))); }
}

void GDX12EnvironmentIBLPass::Data::LoadBox(GDX12DeviceResources& resources,const std::filesystem::path& path)
{
    auto* _resources=&resources;
    const auto* source = Engine::Core::AssetManager::GetInstance().LoadTexture(path);
    if (!source || !source->IsCubeMap() || source->GetArraySize() != 6 ||
        source->GetWidth() == 0 || source->GetWidth() != source->GetHeight() ||
        source->GetMipLevels() == 0 || source->GetMipLevels() > D3D12_REQ_MIP_LEVELS ||
        source->GetSubresourceCount() != 6u * source->GetMipLevels())
        throw std::runtime_error("Environment IBL requires a complete DDS cubemap: " + path.string());

    GDX12TextureDesc desc;
    desc.Format = desc.SRVDesc.Format = FormatToSRGB(source->GetFormat());
    desc.Semantic = ETextureSemantic::Color;
    desc.Width = source->GetWidth();
    desc.Height = source->GetHeight();
    desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
    desc.SRVHeapIndex = Box.IsInitialized() ? Box.GetSRV()->HeapIndex : desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    desc.SRVDesc.TextureCube.MipLevels = source->GetMipLevels();
    const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height,
        6, static_cast<UINT16>(source->GetMipLevels()));
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
    Box.Initialize(desc);

    const UINT count = static_cast<UINT>(source->GetSubresourceCount());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> rowBytes(count);
    UINT64 uploadBytes = 0;
    _resources->Device->GetDevice()->GetCopyableFootprints(&textureDesc, 0, count, 0,
        footprints.data(), rows.data(), rowBytes.data(), &uploadBytes);
    for (UINT i = 0; i < count; ++i)
    {
        const auto& data = source->GetSubresources()[i];
        if (data.RowPitch < rowBytes[i] || data.Data.size() < data.RowPitch * (rows[i] - 1) + rowBytes[i])
            throw std::runtime_error("Environment cubemap has incomplete subresource data: " + path.string());
    }
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    ThrowIfFailed(upload->Map(0, &noRead, &mapped));
    for (UINT i = 0; i < count; ++i)
    {
        const auto& data = source->GetSubresources()[i];
        auto* destination = static_cast<std::byte*>(mapped) + footprints[i].Offset;
        for (UINT row = 0; row < rows[i]; ++row)
            std::memcpy(destination + row * footprints[i].Footprint.RowPitch,
                data.Data.data() + row * data.RowPitch, static_cast<size_t>(rowBytes[i]));
    }
    upload->Unmap(0, nullptr);
    auto* queue = _resources->Device->GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ Box.GetResource()->GetCopyDestBarrier() });
    for (UINT i = 0; i < count; ++i)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION destination(desc.ExternalResource.Get(), i);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(upload.Get(), footprints[i]);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
    }
    commands->ResourceBarrier({ Box.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    LoadedPath=path;
}

void GDX12EnvironmentIBLPass::PrepareFrame()
{
    auto& d=*_data;
    const UINT requested=static_cast<UINT>(source.GetValue());
    d.EffectiveSource=0; d.Aerial=false;
    uint64_t identity=1469598103934665603ull; Hash(identity,d.Submitted.World); Hash(identity,requested);
    uint64_t diagnostic=identity; const size_t count=requested==1?d.Submitted.Boxes.size():d.Submitted.Atmospheres.size(); Hash(diagnostic,count);
    if(requested!=0&&count!=1&&diagnostic!=d.Diagnostic)
        Log::Warning("Sky",count==0?"Selected sky component is absent; using neutral sky/IBL":"Multiple selected sky components; using lowest entity ID",", source=",requested,", count=",count);
    d.Diagnostic=diagnostic;
    SkyParameters constants={};
    const Matrix inverseView=_commonData->ActiveCameraView.Invert();
    const Vector3 camera=inverseView.Translation();
    constants.InverseViewProjection=_commonData->ActiveCameraViewProjection.Invert();
    constants.CameraWorld={camera.x,camera.y,camera.z,0};
    UINT skySize=1,skyMips=1,level=2;
    bool flushed=false;
    const auto flush=[&] { if(!flushed) { _resources->Device->GetCommandQueue()->Flush(); flushed=true; } };
    if(requested==1&&count!=0&&boxEnabled.GetValue()&&boxQuality.GetValue()!=0)
    {
        const auto& selected=*std::min_element(d.Submitted.Boxes.begin(),d.Submitted.Boxes.end(),[](const auto& a,const auto& b){return a.Entity<b.Entity;});
        const auto& settings=selected.Settings;
        if(!Scale(settings.Intensity)||!Rotation(settings.RotationDegrees)) throw std::invalid_argument("Invalid SkyBox intensity/rotation");
        auto path=settings.Texture.is_absolute()?settings.Texture:std::filesystem::path(ASSETS_FOLDER)/settings.Texture;
        path=path.lexically_normal();
        const auto pathString=path.native(); identity=Mix(identity,pathString.data(),pathString.size()*sizeof(wchar_t));
        Hash(identity,selected.Entity); Hash(identity,settings.Revision);
        if(d.LoadedPath!=path||d.LoadedRevision!=settings.Revision)
        { flush(); d.LoadBox(*_resources,path); d.LoadedRevision=settings.Revision; }
        d.EffectiveSource=1; level=static_cast<UINT>(boxQuality.GetValue());
        skySize=d.Box.GetWidth(); skyMips=d.Box.GetResource()->D3DResource->GetDesc().MipLevels;
        constants.Options={0,0,settings.Intensity*boxIntensity.GetValue(),XMConvertToRadians(settings.RotationDegrees+boxRotation.GetValue())};
    }
    else if(requested==2&&count!=0&&atmosphereEnabled.GetValue()&&atmosphereQuality.GetValue()!=0)
    {
        const auto& selected=*std::min_element(d.Submitted.Atmospheres.begin(),d.Submitted.Atmospheres.end(),[](const auto& a,const auto& b){return a.Entity<b.Entity;});
        const auto& settings=selected.Settings;
        const auto finite=[](const Vector3& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
        if(!finite(settings.GroundOrigin)||!finite(settings.GroundAlbedo)||settings.GroundAlbedo.x<0||settings.GroundAlbedo.y<0||settings.GroundAlbedo.z<0||settings.GroundAlbedo.x>1||settings.GroundAlbedo.y>1||settings.GroundAlbedo.z>1||
            !std::isfinite(settings.PlanetRadiusKm)||settings.PlanetRadiusKm<100||settings.PlanetRadiusKm>100000||!Distance(settings.AtmosphereHeightKm)||
            !Scale(settings.RayleighScale)||!Scale(settings.MieScale)||!Scale(settings.OzoneScale)||!Scale(settings.MultipleScattering)||
            !Distance(settings.RayleighHeightKm)||!Distance(settings.MieHeightKm)||!Distance(settings.AerialDistanceKm)||!Anisotropy(settings.MieAnisotropy)||
            !std::isfinite(settings.SunAngularRadiusDegrees)||settings.SunAngularRadiusDegrees<.01f||settings.SunAngularRadiusDegrees>5)
            throw std::invalid_argument("Invalid SkyAtmosphere parameters");
        const float groundOffset=(std::max)(.001f,2*(std::nextafter(settings.PlanetRadiusKm,std::numeric_limits<float>::infinity())-settings.PlanetRadiusKm));
        const float topRadius=settings.PlanetRadiusKm+settings.AtmosphereHeightKm;
        if(topRadius-settings.PlanetRadiusKm<4*groundOffset)
            throw std::invalid_argument("SkyAtmosphere shell is too thin for its floating-point planet radius");
        Hash(identity,selected.Entity);
        d.EffectiveSource=2; level=static_cast<UINT>(atmosphereQuality.GetValue());
        skySize=64u<<level;
        Vector3 direction=-Vector3(_resources->Lighting.LightDirection); if(direction.LengthSquared()<1e-8f) direction=Vector3::UnitY; direction.Normalize();
        const Vector3 local=(camera-settings.GroundOrigin)*1e-5f;
        constants.CameraKm={local.x,-local.z,(std::max)(settings.PlanetRadiusKm+groundOffset,settings.PlanetRadiusKm+local.y),groundOffset};
        constants.SunDirectionRadius={direction.x,-direction.z,direction.y,XMConvertToRadians(settings.SunAngularRadiusDegrees)};
        const auto sun=_resources->Lighting.LightRadiance; const float intensity=sunScale.GetValue();
        constants.SunRadiance={sun.x*intensity,sun.y*intensity,sun.z*intensity,0};
        const float r=settings.RayleighScale*rayleigh.GetValue(),m=settings.MieScale*mie.GetValue(),o=settings.OzoneScale*ozone.GetValue();
        constants.RayleighHeight={.005802f*r,.013558f*r,.0331f*r,settings.RayleighHeightKm};
        constants.MieHeight={.003996f*m,.003996f*m,.003996f*m,settings.MieHeightKm};
        const float g=(std::clamp)(settings.MieAnisotropy+mieG.GetValue()-.8f,-.95f,.95f);
        constants.MieExtinctionG={.00444f*m,.00444f*m,.00444f*m,g};
        constants.OzoneMulti={.00065f*o,.001881f*o,.000085f*o,settings.MultipleScattering*multi.GetValue()};
        constants.GroundRadius={settings.GroundAlbedo.x,settings.GroundAlbedo.y,settings.GroundAlbedo.z,settings.PlanetRadiusKm};
        constants.Options={topRadius,settings.AerialDistanceKm*aerialRange.GetValue()/32,0,0};
        constants.Resolutions={64u<<level,16u<<level,8u<<level,48u<<level};
        constants.Resolutions2={27u<<level,8u<<level,8u<<level,2};
        d.Aerial=settings.AerialPerspective&&aerialEnabled.GetValue();
        if(d.Transmittance.GetWidth()!=constants.Resolutions.x)
        {
            flush();
            CreateTexture(*_resources,d.Transmittance,constants.Resolutions.x,constants.Resolutions.y);
            CreateTexture(*_resources,d.MultipleScattering,constants.Resolutions.z,constants.Resolutions.z);
            CreateTexture(*_resources,d.SkyView,constants.Resolutions.w,constants.Resolutions2.x);
            CreateTexture(*_resources,d.AerialL,constants.Resolutions2.y,constants.Resolutions2.y,constants.Resolutions2.y);
            CreateTexture(*_resources,d.AerialT,constants.Resolutions2.y,constants.Resolutions2.y,constants.Resolutions2.y);
            d.RenderedMedium=0;
        }
    }
    constants.Resolutions2.w=d.EffectiveSource;
    Hash(identity,d.EffectiveSource);
    const UINT filterSize=32u<<level,mips=6u+level;
    if(d.SkySize!=skySize||d.SkyMips!=skyMips)
    {
        flush(); CreateTexture(*_resources,d.Outputs[3],skySize,skySize,1,skyMips,true); CreateTexture(*_resources,d.IblSource,skySize,skySize,1,skyMips,true);
        CreateMipViews(*_resources,d.Outputs[3],skyMips,d.SkyMipViews); CreateMipViews(*_resources,d.IblSource,skyMips,d.IblMipViews); d.SkySize=skySize; d.SkyMips=skyMips; d.RenderedLighting=0;
    }
    if(d.PrefilterSize!=filterSize)
    {
        flush(); CreateTexture(*_resources,d.Outputs[0],filterSize/4,filterSize/4,1,1,true); CreateTexture(*_resources,d.Outputs[1],filterSize,filterSize,1,mips,true);
        CreateMipViews(*_resources,d.Outputs[1],mips,d.PrefilterViews); d.PrefilterSize=filterSize; d.PrefilterMips=mips; d.RenderedLighting=0;
    }
    d.Constants=constants;
    auto mediumConstants=constants; mediumConstants.InverseViewProjection={}; mediumConstants.CameraKm={}; mediumConstants.CameraWorld={}; mediumConstants.SunDirectionRadius={}; mediumConstants.SunRadiance={};
    d.MediumHash=Mix(1469598103934665603ull,&mediumConstants,sizeof(mediumConstants));
    auto lightingConstants=constants; lightingConstants.InverseViewProjection={}; lightingConstants.CameraWorld={};
    d.Lighting=Mix(identity,&lightingConstants,sizeof(lightingConstants));
    if(d.Identity!=identity) { ++_commonData->EnvironmentRevision; ++_commonData->HistoryRevision; d.Identity=identity; }
    if(d.RenderedLighting!=d.Lighting) ++_commonData->EnvironmentLightingRevision;
    d.Parameters->CopyData(_resources->CurrFrameConstantsIndex,constants);
}

void GDX12EnvironmentIBLPass::Execute(GDX12CommandList* commands)
{
    auto& d=*_data;
    const bool lightingChanged=d.RenderedLighting!=d.Lighting;
    if(!lightingChanged&&!d.Aerial&&d.BrdfReady) return;
    commands->BeginPixEvent("Sky and environment IBL",Colors::CornflowerBlue);
    d.BindSky(commands,*_resources);
    const auto bind=[&](UINT index,GDX12Texture& texture) { commands->ResourceBarrier({texture.GetResource()->GetSRVBarrier()}); commands->SetComputeSRV(index,texture.GetSRV()->GPUHandle); };
    if(d.EffectiveSource==2)
    {
        const auto& r=d.Constants.Resolutions; const auto& r2=d.Constants.Resolutions2;
        if(d.RenderedMedium!=d.MediumHash)
        {
            commands->ResourceBarrier({d.Transmittance.GetResource()->GetUnorderedAccessBarrier()}); commands->SetComputeUAV(1,d.Transmittance.GetUAV()->GPUHandle);
            d.DispatchSky(commands,0,(r.x+7)/8,(r.y+7)/8);
            bind(1,d.Transmittance);
            commands->ResourceBarrier({d.MultipleScattering.GetResource()->GetUnorderedAccessBarrier()}); commands->SetComputeUAV(1,d.MultipleScattering.GetUAV()->GPUHandle);
            d.DispatchSky(commands,1,r.z,r.z); d.RenderedMedium=d.MediumHash;
        }
        if(lightingChanged)
        {
            bind(1,d.Transmittance); bind(2,d.MultipleScattering);
            commands->ResourceBarrier({d.SkyView.GetResource()->GetUnorderedAccessBarrier()}); commands->SetComputeUAV(1,d.SkyView.GetUAV()->GPUHandle);
            d.DispatchSky(commands,2,(r.w+7)/8,(r2.x+7)/8);
        }
        if(d.Aerial)
        {
            bind(1,d.Transmittance); bind(2,d.MultipleScattering);
            commands->ResourceBarrier({d.AerialL.GetResource()->GetUnorderedAccessBarrier(),d.AerialT.GetResource()->GetUnorderedAccessBarrier()});
            commands->SetComputeUAV(2,d.AerialL.GetUAV()->GPUHandle); commands->SetComputeUAV(3,d.AerialT.GetUAV()->GPUHandle);
            d.DispatchSky(commands,4,(r2.y+3)/4,(r2.y+3)/4,(r2.y+3)/4);
            commands->ResourceBarrier({d.AerialL.GetResource()->GetSRVBarrier(),d.AerialT.GetResource()->GetSRVBarrier()});
        }
    }
    if(lightingChanged)
    {
        // CubeCS has a runtime source branch, so every shader-used SRV table must be valid in every mode.
        d.BindLuts(commands);
        commands->ResourceBarrier({d.Outputs[3].GetResource()->GetUnorderedAccessBarrier(),d.IblSource.GetResource()->GetUnorderedAccessBarrier()});
        for(UINT mip=0;mip<d.SkyMips;++mip)
        {
            const UINT constants[]={d.SkySize>>mip,mip,1,0};
            commands->GetCommandList()->SetComputeRoot32BitConstants(0,4,constants,0); commands->SetComputeUAV(0,d.SkyMipViews[mip]->GPUHandle);
            d.DispatchSky(commands,3,(constants[0]+7)/8,(constants[0]+7)/8,6);
        }
        commands->ResourceBarrier({d.Outputs[3].GetResource()->GetSRVBarrier()});
        if(d.EffectiveSource==2)
        {
            const UINT constants[]={d.SkySize,0,0,0}; commands->GetCommandList()->SetComputeRoot32BitConstants(0,4,constants,0);
            commands->SetComputeUAV(0,d.IblMipViews[0]->GPUHandle); d.DispatchSky(commands,3,(d.SkySize+7)/8,(d.SkySize+7)/8,6);
        }
        else
        {
            commands->ResourceBarrier({d.Outputs[3].GetResource()->GetCopySourceBarrier(),d.IblSource.GetResource()->GetCopyDestBarrier()});
            commands->CopyResource(d.IblSource.GetResource()->D3DResource.Get(),d.Outputs[3].GetResource()->D3DResource.Get());
            commands->ResourceBarrier({d.Outputs[3].GetResource()->GetSRVBarrier()});
        }
        commands->ResourceBarrier({d.IblSource.GetResource()->GetSRVBarrier()});
    }
    commands->SetComputeRootSignature(d.FilterRoot.get()); commands->SetComputeSRV(0,d.IblSource.GetSRV()->GPUHandle);
    if(lightingChanged)
    {
        commands->ResourceBarrier({d.Outputs[0].GetResource()->GetUnorderedAccessBarrier(),d.Outputs[1].GetResource()->GetUnorderedAccessBarrier()});
        d.Filter(commands,0,d.PrefilterSize/4,0,d.Outputs[0].GetUAV()->GPUHandle,0,6);
        for(UINT mip=0;mip<d.PrefilterMips;++mip) d.Filter(commands,1,d.PrefilterSize>>mip,float(mip)/(d.PrefilterMips-1),d.PrefilterViews[mip]->GPUHandle,0,6);
        commands->ResourceBarrier({d.Outputs[0].GetResource()->GetSRVBarrier(),d.Outputs[1].GetResource()->GetSRVBarrier()});
        d.RenderedLighting=d.Lighting;
    }
    if(!d.BrdfReady)
    {
        commands->ResourceBarrier({d.Outputs[2].GetResource()->GetUnorderedAccessBarrier()}); d.Filter(commands,2,128,0,d.Outputs[2].GetUAV()->GPUHandle,1,1);
        commands->ResourceBarrier({d.Outputs[2].GetResource()->GetSRVBarrier()}); d.BrdfReady=true;
    }
    commands->EndPixEvent();
}

void GDX12EnvironmentIBLPass::ApplyAerial(GDX12CommandList* commands,GDX12Texture& color,GDX12Texture& depth,GDX12Texture& output)
{
    auto& d=*_data;
    if(d.EffectiveSource!=2||!d.Aerial)
    {
        commands->ResourceBarrier({color.GetResource()->GetCopySourceBarrier(),output.GetResource()->GetCopyDestBarrier()});
        commands->CopyResource(output.GetResource()->D3DResource.Get(),color.GetResource()->D3DResource.Get());
        commands->ResourceBarrier({color.GetResource()->GetSRVBarrier(),output.GetResource()->GetSRVBarrier()}); return;
    }
    d.BindSky(commands,*_resources);
    GDX12Texture* textures[]={&d.AerialL,&d.AerialT,&color,&depth};
    for(UINT i=0;i<4;++i) { commands->ResourceBarrier({textures[i]->GetResource()->GetSRVBarrier()}); commands->SetComputeSRV(i+4,textures[i]->GetSRV()->GPUHandle); }
    commands->ResourceBarrier({output.GetResource()->GetUnorderedAccessBarrier()}); commands->SetComputeUAV(1,output.GetUAV()->GPUHandle);
    d.DispatchSky(commands,5,(color.GetWidth()+7)/8,(color.GetHeight()+7)/8); commands->ResourceBarrier({output.GetResource()->GetSRVBarrier()});
}
void GDX12EnvironmentIBLPass::ClearDenendencies() { if(_resources) _resources->Device->GetCommandQueue()->Flush(); GDX12RenderPass::ClearDenendencies(); _data.reset(); }

GDX12SkyAerialPass::GDX12SkyAerialPass(GDX12EnvironmentIBLPass* environment):_environment(environment)
{ _numInputs=2; _numOutputs=1; _flags=0; _outputs.push_back(&_color); }
GDX12SkyAerialPass::~GDX12SkyAerialPass()=default;
void GDX12SkyAerialPass::Initialize()
{
    if(!_environment||_inputs.size()!=2||!_inputs[0]||!_inputs[1]||!_inputs[0]->IsInitialized()||!_inputs[1]->IsInitialized()) throw std::invalid_argument("Sky aerial requires environment, initialized HDR color and depth");
    auto* color=_inputs[0]->GetTexture(); auto* depth=_inputs[1]->GetTexture();
    if(color->GetFormat()!=DXGI_FORMAT_R16G16B16A16_FLOAT||color->GetWidth()!=depth->GetWidth()||color->GetHeight()!=depth->GetHeight()) throw std::invalid_argument("Sky aerial requires matching HDR/depth grids");
    CreateTexture(*_resources,_color,color->GetWidth(),color->GetHeight());
}
void GDX12SkyAerialPass::Execute(GDX12CommandList* commands) { _environment->ApplyAerial(commands,*_inputs[0]->GetTexture(),*_inputs[1]->GetTexture(),_color); }
void GDX12SkyAerialPass::ClearDenendencies() { GDX12RenderPass::ClearDenendencies(); }
