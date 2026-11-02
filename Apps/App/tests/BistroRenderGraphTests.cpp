#include "App.Base/App.h"
#include "../src/BistroScene.h"
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void Set(const wchar_t* name, const char* value)
{
    if (!ConsoleModule{}.FindConsoleVariable(name).SetFromString(value)) throw std::runtime_error("Cannot change fixture setting: " + WStringToString(name));
}

static std::vector<BYTE> CaptureHDR(GDX12Texture& texture)
{
    auto* resource = texture.GetResource()->D3DResource.Get();
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)));
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    const D3D12_RESOURCE_BARRIER barrier = texture.GetResource()->GetCopySourceBarrier();
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) commands->ResourceBarrier(1, &barrier);
    const CD3DX12_TEXTURE_COPY_LOCATION source(resource, 0), destination(readback.Get(), footprint);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    ThrowIfFailed(commands->Close());
    ID3D12CommandList* lists[] = { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Cannot create readback fence event");
    const auto result = fence->SetEventOnCompletion(1, event);
    const auto wait = SUCCEEDED(result) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    ThrowIfFailed(result);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("Bistro readback timed out");
    void* mapped;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped));
    std::vector<BYTE> pixels(size_t(texture.GetWidth()) * texture.GetHeight() * 8);
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(pixels.data() + size_t(y) * texture.GetWidth() * 8,
            static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
            size_t(texture.GetWidth()) * 8);
    readback->Unmap(0, nullptr);
    return pixels;
}

class BistroRenderGraphTests final : public App
{
public:
    BistroRenderGraphTests() : App(GetModuleHandleW(nullptr)) {}
    void Frame()
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Timer.Tick();
        render->BeginFrame();
        render->Update();
        auto work = _lights.Schedule(_jobs, world, 0);
        _gpuData.Schedule(_jobs, world, 0, work).Complete();
        render->Render();
    }

    void Check()
    {
        Timer.Reset();
        Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.SSR.Enabled", "false");
        Set(L"Render.DOF.Enabled", "false");
        Set(L"Render.MotionBlur.Enabled", "false");
        Set(L"Render.Reflection.Enabled", "false");
        Set(L"Render.AO.Implementation", "0");
        Set(L"Render.AO.Enabled", "0");
        Frame();
        render->WaitForUploads();
        auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        for (int early : { 0, 1 })
        {
            Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
            std::vector<BYTE> reference;
            for (bool reuse : { false, true, false })
            {
                Set(L"Render.Graph.ReuseTextures", reuse ? "true" : "false");
                for (int frame = 0; frame < 4; ++frame) Frame();
                render->WaitForUploads();
                output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                const auto pixels = CaptureHDR(*output);
                if (reference.empty()) reference = pixels;
                else if (pixels != reference) throw std::runtime_error("Bistro pooled output differs from dedicated HDR output");
                const auto stats = render->GetRenderGraphStatistics();
                if (reuse && stats.PhysicalTextures >= stats.TransientTextures) throw std::runtime_error("Bistro graph did not reuse textures");
                if (!reuse && stats.PhysicalTextures != stats.TransientTextures) throw std::runtime_error("Dedicated graph mode still shares textures");
                std::cout << "Bistro earlyAO=" << early << " reuse=" << reuse << " textures="
                    << stats.PhysicalTextures << '/' << stats.TransientTextures << " HDR readback identical" << std::endl;
            }
        }
        Set(L"Render.Graph.ReuseTextures", "true");
        Set(L"Render.AO.Enabled", "1");
        Set(L"Render.SSR.Enabled", "true");
        Set(L"Render.DOF.Enabled", "true");
        Set(L"Render.MotionBlur.Enabled", "true");
        Set(L"Render.Reflection.Enabled", "true");
        SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        Set(L"Render.Upscaler.Provider", "1");
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        std::cout << "Bistro graph: pooled/dedicated HDR identity, both AO placements, odd resize and native/XeSS switch passed." << std::endl;
    }
protected:
    bool AddModules() override
    {
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
        auto scenes = std::make_shared<SceneManagerModule>(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro");
        ConfigureBistroProps(*scenes);
        Locator.RegisterModule(scenes);
        return BenchmarkEngine::AddModules();
    }
};

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        BistroRenderGraphTests app;
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
        app.Check();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
