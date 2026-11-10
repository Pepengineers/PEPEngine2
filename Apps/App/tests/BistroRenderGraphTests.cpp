#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/App.h"
#include "App.Base/Rendering/CrossAdapterAO.h"
#include "../src/BistroScene.h"
#include "../src/BistroGrass.h"
#include "../src/Components/BistroInteractionSource.h"
#include "../src/BistroWater.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <map>

static void Set(const wchar_t* name, const char* value)
{
    if (!ConsoleModule{}.FindConsoleVariable(name).SetFromString(value)) throw std::runtime_error("Cannot change fixture setting: " + WStringToString(name));
}

static void CheckMessages(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<BYTE> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

struct GDX12AOTestAccess
{
    static void FreezeNoise(GDX12AmbientOcclusionPass& ao) { ao._frameIndex = 7; }
    static GDX12AmbientOcclusionPass::FrameInput ObserveFrozenFrame(GDX12AmbientOcclusionPass& ao)
    {
        const auto next = ao._frameIndex;
        FreezeNoise(ao);
        auto input = ao.CaptureFrameInput();
        ao._frameIndex = next;
        return input;
    }
};

static std::vector<BYTE> CaptureTexture(GDX12Resource& tracker, UINT pixelBytes)
{
    auto* resource = tracker.D3DResource.Get();
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
    const auto previousState = tracker.GetCurrentState();
    if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, previousState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &barrier);
    }
    const CD3DX12_TEXTURE_COPY_LOCATION source(resource, 0), destination(readback.Get(), footprint);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, previousState);
        commands->ResourceBarrier(1, &barrier);
    }
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
    const size_t rowBytes = size_t(desc.Width) * pixelBytes;
    std::vector<BYTE> pixels(rowBytes * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + size_t(y) * rowBytes,
            static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch, rowBytes);
    readback->Unmap(0, nullptr);
    return pixels;
}

static std::vector<BYTE> CaptureTexture(GDX12Texture& texture, UINT pixelBytes)
{
    return CaptureTexture(*texture.GetResource(), pixelBytes);
}

static float ReadFloatPixel(const std::vector<BYTE>& pixels, size_t offset)
{
    float value;
    std::memcpy(&value, pixels.data() + offset, sizeof(value));
    return value;
}

static void CheckAOReference(const std::vector<BYTE>& visibility, const std::vector<BYTE>& depthPixels, UINT width, UINT height)
{
    if (depthPixels.size() != visibility.size()) throw std::runtime_error("Bistro AO depth extent differs");
    float minimum = 1.0f, maximum = 0.0f;
    size_t visible = 0, occluded = 0;
    for (size_t i = 0; i < visibility.size(); i += sizeof(float))
    {
        const float z = ReadFloatPixel(depthPixels, i);
        const float ao = ReadFloatPixel(visibility, i);
        if (!std::isfinite(z) || !std::isfinite(ao) || z < 0 || z > 1 || ao < 0 || ao > 1)
            throw std::runtime_error("Bistro AO reference contains invalid depth or visibility");
        if (z <= 0) continue;
        ++visible;
        if (ao < 0.999f) ++occluded;
        minimum = (std::min)(minimum, ao);
        maximum = (std::max)(maximum, ao);
    }
    std::cout << "AO reference " << width << 'x' << height
        << " visible=" << visible << " occluded=" << occluded << " min=" << minimum << " max=" << maximum << '\n';
    if (!visible || !occluded || maximum - minimum <= 0.001f)
        throw std::runtime_error("Bistro AO reference has no measurable occlusion on visible geometry");
}

static size_t ChangedPixels(const std::vector<BYTE>& reference, const std::vector<BYTE>& current, size_t pixelBytes)
{
    if (reference.size() != current.size()) throw std::runtime_error("Bistro AO input extent changed");
    size_t changed = 0;
    for (size_t i = 0; i < reference.size(); i += pixelBytes)
        changed += std::memcmp(reference.data() + i, current.data() + i, pixelBytes) != 0;
    return changed;
}

#ifdef _DEBUG
static bool CheckCrossVendorAO(const std::vector<BYTE>& actual, const std::vector<BYTE>& expected, UINT width)
{
    if (actual.size() != expected.size() || actual.empty()) throw std::runtime_error("AO diagnostic extent mismatch");
    const size_t pixels = actual.size() / sizeof(float);
    const size_t height = pixels / width;
    std::vector<float> errors(pixels);
    std::array<size_t, 40> tiles{};
    double sum = 0, squares = 0;
    size_t above1m = 0, above1c = 0, above5c = 0;
    float maximum = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        const float error = std::abs(ReadFloatPixel(actual, i * sizeof(float)) -
            ReadFloatPixel(expected, i * sizeof(float)));
        errors[i] = error;
        sum += error;
        squares += double(error) * error;
        above1m += error > 0.001f;
        above1c += error > 0.01f;
        above5c += error > 0.05f;
        maximum = (std::max)(maximum, error);
        if (error > 1e-5f) ++tiles[(i / width) * 5 / height * 8 + (i % width) * 8 / width];
    }
    const auto p99 = errors.begin() + static_cast<ptrdiff_t>((pixels - 1) * 99 / 100);
    std::nth_element(errors.begin(), p99, errors.end());
    const double mean = sum / pixels;
    const double rms = std::sqrt(squares / pixels);
    std::cout << "Secondary placed vs primary local max=" << maximum << " mean=" << mean << " rms=" << rms
        << " p99=" << *p99 << " >0.001=" << above1m << " >0.01=" << above1c << " >0.05=" << above5c
        << " errorPixelsByTile=";
    for (size_t count : tiles) std::cout << count << ',';
    std::cout << '\n';
    // Frozen 1280x800 Bistro visibility fixture, measured on the NVIDIA/Radeon pair.
    return mean <= 1.0 / 4096.0 && rms <= 1.0 / 1024.0 && *p99 <= 1.0f / 512.0f && maximum <= 1.0f / 8.0f;
}

