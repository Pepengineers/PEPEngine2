#include "Engine.RendererDX12/RenderPasses/GDX12SpecularResolvePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12PostProcessAuxPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ToneMappingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <algorithm>
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
        for (UINT y = 0; y < texture.GetHeight(); ++y)
            std::memcpy(static_cast<char*>(mapping) + footprint.Offset + y * footprint.Footprint.RowPitch,
                static_cast<const char*>(pixels) + y * texture.GetWidth() * pixelBytes, texture.GetWidth() * pixelBytes);
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
        for (UINT y = 0; y < texture.GetHeight(); ++y)
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
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ConsoleModule console;
        const auto set = [&](const wchar_t* name, const char* value)
        { Require(console.FindConsoleVariable(name).SetFromString(value), "Rejected test CVar"); };
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        constexpr UINT width = 65, height = 49;
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = width;
        common.WindowHeight = common.DownscaledHeight = height;
        common.ActiveCameraCBufferIndex = 0;
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), float(width) / height, 0.1f, 1000);
        projection._33 = 0.1f / (1000 - 0.1f); projection._43 = projection._33 * 1000;
        common.CameraViewToClip = projection;
        common.ClipToCameraView = projection.Invert();
        common.ActiveCameraViewProjection = common.ActiveCameraPreviousViewProjection = projection;
        GDX12CameraConstants camera = {};
        camera.View = Identity4x4();
        camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
        camera.InvViewProj = projection.Invert().Transpose();
        camera.NearPlane = 0.1f; camera.FarPlane = 1000;
        for (auto& frame : resources.FrameConstants)
        { frame->CameraCB->Resize(1); frame->CameraCB->CopyData(0, camera); }
        const auto half = PackedVector::XMConvertFloatToHalf;
        const auto constantColor = [&](XMFLOAT4 value)
        {
            std::vector<uint16_t> result(width * height * 4);
            for (size_t i = 0; i < result.size(); i += 4)
            { result[i] = half(value.x); result[i+1] = half(value.y); result[i+2] = half(value.z); result[i+3] = half(value.w); }
            return result;
        };
        GDX12Texture color, depth, motion, normal, f0, ao, radiance, brdf, accumulation, revealage;
        for (auto* texture : { &color, &normal, &f0, &radiance, &accumulation })
            MakeInput(resources, *texture, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (auto* texture : { &depth, &ao, &revealage })
            MakeInput(resources, *texture, width, height, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
        MakeInput(resources, brdf, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto colorPixels = constantColor({4,2,1,1});
        auto normalPixels = constantColor({0,0,1,0});
        auto f0Pixels = constantColor({0.5f,0.5f,0.5f,0});
        auto reflectionPixels = constantColor({8,6,4,1});
        auto zeroPixels = constantColor({0,0,0,0});
        std::vector<float> depthPixels(width * height, projection._43 / 100 - projection._33), ones(width * height, 1);
        std::vector<uint16_t> motionPixels(width * height * 2);
        Upload(device, color, colorPixels.data(), 8); Upload(device, normal, normalPixels.data(), 8);
        Upload(device, f0, f0Pixels.data(), 8); Upload(device, radiance, reflectionPixels.data(), 8);
        Upload(device, accumulation, zeroPixels.data(), 8); Upload(device, revealage, ones.data(), 4);
        Upload(device, depth, depthPixels.data(), 4); Upload(device, ao, ones.data(), 4); Upload(device, motion, motionPixels.data(), 4);
        const uint16_t brdfPixel[] = { half(0.25f),half(0.125f),0,half(1) };
        Upload(device, brdf, brdfPixel, 8);
        GDX12Texture environment;
        GDX12TextureDesc cube;
        cube.Width = cube.Height = 1; cube.Format = cube.SRVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cube.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        cube.SRVHeapIndex = cube.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        cube.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cube.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; cube.SRVDesc.TextureCube.MipLevels = 1;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto allocation = CD3DX12_RESOURCE_DESC::Tex2D(cube.Format, 1, 1, 6, 1);
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &allocation, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cube.ExternalResource)));
        environment.Initialize(cube);
        const uint16_t envPixel[] = {half(2),half(3),half(4),half(1)};
        for (UINT face = 0; face < 6; ++face) Upload(device, environment, envPixel, 8, face);
        GDX12SssrPass ssr;
        ssr.Setup(&resources, nullptr, &common);
        ssr.SetInputs({ &color,&depth,&motion,&normal,&normal,&environment,&brdf }); ssr.Initialize();
        GDX12SpecularResolvePass resolve(&ssr);
        resolve.Setup(&resources, nullptr, &common);
        resolve.SetInputs({ &color,&depth,&normal,&f0,&ao,&environment,&brdf,&radiance }); resolve.Initialize();
        GDX12WBOITCompositionPass composition;
        composition.Setup(&resources, nullptr, &common);
        composition.SetInputs({ &color,&accumulation,&revealage }); composition.Initialize();
        GDX12ToneMappingPass tone;
        tone.Setup(&resources, nullptr, &common); tone.SetInputs({ composition.GetOutput(0) }); tone.Initialize();
        auto* queue = device.GetCommandQueue();
        const auto render = [&]
        {
            ssr.PrepareFrame();
            auto* commands = queue->GetCommandList();
            ssr.Execute(commands); resolve.Execute(commands); composition.Execute(commands); tone.Execute(commands);
            queue->ExecuteCommandList(commands); queue->Flush();
        };
        set(L"Render.SSR.Enabled", "false"); render();
        const auto reference = ReadColor(device, color);
        Require(ReadColor(device, *resolve.GetOutput(0)->GetTexture()) == reference, "SSR Off resolve must copy HDR exactly");
        Require(ReadColor(device, *composition.GetOutput(0)->GetTexture()) == reference, "OIT composition must retain linear HDR above one");
        auto mapped = ReadColor(device, *tone.GetOutput(0)->GetTexture());
        const float expectedLinear = 4.0f / 5.0f;
        const float expectedSRGB = 1.055f * std::pow(expectedLinear, 1.0f/2.4f) - 0.055f;
        Require(std::abs(mapped[0] - expectedSRGB) <= 1.5f/255, "Tone mapping must encode exactly once after HDR processing");
        set(L"Render.SSR.Enabled", "true"); render();
        Require(ReadColor(device, *resolve.GetOutput(0)->GetTexture()) == reference, "Cold SSR must retain environment fallback exactly");
        render(); Require(ssr.HasValidHistory(), "Real SSR must publish ready history");
        auto result = ReadColor(device, *resolve.GetOutput(0)->GetTexture());
        const size_t center = ((height/2) * width + width/2) * 4;
        Require(std::abs(result[center] - 5.5f) < 0.01f && std::abs(result[center+1] - 2.75f) < 0.01f && std::abs(result[center+2] - 1.0f) < 0.01f,
            "Resolve must replace old environment radiance and apply the DFG/AO factor once");
        f0Pixels[center+3] = half(1); Upload(device, f0, f0Pixels.data(), 8); render();
        result = ReadColor(device, *resolve.GetOutput(0)->GetTexture());
        Require(result[center] == reference[center], "Baked color must bypass SSR replacement");

        constexpr UINT sourceWidth = 9, sourceHeight = 7;
        GDX12Texture lowDepth, lowMotion;
        MakeInput(resources, lowDepth, sourceWidth, sourceHeight, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, lowMotion, sourceWidth, sourceHeight, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<float> lowDepthPixels(sourceWidth * sourceHeight);
        std::vector<uint16_t> lowMotionPixels(sourceWidth * sourceHeight * 2);
        for (UINT y=0; y<sourceHeight; ++y) for (UINT x=0; x<sourceWidth; ++x)
        {
            const size_t p=y*sourceWidth+x;
            lowDepthPixels[p] = x < 4 ? 0.2f : 0.8f;
            lowMotionPixels[p*2] = half(x < 4 ? -0.25f : 0.25f);
            lowMotionPixels[p*2+1] = half(y < 3 ? -0.125f : 0.125f);
        }
        Upload(device, lowDepth, lowDepthPixels.data(), 4); Upload(device, lowMotion, lowMotionPixels.data(), 4);
        GDX12PostProcessAuxPass auxiliary;
        auxiliary.Setup(&resources, nullptr, &common); auxiliary.SetInputs({ &lowDepth,&lowMotion }); auxiliary.Initialize();
        common.ActiveCameraJitterOffsetX=0.4f; common.ActiveCameraJitterOffsetY=-0.3f;
        Matrix jitteredProjection = projection;
        jitteredProjection._31 += 0.4f * 2 / sourceWidth;
        jitteredProjection._32 -= -0.3f * 2 / sourceHeight;
        XMFLOAT4 projected;
        XMStoreFloat4(&projected, XMVector4Transform(XMVectorSet(0,0,-100,1), jitteredProjection));
        const float shiftX = projected.x / projected.w * sourceWidth * 0.5f;
        const float shiftY = -projected.y / projected.w * sourceHeight * 0.5f;
        Require(std::abs(shiftX + 0.4f) < 1e-5f && std::abs(shiftY - 0.3f) < 1e-5f,
            "RH projection shear must move geometry by the negative pixel jitter");
        for (UINT frame=0; frame<resources.FrameConstants.size(); ++frame)
        {
            resources.CurrFrameConstantsIndex=frame;
            auto* commands=queue->GetCommandList(); auxiliary.Execute(commands);
            queue->ExecuteCommandList(commands); queue->Flush();
            const auto d=ReadColor(device,*auxiliary.GetOutput(0)->GetTexture());
            const auto m=ReadColor(device,*auxiliary.GetOutput(1)->GetTexture());
            for (UINT y=0;y<height;++y) for(UINT x=0;x<width;++x)
            {
                const UINT sx=std::clamp(int(std::floor((x+0.5f)*sourceWidth/width+shiftX)),0,int(sourceWidth)-1);
                const UINT sy=std::clamp(int(std::floor((y+0.5f)*sourceHeight/height+shiftY)),0,int(sourceHeight)-1);
                const size_t p=y*width+x, s=sy*sourceWidth+sx;
                Require(d[p] == lowDepthPixels[s], "Aux depth must point-sample the jittered source without averaging across boundaries");
                Require(m[p*2] == PackedVector::XMConvertHalfToFloat(lowMotionPixels[s*2]) && m[p*2+1] == PackedVector::XMConvertHalfToFloat(lowMotionPixels[s*2+1]),
                    "Aux motion must come from the same source sample as its depth");
            }
        }
        RenderPipelineCommonData upscaleCommon;
        upscaleCommon.WindowWidth = 256; upscaleCommon.WindowHeight = 144;
        GDX12XeSSUpscalePass upscale;
        upscale.Setup(&resources, nullptr, &upscaleCommon);
        GDX12Texture upscaleInput, upscaleDepth, upscaleMotion;
        const UINT inputWidth = upscaleCommon.DownscaledWidth, inputHeight = upscaleCommon.DownscaledHeight;
        MakeInput(resources, upscaleInput, inputWidth, inputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, upscaleDepth, inputWidth, inputHeight, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, upscaleMotion, inputWidth, inputHeight, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<uint16_t> hdrPixels(inputWidth * inputHeight * 4), stationary(inputWidth * inputHeight * 2);
        std::vector<float> planarDepth(inputWidth * inputHeight, 0.1f);
        for (size_t i=0;i<hdrPixels.size();i+=4)
        { hdrPixels[i]=half(4); hdrPixels[i+1]=half(2); hdrPixels[i+2]=half(1); hdrPixels[i+3]=half(1); }
        Upload(device, upscaleInput, hdrPixels.data(), 8);
        Upload(device, upscaleDepth, planarDepth.data(), 4); Upload(device, upscaleMotion, stationary.data(), 4);
        upscale.SetInputs({ &upscaleDepth,&upscaleMotion,&upscaleInput }); upscale.Initialize();
        GDX12ToneMappingPass upscaleTone;
        upscaleTone.Setup(&resources, nullptr, &upscaleCommon);
        upscaleTone.SetInputs({ upscale.GetOutput(0) }); upscaleTone.Initialize();
        for (UINT frame=0;frame<3;++frame)
        {
            if (frame==2) { ++upscaleCommon.HistoryRevision; upscale.Resize(); }
            auto* commands=queue->GetCommandList();
            upscale.Execute(commands); upscaleTone.Execute(commands);
            queue->ExecuteCommandList(commands); queue->Flush();
            const auto hdr=ReadColor(device,*upscale.GetOutput(0)->GetTexture());
            const size_t centerPixel=((upscaleCommon.WindowHeight/2)*upscaleCommon.WindowWidth+upscaleCommon.WindowWidth/2)*4;
            Require(hdr[centerPixel]>2, "Real XeSS must preserve HDR above one on first/history/reset frames");
            const auto encoded=ReadColor(device,*upscaleTone.GetOutput(0)->GetTexture());
            Require(encoded[centerPixel]>0.8f && encoded[centerPixel]<=1, "Engine tone mapping must restore state after raw XeSS dispatch");
        }
        upscaleTone.ClearDenendencies(); upscale.ClearDenendencies();
        for (UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        {
            SIZE_T size; info->GetMessage(i,nullptr,&size); std::vector<unsigned char> storage(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,message,&size));
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        auxiliary.ClearDenendencies(); tone.ClearDenendencies(); composition.ClearDenendencies(); resolve.ClearDenendencies(); ssr.ClearDenendencies();
        GDX12ShaderCompiler::Shutdown();
        std::cout << "Post-process GPU: linear HDR/OIT, single specular replacement, SSR cold/Off, jittered aux edges and final tone mapping passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
