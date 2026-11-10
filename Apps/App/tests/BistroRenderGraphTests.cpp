#include "Rendering/BistroRenderPipeline.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "App.Base/App.h"
#include "App.Base/Rendering/CrossAdapterAO.h"
#include "App.Base/Rendering/CrossAdapterEnvironment.h"
#include "App.Base/Rendering/CrossAdapterUI.h"
#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/Grass/BistroGrassRoute.h"
#include "Rendering/Grass/BistroGrassShadowRoute.h"
#include "Rendering/UI/BistroUIPosters.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "../src/BistroCharacters.h"
#include "../src/BistroUICanvases.h"
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
#include <functional>
#include <map>
#include <condition_variable>
#include <mutex>
#include <thread>

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

static std::vector<BYTE> CaptureTexture(GDX12Resource& tracker, UINT pixelBytes, UINT subresource = 0)
{
    auto* resource = tracker.D3DResource.Get();
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)));
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
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
    const CD3DX12_TEXTURE_COPY_LOCATION source(resource, subresource), destination(readback.Get(), footprint);
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

static std::vector<BYTE> CaptureFieldBuffer(GDX12Device& device, GDX12Resource& source)
{
    const auto desc = source.D3DResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !desc.Width)
        throw std::runtime_error("P6 grass display is not a packed buffer");
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(desc.Width),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    const auto previous = source.GetCurrentState();
    commands->ResourceBarrier({source.GetCopySourceBarrier()});
    commands->CopyResource(readback.Get(), source.D3DResource.Get());
    commands->ResourceBarrier({source.GetTransitionBarrier(previous)});
    queue->ExecuteCommandList(commands);
    queue->CPUWaitForFenceValue(commands->FenceValue);
    const auto size = static_cast<size_t>(desc.Width);
    std::vector<BYTE> data(size);
    const D3D12_RANGE readRange{0, size};
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    std::memcpy(data.data(), mapped, size);
    const D3D12_RANGE written{};
    readback->Unmap(0, &written);
    return data;
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

template<class Work>
static void WithQueueHeld(GDX12Device& device, GDX12CommandQueue& queue, const Work& work)
{
    ComPtr<ID3D12Fence> gate;
    ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    std::mutex mutex;
    std::condition_variable released;
    bool open = false, expired = false;
    std::thread guard([&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!released.wait_for(lock, std::chrono::seconds(30), [&] { return open; }) && gate->GetCompletedValue() == 0)
        {
            expired = true;
            gate->Signal(1);
        }
    });
    const auto finish = [&]
    {
        const auto result = gate->Signal(1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        released.notify_one();
        guard.join();
        return result;
    };
    try
    {
        ThrowIfFailed(queue.GetCommandQueue()->Wait(gate.Get(), 1));
        work(*gate.Get());
    }
    catch (...)
    {
        finish();
        if (expired) std::cerr << "P6 watchdog released held queue before failure\n";
        throw;
    }
    ThrowIfFailed(finish());
    if (expired) throw std::runtime_error("P6 held GPU queue required its watchdog release");
}

enum class MGPUProductionVariant { Full, CloudSoak, SunOnly, SelectedPoint };

class BistroRenderGraphTests final : public App
{
public:
    explicit BistroRenderGraphTests(bool staticRenderScene) : App(GetModuleHandleW(nullptr)), _staticRenderScene(staticRenderScene) {}
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

    void CheckSharedShadowAlpha(RenderModule& render, World& original)
    {
        auto scenes = Locator.GetModule<SceneManagerModule>();
        auto& assets = Engine::Core::AssetManager::GetInstance();
        const auto directory = std::filesystem::absolute("Build/P6/ShadowAlpha");
        std::filesystem::create_directories(directory);
        Entity lastOriginalTransform = 0;
        original.GetECS().ForEach<TransformComponent>([&](Entity entity, const TransformComponent&)
        {
            lastOriginalTransform = (std::max)(lastOriginalTransform, entity);
        });
        for (bool maskedFirst : {false, true})
        {
            const std::string prefix = maskedFirst ? "P6MaskFirst" : "P6SpecularFirst";
            const auto texturePath = directory / (prefix + ".dds");
            TextureDesc desc;
            desc.Dimension = ETextureDimension::Texture2D;
            desc.Type = ETextureType::Color;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Width = desc.Height = 4;
            desc.MipLevels = 2;
            std::vector<SubTexture> expected;
            for (UINT size : {4u, 2u})
            {
                SubTexture mip;
                mip.Width = mip.Height = size;
                mip.RowPitch = size * 4;
                mip.SlicePitch = size * mip.RowPitch;
                mip.Data.resize(mip.SlicePitch);
                for (size_t i = 0; i < mip.Data.size(); ++i)
                    mip.Data[i] = static_cast<std::byte>((13 + i * 37 + size) & 255);
                expected.push_back(std::move(mip));
            }
            const auto texture = assets.Textures().Register(texturePath);
            if (!assets.Textures().Cache(texture, std::make_unique<Texture>(desc, expected)))
                throw std::runtime_error("P6 could not cache the shared alpha fixture texture");

            const std::string opaqueId = prefix + "Opaque", maskedId = prefix + "Masked";
            const auto worldPath = directory / (prefix + ".world");
            std::ofstream yaml(worldPath);
            yaml << "World:\n  Name: " << prefix << "\n  Resources:\n    Meshes:\n";
            for (const bool masked : {maskedFirst, !maskedFirst})
            {
                const auto& name = masked ? maskedId : opaqueId;
                const auto meshPath = directory / (name + ".obj");
                MeshMaterial material;
                material.Name = name;
                if (masked)
                {
                    material.Type = EMaterialType::Masked;
                    material.DiffuseTexturePath = texturePath;
                }
                else material.SpecularTexturePath = texturePath;
                SubMesh submesh;
                submesh.Vertices.resize(3);
                submesh.Vertices[1].Position = Vector3::UnitX;
                submesh.Vertices[2].Position = Vector3::UnitY;
                submesh.Indices = {0, 1, 2};
                submesh.Bounds = DirectX::BoundingBox({0.5f, 0.5f, 0}, {0.5f, 0.5f, 0});
                const auto bounds = submesh.Bounds;
                const auto mesh = assets.Meshes().Register(MeshAssetLocator{meshPath});
                if (!assets.Meshes().Cache(mesh, std::make_unique<Mesh>(std::vector<SubMesh>{std::move(submesh)},
                    bounds, std::vector<MeshMaterial>{std::move(material)})))
                    throw std::runtime_error("P6 could not cache a shared alpha fixture mesh");
                yaml << "      - Id: " << name << "\n        Source: '" << meshPath.generic_string()
                    << "'\n        SubmitToRenderer: false\n        ImportMaterials: true\n";
            }
            yaml << "  Entities:\n";
            // RenderModule keys GPU transforms by entity ID across the loaded worlds.
            for (Entity entity = 0; entity <= lastOriginalTransform; ++entity)
                yaml << "    - Id: placeholder" << entity << '\n';
            yaml << "    - Id: camera\n      Components:\n"
                "        Transform:\n          Location: [0, 0, -5]\n          Rotation: [0, 0, 0]\n"
                "          Scale: [1, 1, 1]\n        Camera:\n          FOV: 60\n          NearPlane: 0.1\n"
                "          FarPlane: 100\n  ActiveCamera: camera\n";
            yaml.close();
            if (!yaml) throw std::runtime_error("P6 could not write the shared alpha fixture world");
            const auto index = scenes->GetWorldCount();
            if (!scenes->LoadWorld(worldPath)) throw std::runtime_error("P6 shared alpha fixture world failed to load");
            if (scenes->GetWorld(index)->ActiveCamera <= lastOriginalTransform)
                throw std::runtime_error("P6 shared alpha camera overlaps an original Bistro transform ID");
            render.WaitForUploads();
            auto* opaque = render.GetMaterialByName(opaqueId + "_Material_0");
            auto* masked = render.GetMaterialByName(maskedId + "_Material_0");
            if (!opaque || !masked || !masked->Diffuse || opaque->Specular != masked->Diffuse || masked->Type != MaterialType::Masked)
                throw std::runtime_error("P6 fixture did not share the actual Color upload across both meshes");
            const auto& retained = render._gpu->ShadowAlpha;
            const auto source = retained.find(masked->Diffuse);
            if (source == retained.end() || source->second.Mips.size() != expected.size() ||
                source->second.Format != masked->Diffuse->PrimaryDeviceTexture->GetFormat())
                throw std::runtime_error("P6 masked texture cache failed to retain the uploaded mip chain");
            for (size_t mip = 0; mip < expected.size(); ++mip)
                if (source->second.Mips[mip].RowPitch != expected[mip].RowPitch ||
                    source->second.Mips[mip].Bytes != expected[mip].Data)
                    throw std::runtime_error("P6 masked texture cache retained bytes from a different upload");
            if (!scenes->UnloadWorld(index)) throw std::runtime_error("P6 could not unload the shared alpha fixture world");
            render.SetActiveCamera(&original.GetECS().Get<CameraComponent>(original.ActiveCamera));
        }
        std::cout << "P6 shared masked Color upload retained both exact mips in both mesh orders.\n";
    }