static std::unique_ptr<GDX12Resource> MakeAOReplayOutput(GDX12Device& device, UINT width, UINT height,
    GDX12DescriptorHeap& heap, GDX12Descriptor& descriptor)
{
    auto output = std::make_unique<GDX12Resource>();
    output->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    descriptor.InitAsUAV(output->D3DResource.Get(), nullptr, &view, &heap,
        heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    return output;
}

static GDX12AmbientOcclusionPass::PlacedTexture MakeAOReplayInput(GDX12Resource& resource,
    GDX12DescriptorHeap& heap, GDX12Descriptor& descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = resource.D3DResource->GetDesc().Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    descriptor.InitAsSRV(resource.D3DResource.Get(), &view, &heap,
        heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    return { &resource, descriptor.GPUHandle };
}

static void RecordAOReplay(GDX12AmbientOcclusionPass& ao, GDX12CommandQueue& queue,
    const GDX12AmbientOcclusionPass::FrameInput& input, const GDX12AmbientOcclusionPass::PlacedInputs& inputs,
    GDX12AmbientOcclusionPass::PlacedStage stage)
{
    const auto slot = ao.CreatePlacedSlot(stage, input.Width, input.Height, input.Settings.Resolution);
    ao.PreparePlacedSlot(slot, input, inputs);
    auto* commands = queue.GetCommandList();
    const auto& accesses = ao.GetPlacedAccesses(*slot);
    for (const auto& access : accesses)
    {
        const auto state = access.Resource->GetCurrentState();
        if (state == access.State) continue;
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(access.Native.Get(), state, access.State);
        commands->GetCommandList()->ResourceBarrier(1, &barrier);
    }
    ao.RecordPlacedSlot(commands, *slot);
    for (const auto& access : accesses)
    {
        const auto state = access.Resource->GetCurrentState();
        if (state == access.State) continue;
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(access.Native.Get(), access.State, state);
        commands->GetCommandList()->ResourceBarrier(1, &barrier);
    }
    queue.ExecuteCommandList(commands);
    queue.Flush();
}

static std::unique_ptr<GDX12Resource> ReplayEvaluate(GDX12AmbientOcclusionPass& ao, GDX12DeviceResources& resources,
    const GDX12AmbientOcclusionPass::FrameInput& input, GDX12Resource& depth, GDX12Resource& normals)
{
    using AO = GDX12AmbientOcclusionPass;
    auto& device = *resources.Device;
    auto& heap = *resources.SRV_UAV_Heap;
    auto* compute = device.TryGetComputeQueue();
    if (!compute) throw std::runtime_error("No COMPUTE queue for AO replay");
    const auto work = depth.D3DResource->GetDesc();
    GDX12Descriptor depthSRV, normalSRV, outputUAV;
    auto output = MakeAOReplayOutput(device, static_cast<UINT>(work.Width), work.Height, heap, outputUAV);
    AO::PlacedInputs inputs{};
    inputs.PreparedDepth = MakeAOReplayInput(depth, heap, depthSRV);
    inputs.PreparedNormals = MakeAOReplayInput(normals, heap, normalSRV);
    inputs.Visibility = { output.get(), {}, outputUAV.GPUHandle };
    RecordAOReplay(ao, *compute, input, inputs, AO::PlacedStage::Evaluate);
    return output;
}

static std::vector<BYTE> ReplayHalfFinalize(GDX12AmbientOcclusionPass& ao, GDX12DeviceResources& primary,
    const GDX12AmbientOcclusionPass::FrameInput& input, GDX12Texture& depth, GDX12Texture& normals,
    GDX12Resource& preparedDepth, GDX12Resource& preparedNormals, GDX12Resource& visibility)
{
    using AO = GDX12AmbientOcclusionPass;
    auto& heap = *primary.SRV_UAV_Heap;
    GDX12Descriptor depthSRV, normalSRV, visibilitySRV, outputUAV;
    auto output = MakeAOReplayOutput(*primary.Device, input.Width, input.Height, heap, outputUAV);
    AO::PlacedInputs inputs{};
    inputs.Depth = { depth.GetResource(), depth.GetSRV()->GPUHandle };
    inputs.Normals = { normals.GetResource(), normals.GetSRV()->GPUHandle };
    inputs.PreparedDepth = MakeAOReplayInput(preparedDepth, heap, depthSRV);
    inputs.PreparedNormals = MakeAOReplayInput(preparedNormals, heap, normalSRV);
    inputs.Visibility = MakeAOReplayInput(visibility, heap, visibilitySRV);
    inputs.Output = { output.get(), {}, outputUAV.GPUHandle };
    RecordAOReplay(ao, *primary.Device->GetCommandQueue(), input, inputs, AO::PlacedStage::Finalize);
    return CaptureTexture(*output, sizeof(float));
}

static void SaveAOComparison(const char* algorithm, const std::vector<BYTE>& local,
    const std::vector<BYTE>& placed, UINT width, UINT height)
{
    const auto directory = std::filesystem::path(SOLUTION_DIR) / "Build/P4/AOImages";
    std::filesystem::create_directories(directory);
    const auto write = [&](const char* suffix, const auto& read)
    {
        std::ofstream file(directory / (std::string(algorithm) + "-Full-Late-" + suffix + ".pgm"), std::ios::binary);
        file << "P5\n" << width << ' ' << height << "\n255\n";
        for (size_t i = 0; i < local.size(); i += sizeof(float))
            file.put(static_cast<char>(std::round(std::clamp(read(i), 0.0f, 1.0f) * 255.0f)));
        if (!file) throw std::runtime_error("Cannot write AO comparison image");
    };
    write("primary", [&](size_t i) { return ReadFloatPixel(local, i); });
    write("secondary", [&](size_t i) { return ReadFloatPixel(placed, i); });
    write("difference-x16", [&](size_t i) { return 16.0f * std::abs(ReadFloatPixel(local, i) - ReadFloatPixel(placed, i)); });
}
#endif

class BistroRenderGraphTests final : public App
{
public:
    explicit BistroRenderGraphTests(bool aoMatrix) : App(GetModuleHandleW(nullptr)), _aoMatrix(aoMatrix) {}
    void Frame(double step = -1, double elapsed = -1)
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        if (elapsed >= 0) Timer.Tick(step, elapsed); else Timer.Tick(step);
        render->BeginFrame();
        render->Update();
        auto work = _lights.Schedule(_jobs, world, step >= 0 ? static_cast<float>(step) : 0);
        _gpuData.Schedule(_jobs, world, step >= 0 ? static_cast<float>(step) : 0, work).Complete();
        render->Render();
    }

#ifdef _DEBUG
    void CheckCollisionView()
    {
        Timer.Reset();
        Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        const auto scenePasses = render->GetRenderGraphStatistics().Passes;
        auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        for (bool enabled : {true, false, true, false})
        {
            BistroRenderPipeline::ToggleCollisionView();
            for (int frame = 0; frame < 3; ++frame) Frame();
            render->WaitForUploads();
            CheckMessages(*info.Get());
            if (render->GetRenderGraphStatistics().Passes != (enabled ? 1u : scenePasses) ||
                render->GetFrameGenerationStatus().Suspended != enabled)
                throw std::runtime_error("Collision view did not switch the scene graph and frame generation together");
        }
        std::cout << "Collision-only graph, frame generation suspension and return to the scene passed\n";
    }
#endif

    void Check()
    {
        Timer.Reset();
        Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        BistroGrass::Scene grass;
        grass.Update(world, 0);
        auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        camera.Location = Vector3(-300, 200, 20);
        camera.Rotation = Vector3(0, 117, 0);
        camera.DirtyFlag = true;
        world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.SSR.Enabled", "false");
        Set(L"Render.DOF.Enabled", "false");
        Set(L"Render.MotionBlur.Enabled", "false");
        Set(L"Render.Reflection.Enabled", "false");
        Set(L"Render.Particles.Enabled", "true");
        // Atomic particle compaction changes FP16 blend order even with a zero simulation delta.
        Set(L"Render.Particles.DrawEnabled", "false");
        // Atomic culling reorders transparency and the asset's coplanar ceramic/plate-details overlays.
        std::map<GDX12Material*, std::pair<MaterialType, float>> materialStates;
        bool plateOverlayFound = false;
        world.GetECS().ForEach<StaticMeshRenderComponent>([&](Entity, StaticMeshRenderComponent& mesh)
        {
            const auto& imported = render->GetPrimaryGPUMesh(mesh.MeshHandler)->CPUMesh->GetMaterials();
            for (size_t slot = 0; slot < mesh.Materials.size(); ++slot)
            {
                auto* material = mesh.Materials[slot];
                const bool plateOverlay = material && material->Type == MaterialType::Masked &&
                    slot < imported.size() && imported[slot].Name == "Plates_Details";
                plateOverlayFound |= plateOverlay;
                if (material && (material->Type == MaterialType::Transparent || plateOverlay))
                {
                    materialStates.try_emplace(material, material->Type, material->Opacity);
                    material->Type = MaterialType::Transparent;
                    material->Opacity = 0;
                    material->DirtyFlag = true;
                }
            }
        });
        if (!plateOverlayFound) throw std::runtime_error("Fixture did not find the coplanar plate overlay");
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
            for (bool reuse : { false, false, true, false })
            {
                for (bool async : { false, false, true, false })
                {
                    Set(L"Render.Particles.AsyncCompute", async ? "true" : "false");
                    Set(L"Render.Graph.ReuseTextures", reuse ? "true" : "false");
                    for (int frame = 0; frame < 4; ++frame) Frame();
                    render->WaitForUploads();
                    CheckMessages(*info.Get());
                    output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                    const auto pixels = CaptureTexture(*output, 8);
                    if (reference.empty()) reference = pixels;
                    else if (pixels != reference) throw std::runtime_error("Bistro HDR differs from the first dedicated capture (reuse=" +
                        std::to_string(reuse) + ", async=" + std::to_string(async) + ")");
                    const auto stats = render->GetRenderGraphStatistics();
                    if (stats.Passes > 64) throw std::runtime_error("Bistro graph expanded geometry views into individual passes");
                    if (reuse && stats.PhysicalTextures >= stats.TransientTextures) throw std::runtime_error("Bistro graph did not reuse textures");
                    if (!reuse && stats.PhysicalTextures != stats.TransientTextures) throw std::runtime_error("Dedicated graph mode still shares textures");
                    if (stats.AsyncPasses != (async ? 1u : 0u)) throw std::runtime_error("Bistro particle queue selection failed");
                    if (async && stats.JoinBefore != "Fluid surface")
                        throw std::runtime_error("Bistro joined before its first declared depth hazard: " + stats.JoinBefore);
                    std::cout << "Bistro earlyAO=" << early << " reuse=" << reuse << " async=" << async << " passes=" << stats.Passes << " textures="
                        << stats.PhysicalTextures << '/' << stats.TransientTextures << " join=" << stats.JoinBefore
                        << " HDR readback identical" << std::endl;
                }
            }
            const auto directory = std::filesystem::path(SOLUTION_DIR) / "Build/Verification";
            std::filesystem::create_directories(directory);
            std::ofstream graph(directory / (early ? "bistro-early-ao.dot" : "bistro-late-ao.dot"));
            render->WriteRenderGraph(graph);
            if (!graph) throw std::runtime_error("Cannot write Bistro render graph");
        }
        Set(L"Render.Graph.ReuseTextures", "true");
        Set(L"Render.AO.Enabled", "1");
        Set(L"Render.Particles.AsyncCompute", "true");
        for (int early : { 0, 1 })
            for (int implementation = 0; implementation < 4; ++implementation)
            {
                Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
                Set(L"Render.AO.Implementation", std::to_string(implementation).c_str());
                for (int frame = 0; frame < 4; ++frame) Frame();
                render->WaitForUploads();
                CheckMessages(*info.Get());
                if (render->GetRenderGraphStatistics().JoinBefore != "Fluid surface")
                    throw std::runtime_error("Active AO joined its shared depth reader early: " + render->GetRenderGraphStatistics().JoinBefore);
                std::cout << "Bistro shared depth: active AO=" << implementation << " early=" << early << " passed." << std::endl;
            }
        Set(L"Render.SSR.Enabled", "true");
        Set(L"Render.DOF.Enabled", "true");
        Set(L"Render.MotionBlur.Enabled", "true");
        Set(L"Render.Reflection.Enabled", "true");
        Set(L"Render.Particles.AsyncCompute", "true");
        std::cout << "Bistro async: all effects and odd resize started." << std::endl;
        Set(L"Render.Particles.DrawEnabled", "true");
        for (const auto& [material, state] : materialStates)
        {
            material->Type = state.first;
            material->Opacity = state.second;
            material->DirtyFlag = true;
        }
        SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        CheckMessages(*info.Get());
        std::cout << "Bistro async: odd resize passed; Native/XeSS switch started." << std::endl;
        Set(L"Render.Upscaler.Provider", "1");
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        Set(L"Render.Particles.AsyncCompute", "false");
        CheckMessages(*info.Get());
        std::cout << "Bistro graph: pooled/dedicated HDR identity, both AO placements, odd resize and native/XeSS switch passed." << std::endl;
    }
    void GrassExperiment(bool timing)
    {
        Timer.Reset(); Timer.Start();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        ecs.RegisterComponent<BistroInteractionSource>();
        std::vector<Entity> sources;
        for (UINT i = 0; i < 32; ++i)
        {
            auto entity = ecs.CreateEntity();
            entity.AddComponent<BistroInteractionSource>(); sources.push_back(entity.GetId());
        }
        BistroGrass::Scene grass; grass.Update(world, 0);
        Set(L"Render.FrameGeneration.Provider", "0"); Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.AO.ExecutionPoint", "1");
        Set(L"App.Particles.Count", "10000"); BistroParticles::Update(world);
        auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        camera.Location = BistroArch::GrassPosition + Vector3(450,200,-450);
        auto direction = BistroArch::GrassPosition + Vector3(0,70,0) - camera.Location; direction.Normalize();
        camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)), XMConvertToDegrees(std::atan2(direction.x,direction.z)),0);
        camera.DirtyFlag = true;
        ComPtr<ID3D12InfoQueue> info;
        if (!timing)
        {
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture()->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
            ThrowIfFailed(device.As(&info)); info->ClearStoredMessages();
        }
        std::filesystem::create_directories("Build/GrassInteraction");
        std::ofstream samples("Build/GrassInteraction/bistro-timing.csv");
        samples << "wind,series,async,sample,sources,gpu_ms,compute_sum_ms,cpu_submit_ms,drain\n";
        BistroWater::Scene water;
        for (int wind : {0,1})
        {
            Set(L"App.Grass.Wind.Algorithm", wind ? "1" : "0"); grass.Update(world, 0);
            int series = 0;
            for (bool async : {false,true,true,false})
            {
                Set(L"Render.Grass.AsyncCompute", async ? "true" : "false");
                Set(L"Render.Fluid.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"Render.Particles.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"Render.AO.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"App.Water.Count", timing ? "0" : "1"); water.Update(world, 1.0f/60);
                ecs.ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
                { emitter.Reset(); if (emitter.IsEnabled()) emitter.Emit(emitter.GetSettings().MaxParticles); });
                ecs.ForEach<WindFieldComponent>([](Entity, WindFieldComponent& field) { field.Reset(); });
                ecs.ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { patch.ResetHistory(); });
                const int warmup = timing ? 16 : 4, count = timing ? 60 : 8;
                for (int frame = 0; frame < warmup + count; ++frame)
                {
                    for (UINT i = 0; i < sources.size(); ++i)
                    {
                        auto& source = ecs.Get<BistroInteractionSource>(sources[i]);
                        const float x = BistroArch::GrassPosition.x + float(i % 8) * 35 - 120;
                        const float z = BistroArch::GrassPosition.z + std::sin((frame + i) * 0.05f) * 180;
                        source.PreviousHeight = frame ? XMFLOAT4(source.PositionRadius.x, source.PositionRadius.y, source.PositionRadius.z,190) : XMFLOAT4(x,23.3f,z,190);
                        source.PositionRadius = XMFLOAT4(x,23.3f,z,65);
                    }
                    water.Update(world, 1.0f/60);
                    const auto start = std::chrono::steady_clock::now(); Frame(1.0/60,(frame+1)/60.0);
                    const double cpu = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    if (timing) render->WaitForUploads(); // Explicit CPU drain: not pipelined FPS.
                    if (frame >= warmup)
                        samples << wind << ',' << series << ',' << async << ',' << frame-warmup << ",32," << render->GetGPUFrameTimeMS()
                            << ',' << render->GetRenderGraphStatistics().ComputeTimeMS << ',' << cpu << ',' << timing << '\n';
                }
                render->WaitForUploads();
                if (info) CheckMessages(*info.Get());
                std::ofstream graph("Build/GrassInteraction/graph-w" + std::to_string(wind) + "-s" + std::to_string(series) + ".dot");
                render->WriteRenderGraph(graph);
                if (async && render->GetRenderGraphStatistics().AsyncPasses < 1) throw std::runtime_error("Grass COMPUTE dispatch missing");
                std::cout << "Grass experiment wind=" << wind << " series=" << series++ << " async=" << async << " passed; CPU-drain=" << timing << std::endl;
            }
        }
        if (!timing)
        {
            Set(L"Render.Grass.AsyncCompute", "true"); Set(L"Render.Fluid.AsyncCompute", "true");
            Set(L"Render.Particles.AsyncCompute", "true"); Set(L"Render.AO.AsyncCompute", "true");
            for (int scenario = 0; scenario < 4; ++scenario)
            {
                Set(L"Render.Wind.Enabled", scenario == 0 ? "false" : "true");
                Set(L"Render.Grass.Enabled", scenario == 1 ? "false" : "true");
                Set(L"Render.Graph.ReuseTextures", scenario & 1 ? "true" : "false");
                if (scenario == 2) ecs.ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { ++patch.Seed; patch.ResetHistory(); });
                SetWindowPos(GetWindow()->GetWindowHandle(),nullptr,0,0,973+scenario,657,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                world.SetPaused(scenario == 3);
                for (int frame=0;frame<4;++frame) Frame(scenario == 3 ? 0 : 1.0/60,(frame+1)/60.0);
                render->WaitForUploads(); CheckMessages(*info.Get());
            }
            world.SetPaused(false);
            std::cout << "Grass combined async: queued frames, water/particles/AO, disabled wind/grass, seed reset, pause and odd resize passed." << std::endl;
        }
    }
    void Time()
    {
        Timer.Reset(); Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Set(L"Render.FrameGeneration.Provider", "0");
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.Particles.Enabled", "true");
        Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true");
        Set(L"App.Particles.Count", "250000");
        BistroParticles::Update(world);
        std::ofstream samples("Build/async-bistro-timing.csv");
        if (!samples) throw std::runtime_error("Cannot create timing report");
        samples << "series,async,sample,gpu_ms,compute_ms,cpu_submit_ms\n";
        int series = 0;
        for (bool async : {false, true, false})
        {
            Set(L"Render.Particles.AsyncCompute", async ? "true" : "false");
            world.GetECS().ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
            {
                emitter.Reset();
                emitter.Emit(emitter.GetSettings().MaxParticles);
            });
            for (int frame = 0; frame < 24; ++frame) Frame();
            render->WaitForUploads();
            std::cout << "Timing series=" << series << " async=" << async << " 250000 particles, frozen dt=0" << std::endl;
            for (int frame = 0; frame < 120; ++frame)
            {
                const auto started = std::chrono::steady_clock::now();
                Frame();
                const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                render->WaitForUploads();
                const auto& stats = render->GetRenderGraphStatistics();
                samples << series << ',' << async << ',' << frame << ',' << render->GetGPUFrameTimeMS() << ','
                    << (async ? stats.ComputeTimeMS : -1) << ',' << cpu << '\n';
            }
            std::ofstream graph("Build/async-bistro-series-" + std::to_string(series) + ".dot");
            render->WriteRenderGraph(graph);
            ++series;
        }
        Set(L"Render.Particles.AsyncCompute", "false");
        if (!samples) throw std::runtime_error("Cannot write timing report");
    }
    void CheckAOPlacements(bool multiGPU, [[maybe_unused]] bool saveImages, bool strictDiagnostic)
    {
        Timer.Reset(); Timer.Stop();
        Set(L"Render.Fluid.AsyncCompute", "false"); Set(L"Render.Particles.AsyncCompute", "false");
        auto render = Locator.GetModule<RenderModule>();
        const auto devices = render->GetMGPUDevices();
        auto& direct = *devices.Primary->GetCommandQueue();
        auto& passes = *_bistroPipeline->_passes;
        const auto& common = *render->GetRenderPipelineCommonData();
        ComPtr<ID3D12InfoQueue> primaryInfo, secondaryInfo;
        bool strictDiagnosticFailed = false;
        ThrowIfFailed(devices.Primary->GetDevice().As(&primaryInfo));
        primaryInfo->ClearStoredMessages();
        if (multiGPU)
        {
            if (!render->GetMGPURuntime() || !devices.Secondary)
                throw std::runtime_error("Placed Bistro AO needs two physical adapters");
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
            camera.Location = Vector3(-300, 200, 20);
            camera.Rotation = Vector3(0, 117, 0);
            camera.DirtyFlag = true;
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            Set(L"Render.AO.Enabled", "true");
            Set(L"Render.Upscaler.Provider", "0");
            Set(L"Render.FrameGeneration.Provider", "0");
            Set(L"Render.Graph.ReuseTextures", "false");
            ThrowIfFailed(devices.Secondary->GetDevice().As(&secondaryInfo));
            secondaryInfo->ClearStoredMessages();
            Frame();
            render->WaitForUploads();
            CheckMessages(*primaryInfo.Get());
            CheckMessages(*secondaryInfo.Get());
        }
        for (int half : {0, 1})
        {
            Set(L"Render.AO.Resolution", half ? "1" : "0");
            for (int early : {0, 1})
            {
                Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
                for (int implementation = 0; implementation < 4; ++implementation)
                {
                    Set(L"Render.AO.Implementation", std::to_string(implementation).c_str());
                    std::vector<BYTE> reference, referenceDepth, referenceNormals;
#ifdef _DEBUG
                    std::vector<BYTE> placedReference;
#endif
                    DirectX::SimpleMath::Matrix view, projection;
#ifdef _DEBUG
                    GDX12AmbientOcclusionPass::FrameInput referenceInput{};
#endif
                    for (int phase = 0; phase < 3; ++phase)
                    {
                        const bool async = phase == 1;
                        const bool placed = multiGPU && phase == 2;
                        Set(L"Render.AO.AsyncCompute", async ? "true" : "false");
                        if (multiGPU) Set(L"Render.AO.MultiGPU", placed ? "true" : "false");
                        float maxError = 0;
                        for (int frame = 0; frame < 2; ++frame)
                        {
                            const auto previousFinalize = passes.GetLastAmbientOcclusionFinalize();
                            const auto before = direct.GetLastCompletionPoint();
                            if (multiGPU) GDX12AOTestAccess::FreezeNoise(passes.AO);
                            Frame();
                            if (!multiGPU) continue;
                            const auto finalize = passes.GetLastAmbientOcclusionFinalize();
                            const auto tail = direct.GetLastCompletionPoint();
                            if (placed ? (!finalize.IsValid() || !(finalize.Timeline() == tail.Timeline()) ||
                                finalize.Value() <= before.Value() || finalize.Value() >= tail.Value())
                                : (finalize.Value() != previousFinalize.Value()))
                                throw std::runtime_error("Bistro AO Finalize receipt disagrees with the frame's route");

                            render->WaitForUploads();
                            if (placed && !finalize.IsReachedBy(direct.GetCompletionProgress()))
                                throw std::runtime_error("Bistro AO readback preceded Finalize retirement");
                            if (common.ActiveCameraJitterOffsetX != 0 || common.ActiveCameraJitterOffsetY != 0)
                                throw std::runtime_error("Bistro AO parity requires a frozen, unjittered camera. Jitter=" +
                                    std::to_string(common.ActiveCameraJitterOffsetX) + "," +
                                    std::to_string(common.ActiveCameraJitterOffsetY));
                            auto& output = *passes.AO.GetOutput(0)->GetTexture();
                            auto& depth = *passes.Depth.GetOutput(2)->GetTexture();
                            auto& normals = *(early ? passes.Depth : passes.Opaque).GetOutput(3)->GetTexture();
                            if (output.GetWidth() != 1280 || output.GetHeight() != 800)
                                throw std::runtime_error("Bistro cross-vendor AO limits require the frozen 1280x800 grid");
                            if (output.GetResource()->D3DResource->GetDesc().Format != DXGI_FORMAT_R32_FLOAT)
                                throw std::runtime_error("Unexpected Bistro AO lighting input format");
                            if (depth.GetResource()->D3DResource->GetDesc().Format != DXGI_FORMAT_R32_TYPELESS ||
                                normals.GetResource()->D3DResource->GetDesc().Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
                                throw std::runtime_error("Unexpected Bistro AO scene input format");
                            auto pixels = CaptureTexture(output, sizeof(float));
                            auto depthPixels = CaptureTexture(depth, sizeof(float));
                            auto normalPixels = CaptureTexture(normals, 8);
                            if (reference.empty())
                            {
                                CheckAOReference(pixels, depthPixels, output.GetWidth(), output.GetHeight());
                                reference = std::move(pixels);
                                referenceDepth = std::move(depthPixels);
                                referenceNormals = std::move(normalPixels);
                                view = common.CameraView;
                                projection = common.CameraViewToClip;
#ifdef _DEBUG
                                referenceInput = GDX12AOTestAccess::ObserveFrozenFrame(passes.AO);
#endif
                            }
                            else
                            {
                                if (!(view == common.CameraView) || !(projection == common.CameraViewToClip) ||
                                    pixels.size() != reference.size())
                                    throw std::runtime_error("Bistro AO camera or render extent changed between routes");
                                const auto changedDepth = ChangedPixels(referenceDepth, depthPixels, sizeof(float));
                                const auto changedNormals = ChangedPixels(referenceNormals, normalPixels, 8);
                                if (changedDepth || changedNormals)
                                    throw std::runtime_error("Bistro AO frame inputs changed: depth=" +
                                        std::to_string(changedDepth) + " normals=" + std::to_string(changedNormals));
                                size_t aboveTolerance = 0;
                                size_t peakPixel = 0;
                                float frameMax = 0;
                                for (size_t i = 0; i < pixels.size(); i += sizeof(float))
                                {
                                    const float actual = ReadFloatPixel(pixels, i);
                                    const float expected = ReadFloatPixel(reference, i);
                                    if (!std::isfinite(actual) || actual < 0 || actual > 1)
                                        throw std::runtime_error("Invalid Bistro AO visibility");
                                    const float error = std::abs(actual - expected);
                                    aboveTolerance += error > 1e-5f;
                                    if (error > frameMax) { frameMax = error; peakPixel = i / sizeof(float); }
                                }
                                maxError = (std::max)(maxError, frameMax);
#ifdef _DEBUG
                                if (placed && frame == 0)
                                {
                                    std::cout << "AO diagnostic half=" << half << " early=" << early
                                        << " implementation=" << implementation << '\n';
                                    auto& adapter = *passes._placedAO;
                                    const auto& captured = adapter.GetDebugFrame();
                                    const auto& input = captured.Input;
                                    if (input.Width != referenceInput.Width || input.Height != referenceInput.Height ||
                                        input.NoiseIndex != referenceInput.NoiseIndex || !(input.View == referenceInput.View) ||
                                        !(input.JitteredProjection == referenceInput.JitteredProjection) ||
                                        std::memcmp(&input.Settings, &referenceInput.Settings, sizeof(input.Settings)))
                                        throw std::runtime_error("Placed FrameInput differs from local");

                                    std::array<std::shared_ptr<GDX12Resource>, 3> source, imported;
                                    for (size_t channel = 0; channel < source.size(); ++channel)
                                    {
                                        source[channel] = captured.Source[channel].lock();
                                        imported[channel] = captured.Imported[channel].lock();
                                        if (!source[channel] || !imported[channel]) throw std::runtime_error("AO transfer view expired");
                                        const UINT bytes = channel == 1 ? 8 : 4;
                                        const auto changed = ChangedPixels(CaptureTexture(*source[channel], bytes),
                                            CaptureTexture(*imported[channel], bytes), bytes);
                                        std::cout << "AO transport channel=" << channel << " differing pixels=" << changed << '\n';
                                        if (changed) throw std::runtime_error("AO typed transport changed payload pixels");
                                    }
                                    auto& primary = *_bistroPipeline->_resources;
                                    auto primaryResult = ReplayEvaluate(passes.AO, primary, input, *source[0], *source[1]);
                                    const auto primaryReplay = half
                                        ? ReplayHalfFinalize(passes.AO, primary, input, depth, normals, *source[0], *source[1], *primaryResult)
                                        : CaptureTexture(*primaryResult, sizeof(float));
                                    if (ChangedPixels(primaryReplay, reference, sizeof(float)))
                                        throw std::runtime_error("Same-device primary AO replay differs from legacy");
                                    std::cout << "Primary replay matched legacy on every pixel\n";

                                    auto secondaryResult = ReplayEvaluate(adapter._evaluate, adapter._secondary, input, *imported[0], *imported[1]);
                                    const auto secondaryReplay = CaptureTexture(*secondaryResult, sizeof(float));
                                    if (ChangedPixels(secondaryReplay, CaptureTexture(*source[2], sizeof(float)), sizeof(float)))
                                        throw std::runtime_error("Secondary Evaluate replay differs from submitted visibility");
                                    const auto finalReplay = half
                                        ? ReplayHalfFinalize(passes.AO, primary, input, depth, normals, *source[0], *source[1], *imported[2])
                                        : secondaryReplay;
                                    if (ChangedPixels(finalReplay, pixels, sizeof(float)))
                                        throw std::runtime_error("Placed AO lighting output differs from the same-device replay");
                                    placedReference = pixels;
                                    std::cout << "Secondary Evaluate and primary Finalize matched submitted output on every pixel\n";
                                    if (!CheckCrossVendorAO(pixels, reference, input.Width))
                                        throw std::runtime_error("Bistro cross-vendor AO exceeded its visibility error limits");
                                    if (saveImages && !half && !early && implementation <= 1)
                                        SaveAOComparison(implementation == 0 ? "SSAO" : "HBAO", reference, pixels, input.Width, input.Height);
                                }
                                if (placed && frame > 0 && ChangedPixels(pixels, placedReference, sizeof(float)))
                                    throw std::runtime_error("Frozen placed AO changed between frames");
#endif
                                if (frameMax > 1e-5f)
                                {
                                    if (!placed) throw std::runtime_error("Frozen local AO changed between frames or queue types");
                                    strictDiagnosticFailed = true;
                                    std::cout << "AO difference max=" << frameMax << " peak=" << peakPixel % output.GetWidth()
                                        << ',' << peakPixel / output.GetWidth() << " pixels=" << aboveTolerance << '\n';
                                }
                            }
                            if (placed) std::cout << "AO Finalize=" << finalize.Value() << " tail=" << tail.Value() << '\n';
                        }
                        render->WaitForUploads(); CheckMessages(*primaryInfo.Get());
                        if (secondaryInfo) CheckMessages(*secondaryInfo.Get());
                        if (render->GetRenderGraphStatistics().AsyncPasses != (async ? 1u : 0u))
                            throw std::runtime_error("AO placement queue selection failed");
                        std::cout << "AO placement half=" << half << " early=" << early << " implementation=" << implementation
                            << " async=" << async << " placed=" << placed << " maxError=" << maxError
                            << (maxError > 1e-5f ? " above the strict diagnostic threshold." : " passed.") << std::endl;
                    }
                }
            }
        }
        if (multiGPU)
        {
            Set(L"Render.AO.MultiGPU", "false");
            const auto previous = passes.GetLastAmbientOcclusionFinalize();
            Frame();
            if (passes.GetLastAmbientOcclusionFinalize().Value() != previous.Value())
                throw std::runtime_error("Placed AO submitted after toggle off");
            render->WaitForUploads(); CheckMessages(*primaryInfo.Get()); CheckMessages(*secondaryInfo.Get());
        }
        Set(L"Render.AO.Resolution", "0");
        Set(L"Render.AO.ExecutionPoint", "1"); Set(L"Render.AO.Implementation", "2"); Set(L"Render.AO.AsyncCompute", "false");
        if (strictDiagnostic && strictDiagnosticFailed)
            throw std::runtime_error("Bistro placed AO exceeded the strict 1e-5 cross-vendor diagnostic threshold");
        if (multiGPU) std::cout << "Bistro AO exact per-device replay and cross-vendor visibility limits passed\n";
    }
    void Matrix(int argc, char** argv, bool validation)
    {
        int row = -1, warmup = 24, measured = 120;
        bool withAO = false;
        for (int i = 2; i < argc; ++i)
        {
            const std::string_view option(argv[i]);
            if (option == "--matrix-ao") { withAO = true; continue; }
            const auto parse = [&](std::string_view prefix, int& value)
            {
                if (option.substr(0, prefix.size()) != prefix) return false;
                const auto digits = option.substr(prefix.size());
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (error != std::errc{} || end != digits.data() + digits.size() || value < 0)
                    throw std::invalid_argument("Invalid matrix integer");
                return true;
            };
            if (!parse("--row=", row) && !parse("--warmup=", warmup) && !parse("--samples=", measured))
                throw std::invalid_argument("Unknown matrix option");
        }
        const int combinations = withAO ? 8 : 4;
        if (row >= combinations || measured == 0 || warmup > 100000 || measured > 1000000) throw std::invalid_argument("Invalid matrix row or sample count");
        Set(L"Render.AO.AsyncCompute", "false");
        if (withAO)
        {
            Set(L"Render.AO.Enabled", "true"); Set(L"Render.AO.Implementation", "2"); Set(L"Render.AO.Quality", "2");
        }
        Timer.Reset(); Timer.Start();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Set(L"Render.FrameGeneration.Provider", "0"); Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.AO.ExecutionPoint", "1");
        Set(L"Render.Fluid.Enabled", "true"); Set(L"Render.Fluid.Quality", "2"); Set(L"App.Water.Count", "1");
        Set(L"Render.Particles.Enabled", "true"); Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true"); Set(L"App.Particles.Count", "250000");
        BistroGrass::Scene grass; grass.Update(world, 0);
        BistroWater::Scene water; water.Update(world, 1.0f / 60);
        BistroParticles::Update(world);
        auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        camera.Location = BistroWater::Center + Vector3(420,330,420);
        auto direction = BistroWater::Center - camera.Location; direction.Normalize();
        camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)),
            XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
        camera.DirtyFlag = true;
        UINT waterInstances = 0; UINT64 waterParticles = 0, particleCapacity = 0;
        world.GetECS().ForEach<FluidRenderComponent>([&](Entity, FluidRenderComponent& fluid)
        {
            auto settings = fluid.GetSettings(); settings.RenderEnabled = settings.SimulationEnabled = true;
            fluid.SetSettings(settings);
            ++waterInstances;
            if (fluid.GetParticles()) waterParticles += fluid.GetParticles()->size();
        });
        world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, ParticleEmitterComponent& emitter)
        { if (emitter.IsEnabled()) particleCapacity += emitter.GetSettings().MaxParticles; });
        if (waterInstances != 1 || waterParticles != 51975 || particleCapacity != 250000)
            throw std::runtime_error("Matrix ECS water/particle population does not match the frozen workload");
        ComPtr<ID3D12InfoQueue> info;
        if (validation)
        {
            ComPtr<ID3D12Device> device;
            auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
            ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
            ThrowIfFailed(device.As(&info)); info->ClearStoredMessages();
        }
        std::filesystem::create_directories("Build/AsyncWaterMatrix");
        std::ofstream samples("Build/AsyncWaterMatrix/samples.csv");
        if (!samples) throw std::runtime_error("Cannot create matrix CSV");
        samples << "series,order,row,water_async,particles_async,ao_async,sample,dt,gpu_ms,compute_sum_ms,cpu_submit_ms,water_instances,water_particles,particle_capacity,drain\n";
        std::vector<int> rows;
        if (row >= 0) rows.push_back(row);
        else
        {
            for (int i = 0; i < combinations; ++i) rows.push_back(i);
            for (int i = combinations - 1; i >= 0; --i) rows.push_back(i);
        }
        for (size_t series = 0; series < rows.size(); ++series)
        {
            const int mask = rows[series];
            render->WaitForUploads();
            Timer.Reset(); Timer.Start();
            world.GetECS().ForEach<WindFieldComponent>([](Entity, WindFieldComponent& wind) { wind.Reset(); });
            world.GetECS().ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { patch.ResetHistory(); });
            Set(L"Render.Fluid.AsyncCompute", mask & 1 ? "true" : "false");
            Set(L"Render.Particles.AsyncCompute", mask & 2 ? "true" : "false");
            if (withAO) Set(L"Render.AO.AsyncCompute", mask & 4 ? "true" : "false");
            world.GetECS().ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid) { fluid.Reset(); });
            world.GetECS().ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
            { emitter.Reset(); if (emitter.IsEnabled()) emitter.Emit(emitter.GetSettings().MaxParticles); });
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            render->GetRenderPipelineCommonData()->ResetHistory = true;
            // Reset frames warm histories identically; measured CPU time excludes the explicit drain.
            for (int frame = 0; frame < warmup; ++frame) { water.Update(world, 1.0f / 60); Frame(1.0 / 60, (frame + 1) / 60.0); }
            render->WaitForUploads();
            std::cout << "Matrix series=" << series << " row=" << mask << " dt=1/60 water=" << waterParticles
                << " particle_capacity=" << particleCapacity << " CPU-drain" << std::endl;
            for (int frame = 0; frame < measured; ++frame)
            {
                water.Update(world, 1.0f / 60);
                const auto started = std::chrono::steady_clock::now(); Frame(1.0 / 60, (warmup + frame + 1) / 60.0);
                const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                render->WaitForUploads();
                if (info) CheckMessages(*info.Get());
                const auto& stats = render->GetRenderGraphStatistics();
                const UINT expected = !!(mask & 1) + !!(mask & 2) + !!(mask & 4);
                if (stats.AsyncPasses != expected) throw std::runtime_error("Independent matrix queue selection failed");
                if (validation && withAO && (mask & 4) && stats.JoinBefore != "Clustered deferred lighting")
                    throw std::runtime_error("Late AO joined before lighting: " + stats.JoinBefore);
                samples << series << ',' << (row >= 0 ? "single" : (series < size_t(combinations) ? "forward" : "reverse")) << ','
                    << mask << ',' << !!(mask & 1) << ',' << !!(mask & 2) << ',' << !!(mask & 4) << ',' << frame
                    << ",0.016666666666666667," << render->GetGPUFrameTimeMS() << ',' << stats.ComputeTimeMS << ',' << cpu
                    << ',' << waterInstances << ',' << waterParticles << ',' << particleCapacity << ",cpu-drain\n";
            }
            std::ofstream dot("Build/AsyncWaterMatrix/series-" + std::to_string(series) + ".dot"); render->WriteRenderGraph(dot);
            if (!dot) throw std::runtime_error("Cannot write matrix graph");
        }
        if (validation)
        {
            Set(L"Render.Fluid.AsyncCompute", "true"); Set(L"Render.Particles.AsyncCompute", "true");
            if (withAO) Set(L"Render.AO.AsyncCompute", "true");
            for (bool reuse : {false, true})
            {
                Set(L"Render.Graph.ReuseTextures", reuse ? "true" : "false");
                for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 1) / 60.0);
                render->WaitForUploads(); CheckMessages(*info.Get());
            }
            SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 4) / 60.0);
            render->WaitForUploads(); CheckMessages(*info.Get());
            Set(L"Render.Upscaler.Provider", "1");
            for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 7) / 60.0);
            render->WaitForUploads(); CheckMessages(*info.Get());
            std::cout << "Matrix combined water/particles: pooling, queued frames, odd resize and Native/XeSS passed." << std::endl;
        }
        if (!samples) throw std::runtime_error("Cannot write matrix samples");
        Set(L"Render.Fluid.AsyncCompute", "false"); Set(L"Render.Particles.AsyncCompute", "false");
        if (withAO) Set(L"Render.AO.AsyncCompute", "false");
    }
