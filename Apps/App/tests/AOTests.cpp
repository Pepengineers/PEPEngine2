#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Set(std::wstring_view name, const std::string& value)
{
    Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "Runtime CVar write failed");
}

static void CheckSettings()
{
    ConsoleModule console;
    Set(L"Render.AO.ExecutionPoint", "0");
    Require(GetAmbientOcclusionExecutionPoint() == 0, "Early AO execution point was not selected");
    Require(!console.FindConsoleVariable(L"Render.AO.ExecutionPoint").SetFromString("2"), "Invalid AO execution point accepted");
    Set(L"Render.AO.ExecutionPoint", "1");
    Require(GetAmbientOcclusionExecutionPoint() == 1, "Late AO execution point was not selected");
    const auto path = std::filesystem::path(SOLUTION_DIR) / "Build/AO-test.ini";
    {
        std::ofstream ini(path);
        ini << "[ConsoleVariables]\nRender.AO.ExecutionPoint=0\nRender.AO.Implementation=3\nRender.AO.RadiusCm=75\nRender.AO.Quality=1\nRender.AO.SSAO.Samples=27\n";
    }
    const auto result = console.LoadIni(path);
    Require(result.Error.empty(), "AO INI settings rejected");
    Require(GetAmbientOcclusionExecutionPoint() == 0, "INI did not select early AO");
    Set(L"Render.AO.ExecutionPoint", "1");
    auto settings = GetAmbientOcclusionSettings();
    Require(settings.Implementation == 3 && settings.RadiusCm == 75 && settings.Samples == 27, "INI did not reach AO snapshot");
    Set(L"Render.AO.HBAO.Directions", "7");
    Set(L"Render.AO.HBAO.Steps", "5");
    Set(L"Render.AO.DenoisePasses", "4");
    Set(L"Render.AO.XeGTAO.Slices", "4");
    Set(L"Render.AO.XeGTAO.Steps", "7");
    Set(L"Render.AO.CACAO.Quality", "3");
    for (int level = 0; level <= 3; ++level)
    {
        Set(L"Render.AO.Quality", std::to_string(level));
        const auto resolved = GetAmbientOcclusionSettings();
        Require(resolved.Samples == 27 && resolved.Directions == 7 && resolved.Steps == 5 && resolved.DenoisePasses == 4 &&
            resolved.GTAOSlices == 4 && resolved.GTAOSteps == 7 && resolved.CACAOQuality == 3,
            "Quality destroyed a manual override");
    }
    for (const auto name : { L"Render.AO.SSAO.Samples", L"Render.AO.HBAO.Directions", L"Render.AO.HBAO.Steps",
        L"Render.AO.DenoisePasses", L"Render.AO.XeGTAO.Slices", L"Render.AO.XeGTAO.Steps", L"Render.AO.CACAO.Quality" }) Set(name, "-1");
    Require(GetAmbientOcclusionSettings().Samples == 32, "Preset inheritance did not resume");
    for (auto* variable : console.GetConsoleVariables())
    {
        if (variable->GetName().substr(0, 10) != L"Render.AO.") continue;
        Require(variable->GetFlags() == EConsoleVariableFlags::Default, "AO setting is not runtime writable");
        Require(!variable->SetFromString("nan") && !variable->SetFromString("inf") && !variable->SetFromString("1e20"), "Invalid finite/range value accepted");
    }
    Require(!console.FindConsoleVariable(L"Render.AO.Implementation").SetFromString("4"), "Invalid AO algorithm accepted");
    Require(!console.FindConsoleVariable(L"Render.AO.Quality").SetFromString("-1"), "Invalid quality accepted");
    Set(L"Render.AO.FadeStartCm", "500");
    Set(L"Render.AO.FadeEndCm", "100");
    Require(GetAmbientOcclusionSettings().FadeEndCm == 501, "Cross-field fade normalization failed");
    Set(L"Render.AO.FadeStartCm", "5000");
    Set(L"Render.AO.FadeEndCm", "15000");
    Set(L"Render.AO.RadiusCm", "150");
    Set(L"Render.AO.Quality", "2");
    std::cout << "CVar INI/runtime/validation/override tests passed\n";
}