    void CheckMGPUProduction(bool requested, bool expectSecondary, const std::filesystem::path& readback, MGPUProductionVariant variant)
    {
        const bool cloudSoak = variant == MGPUProductionVariant::CloudSoak;
        const bool sunOnly = variant == MGPUProductionVariant::SunOnly;
        const bool selectedPoint = variant == MGPUProductionVariant::SelectedPoint;
        Timer.Reset();
        Timer.Start();
        auto render = Locator.GetModule<RenderModule>();
        const auto devices = render->GetMGPUDevices();
        if (render->GetMGPUStatus().Requested != requested || (devices.Secondary != nullptr) != expectSecondary)
            throw std::runtime_error("P6 adapter state does not match its immutable startup seed");
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& pipeline = *_bistroPipeline;
        if (expectSecondary && !cloudSoak) CheckSharedShadowAlpha(*render, world);
        ComPtr<ID3D12InfoQueue> primaryInfo, secondaryInfo;
        ThrowIfFailed(devices.Primary->GetDevice().As(&primaryInfo));
        if (devices.Secondary) ThrowIfFailed(devices.Secondary->GetDevice().As(&secondaryInfo));
        const auto checkQueues = [&]
        {
            devices.Primary->GetCommandQueue()->Flush();
            if (devices.Secondary) devices.Secondary->GetCommandQueue()->Flush();
            if (auto* runtime = render->GetMGPURuntime()) runtime->Poll();
            CheckMessages(*primaryInfo.Get());
            if (secondaryInfo) CheckMessages(*secondaryInfo.Get());
        };

        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.FrameGeneration.Provider", "0");
        Set(L"Render.Graph.ReuseTextures", "true");
        Set(L"Render.AO.Enabled", "true");
        Set(L"Render.AO.Implementation", "2");
        Set(L"Render.Fluid.Enabled", "true");
        Set(L"Render.Particles.Enabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true");
        Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"App.Particles.Count", "250000");
        if (requested && !cloudSoak)
        {
            for (auto* setting : { L"Render.AO.MultiGPU", L"Render.Fluid.MultiGPU", L"Render.Fluid.MultiGPU.Exact",
                L"Render.Clouds.Bistro", L"Render.Clouds.MultiGPU", L"Render.Grass.MultiGPU", L"Render.Grass.MultiGPU.Exact",
                L"Render.Shadows.MultiGPU", L"Render.UI.Offload" })
                Set(setting, "true");
            Set(L"Render.Clouds.Latest", "false");
            Set(L"Render.Shadows.MultiGPU.Selection", sunOnly ? "1" : selectedPoint ? "3" : "0");
            Set(L"Render.UI.OffloadScope", "0");
            Set(L"Render.UI.Freshness", "0");
        }

