#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.Core/ConsoleModule.h"
#include "Common/GameTimer.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void CheckDebugMessages(ID3D12InfoQueue* info)
{
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> data(bytes); auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        info->GetMessage(i, message, &bytes);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        { std::cerr << message->pDescription << '\n'; throw std::runtime_error("D3D12 validation warning/error"); }
    }
    info->ClearStoredMessages();
}
static void Set(const wchar_t* name, int value)
{
    Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(std::to_string(value)), "CVar write failed");
}
static void Settings()
{
    Require(GetFrameGenerationProvider() == GDX12FrameGenerationProvider::Off, "FG must default to Off");
    auto& provider = ConsoleModule{}.FindConsoleVariable(L"Render.FrameGeneration.Provider");
    Require(provider.GetFlags() == EConsoleVariableFlags::Default, "FG provider is not runtime writable");
    for (auto value : {"-1", "4", "nan", "1.5", "18446744073709551616"})
        Require(!provider.SetFromString(value), "Invalid FG provider accepted");
    for (int value = 0; value <= 3; ++value)
    {
        Set(L"Render.FrameGeneration.Provider", value);
        Require(static_cast<int>(GetFrameGenerationProvider()) == value, "FG selection did not change");
    }
    Set(L"Render.FrameGeneration.Provider", 0);
    Set(L"Render.Upscaler.Provider", 0); Set(L"Render.Upscaler.Quality", 0);
}

static void MakeTarget(GDX12DeviceResources& resources, GDX12Texture& texture, UINT w, UINT h, DXGI_FORMAT format)
{
    if (texture.IsInitialized()) { texture.Resize(w, h); return; }
    GDX12TextureDesc desc;
    desc.Width = w; desc.Height = h; desc.Format = desc.SRVDesc.Format = desc.RTVDesc.Format = format;
    desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get(); desc.RTVHeap = resources.RTVHeap.get();
    desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Texture2D.MipLevels = 1;
    desc.CreateRTV = true; desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
    desc.ClearValue = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? XMFLOAT4{3, 2, 1, 1} :
        format == DXGI_FORMAT_R32_FLOAT ? XMFLOAT4{0.5f, 0, 0, 0} : XMFLOAT4{0, 0, 0, 0};
    desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    texture.Initialize(desc);
}