protected:
    bool AddModules() override
    {
        auto scenes = std::make_shared<SceneManagerModule>(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro");
        auto pipeline = std::make_unique<BistroRenderPipeline>(scenes);
        _bistroPipeline = pipeline.get();
        auto renderer = std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline));
        Locator.RegisterModule(renderer);
        if (!_aoMatrix) ConfigureBistroProps(*scenes, GetJobs());
        Locator.RegisterModule(scenes);
        Locator.RegisterModule(std::make_shared<PhysicsModule>(_jobs));
        return BenchmarkEngine::AddModules();
    }

private:
    bool _aoMatrix;
    BistroRenderPipeline* _bistroPipeline = nullptr;
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        const bool matrix = mode == "--matrix" || mode == "--matrix-validation" || mode == "--matrix-gpu-validation";
        const bool timing = mode == "--timing" || mode == "--grass-timing" || matrix;
        const bool strictAO = mode == "--ao-mgpu-strict-validation" || mode == "--ao-mgpu-strict-gpu-validation";
        const bool multiGPUAO = strictAO || mode == "--ao-mgpu-validation" || mode == "--ao-mgpu-gpu-validation";
#ifndef _DEBUG
        if (multiGPUAO) throw std::invalid_argument("Bistro placed AO validation requires a Debug build");
