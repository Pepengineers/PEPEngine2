#define WinMain BistroEntryPointForAdditionsTest
#include "../src/App.cpp"
#undef WinMain
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <map>
#include <imgui/imgui.h>
#include "BistroReflectionChecks.h"
#include "BistroCharacterChecks.h"
#include "BistroGrassChecks.h"
#include "BistroWaterChecks.h"
#include "BistroParticleControlsChecks.h"

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static void Set(const wchar_t* name, const char* value)
{
    Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "Cannot change Bistro fixture CVar");
}

static std::vector<uint16_t> ReadHDR(GDX12Texture& texture)
{
    auto* resource = texture.GetResource()->D3DResource.Get();
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    const auto desc = resource->GetDesc();
    Require(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "Expected linear HDR output");
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
    Require(event != nullptr, "Cannot create HDR readback event");
    const auto result = fence->SetEventOnCompletion(1, event);
    const auto wait = SUCCEEDED(result) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    ThrowIfFailed(result);
    Require(wait == WAIT_OBJECT_0, "Bistro HDR readback timed out");
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped));
    std::vector<uint16_t> pixels(size_t(texture.GetWidth()) * texture.GetHeight() * 4);
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(pixels.data() + size_t(y) * texture.GetWidth() * 4,
            static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
            size_t(texture.GetWidth()) * 8);
    readback->Unmap(0, nullptr);
    for (size_t i = 0; i < pixels.size(); ++i)
        Require(std::isfinite(PackedVector::XMConvertHalfToFloat(pixels[i])), "Nonfinite Bistro HDR pixel");
    return pixels;
}

static void SaveImage(const std::vector<uint16_t>& pixels, UINT width, UINT height, const char* name)
{
    std::filesystem::create_directories("Build/BistroAdditions");
    std::ofstream file(std::string("Build/BistroAdditions/") + name + ".ppm", std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
        {
            float value = (std::max)(0.0f, PackedVector::XMConvertHalfToFloat(pixels[i + c]));
            value /= 1 + value;
            value = value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
            file.put(static_cast<char>((std::min)(255.0f, value * 255)));
        }
    Require(bool(file), "Cannot save Bistro validation image");
}

static void CheckCounts(World& world, size_t probes, size_t grass, size_t water, size_t characters)
{
    auto& ecs = world.GetECS();
    Require(ecs.GetPool<ReflectionProbeComponent>().Size() == probes, "Wrong Bistro reflection count");
    Require(ecs.GetPool<GrassRenderComponent>().Size() == grass, "Wrong Bistro grass count");
    Require(ecs.GetPool<FluidRenderComponent>().Size() == water, "Wrong Bistro water count");
    Require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == characters, "Wrong Bistro character count");
    Require(ecs.GetPool<FlyingPropComponent>().Size() == 346, "Scene additions changed original Bistro props");
    Require(ecs.GetPool<ParticleEmitterComponent>().Size() == 8, "Scene additions changed original Bistro particles");
}

struct CharacterFrame
{
    Vector3 Position;
    std::vector<Matrix> Pose;
};

static std::map<Entity, CharacterFrame> Characters(World& world)
{
    std::map<Entity, CharacterFrame> result;
    world.GetECS().ForEach<SkeletalMeshRenderComponent, TransformComponent>(
        [&](Entity entity, const SkeletalMeshRenderComponent& mesh, const TransformComponent& transform)
        {
            const auto* scene = AssetManager::GetInstance().Scenes().GetScene(mesh.GetScene());
            Require(scene && mesh.GetClip() >= 0, "Bistro character has no animated scene");
            result.emplace(entity, CharacterFrame{transform.Location,
                scene->EvaluatePose(mesh.GetClip(), mesh.GetTimeSeconds(), mesh.IsLooping())});
        });
    return result;
}

static void Aim(World& world, Vector3 eye, Vector3 target)
{
    auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
    auto direction = target - eye;
    direction.Normalize();
    camera.Location = eye;
    camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)),
        XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
    camera.DirtyFlag = true;
    world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
}

