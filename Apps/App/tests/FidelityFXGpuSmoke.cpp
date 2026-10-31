#include "Engine.RendererDX12/RenderPasses/GDX12FidelityFXPasses.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
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
    void Upload(GDX12Device& device, GDX12Texture& texture, const void* pixels, UINT pixelBytes)
    {
        const auto description = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        device.GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
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
        const CD3DX12_TEXTURE_COPY_LOCATION target(texture.GetResource()->D3DResource.Get(), 0);
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
        std::vector<float> values(texture.GetWidth() * texture.GetHeight() * 4);
        for (UINT y = 0; y < texture.GetHeight(); ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapping) + footprint.Offset + y * footprint.Footprint.RowPitch);
            for (UINT x = 0; x < texture.GetWidth() * 4; ++x)
            {
                const float value = PackedVector::XMConvertHalfToFloat(row[x]);
                Require(std::isfinite(value) && value >= 0 && value < 100000, "FidelityFX output must contain finite HDR values");
                values[y * texture.GetWidth() * 4 + x] = value;
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
        { Require(console.FindConsoleVariable(name).SetFromString(value), "FidelityFX CVar rejected a valid test setting"); };
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        ComPtr<ID3D12InfoQueue1> liveInfo;
        DWORD callback = 0;
        if (SUCCEEDED(info.As(&liveInfo)))
            liveInfo->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                D3D12_MESSAGE_ID, LPCSTR message, void*)
                { if (severity <= D3D12_MESSAGE_SEVERITY_WARNING) std::cerr << message << '\n'; },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callback);
        info->ClearStoredMessages();
        constexpr UINT width = 64, height = 64;
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = width;
        common.WindowHeight = common.DownscaledHeight = height;
        common.ActiveCameraCBufferIndex = 0;
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), 1, 0.1f, 1000);
        projection._33 = 0.1f / (1000 - 0.1f);
        projection._43 = 1000 * projection._33;
        common.CameraViewToClip = projection;
        common.ClipToCameraView = projection.Invert();
        common.ActiveCameraViewProjection = common.ActiveCameraPreviousViewProjection = projection;
        GDX12CameraConstants camera = {};
        camera.View = Identity4x4();
        camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
        camera.InvViewProj = projection.Invert().Transpose();
        camera.NearPlane = 0.1f; camera.FarPlane = 1000;
        resources.FrameConstants[0]->CameraCB->Resize(1);
        resources.FrameConstants[0]->CameraCB->CopyData(0, camera);

        GDX12Texture color, depth, motion, normal;
        MakeInput(resources, color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, depth, width, height, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
        MakeInput(resources, normal, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        std::vector<uint16_t> colorData(width * height * 4), normals(width * height * 4), velocities(width * height * 2);
        std::vector<float> depthData(width * height, projection._43 / 100 - projection._33);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const size_t offset = (y * width + x) * 4;
                const float light = ((x / 2 + y / 2) & 1) ? 4.0f : 0.1f;
                for (UINT c = 0; c < 3; ++c) colorData[offset + c] = PackedVector::XMConvertFloatToHalf(light);
                colorData[offset + 3] = PackedVector::XMConvertFloatToHalf(1);
                normals[offset + 2] = PackedVector::XMConvertFloatToHalf(1);
                normals[offset + 3] = PackedVector::XMConvertFloatToHalf(0.3f);
            }
        Upload(device, color, colorData.data(), 8);
        Upload(device, depth, depthData.data(), 4);
        Upload(device, motion, velocities.data(), 4);
        Upload(device, normal, normals.data(), 8);
        const auto reference = ReadColor(device, color);
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common);
        environment.Initialize();
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        environment.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        float environmentEnergy = 0;
        for (float value : ReadColor(device, *environment.GetOutput(1)->GetTexture())) environmentEnergy += value;
        std::cout << "Environment energy " << environmentEnergy << '\n';
        GDX12SssrPass ssr;
        ssr.Setup(&resources, nullptr, &common);
        ssr.SetInputs({ &color, &depth, &motion, &normal, &normal, environment.GetOutput(1), environment.GetOutput(2) });
        ssr.Initialize();
        Require(!ssr.HasValidHistory(), "A new SSSR context must not expose valid history");
        GDX12DepthOfFieldPass dof;
        dof.Setup(&resources, nullptr, &common);
        dof.SetInputs({ &color, &depth });
        dof.Initialize();
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common);
        ao.SetInputs({ &depth, &normal });
        ao.Initialize();
        const auto render = [&]
        {
            ssr.PrepareFrame();
            dof.PrepareFrame();
            auto* list = queue->GetCommandList();
            ssr.Execute(list);
            dof.Execute(list);
            ao.Execute(list); // An ordinary engine pass must restore state after raw SDK dispatches.
            queue->ExecuteCommandList(list);
            queue->Flush();
        };
        set(L"Render.DOF.Enabled", "true");
        set(L"Render.DOF.FocusDistance", "100");
        for (int frame = 0; frame < 3; ++frame)
        {
            render();
            Require(ssr.HasValidHistory() == (frame >= 1), "SSSR history must become ready after its second dispatch");
            float frameEnergy = 0;
            const auto frameRadiance = ReadColor(device, *ssr.GetOutput(0)->GetTexture());
            for (size_t i = 0; i < frameRadiance.size(); i += 4)
                frameEnergy += frameRadiance[i] + frameRadiance[i + 1] + frameRadiance[i + 2];
            std::cout << "SSSR frame " << frame << " energy " << frameEnergy << '\n';
        }
        auto result = ReadColor(device, *dof.GetOutput(0)->GetTexture());
        float maxFocusError = 0;
        for (size_t i = 0; i < result.size(); ++i) maxFocusError = (std::max)(maxFocusError, std::abs(result[i] - reference[i]));
        Require(maxFocusError < 0.01f, "The DOF focus plane must retain sharp HDR color");
        auto reflections = ReadColor(device, *ssr.GetOutput(0)->GetTexture());
        float energy = 0;
        for (size_t i = 0; i < reflections.size(); i += 4) energy += reflections[i] + reflections[i + 1] + reflections[i + 2];
        std::cout << "SSSR enabled " << ssr.IsEnabled() << " energy " << energy << '\n';
        Require(energy > 0.01f, "SSSR must supply environment fallback radiance");
        set(L"Render.DOF.FocusDistance", "12");
        set(L"Render.DOF.Aperture", "0.5");
        set(L"Render.DOF.Quality", "3");
        render();
        result = ReadColor(device, *dof.GetOutput(0)->GetTexture());
        float blurredDifference = 0;
        for (size_t i = 0; i < result.size(); ++i) blurredDifference += std::abs(result[i] - reference[i]);
        Require(blurredDifference > 1, "Out-of-focus geometry must actually blur");
        set(L"Render.DOF.Quality", "0");
        set(L"Render.SSR.Quality", "0");
        render();
        Require(!ssr.HasValidHistory(), "SSSR Off must invalidate history");
        Require(ReadColor(device, *dof.GetOutput(0)->GetTexture()) == reference, "DOF Off must copy exact input pixels");
        for (float value : ReadColor(device, *ssr.GetOutput(0)->GetTexture())) Require(value == 0, "SSSR Off must clear stale radiance");
        set(L"Render.SSR.Quality", "1");
        set(L"Render.DOF.Quality", "1");
        ++common.HistoryRevision;
        render();
        Require(!ssr.HasValidHistory(), "SSSR re-enable and history revision must restart warmup");
        ReadColor(device, *ssr.GetOutput(0)->GetTexture());
        ReadColor(device, *dof.GetOutput(0)->GetTexture());
        render();
        Require(ssr.HasValidHistory(), "Re-enabled SSSR history must recover on the second dispatch");
        set(L"Render.SSR.TemporalStability", "0");
        ++common.HistoryRevision;
        render();
        Require(!ssr.HasValidHistory(), "A camera cut must restart SSSR history warmup");
        float coldEnergy = 0;
        const auto coldRadiance = ReadColor(device, *ssr.GetOutput(0)->GetTexture());
        for (size_t i = 0; i < coldRadiance.size(); i += 4) coldEnergy += coldRadiance[i] + coldRadiance[i + 1] + coldRadiance[i + 2];
        std::cout << "SSSR cold temporal0 energy " << coldEnergy << '\n';
        ssr.Resize();
        dof.Resize();
        Require(!ssr.HasValidHistory(), "Resizing must invalidate SSSR history");
        render();
        Require(!ssr.HasValidHistory(), "Resized SSSR must warm up its first dispatch");
        render();
        Require(ssr.HasValidHistory(), "Resized SSSR history must recover on the second dispatch");
        Require(!console.FindConsoleVariable(L"Render.SSR.SamplesPerQuad").SetFromString("3"), "Invalid ray count must be rejected");
        Require(!console.FindConsoleVariable(L"Render.DOF.Aperture").SetFromString("0"), "Invalid aperture must be rejected");
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size;
            info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        ssr.ClearDenendencies();
        dof.ClearDenendencies();
        ao.ClearDenendencies();
        environment.ClearDenendencies();
        GDX12ShaderCompiler::Shutdown();
        if (liveInfo) liveInfo->UnregisterMessageCallback(callback);
        std::cout << "FidelityFX GPU: SSSR fallback/history, DOF focus/blur/Off, quality and cached-state restoration passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