static void CreateInput(GDX12DeviceResources& resources, GDX12Texture& texture, UINT width, UINT height, DXGI_FORMAT format, bool renderTarget = false)
{
    GDX12TextureDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Format = desc.SRVDesc.Format = format;
    desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
        desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.SRVDesc.Texture2D.MipLevels = 1;
    if (renderTarget)
    {
        desc.CreateRTV = true;
        desc.RTVHeap = resources.RTVHeap.get();
        desc.RTVHeapIndex = texture.IsInitialized() ? texture.GetRTV()->HeapIndex : desc.RTVHeap->GetAvailableIndex();
        desc.RTVDesc.Format = format;
        desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    }
    texture.Initialize(desc);
}

static void Upload(GDX12Device& device, GDX12Texture& texture, const std::vector<float>& values, UINT channels)
{
    auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* data = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, &data));
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(static_cast<uint8_t*>(data) + footprint.Offset + y * footprint.Footprint.RowPitch,
            values.data() + y * texture.GetWidth() * channels, texture.GetWidth() * channels * sizeof(float));
    upload->Unmap(0, nullptr);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
    const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint);
    const CD3DX12_TEXTURE_COPY_LOCATION target(texture.GetResource()->D3DResource.Get(), 0);
    commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
}

static ScratchImage Capture(GDX12Device& device, GDX12Texture& texture)
{
    ScratchImage image;
    const auto state = texture.GetResource()->GDX12Resource::GetCurrentState();
    ThrowIfFailed(CaptureTexture(device.GetCommandQueue()->GetCommandQueue().Get(),
        texture.GetResource()->D3DResource.Get(), false, image, state, state));
    return image;
}

static std::vector<float> Readback(GDX12Device& device, GDX12Texture& texture)
{
    const auto image = Capture(device, texture);
    const auto* pixels = image.GetImage(0, 0, 0);
    std::vector<float> values(texture.GetWidth() * texture.GetHeight());
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(values.data() + y * texture.GetWidth(), pixels->pixels + y * pixels->rowPitch,
            texture.GetWidth() * sizeof(float));
    for (float value : values) Require(std::isfinite(value) && value >= 0 && value <= 1, "AO output is outside finite [0,1]");
    return values;
}

static void SaveImage(const std::string& name, UINT width, UINT height, const std::vector<float>& pixels)
{
    std::ofstream image(std::filesystem::path(SOLUTION_DIR) / "Build" / (name + ".pgm"), std::ios::binary);
    image << "P5\n" << width << ' ' << height << "\n255\n";
    for (float value : pixels) image.put(static_cast<char>(std::lround(value * 255)));
}

static void FillFixture(GDX12DeviceResources& resources, RenderPipelineCommonData& common,
    GDX12Texture& depth, GDX12Texture& normals, int shape)
{
    const UINT width = depth.GetWidth(), height = depth.GetHeight();
    const float farPlane = shape == 4 ? 200000.0f : 20000.0f;
    Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, static_cast<float>(width) / height, 1, farPlane);
    projection._33 = 1.0f / (farPlane - 1);
    projection._43 = farPlane / (farPlane - 1);
    common.CameraViewToClip = projection;
    common.ActiveCameraCBufferIndex = 0;
    common.DownscaledWidth = common.WindowWidth = width;
    common.DownscaledHeight = common.WindowHeight = height;
    GDX12CameraConstants camera = {};
    camera.View = Matrix::Identity;
    camera.ViewProj = projection.Transpose();
    camera.InvViewProj = projection.Invert().Transpose();
    camera.NearPlane = 1;
    camera.FarPlane = farPlane;
    for (auto& frame : resources.FrameConstants)
    {
        frame->CameraCB->Resize(1);
        frame->CameraCB->CopyData(0, camera);
    }
    std::vector<float> depths(width * height), normalValues(width * height * 4);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const UINT p = y * width + x;
            float z = shape == 4 ? 100000.0f : (shape == 1 && x < width / 2 ? 450.0f : 500.0f);
            if (shape == 5) z = (x / 2) % 2 == 0 ? 496.0f : 500.0f;
            float nx = 0, nz = 1;
            const float rayX = ((x + 0.5f) / width * 2 - 1) / projection._11;
            if (shape == 2 && rayX < -0.2f)
            {
                z = -100.0f / rayX;
                nx = 1;
                nz = 0;
            }
            depths[p] = projection._43 / z - projection._33;
            if (shape == 3) depths[p] = 0;
            normalValues[p * 4] = nx;
            normalValues[p * 4 + 2] = nz;
            normalValues[p * 4 + 3] = 1;
        }
    Upload(*resources.Device, depth, depths, 1);
    Upload(*resources.Device, normals, normalValues, 4);
}