static void CheckProbeOverlay(BistroApp& app, World& world)
{
    const auto saved = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
    const auto window = app.GetWindow()->GetWindowHandle();
    app.MsgProc(window, WM_KEYUP, VK_F1, 0);
    Set(L"App.ReflectionProbes.Count", "1");
    Set(L"App.ReflectionProbes.ShowBounds", "false");
    const auto target = BistroReflections::Tables[0].Centre + Vector3(0, 60, 0);
    Aim(world, target + Vector3(-250,100,-250), target);
    app.BenchmarkFrame();
    Require(ImGui::GetDrawData()->TotalVtxCount == 0, "Hidden settings/probe overlay must have no draw geometry");
    Set(L"App.ReflectionProbes.ShowBounds", "true");
    app.BenchmarkFrame();
    Require(ImGui::GetDrawData()->TotalVtxCount > 0, "Probe bounds and labels must produce visible overlay geometry");
    for (const auto eye : {target, target + Vector3(0,0,2), target + Vector3(-250,100,-250)})
    {
        Aim(world, eye, eye + Vector3(0,0,200));
        app.BenchmarkFrame();
        const auto* data = ImGui::GetDrawData();
        for (int list = 0; list < data->CmdListsCount; ++list)
            for (const auto& vertex : data->CmdLists[list]->VtxBuffer)
                Require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) &&
                    std::abs(vertex.pos.x) < 100000 && std::abs(vertex.pos.y) < 100000,
                    "Probe bounds crossing the camera/near plane must remain clipped and finite");
    }
    Set(L"App.ReflectionProbes.Count", "4");
    auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
    camera.Location = saved.Location; camera.Rotation = saved.Rotation; camera.DirtyFlag = true;
    app.MsgProc(window, WM_KEYUP, VK_F1, 0);
    app.BenchmarkFrame();
}

static void CheckProbeSpheres(BistroApp& app, World& world, RenderModule& render)
{
    Set(L"Render.Upscaler.Provider", "0");
    Set(L"Render.Particles.Enabled", "false");
    Set(L"Render.Grass.Enabled", "false");
    Set(L"Render.Fluid.Enabled", "false");
    Set(L"Render.SSR.Enabled", "false");
    Set(L"Render.DOF.Enabled", "false");
    Set(L"Render.MotionBlur.Enabled", "false");
    Set(L"Render.AO.Enabled", "0");
    Aim(world, Vector3(-800,300,-1100), Vector3(-100,165,-700));
    std::vector<uint16_t> reference;
    const auto difference = [&](const std::vector<uint16_t>& pixels, size_t index)
    {
        float maximum = 0;
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float before = PackedVector::XMConvertHalfToFloat(reference[index + channel]);
            const float after = PackedVector::XMConvertHalfToFloat(pixels[index + channel]);
            maximum = (std::max)(maximum, std::abs(after - before) / (std::max)(1.0f, std::abs(before)));
        }
        return maximum;
    };
    for (const bool enabled : {false,true,false})
    {
        Set(L"App.ReflectionProbes.ShowSpheres", enabled ? "true" : "false");
        for (int frame = 0; frame < 6; ++frame) app.BenchmarkFrame();
        render.WaitForUploads();
        auto* output = render.GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        const auto pixels = ReadHDR(*output);
        if (reference.empty()) reference = pixels;
        else if (!enabled)
        {
            float drift = 0;
            for (size_t i = 0; i < pixels.size(); i += 4) drift = (std::max)(drift, difference(pixels, i));
            // Allow FP16 roundoff in the full scene; the isolated fixture checks exact off identity.
            Require(drift <= 0.002f, "Disabling probe spheres left visible changes in the scene HDR");
            std::cout << "Probe preview off RGB relative drift: " << drift << std::endl;
        }
        else
        {
            size_t changed = 0;
            for (size_t i = 0; i < pixels.size(); i += 4)
                if (difference(pixels, i) > 0.01f) ++changed;
            Require(changed > 1000, "Published probe cubemaps must produce visible sphere pixels above terrace tables");
            SaveImage(pixels, output->GetWidth(), output->GetHeight(), "probe-spheres");
            std::cout << "Probe sphere HDR pixels: " << changed << std::endl;
        }
    }
    Set(L"App.ReflectionProbes.ShowSpheres", "true");
    Set(L"Render.Grass.Enabled", "true");
    Set(L"Render.Fluid.Enabled", "true");
}

