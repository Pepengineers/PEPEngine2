#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionFallbackPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SpecularResolvePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void MakeInput(GDX12DeviceResources& resources, GDX12Texture& texture, UINT width, UINT height, DXGI_FORMAT format)
    {
        GDX12TextureDesc description;
        description.Width = width; description.Height = height;
        description.Format = description.SRVDesc.Format = format;
        description.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        description.SRVHeapIndex = resources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        description.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        description.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.SRVDesc.Texture2D.MipLevels = 1;
        texture.Initialize(description);
    }
    void Upload(GDX12Device& device, GDX12Texture& texture, const void* pixels, UINT pixelBytes, UINT subresource = 0)
    {
        const auto description = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        device.GetDevice()->GetCopyableFootprints(&description, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        void* mapping;
        const D3D12_RANGE noRead = {};
        ThrowIfFailed(upload->Map(0, &noRead, &mapping));
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
            std::memcpy(static_cast<char*>(mapping) + footprint.Offset + y * footprint.Footprint.RowPitch,
                static_cast<const char*>(pixels) + y * footprint.Footprint.Width * pixelBytes, footprint.Footprint.Width * pixelBytes);
        upload->Unmap(0, nullptr);
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION target(texture.GetResource()->D3DResource.Get(), subresource);
        const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint);
        commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
    }
    std::vector<float> ReadColor(GDX12Device& device, GDX12Texture& texture)
    {
        const auto description = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        device.GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION target(readback.Get(), footprint);
        commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
        void* mapping;
        const D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
        ThrowIfFailed(readback->Map(0, &range, &mapping));
        const auto format = texture.GetFormat();
        const UINT channels = format == DXGI_FORMAT_R32_FLOAT ? 1 : format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
        const bool bytes8 = format == DXGI_FORMAT_R8G8B8A8_UNORM;
        const UINT pixelBytes = format == DXGI_FORMAT_R32_FLOAT || bytes8 ? 4 : channels * 2;
        std::vector<float> values(texture.GetWidth() * texture.GetHeight() * channels);
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        {
            const auto* row = static_cast<const unsigned char*>(mapping) + footprint.Offset + y * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < texture.GetWidth(); ++x)
                for (UINT c = 0; c < channels; ++c)
                {
                    const float value = format == DXGI_FORMAT_R32_FLOAT ? reinterpret_cast<const float*>(row)[x] :
                        bytes8 ? row[x * pixelBytes + c] / 255.0f :
                        PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(row)[x * channels + c]);
                    Require(std::isfinite(value), "Post-process output must be finite");
                    values[(y * texture.GetWidth() + x) * channels + c] = value;
                }
        }
        const D3D12_RANGE noWrite = {};
        readback->Unmap(0, &noWrite);
        return values;
    }
}