        if (expectSecondary && !cloudSoak)
        {
            Set(L"App.Grass.Wind.Algorithm", "1");
            Set(L"App.Grass.Wind.GridResolution", "16");
        }
        BistroGrass::Scene grass;
        BistroWater::Scene water;
        BistroCharacters::Scene characters(20261110);
        constexpr double dt = 1.0 / 60;
        grass.Update(world, 0);
        GDX12LocalShadowKey pointKey, spotKey;
        if (expectSecondary && !cloudSoak)
        {
            auto* mask = render->GetMaterialByName("P6MaskFirstMasked_Material_0");
            if (!mask) throw std::runtime_error("P6 missing live masked grass material");
            std::optional<Vector3> grassOrigin;
            world.GetECS().ForEach<GrassRenderComponent, TransformComponent>(
                [&](Entity, GrassRenderComponent& patch, const TransformComponent& transform)
                {
                    patch.Material = mask;
                    if (!grassOrigin) grassOrigin = transform.Location;
                });
            if (!grassOrigin) throw std::runtime_error("P6 missing live grass origin");
            const auto light = [&](LightType type, const char* name)
            {
                auto entity = world.GetECS().CreateEntity();
                entity.AddComponent<NameComponent>(name);
                entity.AddComponent<TransformComponent>(*grassOrigin + Vector3(0, 145, 0), Vector3(90, 0, 0), Vector3(1));
                auto& component = entity.AddComponent<LightComponent>();
                component.Type = type;
                component.Range = 360;
                component.ShadowUpdate = ShadowUpdateMode::EveryFrame;
                return GDX12LocalShadowKey{entity.GetId(), component.GetShadowGeneration()};
            };
            pointKey = light(LightType::Point, "P6GrassPoint");
            spotKey = light(LightType::Spot, "P6GrassSpot");
            if (selectedPoint) Set(L"Render.Shadows.MultiGPU.Entity", std::to_string(pointKey.EntityId).c_str());
        }
        water.Update(world, static_cast<float>(dt));
        characters.Update(world, 0, GetJobs());
        CreateBistroParticles(world);
        if (!world.GetECS().GetPool<SkeletalMeshRenderComponent>().Size())
            throw std::runtime_error("P6 fixture requires a live skinned shadow caster");
        bool firefly = false;
        world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
        {
            firefly |= emitter.IsEnabled() && emitter.GetSettings().Geometry == ParticleGeometry::Firefly &&
                emitter.GetSettings().MaxParticles != 0;
        });
        if (!firefly) throw std::runtime_error("P6 fixture requires a live Firefly reader");
        world.GetECS().ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid)
        {
            auto settings = fluid.GetSettings();
            settings.RenderEnabled = settings.SimulationEnabled = true;
            fluid.SetSettings(settings);
        });
        auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        camera.Location = Vector3(-300, 200, 20);
        camera.Rotation = Vector3(0, 117, 0);
        camera.DirtyFlag = true;
        world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
        int frame = 0;
        const auto step = [&]
        {
            grass.Update(world, static_cast<float>(dt));
            characters.Update(world, static_cast<float>(dt), GetJobs());
            water.Update(world, static_cast<float>(dt));
            BistroParticles::Update(world);
            Frame(dt, ++frame * dt);
        };
        if (cloudSoak)
        {
            CheckCloudLatestSoak(*pipeline._cloudEnvironment, *render->GetMGPURuntime(),
                *devices.Primary, *devices.Secondary, step, *primaryInfo.Get(), *secondaryInfo.Get());
            return;
        }
        const auto surface = [&](size_t index)
        {
            return render->GetLastLegacyReplacement(pipeline._posters->RasterPass(index));
        };
        const auto shadowTrace = [&](GDX12ShadowAtlas atlas)
        {
            const auto& shadows = pipeline._passes->_placedShadows;
            return shadows ? shadows->GetTrace(atlas) : nullptr;
        };
        const auto liveCasterProof = [&](bool needsMesh)
        {
            for (auto atlas : {GDX12ShadowAtlas::Sun, GDX12ShadowAtlas::Local})
                if (const auto* trace = shadowTrace(atlas))
                    for (const auto& group : trace->Groups)
                        if (!group.RecordedGrass.empty() && (!needsMesh || (group.SkinnedDraws && group.MaskedDraws)))
                        {
                            std::cout << "P6 native shadow casters light=" << group.Light.EntityId << ":" << group.Light.Generation
                                << " skin=" << group.SkinnedDraws << " alpha=" << group.MaskedDraws << " grass=";
                            for (const auto id : group.RecordedGrass) std::cout << id << ',';
                            std::cout << '\n';
                            return true;
                        }
            return false;
        };
        const auto log = [&]
        {
            const auto water = pipeline._water->GetMGPUWaterDiagnostics();
            const auto cloud = pipeline._cloudEnvironment->GetDiagnostics();
            const auto cloudFinalize = render->GetLastLegacyReplacement(*pipeline._cloudEnvironment);
            const auto grass = pipeline._grassRoute->GetDiagnostics();
            const auto sun = render->GetLastLegacyReplacement(*pipeline._geometryJobs.at(3));
            const auto local = render->GetLastLegacyReplacement(*pipeline._geometryJobs.at(4));
            const auto ao = pipeline._passes->GetLastAmbientOcclusionFinalize();
            const auto fluid = render->GetLastLegacyReplacement(pipeline._water->Surface);
            const auto screen = render->GetLastScreenUIFinalize();
            std::cout << "P6 frame=" << frame << " AO=" << ao.Value()
                << " W=" << fluid.Value() << ":" << water.SolverInvocations << ":" << water.Status
                << " C=" << cloudFinalize.Value() << ":" << cloud.DisplayedInvocation << ":" << cloud.Submitted << ":" << int(cloud.Effective)
                << " G=" << grass.Sequence << ":" << grass.Acquire.Value() << ":" << grass.Tail.Value()
                << " S2=" << sun.Value() << "," << local.Value()
                << " U2=" << screen.Value() << "," << surface(0).Value() << "," << surface(1).Value() << '\n';
            if (expectSecondary)
            {
                bool shadowReady = sun.IsValid() && local.IsValid();
                if (sunOnly)
                {
                    const auto* trace = shadowTrace(GDX12ShadowAtlas::Sun);
                    shadowReady = sun.IsValid() && !local.IsValid() && trace && trace->Groups.size() == 1 &&
                        trace->Groups.front().ViewCount == 4;
                }
                else if (selectedPoint)
                {
                    const auto* trace = shadowTrace(GDX12ShadowAtlas::Local);
                    shadowReady = !sun.IsValid() && local.IsValid() && trace && trace->Groups.size() == 1 &&
                        trace->Groups.front().Light == pointKey && trace->Groups.front().ViewCount == 6;
                }
                return ao.IsValid() && fluid.IsValid() && water.SecondaryOwns && water.Exact &&
                    cloudFinalize.IsValid() && cloud.Effective == CrossAdapterEnvironment::Route::SecondaryCompute && cloud.DisplayedInvocation &&
                    grass.Version.IsValid() && grass.Exact && grass.Tail.IsValid() &&
                    shadowReady && screen.IsValid() && surface(0).IsValid() && surface(1).IsValid() &&
                    liveCasterProof(!selectedPoint);
            }
            if (ao.IsValid() || fluid.IsValid() || water.SecondaryOwns ||
                cloudFinalize.IsValid() || cloud.Effective == CrossAdapterEnvironment::Route::SecondaryCompute ||
                grass.Version.IsValid() || sun.IsValid() || local.IsValid() || screen.IsValid() ||
                surface(0).IsValid() || surface(1).IsValid())
                throw std::runtime_error("A P6 negative control issued a remote placement");
            return true;
        };
        if (sunOnly || selectedPoint)
        {
            Set(L"Render.AO.Resolution", "0");
            const auto atlas = sunOnly ? GDX12ShadowAtlas::Sun : GDX12ShadowAtlas::Local;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                step();
                const bool combined = log();
                checkQueues();
                if (!combined) continue;
                const auto* trace = shadowTrace(atlas);
                if (!trace || !trace->Finalize.IsReachedBy(devices.Primary->GetCommandQueue()->GetCompletionProgress()))
                    throw std::runtime_error("P6 scoped shadow restore did not complete");
                std::cout << "P6 integrated " << (sunOnly ? "Sun-only" : "selected-point")
                    << " passed all six production receipts with completed shadow restore " << trace->Finalize.Value() << '\n';
                return;
            }
            throw std::runtime_error("P6 scoped GBV did not observe all six routes on the same frame");
        }
        for (int half : {0, 1})
        {
            Set(L"Render.AO.Resolution", half ? "1" : "0");
            bool combined = false;
            for (int attempt = 0; attempt < (expectSecondary ? 20 : 3); ++attempt)
            {
                step();
                combined |= log();
                if (combined && attempt >= 2) break;
            }
            checkQueues();
            if (!pipeline._grass->PatchCount()) throw std::runtime_error("P6 fixture requires live grass patches");
            if (!combined) throw std::runtime_error("P6 did not observe all six routes on the same frame");
        }

        const auto save = [&](const std::string& name, const std::vector<BYTE>& pixels)
        {
            std::filesystem::create_directories(readback);
            std::ofstream output(readback / name, std::ios::binary);
            output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (!output) throw std::runtime_error("Cannot save P6 readback: " + name);
        };
        // Capture the same frame's two material surfaces and the rendered scene that samples them.
        const auto capture = [&](GDX12Texture& texture, UINT pixelBytes, const std::string& name, const char* extension)
        {
            const auto pixels = CaptureTexture(texture, pixelBytes);
            const auto desc = texture.GetResource()->D3DResource->GetDesc();
            if (pixels.empty() || std::none_of(pixels.begin(), pixels.end(), [](BYTE channel) { return channel != 0; }))
                throw std::runtime_error("P6 surface has no rasterized content: " + name);
            save(name + "-" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + extension, pixels);
        };
        for (size_t index = 0; index < BistroUIPosters::Count; ++index)
            capture(*pipeline._posters->Output(index).GetTexture(), 4, "poster-" + std::to_string(index), ".rgba");
        const auto captureHDR = [&](GDX12Texture& texture, const char* name)
        {
            if (texture.GetResource()->D3DResource->GetDesc().Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
                throw std::runtime_error("P6 HDR attribution requires RGBA16F storage");
            capture(texture, 8, name, ".rgba16f");
        };
        const auto finalHDR = [&] { return render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture(); };
        captureHDR(*finalHDR(), "bistro-final");
        if (!requested)
        {
            // Keep the two intermediate textures alive for readback after the frame tail.
            Set(L"Render.Graph.ReuseTextures", "false");
            step();
            checkQueues();
            auto* composition = pipeline._passes->Composition.GetOutput(0)->GetTexture();
            auto* dof = pipeline._passes->DepthOfField.GetOutput(0)->GetTexture();
            if (composition->GetResource()->D3DResource.Get() == dof->GetResource()->D3DResource.Get() ||
                composition->GetResource()->D3DResource.Get() == finalHDR()->GetResource()->D3DResource.Get() ||
                dof->GetResource()->D3DResource.Get() == finalHDR()->GetResource()->D3DResource.Get())
                throw std::runtime_error("P6 HDR stages must have independent native textures");
            captureHDR(*composition, "bistro-no-reuse-composition");
            captureHDR(*dof, "bistro-no-reuse-dof");
            captureHDR(*finalHDR(), "bistro-no-reuse-final");
            Set(L"Render.Graph.ReuseTextures", "true");
        }
        checkQueues();
        if (!expectSecondary)
        {
            using namespace Engine::UI;
            using GDX12Submission::SnapshotReadMode;
            const auto adapter = devices.Primary->GetSubmissionAdapterId();
            const auto expectUI = [&](CanvasId id, bool enabled, SnapshotReadMode mode, std::string_view reason)
            {
                const auto status = render->GetUIPlacementStatus(id);
                if (status.Requested != enabled || status.ActiveAdapter != adapter || status.Mode != mode ||
                    status.Fallback != reason || status.Displayed.IsValid())
                    throw std::runtime_error("P6 negative UI control reported the wrong requested placement or fallback");
            };
            const auto exact = SnapshotReadMode::ExactVersion;
            const auto latest = SnapshotReadMode::LatestCompleted;
            const auto startupReason = requested ? std::string_view(render->GetMGPUStatus().FallbackReason) :
                std::string_view("MGPU disabled");
            if (startupReason.empty()) throw std::runtime_error("P6 startup fallback has no reason");
            if (!requested) expectUI(CanvasId::Invalid, false, exact, {});
            Set(L"Render.UI.Offload", "true");
            Set(L"Render.UI.OffloadScope", "0");
            Set(L"Render.UI.Freshness", "1");
            step();
            log();
            checkQueues();
            expectUI(CanvasId::Invalid, true, exact, startupReason);
            expectUI(BistroUICanvases::Atrium, true, latest, startupReason);
            expectUI(BistroUICanvases::Terrace, true, latest, startupReason);
            expectUI(CanvasId{9999}, true, latest, "Unknown CanvasId");
            Set(L"Render.UI.OffloadScope", "1");
            Set(L"Render.UI.CanvasId", "1");
            expectUI(CanvasId::Invalid, false, exact, {});
            expectUI(BistroUICanvases::Atrium, true, latest, startupReason);
            expectUI(BistroUICanvases::Terrace, false, latest, {});
            std::cout << "P6 UI requested fallback and canvas scope: " << startupReason << '\n';
            return;
        }

        const auto specificKey = pointKey;
        Set(L"Render.Shadows.MultiGPU.Entity", std::to_string(specificKey.EntityId).c_str());
        const auto compareTransport = [&](GDX12ShadowAtlas atlas, GDX12Texture& primary)
        {
            const auto* trace = shadowTrace(atlas);
            if (!trace || !trace->SecondaryDepth) throw std::runtime_error("P6 lacks a retained shadow transport receipt");
            std::map<UINT, std::vector<BYTE>> original, compact;
            const auto sample = [](GDX12Resource& resource, UINT slice, auto& cache) -> const std::vector<BYTE>&
            {
                auto found = cache.find(slice);
                if (found == cache.end()) found = cache.emplace(slice, CaptureTexture(resource, sizeof(float), slice)).first;
                return found->second;
            };
            const UINT strideA = static_cast<UINT>(primary.GetResource()->D3DResource->GetDesc().Width);
            const UINT strideB = static_cast<UINT>(trace->SecondaryDepth->D3DResource->GetDesc().Width);
            size_t compared = 0;
            for (const auto& group : trace->Groups)
            {
                const auto count = group.Kind == GDX12ShadowOutput::Kind::Sun ? 4u :
                    group.Kind == GDX12ShadowOutput::Kind::Point ? 6u : 1u;
                if (group.ViewCount != count) throw std::runtime_error("P6 transported an incomplete shadow group");
                std::cout << "P6 transported shadow " << group.Light.EntityId << ":" << group.Light.Generation << " slices";
                for (UINT face = 0; face < count; ++face)
                {
                    const auto a = group.PrimaryViews[face], b = group.CompactViews[face];
                    const auto& pixelsA = sample(*primary.GetResource(), a.Slice, original);
                    const auto& pixelsB = sample(*trace->SecondaryDepth, b.Slice, compact);
                    if (a.Width != b.Width || a.Height != b.Height)
                        throw std::runtime_error("P6 shadow remap changed the depth rectangle");
                    for (UINT y = 0; y < a.Height; ++y)
                    {
                        const auto offsetA = (size_t(a.Y + y) * strideA + a.X) * sizeof(float);
                        const auto offsetB = (size_t(b.Y + y) * strideB + b.X) * sizeof(float);
                        if (std::memcmp(pixelsA.data() + offsetA, pixelsB.data() + offsetB, a.Width * sizeof(float)) != 0)
                            throw std::runtime_error("P6 restored depth differs from the actual secondary raster");
                    }
                    compared += size_t(a.Width) * a.Height;
                    std::cout << ' ' << b.Slice << "->" << a.Slice;
                }
                std::cout << '\n';
            }
            std::cout << "P6 shadow transport bitwise pixels=" << compared << '\n';
        };
        for (int scope : {1, 2, 3})
        {
            Set(L"Render.Shadows.MultiGPU.Selection", std::to_string(scope).c_str());
            bool matched = false;
            for (int attempt = 0; attempt < 4 && !matched; ++attempt)
            {
                step();
                const auto sun = render->GetLastLegacyReplacement(*pipeline._geometryJobs.at(3));
                const auto local = render->GetLastLegacyReplacement(*pipeline._geometryJobs.at(4));
                matched = sun.IsValid() == (scope == 1) && local.IsValid() == (scope != 1);
            }
            if (!matched) throw std::runtime_error("P6 shadow scope did not issue the expected atlas receipt");
            const auto atlas = scope == 1 ? GDX12ShadowAtlas::Sun : GDX12ShadowAtlas::Local;
            const auto* trace = shadowTrace(atlas);
            if (!trace || trace->Groups.empty() || (scope == 3 &&
                (trace->Groups.size() != 1 || !(trace->Groups.front().Light == specificKey))))
                throw std::runtime_error("P6 shadow route transported a different stable light");
            if (scope == 1 || scope == 3)
            {
                checkQueues();
                auto* primary = (scope == 1 ? pipeline._passes->SunShadow.GetOutput(0) :
                    pipeline._passes->LocalShadow.GetOutput(0))->GetTexture();
                compareTransport(atlas, *primary);
            }
        }
        world.SetPaused(true);
        const auto grassHz = std::to_string(GetGrassMGPUControls().UpdateHz);
        Set(L"Render.Grass.MultiGPU.UpdateHz", "0.001");
        Set(L"Render.Shadows.MultiGPU", "false");
        for (int attempt = 0; attempt < 3; ++attempt) step();
        checkQueues();
        struct FrozenSunInputs
        {
            std::vector<uint64_t> Casters, Skin, FieldLayout;
            std::vector<BYTE> FieldBytes;
            GDX12Submission::TransferVersion FieldVersion;
            uint64_t Light = 14695981039346656037ull;
        };
        const auto mix = [](uint64_t& hash, const auto& value)
        {
            for (size_t i = 0; i < sizeof(value); ++i)
                hash = (hash ^ reinterpret_cast<const unsigned char*>(&value)[i]) * 1099511628211ull;
        };
        const auto captureInputs = [&](const char* route, const CrossAdapterSceneReplica::FrameStamp& stamp)
        {
            FrozenSunInputs input;
            const auto& primary = render->_primaryResources;
            auto& slot = *render->GetCurrentPrimaryFrameConstants();
            const auto* materialTable = slot.MaterialCache->GetCPUData();
            const auto* transforms = slot.TransformCache->GetCPUData();
            primary.Primitives.ForEach([&](auto handle, const auto& primitive)
            {
                if (!primitive.Visible || !primitive.CastsShadows) return;
                uint64_t caster = 14695981039346656037ull;
                mix(caster, handle.Index);
                mix(caster, handle.Generation);
                mix(caster, primitive.GeometryRevision);
                mix(caster, primitive.TransformRevision);
                mix(caster, primitive.DeformationRevision);
                mix(caster, primitive.Bounds.Center);
                mix(caster, primitive.Bounds.Extents);
                mix(caster, primitive.InstanceCount);
                if (!primitive.Source)
                {
                    if (primitive.TransformIndex >= slot.TransformCache->GetElementCount())
                        throw std::runtime_error("P6 frozen Sun caster has no primary transform");
                    mix(caster, transforms[primitive.TransformIndex].WorldMatrix);
                }
                if (primitive.MaterialIndex >= slot.MaterialCache->GetElementCount())
                    throw std::runtime_error("P6 frozen Sun caster has no primary material");
                const auto& material = materialTable[primitive.MaterialIndex];
                mix(caster, material.RenderLayer);
                // Only the masked depth shader samples material textures.
                if (material.RenderLayer == static_cast<UINT>(MaterialType::Masked))
                {
                    mix(caster, material.DiffuseIndex);
                    mix(caster, material.Opacity);
                    mix(caster, primitive.MaterialRevision);
                }
                input.Casters.push_back(caster);
            });
            std::sort(input.Casters.begin(), input.Casters.end());
            for (const auto& [key, state] : render->_skeletalStates)
                for (const auto& part : state.Parts)
                {
                    uint64_t skin = state.DeformationRevision;
                    mix(skin, part.Mesh);
                    mix(skin, part.MeshRevision);
                    mix(skin, part.SubMesh);
                    for (const auto& bone : part.Palette) mix(skin, bone.Current);
                    input.Skin.push_back(skin);
                }
            std::sort(input.Skin.begin(), input.Skin.end());
            mix(input.Light, primary.Lighting.LightDirection);
            mix(input.Light, primary.Lighting.SunShadowFlags);
            mix(input.Light, primary.Lighting.SunShadowRevision);
            for (const auto& viewProj : primary.ShadowSettings.ViewProj) mix(input.Light, viewProj);

            const auto& field = pipeline._grassRoute->GetDiagnostics();
            const auto& snapshot = pipeline._grass->_gpu->CurrentFrame().Snapshots.Current;
            if (!field.Version.IsValid() || !snapshot)
                throw std::runtime_error("P6 frozen Sun requires a real displayed remote grass field");
            input.FieldVersion = field.Version;
            const auto& layout = snapshot->GetLayout();
            uint64_t interaction = layout.InteractionRevision;
            mix(interaction, layout.InteractionResetRevision);
            mix(interaction, layout.ActorRevision);
            mix(interaction, layout.InteractionTimeSeconds);
            mix(interaction, layout.InteractionArea);
            input.FieldLayout.push_back(interaction);
            for (const auto& wind : layout.Wind)
            {
                uint64_t revision = wind.Owner;
                mix(revision, wind.SettingsRevision);
                mix(revision, wind.TimeSeconds);
                input.FieldLayout.push_back(revision);
            }
            input.FieldBytes = CaptureFieldBuffer(*devices.Primary, *snapshot->Presence().Resource);
            save(std::string("sun-grass-") + route + ".bin", input.FieldBytes);
            std::cout << "P6 frozen Sun route=" << route << " frame=" << static_cast<uint64_t>(stamp.Frame)
                << " invocation=" << static_cast<uint64_t>(stamp.Invocation) << " scene=" << stamp.SceneRevision
                << " casters=" << input.Casters.size() << " skin=" << input.Skin.size() << " light=" << input.Light
                << " grass=" << static_cast<uint64_t>(field.Version.VersionEpoch) << ':'
                << static_cast<uint64_t>(field.Version.Invocation) << ':' << field.Sequence
                << " interactionTime=" << layout.InteractionTimeSeconds << " windTimes=";
            for (const auto& wind : layout.Wind) std::cout << wind.TimeSeconds << ',';
            std::cout << '\n';
            return input;
        };
        auto* sunTexture = pipeline._passes->SunShadow.GetOutput(0)->GetTexture();
        const auto sunDesc = sunTexture->GetResource()->D3DResource->GetDesc();
        const auto extent = std::to_string(sunDesc.Width) + "x" + std::to_string(sunDesc.Height);
        const auto legacySun = CaptureTexture(*sunTexture, sizeof(float));
        save("sun-primary-" + extent + ".depth32f", legacySun);
        const auto legacyInputs = captureInputs("legacy",
            {render->GetFrameSubmissionId(), {}, render->_primaryResources.GeometryRevision});
        Set(L"Render.Shadows.MultiGPU.Selection", "1");
        Set(L"Render.Shadows.MultiGPU", "true");
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            step();
            if (shadowTrace(GDX12ShadowAtlas::Sun)) break;
        }
        if (!shadowTrace(GDX12ShadowAtlas::Sun)) throw std::runtime_error("P6 frozen shadow frame did not offload");
        checkQueues();
        const auto routedSun = CaptureTexture(*sunTexture, sizeof(float));
        save("sun-placed-" + extent + ".depth32f", routedSun);
        const auto placedInputs = captureInputs("placed", shadowTrace(GDX12ShadowAtlas::Sun)->Stamp);
        Set(L"Render.Grass.MultiGPU.UpdateHz", grassHz.c_str());
        const bool sameCasters = legacyInputs.Casters == placedInputs.Casters;
        const bool sameSkin = !legacyInputs.Skin.empty() && legacyInputs.Skin == placedInputs.Skin;
        const bool sameGrass = legacyInputs.FieldVersion == placedInputs.FieldVersion &&
            legacyInputs.FieldLayout == placedInputs.FieldLayout && legacyInputs.FieldBytes == placedInputs.FieldBytes;
        const bool sameLight = legacyInputs.Light == placedInputs.Light;
        std::cout << "P6 frozen Sun inputs casters=" << sameCasters << " skin=" << sameSkin
            << " grass=" << sameGrass << " light=" << sameLight << '\n';
        if (!sameCasters || !sameSkin || !sameGrass || !sameLight)
            throw std::runtime_error("P6 Sun draw inputs changed between the frozen depth samples");
        if (legacySun.size() != routedSun.size()) throw std::runtime_error("P6 sun atlas extent changed with placement");
        size_t covered = 0, overlap = 0, differing = 0, aboveTolerance = 0, edgeOutliers = 0, smoothOutliers = 0;
        double sumError = 0, maxError = 0;
        const auto nearDepthEdge = [&](size_t byteOffset, float a, float b)
        {
            constexpr int cascade = SunShadowCascadeResolution;
            const auto pixel = byteOffset / sizeof(float);
            const int width = static_cast<int>(sunDesc.Width);
            const int x = static_cast<int>(pixel % width), y = static_cast<int>(pixel / width);
            const int left = (x / cascade) * cascade, top = (y / cascade) * cascade;
            for (int row = (std::max)(top, y - 1); row <= (std::min)(top + cascade - 1, y + 1); ++row)
                for (int column = (std::max)(left, x - 1); column <= (std::min)(left + cascade - 1, x + 1); ++column)
                {
                    const size_t neighbor = (size_t(row) * width + column) * sizeof(float);
                    if (std::abs(ReadFloatPixel(legacySun, neighbor) - a) > 1.0e-3f ||
                        std::abs(ReadFloatPixel(routedSun, neighbor) - b) > 1.0e-3f)
                        return true;
                }
            return false;
        };
        for (size_t i = 0; i < legacySun.size(); i += sizeof(float))
        {
            const float a = ReadFloatPixel(legacySun, i), b = ReadFloatPixel(routedSun, i);
            if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || b < 0 || a > 1 || b > 1)
                throw std::runtime_error("P6 shadow depth outside [0,1]");
            if (a == 1 && b == 1) continue;
            ++covered;
            overlap += a < 1 && b < 1;
            differing += a != b;
            const double error = std::abs(double(a) - b);
            sumError += error;
            maxError = (std::max)(maxError, error);
            aboveTolerance += error > 1.0e-4;
            if (error > 1.0e-3)
            {
                if (nearDepthEdge(i, a, b)) ++edgeOutliers;
                else ++smoothOutliers;
            }
        }
        if (!covered) throw std::runtime_error("P6 frozen Sun depth has no coverage");
        const double meanError = sumError / covered;
        std::cout << "P6 frozen legacy/secondary sun coverage=" << covered << " overlap=" << overlap
            << " changed=" << differing << " mean=" << meanError << " max=" << maxError
            << " over1e-4=" << aboveTolerance << " edgeOutliers=" << edgeOutliers
            << " smoothOutliers=" << smoothOutliers << '\n';
        // Tested fixture tolerance for depth rasterization across the two adapters.
        if (covered != overlap || meanError > 1.0e-5 || aboveTolerance > covered / 1000 || smoothOutliers)
            throw std::runtime_error("P6 frozen legacy and placed Sun depth exceeded the fixture tolerance");
        world.SetPaused(false);
        Set(L"Render.Shadows.MultiGPU.Selection", "0");

