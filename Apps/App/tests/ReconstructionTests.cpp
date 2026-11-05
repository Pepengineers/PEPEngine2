#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.Core/ConsoleModule.h"
#include "Common/GameTimer.h"
#include "App.Base/Modules/RenderModule.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Set(const wchar_t* name, int value)
{
    Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(std::to_string(value)), "CVar write failed");
}

static void TestRenderSuspension()
{
    GameTimer timer;
    RenderModule renderer(nullptr, &timer);
    auto& common = *renderer.GetRenderPipelineCommonData();
    common.RequestHistoryReset = false;
    renderer.SetFrameGenerationSuspended(false);
    Require(!common.RequestHistoryReset, "Unchanged render activity reset temporal history");
    for (const bool suspended : { true, false, true, false })
    {
        renderer.SetFrameGenerationSuspended(suspended);
        Require(common.RequestHistoryReset, "Pause/resume did not reset history without frame generation");
        common.RequestHistoryReset = false;
        renderer.SetFrameGenerationSuspended(suspended);
        Require(!common.RequestHistoryReset, "Repeated suspension state reset history again");
    }
    common.RequestHistoryReset = true;
    renderer.SetFrameGenerationSuspended(false);
    Require(common.RequestHistoryReset, "Unchanged render activity erased an explicit history reset");
}

static void TestSettings()
{
    auto s = GetReconstructionSettings();
    Require(s.Provider == GDX12UpscalerProvider::XeSS && s.Quality == GDX12UpscalerQuality::Default,
        "Default reconstruction mode changed");
    ConsoleModule console;
    for (const auto* name : { L"Render.Upscaler.Provider", L"Render.Upscaler.Quality", L"Render.Reconstruction.Restart" })
    {
        auto& cvar = console.FindConsoleVariable(name);
        Require(cvar.GetFlags() == EConsoleVariableFlags::Default, "Setting is not runtime writable");
        for (auto* invalid : { "-1", "nan", "inf", "18446744073709551616" })
            Require(!cvar.SetFromString(invalid), "Invalid setting accepted");
    }
    Require(!console.FindConsoleVariable(L"Render.Upscaler.Provider").SetFromString("5"), "Invalid provider accepted");
    Require(!console.FindConsoleVariable(L"Render.Upscaler.Quality").SetFromString("8"), "Invalid preset accepted");
    Set(L"Render.Upscaler.Provider", 2);
    Set(L"Render.Upscaler.Quality", 3);
    Set(L"Render.Reconstruction.Restart", 7);
    s = GetReconstructionSettings();
    Require(s.Provider == GDX12UpscalerProvider::FSR && s.Quality == GDX12UpscalerQuality::Balanced && s.Restart == 7,
        "Runtime settings were not applied");
    Set(L"Render.Upscaler.Provider", 0);
    Set(L"Render.Upscaler.Quality", 0);
    Set(L"Render.Reconstruction.Restart", 0);
}

static void MakeInput(GDX12DeviceResources& resources, GDX12Texture& texture, UINT width, UINT height, DXGI_FORMAT format)
{
    GDX12TextureDesc desc;
    desc.Width = width; desc.Height = height;
    desc.Format = desc.SRVDesc.Format = format;
    desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
        desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.SRVDesc.Texture2D.MipLevels = 1;
    texture.Initialize(desc);
}

static void Upload(GDX12Device& device, GDX12Texture& texture, const void* pixels, UINT bytesPerPixel)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, &mapped));
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
            static_cast<const uint8_t*>(pixels) + y * texture.GetWidth() * bytesPerPixel, texture.GetWidth() * bytesPerPixel);
    upload->Unmap(0, nullptr);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
    const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint), target(texture.GetResource()->D3DResource.Get(), 0);
    commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    queue->ExecuteCommandList(commands);
    queue->Flush();
}

