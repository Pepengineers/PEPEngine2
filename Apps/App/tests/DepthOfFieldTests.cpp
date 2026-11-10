#include "Engine.RendererDX12/GDX12DepthOfField.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Set(const wchar_t* name, const char* value)
{
    Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "CVar write failed");
}

static Matrix Projection(float fov, float nearCm = 0.1f, float farCm = 10000.0f)
{
    Matrix p = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(fov), 16.0f / 9.0f, nearCm, farCm);
    p._33 = nearCm / (farCm - nearCm);
    p._43 = p._33 * farCm;
    return p;
}

static float DeviceDepth(float cm, const Matrix& p)
{
    return (-cm * p._33 + p._43) / cm;
}

static void TestSettingsAndOptics()
{
    Set(L"Render.DOF.Enabled", "true");
    Set(L"Render.DOF.Rings", "0");
    Set(L"Render.DOF.CocLimit", "0");
    Set(L"Render.DOF.FixedCocLimit", "false");
    const unsigned rings[] = { 0, 3, 5, 8, 12 };
    const float limits[] = { 0.0f, 0.025f, 0.05f, 0.1f, 0.1f };
    for (int q = 0; q <= 4; ++q)
    {
        Set(L"Render.DOF.Quality", std::to_string(q).c_str());
        const auto settings = GetDepthOfFieldSettings();
        Require(settings.Rings == rings[q] && settings.CocLimit == limits[q] && settings.Enabled == (q != 0),
            "Legacy quality mapping changed");
    }
    Set(L"Render.DOF.FixedCocLimit", "true");
    for (int q : { 1, 2, 3, 4 })
    {
        Set(L"Render.DOF.Quality", std::to_string(q).c_str());
        Require(GetDepthOfFieldSettings().CocLimit == 0.05f, "Named quality changed inherited CoC");
    }
    Set(L"Render.DOF.Rings", "7");
    Set(L"Render.DOF.CocLimit", "0.07");
    Set(L"Render.DOF.FocusDistanceCm", "200");
    Set(L"Render.DOF.FNumber", "0.7");
    Set(L"Render.DOF.SensorHeightMm", "36");
    for (int q : { 1, 3, 0, 4, 2 })
    {
        Set(L"Render.DOF.Quality", std::to_string(q).c_str());
        const auto settings = GetDepthOfFieldSettings();
        Require(settings.Rings == 7 && settings.CocLimit == 0.07f && settings.FocusDistanceCm == 200.0f &&
            settings.FNumber == 0.7f, "Quality overwrote manual optics or quality overrides");
    }
    for (const auto* name : { L"Render.DOF.FocusDistanceCm", L"Render.DOF.FNumber", L"Render.DOF.SensorHeightMm", L"Render.DOF.CocLimit" })
    {
        auto& cvar = ConsoleModule{}.FindConsoleVariable(name);
        const float before = cvar.GetFloat();
        for (const char* bad : { "nan", "inf", "-1", "10000001" })
            Require(!cvar.SetFromString(bad) && cvar.GetFloat() == before, "Invalid optical value was accepted");
    }
    Require(!ConsoleModule{}.FindConsoleVariable(L"Render.DOF.Quality").SetFromString("5"), "Invalid quality accepted");
    Require(!ConsoleModule{}.FindConsoleVariable(L"Render.DOF.Rings").SetFromString("13"), "Invalid ring count accepted");
    Set(L"Render.DOF.Rings", "0");
    Set(L"Render.DOF.CocLimit", "0");
    Set(L"Render.DOF.FixedCocLimit", "false");
    Set(L"Render.DOF.Quality", "2");
    const auto settings = GetDepthOfFieldSettings();
    Require(settings.Rings == 5 && settings.CocLimit == 0.05f, "Override reset did not restore the preset");
    for (float fov : { 15.0f, 30.0f, 60.0f, 100.0f })
        for (unsigned height : { 65u, 360u, 720u, 1080u })
        {
            const auto p = Projection(fov);
            const auto coc = CalculateDepthOfFieldCoc(settings, XMConvertToRadians(fov), height, p._33, p._34, p._43);
            const double sensorCm = 3.6;
            const double focalCm = sensorCm / (2 * std::tan(XMConvertToRadians(fov) / 2));
            for (float distance : { 30.0f, 100.0f, 200.0f, 1000.0f, 10000.0f })
            {
                // Independent thin-lens derivation: image distance vs the fixed focused sensor plane.
                const double imageAtFocus = focalCm * 200.0 / (200.0 - focalCm);
                const double imageAtObject = focalCm * distance / (distance - focalCm);
                const double expected = (focalCm / (2 * 0.7)) * (1 - imageAtFocus / imageAtObject) * height * 0.5 / sensorCm;
                const double actual = coc.Scale * DeviceDepth(distance, p) + coc.Bias;
                Require(std::abs(actual - expected) < 0.0002 * (1 + std::abs(expected)), "CoC disagrees with independent thin-lens optics");
                Require(distance == 200 || (actual > 0) == (distance < 200), "CoC near/far sign is wrong");
            }
        }
    std::cout << "CPU: CVar validation, presets/overrides, RH reverse-Z thin-lens CoC at four FOVs/resolutions passed.\n";
}