#ifdef _DEBUG
        const auto beforeCollision = pipeline._water->GetMGPUWaterDiagnostics().SolverInvocations;
        BistroRenderPipeline::ToggleCollisionView();
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            step();
            if (render->GetLastLegacyReplacement(pipeline._water->Surface).IsValid())
                throw std::runtime_error("P6 collision-only view issued a Water Surface reader");
        }
        if (pipeline._water->GetMGPUWaterDiagnostics().SolverInvocations <= beforeCollision)
            throw std::runtime_error("P6 hidden water solver stopped advancing in Collision View");
        BistroRenderPipeline::ToggleCollisionView();
        bool resumedWater = false;
        for (int attempt = 0; attempt < 8 && !resumedWater; ++attempt)
        {
            step();
            resumedWater = render->GetLastLegacyReplacement(pipeline._water->Surface).IsValid();
        }
        if (!resumedWater) throw std::runtime_error("P6 Water Surface failed to resume after Collision View");
#endif

        const auto secondaryAdapter = devices.Secondary->GetSubmissionAdapterId();
        for (UINT index = 0; index < BistroUIPosters::Count; ++index)
        {
            Set(L"Render.UI.OffloadScope", "1");
            Set(L"Render.UI.CanvasId", std::to_string(index + 1).c_str());
            Set(L"Render.UI.Quality", index ? "1" : "0");
            step();
            const auto selected = render->GetUIPlacementStatus(static_cast<Engine::UI::CanvasId>(index + 1));
            if (!selected.Displayed.IsValid() || selected.ActiveAdapter != secondaryAdapter ||
                !surface(index).IsValid() || surface(1 - index).IsValid() || render->GetLastScreenUIFinalize().IsValid())
                throw std::runtime_error("P6 canvas scope did not keep the other two UI surfaces local");
        }
        Set(L"Render.UI.OffloadScope", "0");
        Set(L"Render.UI.Freshness", "1");
        bool latestPosters = false;
        for (int attempt = 0; attempt < 12 && !latestPosters; ++attempt)
        {
            step();
            const auto atrium = render->GetUIPlacementStatus(BistroUICanvases::Atrium);
            const auto terrace = render->GetUIPlacementStatus(BistroUICanvases::Terrace);
            latestPosters = atrium.ActiveAdapter == secondaryAdapter && terrace.ActiveAdapter == secondaryAdapter &&
                atrium.Mode == GDX12Submission::SnapshotReadMode::LatestCompleted &&
                terrace.Mode == GDX12Submission::SnapshotReadMode::LatestCompleted &&
                atrium.Displayed.IsValid() && terrace.Displayed.IsValid() && render->GetLastScreenUIFinalize().IsValid();
        }
        if (!latestPosters) throw std::runtime_error("P6 did not observe both live Latest posters and the Exact screen");

        const auto backBufferDesc = [&] { return render->_backBuffer->GetCurrentBuffer()->GetResource()->D3DResource->GetDesc(); };
        const auto beforeResize = backBufferDesc();
        const auto beforeResizeScreen = render->GetUIPlacementStatus(Engine::UI::CanvasId::Invalid).Displayed;
        const auto pendingAtResize = render->GetMGPURuntime()->PendingCount();
        if (!SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            throw std::runtime_error("P6 could not request an odd-size window resize");
        for (int attempt = 0; attempt < 3; ++attempt) step();
        checkQueues();
        const auto afterResize = backBufferDesc();
        const auto afterResizeScreen = render->GetUIPlacementStatus(Engine::UI::CanvasId::Invalid).Displayed;
        if (!render->GetLastScreenUIFinalize().IsValid() || !beforeResizeScreen.IsValid() || !afterResizeScreen.IsValid() ||
            (beforeResize.Width == afterResize.Width && beforeResize.Height == afterResize.Height) ||
            beforeResizeScreen.VersionEpoch == afterResizeScreen.VersionEpoch)
            throw std::runtime_error("P6 screen placement did not resume in a new layout epoch after swapchain resize");
        std::cout << "P6 UI resized " << beforeResize.Width << 'x' << beforeResize.Height << " -> "
            << afterResize.Width << 'x' << afterResize.Height << " pending plans=" << pendingAtResize
            << " screen epoch " << static_cast<uint64_t>(beforeResizeScreen.VersionEpoch) << " -> "
            << static_cast<uint64_t>(afterResizeScreen.VersionEpoch) << '\n';

        for (auto* setting : {L"Render.AO.MultiGPU", L"Render.Clouds.MultiGPU", L"Render.UI.Offload", L"Render.Fluid.MultiGPU"})
            Set(setting, "false");
        Set(L"Render.Grass.AsyncCompute", "true");
        bool legacyAsyncField = false;
        for (int attempt = 0; attempt < 8 && !legacyAsyncField; ++attempt)
        {
            step();
            legacyAsyncField = render->GetRenderGraphStatistics().AsyncPasses != 0 &&
                pipeline._grassRoute->GetDiagnostics().Version.IsValid() &&
                pipeline._grassShadows->GetDiagnostics().SubmittedAtlases == 2 && liveCasterProof(false);
        }
        if (!legacyAsyncField) throw std::runtime_error("P6 could not stage the G2 field after legacy COMPUTE");
        const auto oldRemoteField = pipeline._grassShadows->GetDiagnostics();
        checkQueues();
        Set(L"Render.Grass.AsyncCompute", "false");
        Set(L"App.Grass.Wind.GridResolution", "24");
        bool remoteLayoutChanged = false;
        for (int attempt = 0; attempt < 18 && !remoteLayoutChanged; ++attempt)
        {
            step();
            const auto& field = pipeline._grassShadows->GetDiagnostics();
            const auto& producer = pipeline._grassRoute->GetDiagnostics();
            std::cout << "P6 grass layout frame=" << frame << " G=" << static_cast<uint64_t>(producer.Version.VersionEpoch)
                << ":" << producer.Sequence << ":" << producer.LocalAuthoritative
                << " G2=" << static_cast<uint64_t>(field.Field.VersionEpoch) << ":" << field.Bytes
                << ":" << field.PreparedAtlases << ":" << field.SubmittedAtlases << '\n';
            remoteLayoutChanged = oldRemoteField.Field.IsValid() && field.Field.IsValid() &&
                field.Field.VersionEpoch != oldRemoteField.Field.VersionEpoch && field.Bytes != oldRemoteField.Bytes &&
                pipeline._grassRoute->GetDiagnostics().Version.IsValid() && liveCasterProof(false);
        }
        if (!remoteLayoutChanged) throw std::runtime_error("P6 upstream G and both G2 atlas slots did not retire across the field layout change");


        Set(L"Render.Grass.MultiGPU", "false");
        bool primaryGrassShadows = false;
        for (int attempt = 0; attempt < 20 && !primaryGrassShadows; ++attempt)
        {
            step();
            primaryGrassShadows = pipeline._grassRoute->GetDiagnostics().LocalAuthoritative && liveCasterProof(false);
        }
        if (!primaryGrassShadows)
            throw std::runtime_error("P6 secondary shadow raster did not record grass from the authoritative primary field");

        const auto oldPrimaryField = pipeline._grassShadows->GetDiagnostics();
        checkQueues();
        Set(L"App.Grass.Wind.GridResolution", "16");
        bool primaryLayoutChanged = false;
        for (int attempt = 0; attempt < 12 && !primaryLayoutChanged; ++attempt)
        {
            step();
            const auto& field = pipeline._grassShadows->GetDiagnostics();
            primaryLayoutChanged = oldPrimaryField.Field.IsValid() && field.Field.IsValid() &&
                field.Field.VersionEpoch != oldPrimaryField.Field.VersionEpoch && field.Bytes != oldPrimaryField.Bytes &&
                pipeline._grassRoute->GetDiagnostics().LocalAuthoritative && liveCasterProof(false);
        }
        if (!primaryLayoutChanged) throw std::runtime_error("P6 retired local grass raster blocked a G2 layout change");

        world.GetECS().ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid) { fluid.Reset(); });
        Set(L"Render.Shadows.MultiGPU.Selection", "3");
        const auto requireLiveGrassShadow = [&](GDX12LocalShadowKey light, UINT faces)
        {
            Set(L"Render.Shadows.MultiGPU.Entity", std::to_string(light.EntityId).c_str());
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                step();
                checkQueues();
                const auto* trace = shadowTrace(GDX12ShadowAtlas::Local);
                if (trace && trace->Groups.size() == 1 && trace->Groups.front().Light == light &&
                    trace->Groups.front().ViewCount == faces && !trace->Groups.front().RecordedGrass.empty())
                    return;
            }
            throw std::runtime_error("P6 live local light had no actual grass shadow");
        };
        requireLiveGrassShadow(spotKey, 1);
        requireLiveGrassShadow(pointKey, 6);
        std::cout << "P6 live Local atlas changed from one Spot layer to six Point layers with native grass\n";

        world.SetPaused(true);
        Set(L"Render.Shadows.MultiGPU.Entity", std::to_string(spotKey.EntityId).c_str());
        const auto spotPixels = [&](bool grassCasts)
        {
            Set(L"App.Grass.CastShadows", grassCasts ? "true" : "false");
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                step();
                checkQueues();
                const auto* trace = shadowTrace(GDX12ShadowAtlas::Local);
                if (!trace || trace->Groups.size() != 1 || !(trace->Groups.front().Light == spotKey) ||
                    trace->Groups.front().RecordedGrass.empty() == grassCasts)
                    continue;
                const auto slice = trace->Groups.front().PrimaryViews[0].Slice;
                return CaptureTexture(*pipeline._passes->LocalShadow.GetOutput(0)->GetTexture()->GetResource(), sizeof(float), slice);
            }
            throw std::runtime_error("P6 could not capture the matched spot depth pair");
        };
        const auto grassDepth = spotPixels(true), emptyGrassDepth = spotPixels(false);
        if (grassDepth.size() != emptyGrassDepth.size()) throw std::runtime_error("P6 spot depth extent changed");
        size_t grassPixels = 0;
        for (size_t offset = 0; offset < grassDepth.size(); offset += sizeof(float))
            grassPixels += ReadFloatPixel(grassDepth, offset) < ReadFloatPixel(emptyGrassDepth, offset);
        if (!grassPixels) throw std::runtime_error("P6 native grass draw produced no nearer depth pixels");
        std::cout << "P6 masked native grass affected spot depth pixels=" << grassPixels << '\n';
        Set(L"App.Grass.CastShadows", "true");
        world.SetPaused(false);

        // Isolate cloud Latest from strict techniques sharing the secondary COMPUTE FIFO.
        for (auto* setting : {L"Render.AO.MultiGPU", L"Render.Shadows.MultiGPU", L"Render.Grass.MultiGPU", L"Render.UI.Offload"})
            Set(setting, "false");
        Set(L"Render.Fluid.MultiGPU", "false");
        world.GetECS().ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid) { fluid.Reset(); });
        Set(L"Render.Clouds.MultiGPU", "true");
        Set(L"Render.Clouds.Latest", "true");
        Set(L"Render.Clouds.MaxAgeMs", "1");
        uint64_t displayed = 0;
        for (int attempt = 0; attempt < 16 && !displayed; ++attempt)
        {
            step();
            displayed = pipeline._cloudEnvironment->GetDiagnostics().DisplayedInvocation;
        }
        if (!displayed) throw std::runtime_error("P6 cloud Latest never published a compatible field");
        auto* compute = devices.Secondary->TryGetComputeQueue();
        WithQueueHeld(*devices.Secondary, *compute, [&](ID3D12Fence& gate)
        {
            auto& primary = *devices.Primary->GetCommandQueue();
            const auto prior = primary.GetLastCompletionPoint();
            const auto beforeProducer = compute->GetLastCompletionPoint();
            auto previous = pipeline._cloudEnvironment->GetDiagnostics();
            int repeats = 0;
            double firstAge = 0;
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                step();
                const auto state = pipeline._cloudEnvironment->GetDiagnostics();
                if (state.DisplayedInvocation < previous.DisplayedInvocation)
                    throw std::runtime_error("P6 cloud Latest display went backwards during the producer hold");
                // An already-produced publication can finish importing after the compute gate closes.
                if (state.DisplayedInvocation == previous.DisplayedInvocation && state.Repeats > previous.Repeats &&
                    state.DisplayAge.OverAge && state.DisplayAge.Milliseconds > previous.DisplayAge.Milliseconds)
                {
                    if (repeats == 0) firstAge = previous.DisplayAge.Milliseconds;
                    ++repeats;
                }
                else repeats = 0;
                previous = state;
                if (repeats >= 2 && compute->GetLastCompletionPoint().Value() > beforeProducer.Value()) break;
            }
            const auto tail = primary.GetLastCompletionPoint();
            const auto producer = compute->GetLastCompletionPoint();
            primary.Flush();
            const auto pending = render->GetMGPURuntime()->PendingCount();
            std::cout << "P6 delayed Latest invocation=" << previous.DisplayedInvocation << " repeats=" << repeats
                << " age ms " << firstAge << " -> " << previous.DisplayAge.Milliseconds
                << " primary=" << prior.Value() << " -> " << tail.Value()
                << " producer=" << beforeProducer.Value() << " -> " << producer.Value() << " pending=" << pending << '\n';
            if (gate.GetCompletedValue() != 0 || tail.Value() <= prior.Value() ||
                !tail.IsReachedBy(primary.GetCompletionProgress()) || repeats < 2 ||
                producer.Value() <= beforeProducer.Value() || producer.IsReachedBy(compute->GetCompletionProgress()) ||
                !pending || pending > 16)
                throw std::runtime_error("P6 delayed Latest did not retain completed cloud output with primary progress");
        });
        compute->Flush();
        checkQueues();

        // Exercise the same world loader and indirect-template step used at App startup.
        Set(L"Render.Clouds.MultiGPU", "false");
        Set(L"Render.UI.Offload", "true");
        Set(L"Render.UI.OffloadScope", "0");
        Set(L"Render.UI.Freshness", "0");
        auto scenes = Locator.GetModule<SceneManagerModule>();
        const auto bistroPath = world.GetPath();
        if (!scenes->UnloadWorld(0) || !scenes->LoadWorld(bistroPath))
            throw std::runtime_error("P6 could not unload and reload the authored Bistro world");
        auto& reloaded = *scenes->GetWorld();
        _indirect.Schedule(GetJobs(), reloaded, 0).Complete();
        render->WaitForUploads();
        auto& view = reloaded.GetECS().Get<TransformComponent>(reloaded.ActiveCamera);
        view.Location = Vector3(-300, 200, 20);
        view.Rotation = Vector3(0, 117, 0);
        view.DirtyFlag = true;
        for (int attempt = 0; attempt < 3; ++attempt) Frame(dt, ++frame * dt);
        checkQueues();
        size_t posters = 0;
        reloaded.GetECS().ForEach<NameComponent, StaticMeshRenderComponent>(
            [&](Entity, const NameComponent& name, const StaticMeshRenderComponent&)
            { posters += name.value == "BistroUIPosterAtrium" || name.value == "BistroUIPosterTerrace"; });
        if (posters != 2 || !surface(0).IsValid() || !surface(1).IsValid() || !render->GetLastScreenUIFinalize().IsValid())
            throw std::runtime_error("P6 Bistro reload did not recreate and rebind both actual UI posters");
        std::cout << "P6 reloaded Bistro rebound two poster canvases and the screen canvas\n";
    }

    void CheckCloudLatestSoak(CrossAdapterEnvironment& cloud, RenderMGPURuntime& runtime,
        GDX12Device& primaryDevice, GDX12Device& secondaryDevice, const std::function<void()>& step,
        ID3D12InfoQueue& primaryInfo, ID3D12InfoQueue& secondaryInfo)
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        using namespace GDX12Submission;
        auto& primary = *primaryDevice.GetCommandQueue();
        auto* producer = secondaryDevice.TryGetComputeQueue();
        if (!producer) throw std::runtime_error("P6 Cloud Latest soak requires secondary COMPUTE");
        Set(L"Render.Clouds.Bistro", "true");
        Set(L"Render.Clouds.MultiGPU", "true");
        Set(L"Render.Clouds.Latest", "true");
        Set(L"Render.Clouds.MaxAgeMs", "1");

        for (int attempt = 0; attempt < 16 && !cloud.GetDiagnostics().DisplayedInvocation; ++attempt) step();
        if (!cloud.GetDiagnostics().DisplayedInvocation)
            throw std::runtime_error("P6 Cloud Latest soak could not install a completed snapshot");

        std::array<ComPtr<IDXGIAdapter3>, 2> adapters;
        auto factory = GDX12DeviceFactory::GetFactory();
        ThrowIfFailed(factory->EnumAdapterByLuid(primaryDevice.GetAdapterLuid(), IID_PPV_ARGS(&adapters[0])));
        ThrowIfFailed(factory->EnumAdapterByLuid(secondaryDevice.GetAdapterLuid(), IID_PPV_ARGS(&adapters[1])));
        struct Sample { DWORD Handles = 0; std::array<UINT64, 2> LocalBytes{}; };
        const auto sample = [&]
        {
            Sample result;
            if (!GetProcessHandleCount(GetCurrentProcess(), &result.Handles))
                throw std::runtime_error("P6 soak could not read process handles");
            for (size_t i = 0; i < adapters.size(); ++i)
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                ThrowIfFailed(adapters[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
                result.LocalBytes[i] = info.CurrentUsage;
            }
            CheckMessages(primaryInfo);
            CheckMessages(secondaryInfo);
            return result;
        };

        std::cout << "P6 Cloud Latest soak warmup 30 s\n";
        const auto warmed = Clock::now() + 30s;
        while (Clock::now() < warmed) step();
        const auto baseline = sample();
        const auto start = Clock::now();
        const auto epoch = cloud.GetDiagnostics().EpochResets;
        auto displayed = cloud.GetDiagnostics().DisplayedInvocation;
        std::array<size_t, 5> maxima{};
        auto progress = primary.GetCompletionProgress().CompletedValue;
        unsigned cycles = 0;
        const auto observe = [&]
        {
            step();
            const auto state = cloud.GetDiagnostics();
            const auto transport = cloud.GetTransportStatus();
            const auto& usage = transport.Occupied;
            const auto pending = runtime.PendingCount();
            if (!transport.Active || runtime.IsStopped() || state.Effective != CrossAdapterEnvironment::Route::SecondaryCompute ||
                !state.DisplayedInvocation || state.DisplayedInvocation < displayed || state.EpochResets != epoch ||
                usage.Shared > GDX12Snapshot::SlotCount || usage.Local > GDX12Snapshot::SlotCount ||
                usage.Readers > 32 || usage.Publications > 2 * GDX12Snapshot::SlotCount || pending > 16)
                throw std::runtime_error("P6 Cloud Latest soak lost publication, progress order or a capacity bound");
            displayed = state.DisplayedInvocation;
            const std::array<size_t, 5> counts{usage.Shared, usage.Local, usage.Readers, usage.Publications, pending};
            for (size_t i = 0; i < counts.size(); ++i) maxima[i] = (std::max)(maxima[i], counts[i]);
            return state;
        };
        const auto record = [&](const Sample& measured)
        {
            std::cout << "P6 Cloud Latest soak seconds=" << std::chrono::duration<double>(Clock::now() - start).count()
                << " cycles=" << cycles << " display=" << displayed << " primaryCompleted=" << progress
                << " maxShared=" << maxima[0] << " maxLocal=" << maxima[1] << " maxReaders=" << maxima[2]
                << " maxPublications=" << maxima[3] << " maxPending=" << maxima[4]
                << " handles=" << measured.Handles << " primaryLocalBytes=" << measured.LocalBytes[0]
                << " secondaryLocalBytes=" << measured.LocalBytes[1] << '\n';
        };
        record(baseline);

        auto nextGate = start;
        auto nextSample = start + 15s;
        while (Clock::now() < start + 600s)
        {
            if (Clock::now() >= nextGate)
            {
                const auto ceiling = cloud.GetTransportStatus().LastReservedInvocation;
                WithQueueHeld(secondaryDevice, *producer, [&](ID3D12Fence& gate)
                {
                    const auto beforeFrame = primary.GetLastCompletionPoint();
                    const auto beforeProducer = producer->GetLastCompletionPoint();
                    auto previous = cloud.GetDiagnostics();
                    unsigned repeats = 0;
                    for (int attempt = 0; attempt < 8; ++attempt)
                    {
                        const auto state = observe();
                        if (state.DisplayedInvocation > ceiling)
                            throw std::runtime_error("P6 Cloud Latest displayed a publication reserved behind the gate");
                        const bool repeated = state.DisplayedInvocation == previous.DisplayedInvocation &&
                            state.Repeats > previous.Repeats && state.DisplayAge.OverAge &&
                            state.DisplayAge.Milliseconds > previous.DisplayAge.Milliseconds;
                        repeats = repeated ? repeats + 1 : 0;
                        previous = state;
                        if (repeats >= 2 && producer->GetLastCompletionPoint().Value() > beforeProducer.Value()) break;
                    }
                    const auto tail = primary.GetLastCompletionPoint();
                    const auto pending = producer->GetLastCompletionPoint();
                    while (gate.GetCompletedValue() == 0 && !tail.IsReachedBy(primary.GetCompletionProgress()))
                    {
                        ThrowIfFailed(primaryDevice.GetDevice()->GetDeviceRemovedReason());
                        Sleep(1);
                    }
                    if (gate.GetCompletedValue() != 0 || tail.Value() <= beforeFrame.Value() ||
                        repeats < 2 || pending.Value() <= beforeProducer.Value() ||
                        pending.IsReachedBy(producer->GetCompletionProgress()) || !runtime.PendingCount())
                        throw std::runtime_error("P6 Cloud Latest soak did not observe primary completion with a held producer");
                });
                for (int attempt = 0; attempt < 32 && displayed <= ceiling; ++attempt) observe();
                if (displayed <= ceiling) throw std::runtime_error("P6 Cloud Latest soak did not publish after release");
                ++cycles;
                nextGate += 10s;
            }
            else observe();

            if (Clock::now() >= nextSample)
            {
                const auto completed = primary.GetCompletionProgress().CompletedValue;
                if (completed <= progress) throw std::runtime_error("P6 Cloud Latest soak primary queue stopped progressing");
                progress = completed;
                const auto measured = sample();
                record(measured);
                if (measured.Handles > baseline.Handles + 8 ||
                    measured.LocalBytes[0] > baseline.LocalBytes[0] + 64ull * 1024 * 1024 ||
                    measured.LocalBytes[1] > baseline.LocalBytes[1] + 64ull * 1024 * 1024)
                    throw std::runtime_error("P6 Cloud Latest soak exceeded the warmed handle or local memory bound");
                nextSample += 15s;
            }
        }
        const auto final = sample();
        record(final);
        if (cycles < 30 || final.Handles > baseline.Handles + 2 ||
            final.LocalBytes[0] > baseline.LocalBytes[0] + 32ull * 1024 * 1024 ||
            final.LocalBytes[1] > baseline.LocalBytes[1] + 32ull * 1024 * 1024)
            throw std::runtime_error("P6 Cloud Latest soak failed its cycle or final resource bound");
        Set(L"Render.Clouds.MultiGPU", "false");
        step();
        producer->Flush();
        for (auto* device : { &secondaryDevice, &primaryDevice })
            runtime.GetQueue({device->GetSubmissionAdapterId(), QueueKind::Copy})->Flush();
        primary.Flush();
        runtime.Poll();
        if (runtime.PendingCount()) throw std::runtime_error("P6 Cloud Latest soak retained runtime work after drain");
        CheckMessages(primaryInfo);
        CheckMessages(secondaryInfo);
        std::cout << "P6 Cloud Latest soak passed with " << cycles << " hold/recover cycles and zero pending plans after drain\n";
    }

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
        if (!_staticRenderScene) ConfigureBistroProps(*scenes, GetJobs());
        Locator.RegisterModule(scenes);
        Locator.RegisterModule(std::make_shared<PhysicsModule>(_jobs));
        return BenchmarkEngine::AddModules();
    }

