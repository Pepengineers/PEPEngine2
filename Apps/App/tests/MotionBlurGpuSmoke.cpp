#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
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
                Require(std::isfinite(value) && value >= 0 && value < 100000, "Motion blur output must contain finite HDR values");
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
        { Require(console.FindConsoleVariable(name).SetFromString(value), "Motion blur CVar rejected a valid setting"); };
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
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), float(width) / height, 0.1f, 1000);
        projection._33 = 0.1f / (1000 - 0.1f);
        projection._43 = 1000 * projection._33;
        common.CameraViewToClip = projection;
        common.ClipToCameraView = projection.Invert();
        common.ActiveCameraViewProjection = common.ActiveCameraPreviousViewProjection = projection;

        GDX12Texture color, depth, motion;
        MakeInput(resources, color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, depth, width, height, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<uint16_t> pixels(width * height * 4), velocities(width * height * 2);
        std::vector<float> distances(width * height, projection._43 / 100 - projection._33);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = PackedVector::XMConvertFloatToHalf(4);
            pixels[i + 1] = PackedVector::XMConvertFloatToHalf(2);
            pixels[i + 2] = PackedVector::XMConvertFloatToHalf(0.5f);
            pixels[i + 3] = PackedVector::XMConvertFloatToHalf(1);
        }
        Upload(device, color, pixels.data(), 8);
        Upload(device, depth, distances.data(), 4);
        Upload(device, motion, velocities.data(), 4);
        auto reference = ReadColor(device, color);
        GDX12MotionBlurPass blur;
        blur.Setup(&resources, nullptr, &common);
        blur.SetInputs({ &color, &depth, &motion });
        blur.Initialize();
        auto* queue = device.GetCommandQueue();
        UINT frame = 0;
        const auto render = [&]
        {
            resources.CurrFrameConstantsIndex = frame++ % resources.FrameConstants.size();
            blur.PrepareFrame();
            auto* commands = queue->GetCommandList();
            blur.Execute(commands);
            queue->ExecuteCommandList(commands);
            queue->Flush();
            return ReadColor(device, *blur.GetOutput(0)->GetTexture());
        };
        Require(render() == reference, "Motion blur Off must preserve exact HDR pixels");
        set(L"Render.MotionBlur.Enabled", "true");
        set(L"Render.MotionBlur.MaxRadius", "8");
        set(L"Render.MotionBlur.Samples", "32");
        Require(render() == reference, "Zero motion must preserve exact HDR pixels");
        for (UINT i = 0; i < width * height; ++i)
            velocities[i * 2] = PackedVector::XMConvertFloatToHalf(48.0f / width);
        Upload(device, motion, velocities.data(), 4);
        auto result = render();
        for (size_t i = 0; i < result.size(); ++i)
            Require(std::abs(result[i] - reference[i]) < 0.005f, "Motion blur must preserve constant HDR color including image edges");

        std::fill(pixels.begin(), pixels.end(), 0);
        std::fill(velocities.begin(), velocities.end(), 0);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const size_t i = y * width + x;
                pixels[i * 4 + 3] = PackedVector::XMConvertFloatToHalf(1);
                if (x == 30)
                {
                    pixels[i * 4] = PackedVector::XMConvertFloatToHalf(4);
                    velocities[i * 2] = PackedVector::XMConvertFloatToHalf(48.0f / width);
                    distances[i] = projection._43 / 30 - projection._33;
                }
                if (y == 24)
                {
                    pixels[i * 4 + 1] = PackedVector::XMConvertFloatToHalf(3);
                    velocities[i * 2 + 1] = PackedVector::XMConvertFloatToHalf(-32.0f / height);
                    distances[i] = projection._43 / 50 - projection._33;
                }
            }
        Upload(device, color, pixels.data(), 8);
        Upload(device, depth, distances.data(), 4);
        Upload(device, motion, velocities.data(), 4);
        reference = ReadColor(device, color);
        result = render();
        float redSpread = 0, greenSpread = 0;
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const size_t i = (y * width + x) * 4;
                if (x != 30) redSpread += result[i];
                if (y != 24) greenSpread += result[i + 1];
            }
        std::cout << "Thin-feature spread red=" << redSpread << " green=" << greenSpread << '\n';
        Require(redSpread > 1 && greenSpread > 1, "Both orthogonal thin moving features must contribute outside their original pixels");
        for (auto& value : velocities)
            value = PackedVector::XMConvertFloatToHalf(-PackedVector::XMConvertHalfToFloat(value));
        Upload(device, motion, velocities.data(), 4);
        const auto reverse = render();
        float signError = 0;
        for (size_t i = 0; i < result.size(); ++i) signError = (std::max)(signError, std::abs(result[i] - reverse[i]));
        Require(signError < 0.01f, "Centered exposure must be invariant to reversing every motion vector");
        ++common.HistoryRevision;
        Require(render() == reference, "A camera cut must bypass stale motion for one frame");
        set(L"Render.MotionBlur.ShutterAngle", "0");
        Require(render() == reference, "Zero shutter must preserve exact HDR pixels");
        set(L"Render.MotionBlur.ShutterAngle", "180");
        std::fill(distances.begin(), distances.end(), 0);
        std::fill(velocities.begin(), velocities.end(), 0);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const size_t i = (y * width + x) * 4;
                pixels[i] = PackedVector::XMConvertFloatToHalf((x & 1) ? 4.0f : 0.0f);
                pixels[i + 1] = pixels[i + 2] = 0;
            }
        Upload(device, color, pixels.data(), 8);
        Upload(device, depth, distances.data(), 4);
        Upload(device, motion, velocities.data(), 4);
        reference = ReadColor(device, color);
        common.ActiveCameraPreviousViewProjection = Matrix::CreateRotationY(XMConvertToRadians(15.0f)) * projection;
        Require(render() == reference, "Re-enabling exposure must bypass stale motion for one frame");
        result = render();
        float skyDifference = 0;
        for (size_t i = 0; i < result.size(); ++i) skyDifference += std::abs(result[i] - reference[i]);
        Require(skyDifference > 1, "Sky must blur from camera rotation even with zero G-buffer motion");
        common.ActiveCameraPreviousViewProjection = Matrix::CreateTranslation(50, 0, 0) * projection;
        Require(render() == reference, "Camera translation must not move an infinitely distant sky");
        common.ActiveCameraPreviousViewProjection = projection;
        for (const char* quality : { "1", "2", "3", "0" })
        {
            set(L"Render.MotionBlur.Quality", quality);
            result = render();
            if (quality[0] == '0') Require(result == reference, "Quality Off must preserve exact HDR pixels");
        }
        blur.Resize();
        Require(render() == reference, "Resize must invalidate motion history");
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size;
            info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        blur.ClearDenendencies();
        GDX12ShaderCompiler::Shutdown();
        std::cout << "Motion blur GPU: HDR identity, two thin directions, sign symmetry, quality, cuts and frame slots passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}


