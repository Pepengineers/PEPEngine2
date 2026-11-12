#include "Rendering/BistroRenderPipeline.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "App.Base/App.h"
#include "../src/BistroScene.h"
#include "../src/BistroWater.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include <d3d12sdklayers.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    void Set(const wchar_t* name, const char* value)
    { Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "Cannot change fixture setting"); }
    void DebugErrors(ID3D12Device* device)
    {
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&info)));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        info->ClearStoredMessages();
    }
    std::vector<BYTE> Read(GDX12Device& device, GDX12Resource& resource, UINT pixelBytes = 0)
    {
        const auto desc = resource.D3DResource->GetDesc();
        const bool texture = desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = desc.Width;
        if (texture) device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        const auto prior = resource.GetCurrentState();
        commands->ResourceBarrier({resource.GetCopySourceBarrier()});
        if (texture)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION source(resource.D3DResource.Get(), 0), destination(readback.Get(), footprint);
            commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        else commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, resource.D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({resource.GetTransitionBarrier(prior)});
        queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(queue->FenceValue);
        void* mapped = nullptr; ThrowIfFailed(readback->Map(0, nullptr, &mapped));
        const size_t rowBytes = texture ? size_t(desc.Width) * pixelBytes : size_t(bytes);
        std::vector<BYTE> output(rowBytes * (texture ? desc.Height : 1));
        for (UINT y = 0; y < (texture ? desc.Height : 1); ++y)
            std::memcpy(output.data() + y * rowBytes, static_cast<const BYTE*>(mapped) +
                (texture ? footprint.Offset + y * footprint.Footprint.RowPitch : 0), rowBytes);
        readback->Unmap(0, nullptr);
        return output;
    }
    void Write(const std::filesystem::path& path, const std::vector<BYTE>& bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Require(bool(file), "Cannot write capture");
    }
}