private:
    bool _staticRenderScene;
    BistroRenderPipeline* _bistroPipeline = nullptr;
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        constexpr std::string_view productionPrefix = "--mgpu-production-";
        const bool production = mode.substr(0, productionPrefix.size()) == productionPrefix;
        const bool productionGBV = production && mode.size() >= 4 && mode.substr(mode.size() - 4) == "-gbv";
        auto scenario = production ? mode.substr(productionPrefix.size()) : std::string_view{};
        if (productionGBV) scenario.remove_suffix(4);
        if (production && scenario != "default" && scenario != "unavailable" && scenario != "two-adapters")
            throw std::invalid_argument("Unknown P6 production scenario");
        const std::string_view variantOption = production && argc > 3 ? argv[3] : "";
        auto variant = MGPUProductionVariant::Full;
        if (variantOption == "--cloud-latest-soak") variant = MGPUProductionVariant::CloudSoak;
        else if (variantOption == "--shadow-sun-only") variant = MGPUProductionVariant::SunOnly;
        else if (variantOption == "--shadow-selected-point") variant = MGPUProductionVariant::SelectedPoint;
        else if (!variantOption.empty()) throw std::invalid_argument("Unknown P6 production fixture variant");
        if (variant != MGPUProductionVariant::Full && scenario != "two-adapters")
            throw std::invalid_argument("P6 production fixture variants require two physical adapters");
        const bool matrix = mode == "--matrix" || mode == "--matrix-validation" || mode == "--matrix-gpu-validation";
        const bool timing = mode == "--timing" || mode == "--grass-timing" || matrix;
        const bool strictAO = mode == "--ao-mgpu-strict-validation" || mode == "--ao-mgpu-strict-gpu-validation";
        const bool multiGPUAO = strictAO || mode == "--ao-mgpu-validation" || mode == "--ao-mgpu-gpu-validation";
#ifndef _DEBUG
        if (multiGPUAO || production) throw std::invalid_argument("Bistro placed AO validation requires a Debug build");
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
        if (productionGBV || mode == "--gpu-validation" || mode == "--matrix-gpu-validation" || mode == "--ao-placements-gpu-validation" ||
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
        if (aoMatrix || production) ConsoleModule{}.Initialize();
        BistroRenderGraphTests app(aoMatrix || production);
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
#ifdef _DEBUG
        if (mode == "--collision-view" || mode == "--collision-view-gpu-validation") app.CheckCollisionView();
        else
#endif
        if (production) app.CheckMGPUProduction(scenario != "default", scenario == "two-adapters",
            argc > 2 ? argv[2] : "Build/P6/Readback", variant);
        else if (aoMatrix) app.CheckAOPlacements(multiGPUAO, mode == "--ao-mgpu-validation", strictAO);
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