int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug5> names;
        if (SUCCEEDED(debug.As(&names))) names->SetEnableAutoName(TRUE);
        CheckBistroReflectionCounts();
        CheckBistroCharacterCounts();
        CheckBistroParticleControls();
        BistroApp app(GetModuleHandleW(nullptr));
        Require(app.Initialize(), "Bistro additions initialization failed");
        CheckBistroGrassCounts();
        ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
        auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        CheckBistroWaterCounts(world, [&] { app.BenchmarkFrame(); });
        CheckProbeOverlay(app, world);
        CheckCounts(world, 4, 1, 1, 5);
        const wchar_t* counts[] = {L"App.ReflectionProbes.Count", L"App.Grass.Count", L"App.Water.Count", L"App.NPC.Count", L"App.Fox.Count"};
        for (const auto* name : counts)
            Require(!ConsoleModule{}.FindConsoleVariable(name).SetFromString("-1"), "Negative feature count accepted");
        for (int frame = 0; frame < 48; ++frame) app.BenchmarkFrame();
        const auto initial = Characters(world);
        for (int frame = 0; frame < 16; ++frame) app.BenchmarkFrame();
        const auto animated = Characters(world);
        Require(initial.size() == 5 && animated.size() == 5, "Expected four humans and one fox");
        for (const auto& [entity, before] : initial)
        {
            const auto& after = animated.at(entity);
            Require(before.Position != after.Position, "Bistro character is not walking");
            Require(before.Pose != after.Pose, "Bistro character skeletal pose is not animating");
        }
        world.SetPaused(true);
        app.BenchmarkFrame();
        const auto paused = Characters(world);
        for (int frame = 0; frame < 4; ++frame) app.BenchmarkFrame();
        const auto stillPaused = Characters(world);
        for (const auto& [entity, before] : paused)
        {
            const auto& after = stillPaused.at(entity);
            Require(before.Position == after.Position && before.Pose == after.Pose, "Paused Bistro character moved");
        }
        CheckProbeSpheres(app, world, *render);
        for (const auto* name : counts) Set(name, "0");
        app.BenchmarkFrame();
        CheckCounts(world, 0, 0, 0, 0);
        for (const auto* name : counts) Set(name, "2");
        app.BenchmarkFrame();
        CheckCounts(world, 2, 2, 2, 4);
        Set(counts[0], "4"); Set(counts[1], "1"); Set(counts[2], "1"); Set(counts[3], "4"); Set(counts[4], "1");
        app.BenchmarkFrame();
        CheckCounts(world, 4, 1, 1, 5);
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.Particles.Enabled", "false");
        Set(L"Render.SSR.Enabled", "false");
        Set(L"Render.DOF.Enabled", "false");
        Set(L"Render.MotionBlur.Enabled", "false");
        Set(L"Render.AO.Enabled", "0");
        Set(L"Render.Reflection.Enabled", "false");
        ComPtr<ID3D12InfoQueue> info;
        for (const bool grass : {true, false})
        {
            Vector3 target;
            if (grass)
                world.GetECS().ForEach<GrassRenderComponent, TransformComponent>(
                    [&](Entity, const GrassRenderComponent&, const TransformComponent& transform) { target = transform.Location + Vector3(0, 15, 0); });
            else
                world.GetECS().ForEach<FluidRenderComponent, TransformComponent>(
                    [&](Entity, const FluidRenderComponent&, const TransformComponent& transform) { target = transform.Location; });
            Aim(world, target + Vector3(-150, 130, -180), target);
            const auto* setting = grass ? L"Render.Grass.Enabled" : L"Render.Fluid.Enabled";
            std::vector<uint16_t> off;
            for (const bool enabled : {false, true})
            {
                Set(setting, enabled ? "true" : "false");
                for (int frame = 0; frame < 4; ++frame) app.BenchmarkFrame();
                render->WaitForUploads();
                auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                const auto pixels = ReadHDR(*output);
                if (!info)
                {
                    ComPtr<ID3D12Device> device;
                    ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
                    ThrowIfFailed(device.As(&info));
                }
                if (!enabled) off = pixels;
                else
                {
                    size_t changed = 0;
                    for (size_t i = 0; i < pixels.size(); i += 4)
                        if (pixels[i] != off[i] || pixels[i + 1] != off[i + 1] || pixels[i + 2] != off[i + 2]) ++changed;
                    std::cout << (grass ? "Grass" : "Water") << " changed HDR pixels: " << changed << std::endl;
                    Require(changed > 100, "Bistro feature does not contribute visible GPU pixels");
                    SaveImage(pixels, output->GetWidth(), output->GetHeight(), grass ? "grass" : "water");
                }
            }
        }
        for (const bool fox : {false, true})
        {
            const auto* setting = fox ? L"App.Fox.Count" : L"App.NPC.Count";
            Set(setting, "1");
            app.BenchmarkFrame();
            Vector3 target;
            bool found = false;
            world.GetECS().ForEach<SkeletalMeshRenderComponent, NameComponent, TransformComponent>(
                [&](Entity, const SkeletalMeshRenderComponent&, const NameComponent& name, const TransformComponent& transform)
                {
                    if (name.value == (fox ? "BistroFox0" : "BistroNPC0"))
                    {
                        target = transform.Location + Vector3(0, fox ? 25.0f : 90.0f, 0);
                        found = true;
                    }
                });
            Require(found, "Character validation target missing");
            Aim(world, target + Vector3(fox ? -130.0f : -200.0f, 60, -180), target);
            std::vector<uint16_t> off;
            for (const bool enabled : {false, true})
            {
                Set(setting, enabled ? "1" : "0");
                for (int frame = 0; frame < 4; ++frame) app.BenchmarkFrame();
                render->WaitForUploads();
                auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                const auto pixels = ReadHDR(*output);
                if (!enabled) off = pixels;
                else
                {
                    size_t changed = 0;
                    for (size_t i = 0; i < pixels.size(); i += 4)
                        if (pixels[i] != off[i] || pixels[i + 1] != off[i + 1] || pixels[i + 2] != off[i + 2]) ++changed;
                    std::cout << (fox ? "Fox" : "Human NPC") << " changed HDR pixels: " << changed << std::endl;
                    Require(changed > 100, "Bistro skeletal character does not contribute visible GPU pixels");
                    SaveImage(pixels, output->GetWidth(), output->GetHeight(), fox ? "fox" : "npc");
                }
            }
            if (!fox)
                for (const bool front : {true,false})
                {
                    Vector3 forward;
                    world.GetECS().ForEach<SkeletalMeshRenderComponent, NameComponent, TransformComponent>(
                        [&](Entity, SkeletalMeshRenderComponent& mesh, const NameComponent& name, const TransformComponent& transform)
                        {
                            if (name.value != "BistroNPC0") return;
                            mesh.SetTimeSeconds(front ? 0.3 : 0.8);
                            const float yaw = XMConvertToRadians(transform.Rotation.y);
                            forward = -Vector3(std::sin(yaw),0,std::cos(yaw));
                        });
                    Aim(world, target + forward * (front ? 250.0f : -250.0f) + Vector3(0,35,0), target);
                    for (int frame = 0; frame < 4; ++frame) app.BenchmarkFrame();
                    render->WaitForUploads();
                    auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                    SaveImage(ReadHDR(*output), output->GetWidth(), output->GetHeight(), front ? "npc-front" : "npc-back");
                }
        }
        render->WaitForUploads();
        for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
        {
            SIZE_T bytes = 0;
            info->GetMessage(index, nullptr, &bytes);
            std::vector<BYTE> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(index, message, &bytes));
            Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
        }
        std::cout << "Bistro additions: default/zero/changed counts, walking skeletal animation, pause, grass/water GPU pixels, clean debug layer passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
