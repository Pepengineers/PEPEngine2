#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void Set(const wchar_t* name, const char* value)
    {
        Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "CVar rejected valid value");
    }

    void CheckSettings()
    {
        ConsoleModule console;
        for (const auto* name : { L"Render.MotionBlur.Enabled", L"Render.MotionBlur.Quality",
            L"Render.MotionBlur.ShutterFraction", L"Render.MotionBlur.MaxRadius", L"Render.MotionBlur.Samples",
            L"Render.MotionBlur.CenterWeight", L"Render.MotionBlur.VelocityThreshold" })
            Require(console.FindConsoleVariable(name).GetFlags() == EConsoleVariableFlags::Default, "Runtime CVar is read-only");
        const auto reject = [&](const wchar_t* name, const char* value)
        {
            auto& variable = console.FindConsoleVariable(name);
            const float previous = variable.GetFloat();
            Require(!variable.SetFromString(value) && variable.GetFloat() == previous, "Invalid CVar modified state");
        };
        reject(L"Render.MotionBlur.Quality", "4");
        reject(L"Render.MotionBlur.Quality", "-1");
        reject(L"Render.MotionBlur.Samples", "3");
        reject(L"Render.MotionBlur.Samples", "129");
        reject(L"Render.MotionBlur.MaxRadius", "0");
        reject(L"Render.MotionBlur.MaxRadius", "129");
        reject(L"Render.MotionBlur.ShutterFraction", "nan");
        reject(L"Render.MotionBlur.ShutterFraction", "inf");
        reject(L"Render.MotionBlur.ShutterFraction", "1.01");
        reject(L"Render.MotionBlur.CenterWeight", "0");
        reject(L"Render.MotionBlur.CenterWeight", "101");
        reject(L"Render.MotionBlur.VelocityThreshold", "0");
        reject(L"Render.MotionBlur.VelocityThreshold", "8.01");
        reject(L"Render.MotionBlur.VelocityThreshold", "nan");
        Set(L"Render.MotionBlur.Samples", "47");
        for (const auto* quality : { "1", "2", "3" })
        {
            Set(L"Render.MotionBlur.Quality", quality);
            Require(GetMotionBlurSettings().Samples == 47, "Quality discarded sample override");
        }
        Set(L"Render.MotionBlur.Samples", "0");
        Require(GetMotionBlurSettings().Samples == 35, "Sample inheritance failed");
        Set(L"Render.MotionBlur.Quality", "1");
        Require(GetMotionBlurSettings().Samples == 19, "Low preset failed");
        Set(L"Render.MotionBlur.Quality", "2");
        Require(GetMotionBlurSettings().Samples == 27, "Mid preset failed");
        Set(L"Render.MotionBlur.Quality", "0");
        Require(!GetMotionBlurSettings().Enabled, "Off still enabled");
        Set(L"Render.MotionBlur.Quality", "2");
        const auto ini = std::filesystem::path(L"Build/MotionBlur/settings.ini");
        {
            std::ofstream file(ini);
            file << "[ConsoleVariables]\nRender.MotionBlur.MaxRadius=24\nRender.MotionBlur.ShutterFraction=0.25\n";
        }
        Require(console.LoadIni(ini).Error.empty(), "Motion blur INI load failed");
        Require(GetMotionBlurSettings().MaxRadius == 24 && GetMotionBlurSettings().ShutterFraction == 0.25f,
            "INI did not set motion blur controls");
        Set(L"Render.MotionBlur.MaxRadius", "40");
        Set(L"Render.MotionBlur.ShutterFraction", "0.5");
        std::cout << "CVar ranges, finite validation, INI and overrides passed\n";
    }

    unsigned Channels(GDX12Texture& texture)
    {
        if (texture.GetFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT) return 4;
        if (texture.GetFormat() == DXGI_FORMAT_R8G8B8A8_UNORM) return 4;
        if (texture.GetFormat() == DXGI_FORMAT_R16G16_FLOAT) return 2;
        return 1;
    }

    void InitializeTexture(GDX12Texture& texture, GDX12DeviceResources& resources, UINT width, UINT height, DXGI_FORMAT format)
    {
        GDX12TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = desc.SRVDesc.Format = format;
        desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            desc.CreateRTV = true;
            desc.RTVHeap = resources.RTVHeap.get();
            desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
            desc.RTVDesc.Format = format;
            desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
        texture.Initialize(desc);
    }

    // Upload/readback exercise the actual production textures and shaders, including row padding.
    std::vector<float> Transfer(GDX12Device& device, GDX12Texture& texture, const std::vector<float>* upload = nullptr)
    {
        auto desc = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes = 0;
        device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = CD3DX12_HEAP_PROPERTIES(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> staging;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&staging)));
        const unsigned channels = Channels(texture);
        const bool unorm = texture.GetFormat() == DXGI_FORMAT_R8G8B8A8_UNORM;
        const bool half = !unorm && channels != 1;
        const unsigned elementBytes = unorm ? 1 : half ? 2 : 4;
        auto convert = [&](void* mapped, std::vector<float>& values, bool writing)
        {
            for (UINT y = 0; y < texture.GetHeight(); ++y)
                for (UINT x = 0; x < texture.GetWidth() * channels; ++x)
                {
                    auto* address = static_cast<unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch + x * elementBytes;
                    auto& value = values[y * texture.GetWidth() * channels + x];
                    if (writing)
                    {
                        if (unorm) *address = static_cast<unsigned char>(value * 255);
                        else if (half) *reinterpret_cast<uint16_t*>(address) = PackedVector::XMConvertFloatToHalf(value);
                        else *reinterpret_cast<float*>(address) = value;
                    }
                    else
                    {
                        value = unorm ? *address / 255.0f : half ?
                            PackedVector::XMConvertHalfToFloat(*reinterpret_cast<uint16_t*>(address)) : *reinterpret_cast<float*>(address);
                        Require(std::isfinite(value), "GPU readback contains NaN/Inf");
                    }
                }
        };
        std::vector<float> values(texture.GetWidth() * texture.GetHeight() * channels);
        if (upload)
        {
            Require(upload->size() == values.size(), "Wrong upload size");
            values = *upload;
            void* mapped = nullptr;
            const D3D12_RANGE empty = {};
            ThrowIfFailed(staging->Map(0, &empty, &mapped));
            convert(mapped, values, true);
            staging->Unmap(0, nullptr);
        }
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ upload ? texture.GetResource()->GetCopyDestBarrier() : texture.GetResource()->GetCopySourceBarrier() });
        D3D12_TEXTURE_COPY_LOCATION image = {}, memory = {};
        image.pResource = texture.GetResource()->D3DResource.Get();
        image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        memory.pResource = staging.Get();
        memory.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        memory.PlacedFootprint = footprint;
        commands->GetCommandList()->CopyTextureRegion(upload ? &image : &memory, 0, 0, 0, upload ? &memory : &image, nullptr);
        commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
        if (!upload)
        {
            void* mapped = nullptr;
            const D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
            ThrowIfFailed(staging->Map(0, &range, &mapped));
            convert(mapped, values, false);
            const D3D12_RANGE empty = {};
            staging->Unmap(0, &empty);
        }
        return values;
    }

    float ReverseDepth(float z) { return (1000.0f / z - 1.0f) / 9999.0f; }

    float Halton(unsigned index, unsigned base)
    {
        float value = 0;
        float fraction = 1;
        while (index)
        {
            fraction /= base;
            value += fraction * (index % base);
            index /= base;
        }
        return value;
    }

    GDX12CameraConstants Camera(float yaw = 0, float translation = 0, float jitterX = 0, float jitterY = 0,
        UINT width = 257, UINT height = 131)
    {
        GDX12CameraConstants result = {};
        result.NearPlane = 0.1f;
        result.FarPlane = 1000;
        auto projection = Matrix::CreatePerspectiveFieldOfView(XM_PI / 3.0f, float(width) / height, 1000, 0.1f);
        auto view = Matrix::CreateRotationY(yaw) * Matrix::CreateTranslation(translation, 0, 0);
        const auto vp = view * projection;
        XMStoreFloat4x4(&result.ViewProjNoJitter, XMMatrixTranspose(vp));
        XMStoreFloat4x4(&result.PrevViewProjNoJitter, XMMatrixTranspose(projection));
        projection._31 -= jitterX * 2 / width;
        projection._32 += jitterY * 2 / height;
        XMStoreFloat4x4(&result.ViewProj, XMMatrixTranspose(view * projection));
        XMStoreFloat4x4(&result.InvViewProj, XMMatrixTranspose((view * projection).Invert()));
        XMStoreFloat4x4(&result.View, XMMatrixTranspose(view));
        const auto location = view.Invert().Translation();
        result.CameraLocation = { location.x, location.y, location.z };
        return result;
    }

    void WritePPM(const char* path, const std::vector<float>& image, UINT width, UINT height)
    {
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        for (size_t i = 0; i < image.size(); i += 4)
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto value = static_cast<unsigned char>(std::pow((std::max)(0.0f, image[i + c]) / (1 + (std::max)(0.0f, image[i + c])), 1 / 2.2f) * 255);
                out.write(reinterpret_cast<const char*>(&value), 1);
            }
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        CheckSettings();
        if (argc == 2 && std::string_view(argv[1]) == "--cpu") return 0;
        Require(argc == 1, "Usage: MotionBlurTests [--cpu]");
        // Shared with all other feature chats, held for the entire GPU workload.
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(mutex != nullptr, "Cannot create GPU validation mutex");
        const auto wait = WaitForSingleObject(mutex, 0);
        if (wait == WAIT_TIMEOUT)
        {
            CloseHandle(mutex);
            std::cout << "GPU validation mutex busy; retry later\n";
            return 75;
        }
        Require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "Cannot acquire GPU validation mutex");
        struct MutexRelease
        {
            HANDLE Value;
            ~MutexRelease() { ReleaseMutex(Value); CloseHandle(Value); }
        } release{ mutex };
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
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = 257;
        common.WindowHeight = common.DownscaledHeight = 131;
        common.ActiveCameraCBufferIndex = 0;
        auto camera = Camera();
        for (auto& frame : resources.FrameConstants)
        {
            frame->CameraCB->Resize(1);
            frame->CameraCB->CopyData(0, camera);
        }
        GDX12Texture color, depth, motion;
        InitializeTexture(color, resources, 257, 131, DXGI_FORMAT_R16G16B16A16_FLOAT);
        InitializeTexture(depth, resources, 257, 131, DXGI_FORMAT_R32_FLOAT);
        InitializeTexture(motion, resources, 257, 131, DXGI_FORMAT_R16G16_FLOAT);
        color.GetResource()->D3DResource->SetName(L"Fixture.Color");
        depth.GetResource()->D3DResource->SetName(L"Fixture.Depth");
        motion.GetResource()->D3DResource->SetName(L"Fixture.Motion");
        GDX12MotionBlurPass pass;
        pass.Setup(&resources, nullptr, &common);
        pass.SetInputs({ &color, &depth, &motion });
        pass.Initialize();
        auto* queue = device.GetCommandQueue();
        const auto execute = [&]
        {
            resources.CurrFrameConstantsIndex = (resources.CurrFrameConstantsIndex + 1) % resources.FrameConstants.size();
            auto* commands = queue->GetCommandList();
            pass.Execute(commands);
            queue->ExecuteCommandList(commands);
            queue->Flush();
            common.ResetHistory = false;
        };
        std::vector<float> colors(257 * 131 * 4), depths(257 * 131, ReverseDepth(10)), motions(257 * 131 * 2, 0);
        for (UINT y = 0; y < 131; ++y)
            for (UINT x = 0; x < 257; ++x)
            {
                const size_t i = (y * 257 + x) * 4;
                colors[i] = x % 8 < 4 ? 8.0f : 0.25f;
                colors[i + 1] = y % 8 < 4 ? 2.0f : 0.125f;
                colors[i + 2] = 0.5f;
                colors[i + 3] = 1;
            }
        Transfer(device, color, &colors);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        execute();
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Stationary image is not exact identity");
        for (size_t i = 0; i < motions.size(); i += 2) motions[i] = 64.0f / 257;
        Transfer(device, motion, &motions);
        Set(L"Render.MotionBlur.Enabled", "false");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Off is not exact identity");
        Set(L"Render.MotionBlur.Enabled", "true");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Re-enable used old motion");
        execute();
        const auto positive = Transfer(device, *pass.GetOutput(0)->GetTexture());
        Require(positive != colors, "Moving image was not blurred");
        Set(L"Render.MotionBlur.Samples", "47");
        execute();
        const auto overridden = Transfer(device, *pass.GetOutput(0)->GetTexture());
        Require(overridden != positive, "Manual sample override did not change GPU work");
        Set(L"Render.MotionBlur.Quality", "3");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == overridden, "GPU quality change discarded manual samples");
        Set(L"Render.MotionBlur.Quality", "2");
        Set(L"Render.MotionBlur.Samples", "0");
        for (auto& value : motions) value = -value;
        Transfer(device, motion, &motions);
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == positive, "Centered exposure changes with velocity sign");
        common.ResetHistory = true;
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Camera cut used stale blur");
        Require(Transfer(device, *pass.GetOutput(1)->GetTexture()) == std::vector<float>(motions.size(), 0), "Cut did not reset motion");
        Set(L"Render.MotionBlur.ShutterFraction", "0");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Zero shutter is not identity");
        Set(L"Render.MotionBlur.ShutterFraction", "0.5");

        // A thin moving foreground feature must spread onto a stationary background.
        std::fill(colors.begin(), colors.end(), 0.0f);
        std::fill(motions.begin(), motions.end(), 0.0f);
        for (UINT y = 0; y < 131; ++y)
            for (UINT x = 0; x < 257; ++x)
            {
                const size_t i = y * 257 + x;
                colors[i * 4 + 2] = 1;
                colors[i * 4 + 3] = 1;
                if (x >= 126 && x <= 130)
                {
                    colors[i * 4] = 8;
                    colors[i * 4 + 2] = 0;
                    depths[i] = ReverseDepth(2);
                    motions[i * 2] = 64.0f / 257;
                }
            }
        Transfer(device, color, &colors);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        execute();
        execute();
        const auto feature = Transfer(device, *pass.GetOutput(0)->GetTexture());
        Require(feature[(65 * 257 + 133) * 4] > 0.05f, "Foreground feature does not spread");
        Require(feature[(65 * 257 + 128) * 4] > 0.1f, "Thin feature disappeared");
        Require(feature[(65 * 257 + 160) * 4] == 0, "Foreground spread exceeds shutter radius");
        WritePPM("Build/MotionBlur/feature-input.ppm", colors, 257, 131);
        WritePPM("Build/MotionBlur/feature-output.ppm", feature, 257, 131);
        Set(L"Render.MotionBlur.MaxRadius", "2");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture())[(65 * 257 + 133) * 4] == 0,
            "Output-pixel radius override did not bound foreground spreading");
        Set(L"Render.MotionBlur.MaxRadius", "40");
        Set(L"Render.MotionBlur.CenterWeight", "1");
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture())[(65 * 257 + 128) * 4] > feature[(65 * 257 + 128) * 4],
            "Center-weight override did not preserve the thin feature more strongly");
        Set(L"Render.MotionBlur.CenterWeight", "40");

        // A stationary foreground must occlude a rapidly moving background.
        for (UINT y = 0; y < 131; ++y)
            for (UINT x = 0; x < 257; ++x)
            {
                const size_t i = y * 257 + x;
                const bool foreground = x >= 126 && x <= 130;
                colors[i * 4] = foreground ? 0.0f : 8.0f;
                colors[i * 4 + 1] = foreground ? 1.0f : 0.0f;
                colors[i * 4 + 2] = 0;
                depths[i] = ReverseDepth(foreground ? 2.0f : 10.0f);
                motions[i * 2] = foreground ? 0 : 64.0f / 257;
            }
        Transfer(device, color, &colors);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        execute();
        const auto occluded = Transfer(device, *pass.GetOutput(0)->GetTexture());
        Require(occluded[(65 * 257 + 128) * 4] == 0 && occluded[(65 * 257 + 128) * 4 + 1] == 1,
            "Moving background leaked through stationary foreground");

        // A faster horizontal neighbor must not erase a pixel's vertical blur direction.
        for (UINT y = 0; y < 131; ++y)
            for (UINT x = 0; x < 257; ++x)
            {
                const size_t i = y * 257 + x;
                colors[i * 4] = x >= 160 ? 8.0f : 0.0f;
                colors[i * 4 + 1] = x < 160 && y % 16 < 8 ? 2.0f : 0.0f;
                motions[i * 2] = x >= 160 ? 0.6f : 0;
                motions[i * 2 + 1] = x < 160 ? 0.4f : 0;
                depths[i] = ReverseDepth(10);
            }
        Transfer(device, color, &colors);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        execute();
        const auto crossed = Transfer(device, *pass.GetOutput(0)->GetTexture());
        const size_t crossing = (65 * 257 + 128) * 4;
        Require(crossed[crossing] == 0 && crossed[crossing + 1] > 0.1f && crossed[crossing + 1] < 1.9f,
            "Gather lost the secondary motion direction");
        for (auto& value : motions) value = -value;
        Transfer(device, motion, &motions);
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == crossed, "Crossed motion is not sign symmetric");
        WritePPM("Build/MotionBlur/crossed.ppm", crossed, 257, 131);

        // Sky has rotational motion but must ignore camera translation, including projection jitter.
        std::fill(depths.begin(), depths.end(), 0.0f);
        std::fill(motions.begin(), motions.end(), 0.0f);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        camera = Camera(0, 50, 0.25f, -0.375f);
        for (auto& frame : resources.FrameConstants) frame->CameraCB->CopyData(0, camera);
        execute();
        auto sky = Transfer(device, *pass.GetOutput(1)->GetTexture());
        Require(std::all_of(sky.begin(), sky.end(), [](float v) { return std::abs(v) < 0.00001f; }), "Sky responds to translation/jitter");
        camera = Camera(0.05f, 50, 0.25f, -0.375f);
        for (auto& frame : resources.FrameConstants) frame->CameraCB->CopyData(0, camera);
        execute();
        sky = Transfer(device, *pass.GetOutput(1)->GetTexture());
        Require(std::abs(sky[(65 * 257 + 128) * 2]) > 0.01f, "Sky ignores camera rotation");

        // Resize all connected grids, and check the first frame is neutral.
        queue->Flush();
        color.Resize(129, 67);
        depth.Resize(129, 67);
        motion.Resize(129, 67);
        common.WindowWidth = common.DownscaledWidth = 129;
        common.WindowHeight = common.DownscaledHeight = 67;
        pass.Resize();
        colors.assign(129 * 67 * 4, 2);
        depths.assign(129 * 67, ReverseDepth(10));
        motions.assign(129 * 67 * 2, 0.1f);
        Transfer(device, color, &colors);
        Transfer(device, depth, &depths);
        Transfer(device, motion, &motions);
        execute();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == colors, "Resize retained blur history");
        for (const auto* radius : { "1", "16", "128", "40" })
        {
            Set(L"Render.MotionBlur.MaxRadius", radius);
            execute();
            const auto resized = Transfer(device, *pass.GetOutput(0)->GetTexture());
            for (size_t i = 0; i < resized.size(); ++i)
                if (resized[i] != colors[i])
                {
                    std::cerr << "radius=" << radius << " index=" << i << " actual=" << resized[i] << " expected=" << colors[i] << '\n';
                    throw std::runtime_error("Radius change corrupted constant HDR");
                }
        }

        // The same moving HDR workload at native and XeSS input grids.
        common.WindowWidth = common.DownscaledWidth = 1280;
        common.WindowHeight = common.DownscaledHeight = 720;
        const auto workload = [&](UINT width, UINT height)
        {
            queue->Flush();
            color.Resize(width, height);
            depth.Resize(width, height);
            motion.Resize(width, height);
            pass.Resize();
            colors.resize(width * height * 4);
            depths.assign(width * height, ReverseDepth(10));
            motions.resize(width * height * 2);
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                {
                    const size_t i = y * width + x;
                    const float u = (x + 0.5f) / width;
                    const float v = (y + 0.5f) / height;
                    colors[i * 4] = std::sin(u * 160) > 0 ? 4.0f : 0.125f;
                    colors[i * 4 + 1] = std::sin(v * 90) > 0 ? 2.0f : 0.125f;
                    colors[i * 4 + 2] = 0.25f;
                    colors[i * 4 + 3] = 1;
                    motions[i * 2] = 0.05f;
                    motions[i * 2 + 1] = -0.03f;
                }
            Transfer(device, color, &colors);
            Transfer(device, depth, &depths);
            Transfer(device, motion, &motions);
            camera = Camera(0, 0, 0, 0, width, height);
            for (auto& frame : resources.FrameConstants) frame->CameraCB->CopyData(0, camera);
            execute();
            execute();
        };
        const auto benchmark = [&](const char* grid)
        {
            GDX12GPUTimer timer(&device);
            const float scale = (std::max)(float(color.GetWidth()) / common.WindowWidth, float(color.GetHeight()) / common.WindowHeight);
            const UINT tile = static_cast<UINT>(std::ceil(GetMotionBlurSettings().MaxRadius * scale));
            UINT64 allocated = 0;
            for (unsigned resource = 0; resource < 5; ++resource)
            {
                const UINT width = resource >= 3 ? (color.GetWidth() + tile - 1) / tile : color.GetWidth();
                const UINT height = resource >= 3 ? (color.GetHeight() + tile - 1) / tile : color.GetHeight();
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(resource == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT,
                    width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                allocated += device.GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            }
            std::cout << grid << " five_texture_allocation_bytes=" << allocated << '\n';
            for (const auto* quality : { "1", "2", "3" })
            {
                Set(L"Render.MotionBlur.Quality", quality);
                double total = 0;
                for (unsigned frame = 0; frame < 21; ++frame)
                {
                    auto* commands = queue->GetCommandList();
                    timer.Start(commands);
                    pass.Execute(commands);
                    timer.Stop(commands);
                    queue->ExecuteCommandList(commands);
                    timer.OnCommandListExecuted(commands->FenceValue);
                    queue->Flush();
                    if (frame >= 5) total += timer.GetTimeMS();
                }
                std::cout << grid << ' ' << color.GetWidth() << 'x' << color.GetHeight()
                    << " quality=" << quality << " samples=" << GetMotionBlurSettings().Samples
                    << " motion_blur_gpu_ms=" << total / 16 << '\n';
            }
        };
        workload(1280, 720);
        benchmark("native");
        const auto native = Transfer(device, *pass.GetOutput(0)->GetTexture());
        WritePPM("Build/MotionBlur/native.ppm", native, 1280, 720);

        GDX12XeSSUpscalePass xess;
        xess.Setup(&resources, nullptr, &common);
        xess.SetInputs({ &depth, pass.GetOutput(1), pass.GetOutput(0) });
        workload(common.DownscaledWidth, common.DownscaledHeight);
        xess.Initialize();
        auto* warmup = queue->GetCommandList();
        pass.Execute(warmup);
        xess.Execute(warmup);
        queue->ExecuteCommandList(warmup);
        queue->Flush();
        benchmark("before-XeSS");
        // Repeat a deterministic frame with reset to compare current-frame filter quality.
        common.ResetHistory = false;
        auto* commands = queue->GetCommandList();
        pass.Execute(commands);
        common.ResetHistory = true;
        xess.Execute(commands);
        common.ResetHistory = false;
        queue->ExecuteCommandList(commands);
        queue->Flush();
        const auto upscaled = Transfer(device, *xess.GetOutput(0)->GetTexture());
        Require(upscaled.size() == native.size(), "XeSS output grid is wrong");
        double error = 0;
        float maximum = 0;
        for (size_t i = 0; i < native.size(); i += 4)
            for (unsigned c = 0; c < 3; ++c)
            {
                error += std::abs(native[i + c] - upscaled[i + c]);
                maximum = (std::max)(maximum, upscaled[i + c]);
            }
        error /= (native.size() / 4) * 3;
        Require(maximum > 1, "XeSS HDR path clipped highlights");
        Require(error < 0.08, "XeSS motion-blur image diverged from native reference");
        std::cout << "XeSS/native linear HDR MAE=" << error << " maximum=" << maximum << '\n';
        WritePPM("Build/MotionBlur/xess.ppm", upscaled, 1280, 720);

        // Static and moving analytic worlds with real projection jitter excluded from motion.
        const UINT inputWidth = color.GetWidth(), inputHeight = color.GetHeight();
        for (unsigned moving : { 0u, 1u })
        {
            motions.assign(inputWidth * inputHeight * 2, 0);
            if (moving)
                for (size_t i = 0; i < motions.size(); i += 2)
                {
                    motions[i] = 0.05f;
                    motions[i + 1] = -0.03f;
                }
            Transfer(device, motion, &motions);
            for (unsigned frame = 0; frame < 16; ++frame)
            {
                const float jx = Halton(frame + 1, 2) - 0.5f;
                const float jy = Halton(frame + 1, 3) - 0.5f;
                for (UINT y = 0; y < inputHeight; ++y)
                    for (UINT x = 0; x < inputWidth; ++x)
                    {
                        const size_t i = (y * inputWidth + x) * 4;
                        colors[i] = 2 + std::sin((x + 0.5f - jx) / inputWidth * 20 + moving * frame * 0.5f);
                        colors[i + 1] = 2 + std::sin((y + 0.5f - jy) / inputHeight * 20 + moving * frame * 0.3f);
                    }
                Transfer(device, color, &colors);
                camera = Camera(0, 0, jx, jy, inputWidth, inputHeight);
                resources.CurrFrameConstantsIndex = frame % resources.FrameConstants.size();
                resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraCB->CopyData(0, camera);
                common.ActiveCameraJitterOffsetX = jx;
                common.ActiveCameraJitterOffsetY = jy;
                common.ResetHistory = frame == 0;
                auto* temporal = queue->GetCommandList();
                pass.Execute(temporal);
                xess.Execute(temporal);
                queue->ExecuteCommandList(temporal);
                queue->Flush();
            }
            const auto stationary = Transfer(device, *xess.GetOutput(0)->GetTexture());
            double jitterError = 0;
            for (UINT y = 8; y < 712; ++y)
                for (UINT x = 8; x < 1272; ++x)
                {
                    const size_t i = (y * 1280 + x) * 4;
                    jitterError += std::abs(stationary[i] - (2 + std::sin((x + 0.5f) / 1280 * 20 + moving * 15 * 0.5f)));
                    jitterError += std::abs(stationary[i + 1] - (2 + std::sin((y + 0.5f) / 720 * 20 + moving * 15 * 0.3f)));
                }
            jitterError /= 1264 * 704 * 2;
            Require(jitterError < 0.03, "XeSS motion/jitter did not converge to the analytic moving/static image");
            std::cout << "XeSS 16-frame moving=" << moving << " jitter linear HDR MAE=" << jitterError << '\n';
        }

        GDX12Texture display;
        InitializeTexture(display, resources, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM);
        GDX12OutputToScreenPass toneMap;
        toneMap.Setup(&resources, nullptr, &common);
        toneMap.SetInputs({ xess.GetOutput(0), &display });
        toneMap.Initialize();
        auto* finalFrame = queue->GetCommandList();
        pass.Execute(finalFrame);
        xess.Execute(finalFrame);
        toneMap.Execute(finalFrame);
        queue->ExecuteCommandList(finalFrame);
        queue->Flush();
        const auto displayPixels = Transfer(device, display);
        const size_t centerPixel = (360 * 1280 + 640) * 4;
        const float hdrCenter = Transfer(device, *xess.GetOutput(0)->GetTexture())[centerPixel];
        const float linear = hdrCenter / (1 + hdrCenter);
        const float expectedDisplay = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
        Require(std::abs(displayPixels[centerPixel] - expectedDisplay) < 1.5f / 255,
            "XeSS -> tone mapping lost native command-list bindings or encoded HDR twice");
        std::cout << "Same-list motion blur -> XeSS -> tone map passed\n";
        toneMap.ClearDenendencies();
        xess.ClearDenendencies();
        workload(1280, 720);
        benchmark("native-repeat");
        for (unsigned frame = 0; frame < 12; ++frame)
        {
            resources.CurrFrameConstantsIndex = frame % resources.FrameConstants.size();
            Set(L"Render.MotionBlur.Quality", frame % 2 ? "3" : "1");
            if (frame == 4) Set(L"Render.MotionBlur.MaxRadius", "1");
            if (frame == 6) Set(L"Render.MotionBlur.MaxRadius", "40");
            auto* queued = queue->GetCommandList();
            pass.Execute(queued);
            queue->ExecuteCommandList(queued);
        }
        queue->Flush();
        Require(Transfer(device, *pass.GetOutput(0)->GetTexture()) == native,
            "In-flight frame slots or runtime quality/radius changes corrupted the output");
        bool debugError = false;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            info->GetMessage(i, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << message->pDescription << '\n';
                debugError = true;
            }
        }
        Require(!debugError, "D3D12 debug layer reported errors");
        pass.ClearDenendencies();
        std::cout << "GPU identity, shutter/sign, foreground spreading, sky, cut/resize/re-enable, odd grids and frame slots passed; debug layer clean\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
