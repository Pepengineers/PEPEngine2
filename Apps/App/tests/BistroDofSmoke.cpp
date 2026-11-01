#include "App.Base/App.h"
#include <iostream>
#include <cmath>
#include <fstream>
#include <stdexcept>

static void CaptureHDR(GDX12Texture& texture, int quality)
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
    std::ofstream image("Build/bistro-dof-q" + std::to_string(quality) + ".ppm", std::ios::binary);
    image << "P6\n" << texture.GetWidth() << ' ' << texture.GetHeight() << "\n255\n";
    float maximum = 0;
    for (unsigned y = 0; y < texture.GetHeight(); ++y)
        for (unsigned x = 0; x < texture.GetWidth(); ++x)
        {
            const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) +
                footprint.Offset + y * footprint.Footprint.RowPitch + x * 8);
            for (unsigned c = 0; c < 3; ++c)
            {
                float value = PackedVector::XMConvertHalfToFloat(pixel[c]);
                if (!std::isfinite(value)) throw std::runtime_error("Bistro HDR contains a non-finite pixel");
                maximum = (std::max)(maximum, value);
                value = (std::max)(0.0f, value);
                value /= 1.0f + value;
                value = value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
                image.put(static_cast<char>(value * 255.0f));
            }
        }
    readback->Unmap(0, nullptr);
    if (!image) throw std::runtime_error("Cannot write Bistro readback image");
    std::cout << "Bistro HDR readback Q" << quality << " max=" << maximum << std::endl;
}

class BistroDofSmoke final : public App
{
public:
    BistroDofSmoke() : App(GetModuleHandle(nullptr)) {}

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
        render->Update();
        auto work = _lights.Schedule(_jobs, world, 0);
        _gpuData.Schedule(_jobs, world, 0, work).Complete();
        render->Render();
    }

    void InspectBottles()
    {
        Frame();
        auto& ecs = Locator.GetModule<SceneManagerModule>()->GetWorld()->GetECS();
        Vector3 target;
        bool found = false;
        ecs.ForEach<NameComponent, StaticMeshRenderComponent>([&](Entity, NameComponent& name, StaticMeshRenderComponent& mesh)
        {
            if (!found && name.value.find("LiquorBottle") != std::string::npos)
            {
                target = mesh.Bounds.Center;
                found = true;
                std::cout << "Existing bottle: " << name.value << " center=" << target.x << ',' << target.y << ',' << target.z << std::endl;
            }
        });
        if (!found) throw std::runtime_error("No existing Bistro liquor bottle found");
        auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& pose = ecs.Get<TransformComponent>(world->ActiveCamera);
        pose.Location = target + Vector3(0, 0, -80);
        pose.Rotation = Vector3::Zero;
        pose.DirtyFlag = true;
        auto& camera = ecs.Get<CameraComponent>(world->ActiveCamera);
        camera.FOV = 45;
        camera.DirtyFlag = true;
        auto render = Locator.GetModule<RenderModule>();
        render->SetActiveCamera(&camera);
        render->GetRenderPipelineCommonData()->ResetUpscalerHistory = true;
        const auto set = [&](const wchar_t* name, const char* value)
        {
            if (!ConsoleModule{}.FindConsoleVariable(name).SetFromString(value)) throw std::runtime_error("Runtime DoF write failed");
        };
        set(L"Render.DOF.Enabled", "true");
        set(L"Render.DOF.FocusDistanceCm", "80");
        set(L"Render.DOF.FNumber", "0.7");
        set(L"Render.DOF.SensorHeightMm", "36");
        for (int quality : { 0, 1, 2, 3 })
        {
            set(L"Render.DOF.Quality", std::to_string(quality).c_str());
            const std::wstring title = L"DOF Bistro Q" + std::to_wstring(quality);
            SetWindowTextW(GetWindow()->GetWindowHandle(), title.c_str());
            std::cout << "Bistro runtime quality=" << quality << std::endl;
            for (unsigned frame = 0; frame < 32; ++frame) Frame();
            render->WaitForUploads();
            CaptureHDR(*render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture(), quality);
            if (render->GetRenderPipelineCommonData()->ResetUpscalerHistory)
                throw std::runtime_error("Bistro XeSS did not consume history reset");
        }
    }
protected:
    bool AddModules() override
    {
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
        Locator.RegisterModule(std::make_shared<SceneManagerModule>(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro"));
        return BenchmarkEngine::AddModules();
    }
};

int main()
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
    if (!mutex) return 1;
    const auto wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return 1; }
    int result = 0;
    try
    {
        BistroDofSmoke app;
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
        app.InspectBottles();
        std::cout << "Bistro existing geometry: Off/Low/Mid/High runtime transitions completed." << std::endl;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; result = 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}