class BistroRenderGraphTests final : public App
{
public:
    BistroRenderGraphTests() : App(GetModuleHandleW(nullptr)) {}
    std::pair<ComPtr<ID3D12Device>, ComPtr<ID3D12Device>> Devices()
    {
        ComPtr<ID3D12Device> primary;
        ThrowIfFailed(Locator.GetModule<RenderModule>()->GetPrimaryDevice()->GetDevice().As(&primary));
        return {primary, _secondary};
    }
    void Frame()
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessage(&message); }
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Timer.Tick(1.0 / 60);
        render->BeginFrame(); render->Update();
        auto work = _lights.Schedule(_jobs, world, 1.0f / 60);
        _gpuData.Schedule(_jobs, world, 1.0f / 60, work).Complete();
        render->Render();
    }
    void Capture(const std::filesystem::path& directory, const char* stage, bool requireMotion = false)
    {
        auto render = Locator.GetModule<RenderModule>(); render->WaitForUploads();
        auto& device = *render->GetPrimaryDevice();
        auto* owners = _pipeline->_water->Surface.GetOutput(3)->GetTexture();
        auto* motion = _pipeline->_water->Surface.GetOutput(1)->GetTexture();
        auto* geometry = _pipeline->_water->Surface.GetOutput(2)->GetTexture();
        auto* color = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        Require(owners && motion && geometry && color, "Water surface outputs disappeared");
        Require(owners->GetFormat() == DXGI_FORMAT_R32_UINT && motion->GetFormat() == DXGI_FORMAT_R16G16_FLOAT &&
            color->GetFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT, "Unexpected capture formats");
        const auto ownerBytes = Read(device, *owners->GetResource(), 4);
        const auto motionBytes = Read(device, *motion->GetResource(), 4);
        const auto geometryBytes = Read(device, *geometry->GetResource(), 16);
        const auto hdr = Read(device, *color->GetResource(), 8);
        size_t visible = 0, moving = 0;
        for (size_t pixel = 0; pixel < ownerBytes.size() / 4; ++pixel)
        {
            UINT owner; std::memcpy(&owner, ownerBytes.data() + pixel * 4, 4);
            if (!owner) continue;
            ++visible;
            uint16_t half[2]; std::memcpy(half, motionBytes.data() + pixel * 4, 4);
            const float x = PackedVector::XMConvertHalfToFloat(half[0]), y = PackedVector::XMConvertHalfToFloat(half[1]);
            Require(std::isfinite(x) && std::isfinite(y), "Water motion is nonfinite");
            moving += std::abs(x) + std::abs(y) > 1e-6f;
            float values[4]; std::memcpy(values, geometryBytes.data() + pixel * 16, 16);
            for (float value : values) Require(std::isfinite(value), "Water geometry is nonfinite");
        }
        Require(visible > 0, "No visible water pixels");
        if (requireMotion) Require(moving > 0, "Moving container produced no visible water motion");
        std::ofstream ppm(directory / (std::string(stage) + ".ppm"), std::ios::binary);
        ppm << "P6\n" << color->GetWidth() << ' ' << color->GetHeight() << "\n255\n";
        for (size_t pixel = 0; pixel < hdr.size() / 8; ++pixel)
            for (UINT channel = 0; channel < 3; ++channel)
            {
                uint16_t half; std::memcpy(&half, hdr.data() + pixel * 8 + channel * 2, 2);
                float value = PackedVector::XMConvertHalfToFloat(half);
                Require(std::isfinite(value), "Composited HDR is nonfinite");
                value = (std::max)(0.0f, value); value /= 1 + value;
                value = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1 / 2.4f) - .055f;
                ppm.put(static_cast<char>(value * 255));
            }
        Require(bool(ppm), "Cannot write preview");
        Write(directory / (std::string(stage) + ".rgba16f"), hdr);
        Write(directory / (std::string(stage) + ".owners.r32u"), ownerBytes);
        Write(directory / (std::string(stage) + ".motion.rg16f"), motionBytes);
        Write(directory / (std::string(stage) + ".geometry.rgba32f"), geometryBytes);
        std::ofstream meta(directory / (std::string(stage) + ".json"));
        meta << "{\"width\":" << color->GetWidth() << ",\"height\":" << color->GetHeight()
            << ",\"water_pixels\":" << visible << ",\"moving_water_pixels\":" << moving << "}\n";
        Require(bool(meta), "Cannot write capture metadata");
        DebugErrors(device.GetDevice().Get()); DebugErrors(_secondary.Get());
        std::cout << "PASS image stage=" << stage << " water_pixels=" << visible << " moving=" << moving << '\n';
    }
    std::vector<BYTE> Positions(UINT count)
    {
        auto render = Locator.GetModule<RenderModule>(); render->WaitForUploads();
        for (const auto& access : _pipeline->_water->Surface.GetBufferAccesses())
            if (access.Resource->D3DResource->GetDesc().Width == UINT64(count) * sizeof(GDX12FluidSolver::Particle))
            {
                const auto particles = Read(*render->GetPrimaryDevice(), *access.Resource);
                std::vector<BYTE> result(size_t(count) * sizeof(XMFLOAT3));
                for (UINT i = 0; i < count; ++i)
                    std::memcpy(result.data() + i * sizeof(XMFLOAT3), particles.data() + i * sizeof(GDX12FluidSolver::Particle), sizeof(XMFLOAT3));
                return result;
            }
        throw std::runtime_error("Cannot find displayed position buffer");
    }
    void Check(const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld(); auto& ecs = world.GetECS();
        const int secondary = ConsoleModule{}.FindConsoleVariable(L"Render.Fluid.SecondaryAdapter").GetInt();
        const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
        Require(secondary >= 0 && size_t(secondary) < adapters.size(), "Configure a secondary water adapter before startup");
        ThrowIfFailed(D3D12CreateDevice(adapters[secondary].Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_secondary)));
        Set(L"Render.Upscaler.Provider", "0"); Set(L"Render.FrameGeneration.Provider", "0");
        Set(L"Render.Fluid.Enabled", "true"); Set(L"Render.Fluid.Quality", "2"); Set(L"App.Water.Count", "1");
        BistroWater::Scene water; water.Update(world, 0);
        auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        camera.Location = BistroWater::Center + Vector3(420, 330, 420);
        auto direction = BistroWater::Center - camera.Location; direction.Normalize();
        camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)), XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
        camera.DirtyFlag = true;
        UINT count = 0;
        ecs.ForEach<FluidRenderComponent>([&](Entity, FluidRenderComponent& fluid) { count += static_cast<UINT>(fluid.GetParticles()->size()); });
        Require(count > 0, "No water particles");
        std::cout << "Water image fixture particles=" << count << " primary=" << render->GetPrimaryDevice()->GetDeviceFeatures().Name << '\n';
        Timer.Reset(); Timer.Start(); Frame();
        Require(_pipeline->_water->Surface.GetSimulationBufferAccesses().empty(), "Fixture is using the single-GPU solver");
        Capture(directory, "seed"); const auto initial = Positions(count);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do { for (int i = 0; i < 4; ++i) Frame(); }
        while (Positions(count) == initial && std::chrono::steady_clock::now() < deadline);
        Require(Positions(count) != initial, "Secondary solver never changed displayed positions before deadline");
        Capture(directory, "advanced");
        ecs.ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid)
        { auto settings = fluid.GetSettings(); settings.SimulationEnabled = false; fluid.SetSettings(settings); });
        for (int i = 0; i < 12; ++i) Frame(); Capture(directory, "paused");
        for (int i = 0; i < 8; ++i)
        {
            ecs.ForEach<FluidRenderComponent, TransformComponent>([](Entity, FluidRenderComponent&, TransformComponent& transform)
            { transform.Location.x += 1; transform.DirtyFlag = true; });
            Frame();
        }
        Capture(directory, "moved", true);
        ecs.ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid)
        { auto settings = fluid.GetSettings(); settings.SimulationEnabled = true; fluid.SetSettings(settings); fluid.Reset(); });
        for (int i = 0; i < 4; ++i) Frame(); Capture(directory, "reset");
        Set(L"App.Water.Count", "0"); water.Update(world, 0); Frame();
        Require(!_pipeline->_water->Surface.GetOutput(3)->GetTexture(), "Removed water retained its owner output");
        Set(L"App.Water.Count", "1"); water.Update(world, 0);
        for (int i = 0; i < 4; ++i) Frame(); Capture(directory, "recreated");
        SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < 4; ++i) Frame(); Capture(directory, "resized");
        Set(L"Render.Fluid.Enabled", "false"); for (int i = 0; i < 3; ++i) Frame();
        Require(!_pipeline->_water->Surface.GetOutput(3)->GetTexture(), "Disabled water retained its owner output");
        Set(L"Render.Fluid.Enabled", "true"); for (int i = 0; i < 4; ++i) Frame(); Capture(directory, "reenabled");
        Frame(); Frame();
        std::cout << "PASS Bistro offload image/motion/pause/reset/recreate/resize/disable; queued shutdown follows\n";
    }