static void GPU(HWND window)
{
    ComPtr<ID3D12Debug> debug;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    const bool streamline = GDX12StreamlineSDK::Get().Initialize();
    std::cout << "Streamline startup: " << (streamline ? "enabled" : "unavailable") << '\n';
    GDX12Device device;
    ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
    GDX12DeviceResources resources; resources.Initialize(&device);
    ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info));
    info->ClearStoredMessages(); // Existing empty-scene cache initialization is checked elsewhere.
    GameTimer timer; timer.Reset();
    RenderPipelineCommonData common;
    common.WindowWidth = 1280; common.WindowHeight = 720;
    common.ActiveCameraFOV = 60; common.ActiveCameraNearPlane = 1; common.ActiveCameraFarPlane = 100000;
    common.GameTimer = &timer;
    common.CameraViewToClip = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), 1280.0f / 720, 1, 100000);
    common.CameraViewToClip._33 = 1.0f / (100000.0f - 1); common.CameraViewToClip._43 = 100000.0f / (100000.0f - 1);
    common.ClipToCameraView = common.CameraViewToClip.Invert(); common.CameraProjection = common.CameraViewToClip;
    {
        GDX12SwapChain swap(&device, window, DXGI_FORMAT_R8G8B8A8_UNORM, 2, common.WindowWidth, common.WindowHeight, resources.RTVHeap.get());
        GDX12Texture color, depth, motion;
        GDX12ReconstructionPass sr;
        sr.Setup(&resources, nullptr, &common);
        const auto targets = [&]
        {
            MakeTarget(resources, color, common.DownscaledWidth, common.DownscaledHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
            MakeTarget(resources, depth, common.DownscaledWidth, common.DownscaledHeight, DXGI_FORMAT_R32_FLOAT);
            MakeTarget(resources, motion, common.DownscaledWidth, common.DownscaledHeight, DXGI_FORMAT_R16G16_FLOAT);
        };
        targets(); sr.SetInputs({&depth, &motion, &color}); sr.Initialize();
        GDX12OutputToScreenPass output;
        output.Setup(&resources, nullptr, &common); output.SetInputs({sr.GetOutput(0), &swap}); output.Initialize();
        auto& fg = swap.GetFrameGeneration();
        const auto frame = [&]
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            Require(IsWindow(window), "Test window closed");
            timer.Tick(); swap.BeginFrame(common);
            if (sr.ApplySettings()) { targets(); sr.Resize(); }
            if (common.RequestHistoryReset) { ++common.HistoryRevision; common.RequestHistoryReset = false; common.ResetHistory = true; }
            fg.BeginRendering(common);
            auto* queue = device.GetCommandQueue(); auto* commands = queue->GetCommandList();
            commands->ResourceBarrier({color.GetResource()->GetRenderTargetBarrier(), depth.GetResource()->GetRenderTargetBarrier(), motion.GetResource()->GetRenderTargetBarrier()});
            const float hdr[] = {3, 2, 1, 1}, z[] = {0.5f, 0, 0, 0}, mv[] = {0, 0, 0, 0};
            commands->GetCommandList()->ClearRenderTargetView(color.GetRTV()->CPUHandle, hdr, 0, nullptr);
            commands->GetCommandList()->ClearRenderTargetView(depth.GetRTV()->CPUHandle, z, 0, nullptr);
            commands->GetCommandList()->ClearRenderTargetView(motion.GetRTV()->CPUHandle, mv, 0, nullptr);
            sr.Execute(commands); output.Execute(commands);
            fg.Prepare(commands, &depth, &motion, common);
            queue->ExecuteCommandList(commands); fg.EndRendering(); swap.Present(); queue->Flush();
            CheckDebugMessages(info.Get());
            common.ResetHistory = false;
        };
        int revision = 0;
        for (int provider : {0, 1, 0, 3, 0, 2, 0})
        {
            Set(L"Render.FrameGeneration.Provider", provider);
            frame();
            const bool started = fg.GetStatus().Effective != GDX12FrameGenerationProvider::Off;
            for (int i = 0; i < 32; ++i) frame();
            const auto status = fg.GetStatus();
            Require(static_cast<int>(status.Requested) == provider, "Requested FG provider lost");
            if (!provider) Require(status.Effective == GDX12FrameGenerationProvider::Off, "FG Off was not applied");
            else if (!started)
            {
                Require(status.Effective == GDX12FrameGenerationProvider::Off && !status.FallbackReason.empty(), "Unsupported FG lacks explicit fallback");
                Require(status.FallbackReason.find("failed (4)") == std::string::npos &&
                    status.FallbackReason.find("failed (6)") == std::string::npos &&
                    status.FallbackReason.find("failed (-4)") == std::string::npos,
                    "FG initialization rejected the integration parameters or provider ABI");
                std::cout << "FG " << provider << " unavailable: " << status.FallbackReason << '\n';
                continue;
            }
            else
            {
                Require(status.Effective == status.Requested, "FG started but failed during rendering");
                Require(status.GeneratedFrames > 0, "Enabled FG never generated a frame");
                std::cout << "FG " << provider << " generated " << status.GeneratedFrames << " frames\n";
            }
            const auto generation = status.Generation;
            Set(L"Render.Reconstruction.Restart", ++revision); frame();
            Require(fg.GetStatus().Generation > generation, "FG explicit restart ignored");
            swap.SetFrameGenerationSuspended(true); Require(fg.GetStatus().Suspended, "FG suspension missing");
            swap.SetFrameGenerationSuspended(false); frame();
            common.WindowWidth = common.WindowWidth == 1280 ? 1281 : 1280;
            common.WindowHeight = common.WindowHeight == 720 ? 721 : 720;
            swap.Resize(common.WindowWidth, common.WindowHeight);
            sr.QueryRenderTargetResolution(); targets(); sr.Resize(); common.RequestHistoryReset = true;
            for (int i = 0; i < 8; ++i) frame();
            for (int srProvider : {1, 2, 3, 4, 0})
            {
                Set(L"Render.Upscaler.Provider", srProvider); frame();
                for (int i = 0; i < 8; ++i) frame();
                Require(fg.GetStatus().Effective == status.Effective, "Changing SR broke FG");
                if (srProvider && sr.GetStatus().Effective == GDX12UpscalerProvider::Native)
                    std::cout << "SR " << srProvider << " unavailable: " << sr.GetStatus().FallbackReason << '\n';
            }
        }
        output.ClearDenendencies(); sr.ClearDenendencies(); swap.Reset();
    }
    CheckDebugMessages(info.Get());
    GDX12ShaderCompiler::Shutdown(); GDX12DeviceFactory::Reset(); GDX12StreamlineSDK::Get().Shutdown();
}

int main(int argc, char** argv)
{
    HANDLE gate = nullptr; HWND window = nullptr;
    try
    {
        Settings();
        if (argc > 1 && std::string(argv[1]) == "--cpu") { std::cout << "FG settings passed\n"; return 0; }
        gate = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(gate != nullptr, "Cannot create GPU mutex");
        const auto wait = WaitForSingleObject(gate, INFINITE);
        Require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "Cannot acquire GPU mutex");
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"PEPEngine2FGTests";
        Require(RegisterClassW(&cls) != 0, "Cannot register test window");
        window = CreateWindowW(cls.lpszClassName, L"PEPEngine2 frame generation validation", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1300, 760, nullptr, nullptr, cls.hInstance, nullptr);
        Require(window != nullptr, "Cannot create test window"); ShowWindow(window, SW_SHOWNOACTIVATE);
        GPU(window);
        DestroyWindow(window); ReleaseMutex(gate); CloseHandle(gate);
        std::cout << "FG provider/restart/pause/resize/SR independence checks passed\n"; return 0;
    }
    catch (const std::exception& e)
    {
        if (window && IsWindow(window)) DestroyWindow(window);
        if (gate) { ReleaseMutex(gate); CloseHandle(gate); }
        std::cerr << e.what() << '\n'; return 1;
    }
}