static void CreateFixtureTexture(GDX12Texture& texture, GDX12DeviceResources& resources, unsigned width, unsigned height, DXGI_FORMAT format)
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
    texture.Initialize(desc);
}

static void Upload(GDX12Device& device, GDX12Texture& texture, const void* pixels, unsigned bytesPerPixel)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* mapped;
    ThrowIfFailed(upload->Map(0, nullptr, &mapped));
    for (unsigned y = 0; y < texture.GetHeight(); ++y)
        memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
            static_cast<const uint8_t*>(pixels) + y * texture.GetWidth() * bytesPerPixel, texture.GetWidth() * bytesPerPixel);
    upload->Unmap(0, nullptr);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
    CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint);
    CD3DX12_TEXTURE_COPY_LOCATION destination(texture.GetResource()->D3DResource.Get(), 0);
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    queue->ExecuteCommandList(commands);
    queue->Flush();
}

static std::vector<uint16_t> Read(GDX12Device& device, GDX12Texture& texture, unsigned bytesPerPixel = 8)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped));
    std::vector<uint16_t> pixels(texture.GetWidth() * texture.GetHeight() * bytesPerPixel / 2);
    for (unsigned y = 0; y < texture.GetHeight(); ++y)
        memcpy(pixels.data() + y * texture.GetWidth() * bytesPerPixel / 2,
            static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch, texture.GetWidth() * bytesPerPixel);
    readback->Unmap(0, nullptr);
    return pixels;
}

static std::vector<uint16_t> FixtureImage(unsigned width, unsigned height)
{
    std::vector<uint16_t> pixels(width * height * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const bool checker = ((x / 2 + y / 2) & 1) != 0;
            const bool highlight = x % (width / 8) < 8 && y % (height / 4) < 8;
            for (unsigned c = 0; c < 3; ++c)
                pixels[(y * width + x) * 4 + c] = PackedVector::XMConvertFloatToHalf(highlight ? 64.0f : checker ? 0.5f : 0.0f);
            pixels[(y * width + x) * 4 + 3] = PackedVector::XMConvertFloatToHalf(1.0f);
        }
    return pixels;
}