int main()
{
    std::cout << std::unitbuf;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ConsoleModule console;
        const auto set = [&](const wchar_t* name, const char* value)
        { Require(console.FindConsoleVariable(name).SetFromString(value), "Rejected probe-lighting CVar"); };
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        GDX12Device device; ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info)); info->ClearStoredMessages();
        constexpr UINT width=65,height=49;
        RenderPipelineCommonData common;
        common.WindowWidth=common.DownscaledWidth=width; common.WindowHeight=common.DownscaledHeight=height;
        common.ActiveCameraCBufferIndex=0;
        Matrix projection=Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.f),float(width)/height,.1f,1000);
        projection._33=.1f/(1000-.1f); projection._43=1000*projection._33;
        common.CameraViewToClip=projection; common.ClipToCameraView=projection.Invert();
        const auto cameraAt = [&](float x)
        {
            const Matrix view=Matrix::CreateLookAt(Vector3(x,0,0),Vector3(x,0,-100),Vector3::UnitY);
            GDX12CameraConstants camera={}; camera.View=view.Transpose(); camera.ViewProj=(view*projection).Transpose();
            camera.ViewProjNoJitter=camera.PrevViewProjNoJitter=camera.ViewProj; camera.InvViewProj=(view*projection).Invert().Transpose();
            camera.CameraLocation={x,0,0}; camera.NearPlane=.1f; camera.FarPlane=1000;
            for(auto& frame:resources.FrameConstants) {frame->CameraCB->Resize(1);frame->CameraCB->CopyData(0,camera);}
            common.ActiveCameraView=view; common.ActiveCameraViewProjection=common.ActiveCameraPreviousViewProjection=view*projection;
        };
        cameraAt(0);
        const auto half=PackedVector::XMConvertFloatToHalf;
        const auto pixels=[&](XMFLOAT4 color,UINT count=0)
        {
            if (!count) count=width*height;
            std::vector<uint16_t> result(count*4);
            for(UINT i=0;i<count;++i) {result[i*4]=half(color.x);result[i*4+1]=half(color.y);result[i*4+2]=half(color.z);result[i*4+3]=half(color.w);}
            return result;
        };
        const auto cube=[&](GDX12Texture& texture,const std::array<XMFLOAT4,6>& faces)
        {
            GDX12TextureDesc desc;desc.Width=desc.Height=2;desc.Format=desc.SRVDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get();desc.SRVHeapIndex=resources.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex,TextureCube_RangeLength);
            desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;desc.SRVDesc.TextureCube.MipLevels=2;
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto allocation=CD3DX12_RESOURCE_DESC::Tex2D(desc.Format,2,2,6,2);
            ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&allocation,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&desc.ExternalResource)));
            texture.Initialize(desc);
            for(UINT face=0;face<6;++face)
            {
                auto color=pixels(faces[face],4);Upload(device,texture,color.data(),8,face*2);
                color=pixels({.5f,.5f,.5f,1},1);Upload(device,texture,color.data(),8,face*2+1);
            }
        };
        GDX12Texture global,red,blue;
        std::array<XMFLOAT4,6> faceColors;faceColors.fill({.2f,.4f,.8f,1});cube(global,faceColors);
        faceColors.fill({0,8,0,1});faceColors[4]={8,0,0,1};cube(red,faceColors);
        faceColors.fill({0,0,8,1});cube(blue,faceColors);
        GDX12Texture depth,normal,motion,color,albedo,f0,emissive,ao,brdf,shadow,localShadow;
        for(auto* t:{&normal,&color,&albedo,&f0,&emissive})MakeInput(resources,*t,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT);
        for(auto* t:{&depth,&ao})MakeInput(resources,*t,width,height,DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources,motion,width,height,DXGI_FORMAT_R16G16_FLOAT);
        MakeInput(resources,brdf,1,1,DXGI_FORMAT_R16G16B16A16_FLOAT);MakeInput(resources,shadow,1,1,DXGI_FORMAT_R32_FLOAT);
        GDX12TextureDesc arrayDesc;arrayDesc.Width=arrayDesc.Height=1;arrayDesc.Format=arrayDesc.SRVDesc.Format=DXGI_FORMAT_R32_FLOAT;
        arrayDesc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get();arrayDesc.SRVHeapIndex=resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources);
        arrayDesc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;arrayDesc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;arrayDesc.SRVDesc.Texture2DArray.ArraySize=1;arrayDesc.SRVDesc.Texture2DArray.MipLevels=1;
        localShadow.Initialize(arrayDesc);
        auto normalPixels=pixels({0,0,1,0});Upload(device,normal,normalPixels.data(),8);
        auto values=pixels({1,2,3,1});Upload(device,color,values.data(),8);Upload(device,emissive,values.data(),8);
        values=pixels({0,0,0,0});Upload(device,albedo,values.data(),8);values=pixels({.5f,.5f,.5f,0});Upload(device,f0,values.data(),8);
        std::vector<float> depths(width*height,projection._43/100-projection._33),ones(width*height,1);
        std::vector<uint16_t> zeroMotion(width*height*2);Upload(device,depth,depths.data(),4);Upload(device,ao,ones.data(),4);Upload(device,motion,zeroMotion.data(),4);
        const uint16_t lut[]={half(.25f),half(.125f),0,half(1)};Upload(device,brdf,lut,8);float one=1;Upload(device,shadow,&one,4);Upload(device,localShadow,&one,4);
        GDX12ReflectionLightingPass lighting;
        lighting.Setup(&resources,nullptr,&common);lighting.Initialize();
        GDX12ReflectionFallbackPass fallback;
        fallback.Setup(&resources,nullptr,&common);fallback.SetInputs({&depth,&normal,&global,&f0});fallback.Initialize();
        GDX12PublishedReflectionProbe first;first.World=1;first.Description.Position={0,0,-100};first.Description.Extents=Vector3(30);first.Texture=&red;first.MipCount=2;first.Revision=1;
        auto second=first;second.Texture=&blue;second.Description.Entity=1;
        auto* queue=device.GetCommandQueue();UINT frame=0;
        const auto render=[&](std::vector<GDX12PublishedReflectionProbe> probes)
        {
            resources.CurrFrameConstantsIndex=frame++%resources.FrameConstants.size();
            lighting.Submit(std::move(probes));lighting.PrepareFrame();
            auto* list=queue->GetCommandList();lighting.Execute(list);fallback.Execute(list);queue->ExecuteCommandList(list);queue->Flush();
            return ReadColor(device,*fallback.GetOutput(0)->GetTexture());
        };
        const size_t center=((height/2)*width+width/2)*4;
        auto result=render({});Require(result[center]==-1 && result[center+3]==0,"No probes must retain the stock global SSR fallback sentinel and roughness");
        result=render({first});Require(std::abs(result[center]-8)<.01f && result[center+1]==0,"Inside probe must sample its +Z face with zero-axis direction");
        Require(result[0]==-1,"Outside all influence volumes must use global fallback, without box projection");
        result=render({first,second});Require(std::abs(result[center]-4)<.01f&&std::abs(result[center+2]-4)<.01f,"Overlapping probes must normalize weights");
        cameraAt(15);result=render({first});Require(std::abs(result[center]-4.1f)<.02f&&std::abs(result[center+1]-.2f)<.02f,"Probe edge must blend continuously into global radiance");
        cameraAt(0);normalPixels=pixels({.70710678f,0,.70710678f,0});Upload(device,normal,normalPixels.data(),8);
        result=render({first});Require(result[center+1]>7.9f&&result[center]<.01f,"Box projection must choose the hit cube face, not camera direction");
        normalPixels=pixels({.4472136f,0,.89442719f,0});Upload(device,normal,normalPixels.data(),8);
        auto parallaxProbe=first;parallaxProbe.Description.Position.z=-129;
        result=render({parallaxProbe});
        // Raw reflection favors +X; the surface is one unit from the box's +Z boundary.
        Require(result[center]>.4f&&result[center+1]<.45f,"Parallax correction must select the nearby +Z wall instead of the raw +X reflection face");
        normalPixels=pixels({0,0,1,1});Upload(device,normal,normalPixels.data(),8);result=render({first});Require(std::abs(result[center]-.5f)<.01f,"Per-probe roughness must use its actual mip count");
        normalPixels=pixels({0,0,1,0});Upload(device,normal,normalPixels.data(),8);
        set(L"Render.Reflection.Enabled","false");result=render({first});Require(result[center]==-1,"Reflection Off must ignore existing publications");
        set(L"Render.Reflection.Enabled","true");set(L"Render.Reflection.Quality","0");result=render({first});Require(result[center]==-1,"Reflection Quality Off must ignore existing publications");set(L"Render.Reflection.Quality","2");
        values=pixels({.5f,.5f,.5f,1});Upload(device,f0,values.data(),8);result=render({first});
        Require(result[center]==-1&&result[center+3]==1,"Baked material must be masked out of stochastic SSR tracing");
        values=pixels({.5f,.5f,.5f,0});Upload(device,f0,values.data(),8);
        std::vector<float> sky(width*height,0);Upload(device,depth,sky.data(),4);result=render({first});
        Require(result[center]==-1&&result[center+3]==1,"Sky must be masked out of stochastic SSR tracing");Upload(device,depth,depths.data(),4);
        // A locally constant miss signal isolates SDK fallback from its neighborhood denoising.
        auto missProbe=first;missProbe.Description.Extents=Vector3(1000000);
        result=render({missProbe});
        GDX12DeferredLightingPass deferred;deferred.Setup(&resources,nullptr,&common);
        deferred.SetInputs({&albedo,&normal,&f0,&emissive,&depth,&shadow,&global,&global,&brdf,&global,&localShadow,&ao});deferred.Initialize();
        GDX12MainConstants main={};main.LightRadiance={};main.LocalLightCount=0;main.SunShadowFlags=0;
        for(auto& constants:resources.FrameConstants)constants->MainCB->CopyData(0,main);
        auto* list=queue->GetCommandList();deferred.Execute(list);queue->ExecuteCommandList(list);queue->Flush();
        auto shaded=ReadColor(device,*deferred.GetOutput(0)->GetTexture());
        Require(std::abs(shaded[center]-3)<.02f&&std::abs(shaded[center+1]-2)<.02f&&std::abs(shaded[center+2]-3)<.02f,"Deferred PBR must apply probe radiance and DFG once, retaining emissive");
        GDX12SssrPass ssr;ssr.Setup(&resources,nullptr,&common);ssr.SetInputs({deferred.GetOutput(0),&depth,&motion,&normal,fallback.GetOutput(0),&global,&brdf});ssr.Initialize();
        GDX12SpecularResolvePass resolve(&ssr);resolve.Setup(&resources,nullptr,&common);resolve.SetInputs({deferred.GetOutput(0),&depth,&normal,&f0,&ao,&global,&brdf,ssr.GetOutput(0)});resolve.Initialize();
        auto prepared=ReadColor(device,*fallback.GetOutput(0)->GetTexture());
        for(UINT y=height/2-8;y<=height/2+8;++y)for(UINT x=width/2-8;x<=width/2+8;++x)
        {
            const size_t p=(y*width+x)*4;
            Require(std::abs(prepared[p]-8)<.01f&&prepared[p+1]<.001f&&prepared[p+2]<.001f&&prepared[p+3]==0,
                "The SSR miss oracle must have constant probe radiance over the denoiser neighborhood");
        }
        std::cout<<"Probe SSR input center "<<prepared[center]<<","<<prepared[center+1]<<","<<prepared[center+2]<<" roughness "<<prepared[center+3]<<" shaded "<<shaded[center]<<","<<shaded[center+1]<<","<<shaded[center+2]<<'\n';
        for(UINT i=0;i<3;++i)
        {
            ssr.PrepareFrame();list=queue->GetCommandList();ssr.Execute(list);resolve.Execute(list);queue->ExecuteCommandList(list);queue->Flush();
            const auto sample=ReadColor(device,*ssr.GetOutput(0)->GetTexture());
            std::cout<<"Probe SSR frame "<<i<<" center "<<sample[center]<<","<<sample[center+1]<<","<<sample[center+2]<<" ready "<<ssr.HasValidHistory()<<'\n';
        }
        auto reflected=ReadColor(device,*ssr.GetOutput(0)->GetTexture());
        Require(reflected[center]>7.5f&&reflected[center+1]<.1f,"SSSR miss must use local probe fallback, not global environment or source color");
        auto resolved=ReadColor(device,*resolve.GetOutput(0)->GetTexture());
        Require(std::abs(resolved[center]-shaded[center])<.15f,"Specular resolve must replace the same local environment used by deferred lighting");
        GDX12WBOITTransparencyPass transparency;transparency.Setup(&resources,nullptr,&common);
        transparency.SetInputs({&depth,&shadow,&global,&global,&brdf,&localShadow});transparency.Initialize();
        fallback.Resize();result=render({first});Require(result[center]>7.9f,"Resized fallback must use the current frame publication");
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes;info->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());ThrowIfFailed(info->GetMessage(i,message,&bytes));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(message->pDescription);}
        queue->Flush();transparency.ClearDenendencies();resolve.ClearDenendencies();ssr.ClearDenendencies();deferred.ClearDenendencies();fallback.ClearDenendencies();lighting.ClearDenendencies();GDX12ShaderCompiler::Shutdown();
        std::cout<<"Probe lighting GPU: box projection, overlap, global/Off, roughness, PBR once, SSSR miss and frame slots passed.\n";return 0;
    }
    catch(const DxException& error){std::wcerr<<error.ToString()<<'\n';}catch(const std::exception& error){std::cerr<<error.what()<<'\n';}return 1;
}