protected:
    bool AddModules() override
    {
        auto scenes = std::make_shared<SceneManagerModule>(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro");
        auto pipeline = std::make_unique<BistroRenderPipeline>(scenes); _pipeline = pipeline.get();
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline)));
        ConfigureBistroProps(*scenes, GetJobs()); Locator.RegisterModule(scenes);
        Locator.RegisterModule(std::make_shared<PhysicsModule>(_jobs));
        return BenchmarkEngine::AddModules();
    }
private:
    BistroRenderPipeline* _pipeline = nullptr;
    ComPtr<ID3D12Device> _secondary;
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        bool gpuValidation = false;
        std::filesystem::path output = "Build/WaterOffloadBistro";
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument == "--gpu-validation") gpuValidation = true;
            else if (argument == "--output" && i + 1 < argc) output = argv[++i];
            else throw std::invalid_argument("Usage: [--gpu-validation] [--output directory]");
        }
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        if (gpuValidation)
        {
            ComPtr<ID3D12Debug1> validation; ThrowIfFailed(debug.As(&validation)); validation->SetEnableGPUBasedValidation(TRUE);
        }
        std::pair<ComPtr<ID3D12Device>, ComPtr<ID3D12Device>> devices;
        { BistroRenderGraphTests app; Require(app.Initialize(), "Bistro initialization failed"); app.Check(output); devices = app.Devices(); }
        DebugErrors(devices.first.Get()); DebugErrors(devices.second.Get());
        ThrowIfFailed(devices.first->GetDeviceRemovedReason()); ThrowIfFailed(devices.second->GetDeviceRemovedReason());
        std::cout << "PASS queued Bistro shutdown\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