static void SaveImage(const std::string& name, const std::vector<uint16_t>& pixels, unsigned width, unsigned height)
{
    std::ofstream file(name, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (unsigned c = 0; c < 3; ++c)
        {
            float value = (std::max)(0.0f, PackedVector::XMConvertHalfToFloat(pixels[i + c]));
            value = value / (1 + value);
            file.put(static_cast<char>(std::pow(value, 1.0f / 2.2f) * 255.0f));
        }
}

static void ValidateImage(const std::vector<uint16_t>& pixels)
{
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (unsigned c = 0; c < 3; ++c)
        {
            const float v = PackedVector::XMConvertHalfToFloat(pixels[i + c]);
            Require(std::isfinite(v) && v >= 0, "DoF returned invalid HDR pixels");
        }
}

static std::vector<uint16_t> Run(GDX12Device& device, GDX12RenderPass& pass)
{
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    pass.Execute(commands);
    queue->ExecuteCommandList(commands);
    return Read(device, *pass.GetOutput(0)->GetTexture());
}

static double MeasurePass(GDX12Device& device, GDX12RenderPass& pass)
{
    GDX12GPUTimer timer(&device);
    auto* queue = device.GetCommandQueue();
    std::vector<double> samples;
    for (unsigned frame = 0; frame < 24; ++frame)
    {
        auto* commands = queue->GetCommandList();
        timer.Start(commands);
        pass.Execute(commands);
        timer.Stop(commands);
        queue->ExecuteCommandList(commands);
        timer.OnCommandListExecuted(commands->FenceValue);
        queue->Flush();
        if (frame >= 8) samples.push_back(timer.GetTimeMS());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

static UINT64 LocalMemoryUsage(GDX12Device& device)
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter3> adapter;
    ThrowIfFailed(factory->EnumAdapterByLuid(device.GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    ThrowIfFailed(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
    return info.CurrentUsage;
}

static void TestGPU(GDX12Device& device, GDX12DeviceResources& resources, unsigned width, unsigned height)
{
    RenderPipelineCommonData common;
    common.ActiveCameraFOV = 30.0f;
    common.CameraViewToClip = Projection(30);
    common.WindowWidth = common.DownscaledWidth = width;
    common.WindowHeight = common.DownscaledHeight = height;
    GDX12Texture color, depth;
    CreateFixtureTexture(color, resources, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateFixtureTexture(depth, resources, width, height, DXGI_FORMAT_R32_FLOAT);
    auto pixels = FixtureImage(width, height);
    std::vector<float> depths(width * height, DeviceDepth(200, common.CameraViewToClip));
    Upload(device, color, pixels.data(), 8);
    Upload(device, depth, depths.data(), 4);
    GDX12DepthOfFieldPass dof;
    dof.Setup(&resources, nullptr, &common);
    dof.SetInputs({ &color, &depth });
    dof.Initialize();
    Set(L"Render.DOF.Enabled", "false");
    Require(Run(device, dof) == pixels, "Disabled DoF is not an exact copy");
    Set(L"Render.DOF.Enabled", "true");
    Set(L"Render.DOF.Quality", "0");
    Require(Run(device, dof) == pixels, "Quality Off is not an exact copy");
    Set(L"Render.DOF.Quality", "4");
    auto focused = Run(device, dof);
    Require(focused == pixels, "In-focus DoF changed HDR pixels");
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            depths[y * width + x] = DeviceDepth(x < width / 3 ? 50.0f : x < 2 * width / 3 ? 200.0f : 1600.0f, common.CameraViewToClip);
    Upload(device, depth, depths.data(), 4);
    std::vector<std::vector<uint16_t>> outputs;
    for (int quality : { 1, 2, 3, 4 })
    {
        Set(L"Render.DOF.Quality", std::to_string(quality).c_str());
        GDX12GPUTimer timer(&device);
        auto* queue = device.GetCommandQueue();
        Run(device, dof);
        std::vector<double> times;
        for (unsigned frame = 0; frame < 16; ++frame)
        {
            resources.CurrFrameConstantsIndex = frame % static_cast<unsigned>(resources.FrameConstants.size());
            auto* commands = queue->GetCommandList();
            timer.Start(commands);
            dof.Execute(commands);
            timer.Stop(commands);
            queue->ExecuteCommandList(commands);
            timer.OnCommandListExecuted(commands->FenceValue);
            queue->Flush();
            times.push_back(timer.GetTimeMS());
        }
        std::sort(times.begin(), times.end());
        auto output = Read(device, *dof.GetOutput(0)->GetTexture());
        ValidateImage(output);
        unsigned nearChanges = 0, farChanges = 0, focusedChanges = 0, bokeh = 0;
        for (unsigned y = 8; y + 8 < height; ++y)
            for (unsigned x = 8; x + 8 < width; ++x)
            {
                const auto i = (y * width + x) * 4;
                const float value = PackedVector::XMConvertHalfToFloat(output[i]);
                const float original = PackedVector::XMConvertHalfToFloat(pixels[i]);
                if (std::abs(value - original) > 0.03f)
                {
                    if (x < width / 3 - 32) ++nearChanges;
                    if (x > 2 * width / 3 + 32) ++farChanges;
                    if (x > width / 3 + static_cast<unsigned>(GetDepthOfFieldSettings().CocLimit * height) + 16 &&
                        x + static_cast<unsigned>(GetDepthOfFieldSettings().CocLimit * height) + 16 < 2 * width / 3) ++focusedChanges;
                }
                if (original <= 0.5f && value > 1.0f) ++bokeh;
            }
        Require(nearChanges > 100 && farChanges > 100, "Near/far depths did not blur");
        Require(focusedChanges == 0, "Blur leaked into the interior of the focus band");
        Require(bokeh > 0, "HDR highlights did not spread above SDR white");
        std::cout << width << 'x' << height << " Q" << quality << " rings=" << GetDepthOfFieldSettings().Rings
            << " medianGPUms=" << times[times.size() / 2] << " near=" << nearChanges << " far=" << farChanges
            << " focusChanged=" << focusedChanges << " HDRbokeh=" << bokeh << " processLocalMiB=" << LocalMemoryUsage(device) / 1048576.0 << '\n';
        SaveImage("Build/dof-q" + std::to_string(quality) + "-" + std::to_string(width) + ".ppm", output, width, height);
        outputs.push_back(std::move(output));
    }
    Require(outputs[0] != outputs[1] && outputs[1] != outputs[2], "Quality did not change actual GPU output");
    Set(L"Render.DOF.Quality", "1");
    Require(Run(device, dof) == outputs.front(), "Returning from Epic to Low changed the deterministic output");
    Set(L"Render.DOF.Quality", "4");
    Set(L"Render.DOF.Enabled", "false");
    Require(Run(device, dof) == pixels, "Off after SDK dispatch is not an exact copy");
    Set(L"Render.DOF.Enabled", "true");
    Require(Run(device, dof) == outputs.back(), "Re-enabling DoF changed deterministic output");
    auto* bufferedQueue = device.GetCommandQueue();
    const auto bufferedFrames = static_cast<unsigned>(resources.FrameConstants.size());
    std::vector<UINT64> fences(bufferedFrames, 0);
    for (unsigned frame = 0; frame < 32; ++frame)
    {
        const unsigned slot = frame % bufferedFrames;
        if (fences[slot]) bufferedQueue->CPUWaitForFenceValue(fences[slot]);
        resources.CurrFrameConstantsIndex = slot;
        auto* commands = bufferedQueue->GetCommandList();
        dof.Execute(commands);
        bufferedQueue->ExecuteCommandList(commands);
        fences[slot] = commands->FenceValue;
    }
    Require(Read(device, *dof.GetOutput(0)->GetTexture()) == outputs.back(), "In-flight frame slots corrupted SDK resources");
    // Resize the same linked pass through odd sizes, then restore and check resource/state lifetime.
    color.Resize(width + 1, height + 1);
    depth.Resize(width + 1, height + 1);
    dof.Resize();
    auto oddImage = FixtureImage(width + 1, height + 1);
    std::vector<float> oddDepth((width + 1) * (height + 1), DeviceDepth(1600, common.CameraViewToClip));
    Upload(device, color, oddImage.data(), 8);
    Upload(device, depth, oddDepth.data(), 4);
    ValidateImage(Run(device, dof));
    color.Resize(width, height);
    depth.Resize(width, height);
    dof.Resize();
    Upload(device, color, pixels.data(), 8);
    Upload(device, depth, depths.data(), 4);
    Require(Run(device, dof) == outputs.back(), "Resize/restore changed DoF output");
    color.Resize(8, 9);
    depth.Resize(8, 9);
    dof.Resize();
    auto tiny = FixtureImage(8, 9);
    Upload(device, color, tiny.data(), 8);
    Require(Run(device, dof) == tiny, "Sub-16-pixel targets must safely bypass the SDK mip chain");
    dof.ClearDenendencies();
}

static void TestDepthEdges(GDX12Device& device, GDX12DeviceResources& resources)
{
    RenderPipelineCommonData common;
    common.ActiveCameraFOV = 30;
    common.CameraViewToClip = Projection(30);
    GDX12Texture color, depth;
    CreateFixtureTexture(color, resources, 320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateFixtureTexture(depth, resources, 320, 180, DXGI_FORMAT_R32_FLOAT);
    std::vector<uint16_t> pixels(320 * 180 * 4, 0);
    std::vector<float> depths(320 * 180);
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 0; x < 320; ++x)
        {
            pixels[(y * 320 + x) * 4 + (x < 160 ? 0 : 2)] = PackedVector::XMConvertFloatToHalf(1.0f);
            pixels[(y * 320 + x) * 4 + 3] = PackedVector::XMConvertFloatToHalf(1.0f);
            depths[y * 320 + x] = DeviceDepth(x < 160 ? 50.0f : 200.0f, common.CameraViewToClip);
        }
    Upload(device, color, pixels.data(), 8);
    Upload(device, depth, depths.data(), 4);
    GDX12DepthOfFieldPass dof;
    dof.Setup(&resources, nullptr, &common);
    dof.SetInputs({ &color, &depth });
    dof.Initialize();
    Set(L"Render.DOF.Quality", "3");
    auto output = Run(device, dof);
    const auto channel = [&](unsigned x, unsigned c) { return PackedVector::XMConvertHalfToFloat(output[(90 * 320 + x) * 4 + c]); };
    Require(channel(80, 2) < 0.001f, "Far color leaked into the near interior");
    Require(channel(165, 0) > 0.01f, "Foreground blur did not spread across its silhouette");
    Require(channel(250, 0) < 0.001f && channel(250, 2) == 1.0f, "Foreground blur reached beyond its CoC support");
    Set(L"Render.DOF.RingMerge", "false");
    ValidateImage(Run(device, dof));
    Set(L"Render.DOF.RingMerge", "true");
    common.ActiveCameraFOV = 60;
    common.CameraViewToClip = Projection(60);
    Require(Run(device, dof) != output, "Runtime FOV did not change physical blur");
    dof.ClearDenendencies();
    std::cout << "GPU: colored foreground silhouette, bounded edge leakage, ring merging and runtime FOV passed.\n";
}

static void TestXeSSGrid(GDX12Device& device, GDX12DeviceResources& resources)
{
    RenderPipelineCommonData common;
    common.WindowWidth = 640;
    common.WindowHeight = 360;
    common.ActiveCameraFOV = 30;
    common.CameraViewToClip = Projection(30);
    GDX12XeSSUpscalePass upscale;
    upscale.Setup(&resources, nullptr, &common);
    const auto width = common.DownscaledWidth;
    const auto height = common.DownscaledHeight;
    GDX12Texture color, depth, motion;
    CreateFixtureTexture(color, resources, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateFixtureTexture(depth, resources, width, height, DXGI_FORMAT_R32_FLOAT);
    CreateFixtureTexture(motion, resources, width, height, DXGI_FORMAT_R16G16_FLOAT);
    auto input = FixtureImage(width, height);
    std::vector<float> depths(width * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            depths[y * width + x] = DeviceDepth(x < width / 3 ? 50.0f : 1600.0f, common.CameraViewToClip);
    std::vector<uint32_t> zeroMotion(width * height, 0);
    Upload(device, color, input.data(), 8);
    Upload(device, depth, depths.data(), 4);
    Upload(device, motion, zeroMotion.data(), 4);
    Set(L"Render.DOF.Quality", "2");
    GDX12DepthOfFieldPass nativeDof;
    nativeDof.Setup(&resources, nullptr, &common);
    nativeDof.SetInputs({ &color, &depth });
    nativeDof.Initialize();
    upscale.SetInputs(std::vector<IRenderPassLink*>{ &depth, &motion, nativeDof.GetOutput(0) });
    upscale.Initialize();
    auto* queue = device.GetCommandQueue();
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        auto* commands = queue->GetCommandList();
        nativeDof.Execute(commands);
        Require(frame != 0 || common.ResetUpscalerHistory, "First DoF frame did not reset XeSS history");
        upscale.Execute(commands);
        Require(!common.ResetUpscalerHistory, "XeSS did not consume the history reset");
        queue->ExecuteCommandList(commands);
        queue->Flush();
    }
    auto beforeUpscale = Read(device, *upscale.GetOutput(0)->GetTexture());
    ValidateImage(beforeUpscale);
    SaveImage("Build/dof-before-xess.ppm", beforeUpscale, 640, 360);
    common.ResetUpscalerHistory = false;
    Set(L"Render.DOF.FocusDistanceCm", "250");
    auto* commands = queue->GetCommandList();
    nativeDof.Execute(commands);
    Require(common.ResetUpscalerHistory, "Runtime focus change did not reset XeSS history");
    queue->ExecuteCommandList(commands);
    queue->Flush();
    Set(L"Render.DOF.FocusDistanceCm", "200");
    // A fresh context removes the previous path's history from the ordering comparison.
    upscale.ClearDenendencies();
    GDX12XeSSUpscalePass upscaleSharp;
    upscaleSharp.Setup(&resources, nullptr, &common);
    upscaleSharp.SetInputs(std::vector<IRenderPassLink*>{ &depth, &motion, &color });
    upscaleSharp.Initialize();
    GDX12Texture fullDepth;
    CreateFixtureTexture(fullDepth, resources, 640, 360, DXGI_FORMAT_R32_FLOAT);
    std::vector<float> fullDepths(640 * 360);
    for (unsigned y = 0; y < 360; ++y)
        for (unsigned x = 0; x < 640; ++x)
            fullDepths[y * 640 + x] = DeviceDepth(x < 640 / 3 ? 50.0f : 1600.0f, common.CameraViewToClip);
    Upload(device, fullDepth, fullDepths.data(), 4);
    GDX12DepthOfFieldPass outputDof;
    outputDof.Setup(&resources, nullptr, &common);
    outputDof.SetInputs({ upscaleSharp.GetOutput(0), &fullDepth });
    outputDof.Initialize();
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        commands = queue->GetCommandList();
        upscaleSharp.Execute(commands);
        outputDof.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
    }
    auto afterUpscale = Read(device, *outputDof.GetOutput(0)->GetTexture());
    ValidateImage(afterUpscale);
    SaveImage("Build/dof-after-xess-analytic-depth.ppm", afterUpscale, 640, 360);
    double difference = 0;
    unsigned hdrPixels = 0;
    for (size_t i = 0; i < beforeUpscale.size(); i += 4)
    {
        const auto a = PackedVector::XMConvertHalfToFloat(beforeUpscale[i]);
        const auto b = PackedVector::XMConvertHalfToFloat(afterUpscale[i]);
        difference += std::abs(a - b);
        if (a > 1.0f) ++hdrPixels;
    }
    Require(hdrPixels > 0, "XeSS HDR integration clipped DoF bokeh");
    std::cout << "XeSS: native grid " << width << 'x' << height << " -> 640x360, HDR pixels=" << hdrPixels
        << ", before/after mean absolute HDR difference=" << difference / (640 * 360) << '\n';
    std::cout << "Ordering cost: native DoF ms=" << MeasurePass(device, nativeDof)
        << ", output DoF ms=" << MeasurePass(device, outputDof) << '\n';
    common.WindowWidth = 641;
    common.WindowHeight = 361;
    upscaleSharp.QueryRenderTargetResolution();
    color.Resize(common.DownscaledWidth, common.DownscaledHeight);
    depth.Resize(common.DownscaledWidth, common.DownscaledHeight);
    motion.Resize(common.DownscaledWidth, common.DownscaledHeight);
    upscaleSharp.Resize();
    fullDepth.Resize(641, 361);
    outputDof.Resize();
    auto resizedImage = FixtureImage(common.DownscaledWidth, common.DownscaledHeight);
    std::vector<float> resizedDepth(common.DownscaledWidth * common.DownscaledHeight, DeviceDepth(1600, common.CameraViewToClip));
    zeroMotion.assign(common.DownscaledWidth * common.DownscaledHeight, 0);
    fullDepths.assign(641 * 361, DeviceDepth(1600, common.CameraViewToClip));
    Upload(device, color, resizedImage.data(), 8);
    Upload(device, depth, resizedDepth.data(), 4);
    Upload(device, motion, zeroMotion.data(), 4);
    Upload(device, fullDepth, fullDepths.data(), 4);
    Require(common.ResetUpscalerHistory, "XeSS resize did not request history reset");
    commands = queue->GetCommandList();
    upscaleSharp.Execute(commands);
    outputDof.Execute(commands);
    queue->ExecuteCommandList(commands);
    ValidateImage(Read(device, *outputDof.GetOutput(0)->GetTexture()));
    std::cout << "GPU: odd XeSS resize and context/history recreation passed.\n";
    outputDof.ClearDenendencies();
    upscaleSharp.ClearDenendencies();
    nativeDof.ClearDenendencies();
}

static void TestToneMapping(GDX12Device& device, GDX12DeviceResources& resources)
{
    GDX12Texture hdr, display;
    CreateFixtureTexture(hdr, resources, 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT);
    std::vector<uint16_t> pixels(32 * 32 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (unsigned c = 0; c < 4; ++c)
            pixels[i + c] = PackedVector::XMConvertFloatToHalf(c == 0 ? 4.0f : c == 2 ? 0.25f : 1.0f);
    Upload(device, hdr, pixels.data(), 8);
    GDX12TextureDesc desc;
    desc.Width = desc.Height = 32;
    desc.Format = desc.RTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.CreateSRV = false;
    desc.CreateRTV = true;
    desc.RTVHeap = resources.RTVHeap.get();
    desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
    desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    display.Initialize(desc);
    RenderPipelineCommonData common;
    GDX12OutputToScreenPass tone;
    tone.Setup(&resources, nullptr, &common);
    tone.SetInputs({ &hdr, &display });
    tone.Initialize();
    auto* commands = device.GetCommandQueue()->GetCommandList();
    tone.Execute(commands);
    device.GetCommandQueue()->ExecuteCommandList(commands);
    auto output = Read(device, display, 4);
    const auto* bytes = reinterpret_cast<const uint8_t*>(output.data());
    for (unsigned i = 0; i < 32 * 32; ++i)
    {
        Require(std::abs(int(bytes[i * 4]) - 231) <= 1 && std::abs(int(bytes[i * 4 + 1]) - 188) <= 1 &&
            std::abs(int(bytes[i * 4 + 2]) - 124) <= 1 && bytes[i * 4 + 3] == 255, "Final HDR tone mapping is not Reinhard + sRGB exactly once");
    }
    std::cout << "GPU: final tone mapping RGBA8 readback passed.\n";
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    HANDLE mutex = nullptr;
    try
    {
        std::ofstream config("Build/dof-settings.ini");
        config << "[ConsoleVariables]\nRender.DOF.FocusDistanceCm=200\nRender.DOF.Quality=3\n";
        config.close();
        const auto loaded = ConsoleModule{}.LoadIni("Build/dof-settings.ini");
        Require(loaded.Error.empty() && GetDepthOfFieldSettings().Quality == 3 &&
            GetDepthOfFieldSettings().FocusDistanceCm == 200, "DoF INI values were not loaded before GPU setup");
        for (const auto* name : { L"Render.DOF.Enabled", L"Render.DOF.Quality", L"Render.DOF.FocusDistanceCm",
            L"Render.DOF.FNumber", L"Render.DOF.SensorHeightMm", L"Render.DOF.CocLimit", L"Render.DOF.FixedCocLimit",
            L"Render.DOF.Rings", L"Render.DOF.RingMerge" })
            Require(ConsoleModule{}.FindConsoleVariable(name).GetFlags() == EConsoleVariableFlags::Default, "DoF CVar is not writable at runtime");
        TestSettingsAndOptics();
        if (argc == 2 && std::string(argv[1]) == "--cpu") return 0;
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(mutex != nullptr, "Cannot create GPU validation mutex");
        auto wait = WaitForSingleObject(mutex, INFINITE);
        Require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "Cannot acquire GPU validation mutex");
        if (argc == 2 && std::string(argv[1]) == "--five-frames") Set(L"Render.NumFrames", "5");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        std::cout << "Adapter: " << device.GetDeviceFeatures().Name << '\n';
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        TestGPU(device, resources, 640, 360);
        TestGPU(device, resources, 1280, 720);
        TestDepthEdges(device, resources);
        TestXeSSGrid(device, resources);
        TestToneMapping(device, resources);
        device.GetCommandQueue()->Flush();
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << message->pDescription << '\n';
                Require(false, "D3D12 validation warning/error");
            }
        }
        GDX12ShaderCompiler::Shutdown();
        std::cout << "GPU: focus, near/far, HDR bokeh, exact Off, presets, frame slots, odd resize/re-enable passed; clean debug layer.\n";
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 1;
}
