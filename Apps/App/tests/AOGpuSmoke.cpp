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
        GDX12TextureDesc desc;
        desc.Width = width; desc.Height = height;
        desc.Format = desc.SRVDesc.Format = format;
        desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        texture.Initialize(desc);
    }

    void Upload(GDX12Device& device, GDX12Texture& texture, const void* pixels, UINT bytesPerPixel)
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
                static_cast<const char*>(pixels) + y * texture.GetWidth() * bytesPerPixel, texture.GetWidth() * bytesPerPixel);
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

    std::vector<float> Read(GDX12Device& device, GDX12Texture& texture)
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
        std::vector<float> pixels(texture.GetWidth() * texture.GetHeight());
        for (UINT y = 0; y < texture.GetHeight(); ++y)
            std::memcpy(pixels.data() + y * texture.GetWidth(),
                static_cast<const char*>(mapping) + footprint.Offset + y * footprint.Footprint.RowPitch,
                texture.GetWidth() * sizeof(float));
        const D3D12_RANGE noWrite = {};
        readback->Unmap(0, &noWrite);
        for (float value : pixels)
            Require(std::isfinite(value) && value >= 0 && value <= 1, "AO must be finite normalized visibility");
        return pixels;
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
        { Require(console.FindConsoleVariable(name).SetFromString(value), "AO CVar rejected a test setting"); };
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
        constexpr UINT width = 65, height = 49;
        GDX12Texture depth, normal;
        MakeInput(resources, depth, width, height, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, normal, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), float(width) / height, 0.1f, 1000.0f);
        projection._33 = 0.1f / (1000.0f - 0.1f);
        projection._43 = 1000.0f * projection._33;
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = width;
        common.WindowHeight = common.DownscaledHeight = height;
        common.ActiveCameraCBufferIndex = 0;
        common.CameraViewToClip = projection;
        common.ClipToCameraView = projection.Invert();
        GDX12CameraConstants camera = {};
        camera.View = Identity4x4();
        camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
        camera.InvViewProj = projection.Invert().Transpose();
        camera.NearPlane = 0.1f; camera.FarPlane = 1000.0f;
        for (auto& frame : resources.FrameConstants)
        {
            frame->CameraCB->Resize(1);
            frame->CameraCB->CopyData(0, camera);
        }
        std::vector<XMFLOAT4> normals(width * height, { 0, 0, 1, 0.5f });
        Upload(device, normal, normals.data(), sizeof(XMFLOAT4));
        std::vector<float> depths(width * height, projection._43 / 10.0f - projection._33);
        Upload(device, depth, depths.data(), sizeof(float));
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common);
        ao.SetInputs({ &depth, &normal });
        ao.Initialize();
        UINT frameIndex = 0;
        const auto render = [&]
        {
            resources.CurrFrameConstantsIndex = frameIndex++ % static_cast<UINT>(resources.FrameConstants.size());
            auto* queue = device.GetCommandQueue();
            auto* commands = queue->GetCommandList();
            ao.Execute(commands);
            queue->ExecuteCommandList(commands);
            queue->Flush();
            return Read(device, *ao.GetOutput(0)->GetTexture());
        };
        set(L"Render.AO.Radius", "5");
        set(L"Render.AO.Quality", "3");
        for (UINT mode = 0; mode < 3; ++mode)
        {
            const auto modeValue = std::to_string(mode);
            set(L"Render.AO.Implementation", modeValue.c_str());
            for (const auto* denoise : { "0", "1", "4" })
            {
                set(L"Render.AO.DenoisePasses", denoise);
                const auto pixels = render();
                std::cout << "AO " << mode << " denoise " << denoise << " flat minimum "
                    << *std::min_element(pixels.begin(), pixels.end()) << " center " << pixels[(height / 2) * width + width / 2]
                    << " min index " << std::distance(pixels.begin(), std::min_element(pixels.begin(), pixels.end())) << '\n';
                // XeGTAO's projected-normal approximation darkens oblique corners even without occluders.
                const float minimum = mode == 2 ? 0.85f : 0.97f;
                Require(*std::min_element(pixels.begin(), pixels.end()) > minimum &&
                    pixels[(height / 2) * width + width / 2] > 0.97f,
                    "A flat plane must preserve neutral center and the algorithm's unoccluded baseline");
                if (mode == 2 && std::string(denoise) == "0")
                    Require(std::abs(pixels[0] - 0.91764706f) < 1.5f / 255.0f,
                        "XeGTAO corner must match the independently evaluated planar reference");
            }
        }
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width / 2; ++x) depths[y * width + x] = projection._43 / 8.0f - projection._33;
        Upload(device, depth, depths.data(), sizeof(float));
        set(L"Render.AO.DenoisePasses", "1");
        for (UINT mode = 0; mode < 3; ++mode)
        {
            const auto modeValue = std::to_string(mode);
            set(L"Render.AO.Implementation", modeValue.c_str());
            const auto pixels = render();
            float minimum = 1;
            for (UINT y = 8; y < height - 8; ++y)
                for (UINT x = width / 2; x < width / 2 + 8; ++x) minimum = (std::min)(minimum, pixels[y * width + x]);
            std::cout << "AO " << mode << " depth-step minimum " << minimum << '\n';
            Require(minimum < 0.97f, "The depth discontinuity must produce actual AO in every algorithm");
        }
        set(L"Render.AO.Quality", "0");
        for (float value : render()) Require(value == 1, "Off must clear stale AO to neutral visibility");
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size;
            info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        std::cout << "AO GPU: three algorithms, odd viewport, flat/depth-step, denoise and Off passed.\n";
        if (liveInfo) liveInfo->UnregisterMessageCallback(callback);
        ao.ClearDenendencies();
        GDX12ShaderCompiler::Shutdown();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