static void TestGPU(GDX12Device& device, GDX12DeviceResources& resources)
{
    GameTimer timer;
    timer.Reset(); timer.Tick();
    RenderPipelineCommonData common;
    common.WindowWidth = 320; common.WindowHeight = 192;
    common.ActiveCameraFOV = 60;
    common.ActiveCameraNearPlane = 0.1f; common.ActiveCameraFarPlane = 10000;
    common.GameTimer = &timer;
    GDX12Texture color, depth, motion;
    GDX12ReconstructionPass pass;
    pass.Setup(&resources, nullptr, &common);
    pass.SetInputs({ &depth, &motion, &color });
    auto* stable = pass.GetOutput(0);
    const auto inputs = [&]
    {
        const UINT w = common.DownscaledWidth, h = common.DownscaledHeight;
        MakeInput(resources, color, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, depth, w, h, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, motion, w, h, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<uint16_t> colors(w * h * 4), velocities(w * h * 2, 0);
        for (size_t i = 0; i < colors.size(); i += 4) { colors[i] = 0x4200; colors[i + 1] = 0x4000; colors[i + 2] = colors[i + 3] = 0x3c00; }
        const std::vector<float> depths(w * h, 0.5f);
        Upload(device, color, colors.data(), 8); Upload(device, depth, depths.data(), 4); Upload(device, motion, velocities.data(), 4);
    };
    inputs(); pass.Initialize();
    Require(stable->GetTexture() == &color, "Native mode did not preserve the original color");
    const auto exercise = [&]
    {
        Require(pass.GetOutput(0) == stable && stable->IsInitialized(), "Provider change invalidated downstream links");
        Require(common.RequestHistoryReset && common.ResetUpscalerHistory, "Provider change did not request temporal reset");
        common.ResetHistory = true; ++common.HistoryRevision;
        for (int frame = 0; frame < 2; ++frame)
        {
            auto* queue = device.GetCommandQueue();
            auto* commands = queue->GetCommandList();
            pass.Execute(commands); queue->ExecuteCommandList(commands); queue->Flush();
            Require(!common.ResetUpscalerHistory, "SR did not consume temporal reset");
        }
        auto* output = stable->GetTexture();
        Require(output->GetWidth() == common.WindowWidth && output->GetHeight() == common.WindowHeight,
            "Reconstruction returned the wrong output grid");
        ScratchImage image;
        const auto state = output->GetResource()->GDX12Resource::GetCurrentState();
        ThrowIfFailed(CaptureTexture(device.GetCommandQueue()->GetCommandQueue().Get(),
            output->GetResource()->D3DResource.Get(), false, image, state, state));
        const auto* pixels = image.GetImage(0, 0, 0);
        const auto* pixel = reinterpret_cast<const uint16_t*>(pixels->pixels + pixels->rowPitch * (pixels->height / 2)) + (pixels->width / 2) * 4;
        const float red = PackedVector::XMConvertHalfToFloat(pixel[0]);
        Require(std::isfinite(red) && std::abs(red - 3) < 0.2f, "SR clipped, tone mapped or corrupted scene-linear HDR");
    };
    for (int provider : { 1, 0, 2, 0, 3, 0, 4, 0, 1 })
    {
        Set(L"Render.Upscaler.Provider", provider);
        Require(pass.ApplySettings(), "Provider change was ignored");
        inputs(); pass.Resize(); exercise();
        const auto& status = pass.GetStatus();
        if (provider && status.Effective == GDX12UpscalerProvider::Native)
        {
            Require(!status.FallbackReason.empty(), "Fallback omitted the unsupported reason");
            std::cout << "Provider " << provider << " unavailable: " << status.FallbackReason << '\n';
        }
    }
    Set(L"Render.Upscaler.Quality", 2);
    Require(pass.ApplySettings(), "Quality change was ignored");
    inputs(); pass.Resize(); exercise();
    const auto generation = pass.GetStatus().Generation;
    Require(!pass.ApplySettings(), "Unchanged settings recreate resources every frame");
    Set(L"Render.Reconstruction.Restart", 1);
    Require(pass.ApplySettings() && pass.GetStatus().Generation == generation + 1, "Explicit restart did not recreate the provider");
    inputs(); pass.Resize(); exercise();
    common.WindowWidth = 321; common.WindowHeight = 193;
    pass.QueryRenderTargetResolution(); inputs(); pass.Resize(); exercise();
    Set(L"Render.Upscaler.Provider", 2); Set(L"Render.Upscaler.Quality", 7);
    Require(pass.ApplySettings(), "Unsupported preset request ignored");
    inputs(); pass.Resize();
    Require(pass.GetStatus().Effective == GDX12UpscalerProvider::Native && !pass.GetStatus().FallbackReason.empty(),
        "Unsupported preset did not produce native fallback with a reason");
    exercise();
    pass.ClearDenendencies();
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    std::cout.setf(std::ios::unitbuf);
    HANDLE gate = nullptr;
    try
    {
        TestSettings();
        TestRenderSuspension();
        if (argc > 1 && std::string(argv[1]) == "--cpu") return 0;
        gate = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(gate != nullptr, "Cannot create GPU validation mutex");
        const auto wait = WaitForSingleObject(gate, INFINITE);
        Require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "Cannot acquire GPU validation mutex");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        std::cout << "Streamline startup: " << (GDX12StreamlineSDK::Get().Initialize() ? "enabled" : "unavailable") << '\n';
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages(); // Existing empty-scene caches are outside reconstruction.
        TestGPU(device, resources);
        device.GetCommandQueue()->Flush();
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            { std::cerr << message->pDescription << '\n'; throw std::runtime_error("D3D12 validation warning/error"); }
        }
        GDX12DeviceFactory::Reset(); GDX12StreamlineSDK::Get().Shutdown();
        ReleaseMutex(gate); CloseHandle(gate);
        std::cout << "Reconstruction CVar, provider/quality/restart, HDR, fallback and odd-resize checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        if (gate) { ReleaseMutex(gate); CloseHandle(gate); }
        std::cerr << e.what() << '\n'; return 1;
    }
}