static void CheckDebug(ID3D12InfoQueue* info)
{
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        info->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        info->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            std::cerr << message->pDescription << '\n';
            throw std::runtime_error("DX12 validation message");
        }
    }
    info->ClearStoredMessages();
}

static void CheckHDR(GDX12DeviceResources& resources)
{
    RenderPipelineCommonData common;
    common.WindowWidth = 128;
    common.WindowHeight = 96;
    GDX12XeSSUpscalePass upscale;
    upscale.Setup(&resources, nullptr, &common);
    GDX12Texture opaque, accumulation, revealage, depth, motion, screen;
    auto* device = resources.Device;
    const auto fill = [&]()
    {
        const UINT w = common.DownscaledWidth, h = common.DownscaledHeight;
        CreateInput(resources, opaque, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT);
        CreateInput(resources, accumulation, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT);
        CreateInput(resources, revealage, w, h, DXGI_FORMAT_R32_FLOAT);
        CreateInput(resources, depth, w, h, DXGI_FORMAT_R32_FLOAT);
        CreateInput(resources, motion, w, h, DXGI_FORMAT_R32G32_FLOAT);
        CreateInput(resources, screen, common.WindowWidth, common.WindowHeight, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        std::vector<float> color(w * h * 4);
        for (UINT p = 0; p < w * h; ++p)
        {
            color[p * 4] = 3;
            color[p * 4 + 1] = 2;
            color[p * 4 + 2] = color[p * 4 + 3] = 1;
        }
        Upload(*device, opaque, color, 4);
        Upload(*device, accumulation, std::vector<float>(w * h * 4, 0), 4);
        Upload(*device, revealage, std::vector<float>(w * h, 1), 1);
        Upload(*device, depth, std::vector<float>(w * h, 0.002f), 1);
        Upload(*device, motion, std::vector<float>(w * h * 2, 0), 2);
    };
    fill();
    GDX12WBOITCompositionPass composition;
    composition.Setup(&resources, nullptr, &common);
    composition.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    composition.SetInputs({ &opaque, &accumulation, &revealage });
    composition.Initialize();
    upscale.SetInputs({ &depth, &motion, composition.GetOutput(0) });
    upscale.Initialize();
    GDX12OutputToScreenPass output;
    output.Setup(&resources, nullptr, &common);
    output.SetInputs({ upscale.GetOutput(0), &screen });
    output.Initialize();
    for (int resized = 0; resized < 2; ++resized)
    {
        if (resized)
        {
            common.WindowWidth = 129;
            common.WindowHeight = 97;
            upscale.QueryRenderTargetResolution();
            fill();
            composition.Resize();
            upscale.Resize();
            Require(common.ResetUpscalerHistory, "Resize did not request XeSS history reset");
        }
        for (int frame = 0; frame < 6; ++frame)
        {
            auto* commands = device->GetCommandQueue()->GetCommandList();
            composition.Execute(commands);
            upscale.Execute(commands);
            output.Execute(commands);
            device->GetCommandQueue()->ExecuteCommandList(commands);
            device->GetCommandQueue()->Flush();
            Require(!common.ResetUpscalerHistory, "XeSS did not consume history reset");
        }
        for (auto* texture : { composition.GetOutput(0)->GetTexture(), upscale.GetOutput(0)->GetTexture() })
        {
            const auto hdr = Capture(*device, *texture);
            const auto* image = hdr.GetImage(0, 0, 0);
            const auto* pixel = reinterpret_cast<const uint16_t*>(image->pixels + image->rowPitch * (image->height / 2)) + (image->width / 2) * 4;
            Require(std::abs(PackedVector::XMConvertHalfToFloat(pixel[0]) - 3.0f) < 0.1f,
                "HDR was clipped or tone mapped before final output");
        }
        const auto ldr = Capture(*device, screen);
        const auto* image = ldr.GetImage(0, 0, 0);
        const auto* pixel = image->pixels + image->rowPitch * (image->height / 2) + (image->width / 2) * 4;
        Require(std::abs(int(pixel[0]) - 225) <= 2 && std::abs(int(pixel[1]) - 213) <= 2 && std::abs(int(pixel[2]) - 188) <= 2,
            "Final tone mapping/sRGB conversion is incorrect");
    }
    upscale.ClearDenendencies();
    std::cout << "HDR composition/XeSS/tone mapping/reset/odd resize passed\n";
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        struct GpuLock
        {
            HANDLE Mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
            GpuLock()
            {
                Require(Mutex != nullptr, "Cannot create GPU validation mutex");
                const auto result = WaitForSingleObject(Mutex, INFINITE);
                Require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED, "Cannot acquire GPU validation mutex");
            }
            ~GpuLock() { ReleaseMutex(Mutex); CloseHandle(Mutex); }
        } gpuLock;
        std::cout.setf(std::ios::unitbuf);
        CheckSettings();
        if (argc > 1 && std::string(argv[1]) == "--cpu") return 0;
        BenchmarkEngine engine;
        Set(L"Render.AO.RadiusCm", "150");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (argc > 1)
        {
            ComPtr<ID3D12Debug1> gpuValidation;
            ThrowIfFailed(debug.As(&gpuValidation));
            gpuValidation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device device;
        auto adapter = GDX12DeviceFactory::GetMostPerformantAdapter();
        ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages(); // Existing zero-sized scene caches are outside this fixture.
        RenderPipelineCommonData common;
        GDX12Texture depth, normals;
        CreateInput(resources, depth, 65, 49, DXGI_FORMAT_R32_FLOAT);
        CreateInput(resources, normals, 65, 49, DXGI_FORMAT_R32G32B32A32_FLOAT);
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common);
        ao.SetInputs({ &depth, &normals });
        ao.Initialize();
        auto* queue = device.GetCommandQueue();
        GDX12GPUTimer timer(&device);
        auto run = [&]()
        {
            auto* commands = queue->GetCommandList();
            timer.Start(commands);
            ao.Execute(commands);
            timer.Stop(commands);
            queue->ExecuteCommandList(commands);
            timer.OnCommandListExecuted(commands->FenceValue);
            queue->Flush();
            CheckDebug(info.Get());
            return Readback(device, *ao.GetOutput(0)->GetTexture());
        };
        for (int algorithm = 0; algorithm < 4; ++algorithm)
        {
            Set(L"Render.AO.Implementation", std::to_string(algorithm));
            Set(L"Render.AO.Enabled", "1");
            Set(L"Render.AO.Quality", "2");
            std::vector<float> plane;
            for (int shape = 0; shape < 5; ++shape)
            {
                FillFixture(resources, common, depth, normals, shape);
                const auto pixels = run();
                const float minimum = *std::min_element(pixels.begin(), pixels.end());
                const float mean = std::accumulate(pixels.begin(), pixels.end(), 0.0f) / pixels.size();
                std::cout << "algorithm=" << algorithm << " shape=" << shape << " min=" << minimum << " mean=" << mean << '\n';
                if (shape == 0)
                {
                    // XeGTAO's projected-normal approximation darkens oblique slices at this 90-degree FOV.
                    const float tolerance = algorithm == 2 ? 0.70f : 0.96f;
                    Require(minimum > tolerance && pixels[24 * 65 + 32] > 0.98f,
                        "Unoccluded plane exceeds the algorithm's angular/quantization tolerance");
                }
                if (shape == 0)
                {
                    plane = pixels;
                    common.ResetUpscalerHistory = false;
                    run();
                    Require(!common.ResetUpscalerHistory, "Unchanged AO settings reset temporal accumulation");
                }
                if (shape == 1 || shape == 2)
                {
                    float reduction = 0;
                    const UINT firstX = shape == 1 ? 32 : 26;
                    for (UINT y = 16; y < 33; ++y)
                        for (UINT x = firstX; x < firstX + 6; ++x)
                            reduction += plane[y * 65 + x] - pixels[y * 65 + x];
                    Require(reduction / (17 * 6) > 0.01f, "Discontinuity/corner does not occlude more than the matching plane");
                }
                if (shape == 3 || shape == 4) Require(minimum == 1, "Background/faded far geometry is not neutral");
                SaveImage("ao-" + std::to_string(algorithm) + "-" + std::to_string(shape), 65, 49, pixels);
            }
            FillFixture(resources, common, depth, normals, 1);
            common.ResetUpscalerHistory = false;
            Set(L"Render.AO.Enabled", "0");
            for (float p : run()) Require(p == 1, "Disabled AO is not exactly neutral");
            Require(common.ResetUpscalerHistory, "AO switch did not request temporal reset");
            Set(L"Render.AO.Enabled", "1");
            Set(L"Render.AO.Quality", "0");
            for (float p : run()) Require(p == 1, "Quality Off is not exactly neutral");
            Set(L"Render.AO.Quality", "2");
            Set(L"Render.AO.DenoisePasses", "0");
            const auto raw = run();
            Set(L"Render.AO.DenoisePasses", "2");
            const auto filtered = run();
            Require(raw != filtered, "Denoise has no GPU effect");
            Set(L"Render.AO.DenoisePasses", "-1");
            Set(L"Render.AO.Quality", "1");
            const auto low = run();
            Set(L"Render.AO.Quality", "3");
            const auto high = run();
            Require(low != high, "Quality presets have no GPU effect");
            Set(L"Render.AO.Intensity", "0");
            for (float p : run()) Require(p == 1, "Zero intensity is not neutral");
            Set(L"Render.AO.Intensity", "1");
            const auto checkOverride = [&](std::wstring_view name, const char* a, const char* b, const char* restore)
            {
                Set(name, a);
                const auto first = run();
                Set(name, b);
                const auto second = run();
                Set(name, restore);
                if (first == second) throw std::runtime_error("AO parameter has no GPU effect: " + WStringToString(std::wstring(name)));
            };
            checkOverride(L"Render.AO.RadiusCm", "0.1", "150", "150");
            checkOverride(L"Render.AO.Sharpness", "0", "1", "0.98");
            if (algorithm == 0)
            {
                checkOverride(L"Render.AO.SSAO.Samples", "4", "64", "-1");
                checkOverride(L"Render.AO.SSAO.BiasCm", "0", "100", "0.5");
            }
            if (algorithm == 1)
            {
                checkOverride(L"Render.AO.HBAO.Directions", "2", "16", "-1");
                checkOverride(L"Render.AO.HBAO.Steps", "2", "16", "-1");
                checkOverride(L"Render.AO.HorizonBias", "0", "0.2", "0.06");
            }
            if (algorithm == 2)
            {
                checkOverride(L"Render.AO.XeGTAO.Slices", "1", "8", "-1");
                checkOverride(L"Render.AO.XeGTAO.Steps", "1", "16", "-1");
                checkOverride(L"Render.AO.XeGTAO.Falloff", "0.05", "1", "0.615");
                checkOverride(L"Render.AO.XeGTAO.ThinOccluder", "0", "0.7", "0");
            }
            if (algorithm == 3)
            {
                checkOverride(L"Render.AO.CACAO.Quality", "0", "3", "-1");
                checkOverride(L"Render.AO.CACAO.Power", "0.5", "5", "1.5");
                checkOverride(L"Render.AO.CACAO.Clamp", "0", "0.98", "0.98");
                // Native detail AO excludes depth discontinuities; exercise shallow two-pixel grooves.
                FillFixture(resources, common, depth, normals, 5);
                checkOverride(L"Render.AO.CACAO.Detail", "0", "5", "0.5");
                FillFixture(resources, common, depth, normals, 1);
                checkOverride(L"Render.AO.CACAO.AdaptiveLimit", "0", "1", "0.45");
                checkOverride(L"Render.AO.HorizonBias", "0", "0.2", "0.06");
                checkOverride(L"Render.AO.DenoisePasses", "0", "8", "-1");
            }
        }
        // Compare queued frame slots with isolated GPU results to detect upload-buffer reuse.
        for (int algorithm = 0; algorithm < 4; ++algorithm)
        {
            Set(L"Render.AO.Implementation", std::to_string(algorithm));
            const UINT frames = static_cast<UINT>(resources.FrameConstants.size());
            std::vector<float> expected(frames);
            std::vector<ComPtr<ID3D12Resource>> captures(frames);
            for (UINT slot = 0; slot < frames; ++slot)
            {
                Set(L"Render.AO.Intensity", std::to_string(0.5f + 0.5f * slot));
                expected[slot] = run()[24 * 65 + 32];
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
                ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&captures[slot])));
            }
            for (UINT slot = 0; slot < frames; ++slot)
            {
                resources.CurrFrameConstantsIndex = slot;
                Set(L"Render.AO.Intensity", std::to_string(0.5f + 0.5f * slot));
                auto* commands = queue->GetCommandList();
                ao.Execute(commands);
                auto* result = ao.GetOutput(0)->GetTexture();
                commands->ResourceBarrier({ result->GetResource()->GetCopySourceBarrier() });
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
                footprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256 };
                const CD3DX12_TEXTURE_COPY_LOCATION target(captures[slot].Get(), footprint);
                const CD3DX12_TEXTURE_COPY_LOCATION source(result->GetResource()->D3DResource.Get(), 0);
                const D3D12_BOX box = { 32, 24, 0, 33, 25, 1 };
                commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
                commands->ResourceBarrier({ result->GetResource()->GetSRVBarrier() });
                queue->ExecuteCommandList(commands);
            }
            queue->Flush();
            for (UINT slot = 0; slot < frames; ++slot)
            {
                void* data = nullptr;
                ThrowIfFailed(captures[slot]->Map(0, nullptr, &data));
                const float actual = *static_cast<const float*>(data);
                captures[slot]->Unmap(0, nullptr);
                Require(std::abs(actual - expected[slot]) < 1e-5f, "Queued frame constants were overwritten");
            }
            CheckDebug(info.Get());
        }
        Set(L"Render.AO.Intensity", "1");
        std::cout << "Queued frame-slot readbacks match isolated results for all implementations\n";
        for (const auto size : { std::pair<UINT, UINT>{17, 13}, {1, 1}, {321, 241}, {1280, 720} })
        {
            CreateInput(resources, depth, size.first, size.second, DXGI_FORMAT_R32_FLOAT);
            CreateInput(resources, normals, size.first, size.second, DXGI_FORMAT_R32G32B32A32_FLOAT);
            ao.Resize();
            FillFixture(resources, common, depth, normals, 1);
            for (int algorithm = 0; algorithm < 4; ++algorithm)
            {
                Set(L"Render.AO.Implementation", std::to_string(algorithm));
                for (int level = 1; level <= 3; ++level)
                {
                    Set(L"Render.AO.Quality", std::to_string(level));
                    run();
                    if (size.first == 1280 && argc == 1)
                    {
                        double total = 0;
                        for (int repeat = 0; repeat < 10; ++repeat) { run(); total += timer.GetTimeMS(); }
                        DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
                        ThrowIfFailed(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
                        std::cout << "VRAM process MiB=" << memory.CurrentUsage / (1024.0 * 1024.0) << '\n';
                        std::cout << "TIMING algorithm=" << algorithm << " quality=" << level << " ms=" << total / 10 << '\n';
                    }
                }
            }
            std::cout << "Resize/quality/range passed " << size.first << 'x' << size.second << '\n';
        }
        CheckDebug(info.Get());
        if (argc == 1)
        {
            CheckHDR(resources);
            CheckDebug(info.Get());
        }
        else std::cout << "Focused AO GPU-based validation excludes separately checked vendor XeSS.\n";
        std::cout << "AO GPU tests passed\n";
        return 0;
    }
    catch (const DxException& e) { std::wcerr << e.ToString() << '\n'; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}