#endif
        const bool aoMatrix = multiGPUAO || mode == "--ao-placements" || mode == "--ao-placements-gpu-validation";
        const bool validationMode = mode == "--matrix-validation" || mode == "--matrix-gpu-validation";
        std::cout.setf(std::ios::unitbuf);
        ComPtr<ID3D12Debug> debug;
        if (!timing || validationMode)
        {
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
        }
        if (mode == "--gpu-validation" || mode == "--matrix-gpu-validation" || mode == "--ao-placements-gpu-validation" ||
            mode == "--ao-mgpu-gpu-validation" || mode == "--ao-mgpu-strict-gpu-validation" ||
            mode == "--grass-gpu-validation" || mode == "--collision-view-gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GameTimer fixedClock; fixedClock.Reset(); fixedClock.Tick(1.0 / 60, 2.0);
        if (std::abs(fixedClock.DeltaTime() - 1.0f / 60) > 1e-7f || std::abs(fixedClock.TotalTime() - 2.0f) > 1e-6f)
            throw std::runtime_error("Deterministic matrix clock self-check failed");
        fixedClock.Stop(); fixedClock.Tick(1.0 / 60, 3.0);
        if (fixedClock.DeltaTime() != 0) throw std::runtime_error("Paused fixed clock advanced");
        if (aoMatrix) ConsoleModule{}.Initialize();
        BistroRenderGraphTests app(aoMatrix);
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
#ifdef _DEBUG
        if (mode == "--collision-view" || mode == "--collision-view-gpu-validation") app.CheckCollisionView();
        else
#endif
        if (aoMatrix) app.CheckAOPlacements(multiGPUAO, mode == "--ao-mgpu-validation", strictAO);
        else if (mode == "--grass-timing" || mode == "--grass-validation" || mode == "--grass-gpu-validation") app.GrassExperiment(timing);
        else if (matrix) app.Matrix(argc, argv, validationMode);
        else if (timing) app.Time();
        else app.Check();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
