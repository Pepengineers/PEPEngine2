#define main BistroAdditionsEntryForInstancingTest
#include "BistroAdditionsTests.cpp"
#undef main
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include <set>

static void CheckDebugMessages(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&info)));
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
    {
        SIZE_T bytes = 0;
        ThrowIfFailed(info->GetMessage(index, nullptr, &bytes));
        std::vector<BYTE> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info->GetMessage(index, message, &bytes));
        Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
    }
}

template<class T> static std::vector<T> ReadBuffer(GDX12Resource& source, size_t count)
{
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(source.D3DResource->GetDevice(IID_PPV_ARGS(&device)));
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(T));
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    D3D12_COMMAND_QUEUE_DESC queueDescription = {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)));
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    const D3D12_RESOURCE_BARRIER barrier = source.GetCopySourceBarrier();
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, source.D3DResource.Get(), 0, count * sizeof(T));
    ThrowIfFailed(commands->Close());
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "Cannot create buffer readback event");
    const auto result = fence->SetEventOnCompletion(1, event);
    const auto waited = SUCCEEDED(result) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    ThrowIfFailed(result);
    Require(waited == WAIT_OBJECT_0, "Instance buffer readback timed out");
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped));
    std::vector<T> values(count);
    std::memcpy(values.data(), mapped, count * sizeof(T));
    readback->Unmap(0, nullptr);
    return values;
}

template<class T> static void Upload(GDX12Device& device, GDX12UploadBuffer<T>& target, const std::vector<T>& values)
{
    GDX12UploadBuffer<T> staging(&device, static_cast<UINT>(values.size()), EBufferType::Upload, false);
    staging.CopyData(0, values.data(), static_cast<UINT>(values.size()));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({target.GetResource().GetCopyDestBarrier()});
    commands->GetCommandList()->CopyBufferRegion(target.GetResource().D3DResource.Get(), 0,
        staging.GetResource().D3DResource.Get(), 0, values.size() * sizeof(T));
    commands->ResourceBarrier({target.GetResource().GetNonPixelShaderResourceBarrier()});
    queue->ExecuteCommandList(commands);
    queue->Flush();
}

static std::vector<UINT> ReadVisibleIDs(GDX12Device& device, GDX12InstanceBatcher& batcher)
{
    const auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(&device,
        SOLUTION_DIR L"Apps/App/tests/shaders/ReadVisibleIDs.hlsl", nullptr, "CS", "cs");
    ComPtr<ID3DBlob> errors;
    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsUnorderedAccessView(0);
    const CD3DX12_ROOT_SIGNATURE_DESC rootDescription(2, parameters);
    ComPtr<ID3DBlob> serialized;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ComPtr<ID3D12RootSignature> root;
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription = {};
    pipelineDescription.pRootSignature = root.Get();
    pipelineDescription.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    ThrowIfFailed(device.GetDevice()->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)));
    GDX12UploadBuffer<UINT> output(&device, 5, EBufferType::Default, false);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({output.GetResource().GetUnorderedAccessBarrier()});
    auto* list = commands->GetCommandList().Get();
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootShaderResourceView(0, batcher.VisibleInstancesAddress());
    list->SetComputeRootUnorderedAccessView(1, output.GetElementAddress(0));
    list->Dispatch(1, 1, 1);
    commands->InvalidateCachedState();
    queue->ExecuteCommandList(commands);
    queue->Flush();
    return ReadBuffer<UINT>(output.GetResource(), 5);
}

static void CheckBatchCompaction()
{
    GDX12Device device;
    ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
    std::vector<GDX12InstanceData> instances(8);
    for (const UINT index : {0u, 3u, 7u}) { instances[index].SkinBatch = 0; instances[index].SkinBatchStart = 0; }
    for (const UINT index : {1u, 5u}) { instances[index].SkinBatch = 1; instances[index].SkinBatchStart = 3; }
    GDX12UploadBuffer<GDX12InstanceData> instanceBuffer(&device, 8, EBufferType::Default, false);
    Upload(device, instanceBuffer, instances);
    GDX12UploadBuffer<UINT> counters(&device, 4, EBufferType::Default, false);
    GDX12InstanceBatcher batcher(&device);
    for (const UINT capacity : {8u, 32u})
    {
        std::vector<GDX12IndirectDrawArgs> source(capacity);
        const UINT shuffled[] = {3, 2, 5, 0, 4, 7, 1};
        for (UINT index = 0; index < std::size(shuffled); ++index)
        {
            auto& command = source[index];
            command.InstanceID = shuffled[index];
            command.InstanceCount = 1;
            const auto batch = instances[command.InstanceID].SkinBatch;
            command.IndexCountPerInstance = batch == UINT_MAX ? 20 + command.InstanceID : 100 + batch;
            command.StartIndexLocation = batch == UINT_MAX ? command.InstanceID : 10 + batch;
        }
        GDX12UploadBuffer<GDX12IndirectDrawArgs> input(&device, capacity, EBufferType::Default, false);
        Upload(device, input, source);
        Upload(device, counters, std::vector<UINT>{11, 12, 7, 14});
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        batcher.Build(commands, instanceBuffer, input, counters, 2, 5, 2);
        queue->ExecuteCommandList(commands); queue->Flush();
        const auto counts = ReadBuffer<UINT>(counters.GetResource(), 4);
        Require(counts == std::vector<UINT>({11, 12, 4, 14}), "Batch count or cascade counter offset changed unrelated counters");
        const auto packed = ReadBuffer<GDX12IndirectDrawArgs>(batcher.Commands().GetResource(), counts[2]);
        std::set<UINT> staticIDs, batches;
        for (const auto& command : packed)
        {
            Require(command.InstanceID < instances.size(), "Packed command has invalid root instance ID");
            const auto group = instances[command.InstanceID].SkinBatch;
            if (group == UINT_MAX)
            {
                Require(command.InstanceCount == 1 && command.IndexCountPerInstance == 20 + command.InstanceID &&
                    command.StartIndexLocation == command.InstanceID, "Static command changed during skeletal batching");
                staticIDs.insert(command.InstanceID);
            }
            else
            {
                Require(command.InstanceCount == (group == 0 ? 3u : 2u) && command.IndexCountPerInstance == 100 + group &&
                    command.StartIndexLocation == 10 + group && command.BaseVertexLocation == 0 && command.StartInstanceLocation == 0,
                    "Skeletal batch lost its topology or visible instance count");
                batches.insert(group);
            }
        }
        Require(staticIDs == std::set<UINT>({2, 4}) && batches == std::set<UINT>({0, 1}), "Packed commands lost or duplicated a static object or mesh group");
        auto visible = ReadVisibleIDs(device, batcher);
        std::sort(visible.begin(), visible.begin() + 3); std::sort(visible.begin() + 3, visible.end());
        Require(visible == std::vector<UINT>({0, 3, 7, 1, 5}), "Visible instance ID ranges mixed groups or lost individual actors");
        Upload(device, counters, std::vector<UINT>{11, 12, 0, 14});
        commands = queue->GetCommandList();
        batcher.Build(commands, instanceBuffer, input, counters, 2, 5, 2);
        queue->ExecuteCommandList(commands); queue->Flush();
        Require(ReadBuffer<UINT>(counters.GetResource(), 4) == std::vector<UINT>({11, 12, 0, 14}), "An empty visible list reused stale batch commands");
    }
    CheckDebugMessages(device.GetDevice().Get());
    std::cout << "Instance batch GPU checks: mixed commands, shuffled IDs, empty list, growth and cascade counter offset passed.\n";
}

static std::vector<uint16_t> Capture(BistroApp& app, RenderModule& render)
{
    for (int frame = 0; frame < 4; ++frame) app.BenchmarkFrame(1.0 / 60);
    render.WaitForUploads();
    return ReadHDR(*render.GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture());
}

static size_t ChangedPixels(const std::vector<uint16_t>& before, const std::vector<uint16_t>& after)
{
    Require(before.size() == after.size(), "Instancing comparison resolution changed");
    size_t changed = 0;
    for (size_t index = 0; index < before.size(); index += 4)
        changed += before[index] != after[index] || before[index + 1] != after[index + 1] || before[index + 2] != after[index + 2];
    return changed;
}

static void CheckBistroInstancing()
{
    BistroApp app(GetModuleHandleW(nullptr), 0x5eed);
    Require(app.Initialize(), "Bistro instancing fixture failed to initialize");
    ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
    app.MsgProc(app.GetWindow()->GetWindowHandle(), WM_KEYUP, VK_F1, 0);
    Set(L"App.Bounds.Show", "false"); Set(L"App.ReflectionProbes.Count", "0");
    Set(L"Render.Upscaler.Provider", "0"); Set(L"Render.FrameGeneration.Provider", "0");
    Set(L"Render.Particles.Enabled", "false"); Set(L"Render.Grass.Enabled", "false"); Set(L"Render.Fluid.Enabled", "false");
    Set(L"Render.SSR.Enabled", "false"); Set(L"Render.DOF.Enabled", "false");
    Set(L"Render.MotionBlur.Enabled", "false"); Set(L"Render.AO.Enabled", "0"); Set(L"Render.Reflection.Enabled", "false");
    auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
    world.SetPaused(true);
    std::array<Entity, 4> actors = {InvalidEntity, InvalidEntity, InvalidEntity, InvalidEntity};
    const char* names[] = {"BistroNPC0", "BistroNPC1", "BistroFox0", "BistroFox1"};
    world.GetECS().ForEach<NameComponent, TransformComponent, SkeletalMeshRenderComponent>(
        [&](Entity entity, const NameComponent& name, TransformComponent& transform, SkeletalMeshRenderComponent& mesh)
        {
            transform.Location.y = 1000000;
            for (size_t index = 0; index < actors.size(); ++index)
                if (name.value == names[index])
                {
                    actors[index] = entity;
                    transform.Location = Vector3(index % 2 ? 150.0f : -150.0f, 10000, index < 2 ? 0.0f : -150.0f);
                    transform.Rotation = Vector3::Zero;
                    mesh.SetTimeSeconds(index % 2 ? 0.6 : 0.1);
                }
            transform.DirtyFlag = true;
        });
    for (const auto entity : actors) Require(entity != InvalidEntity, "Instancing fixture needs at least two humans and two foxes");
    Aim(world, Vector3(0, 10200, -800), Vector3(0, 10080, 0));
    Set(L"Render.Skinning.Instancing", "false");
    const auto separate = Capture(app, *render);
    auto* separateFrame = render->GetCurrentPrimaryFrameConstants();
    const auto cameraIndex = render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex;
    const auto separateDrawCount = ReadBuffer<UINT>(separateFrame->CameraVisibilityCommands.at(cameraIndex).OpaqueDrawCounter->GetResource(), 1)[0];
    Set(L"Render.Skinning.Instancing", "true");
    const auto instanced = Capture(app, *render);
    { auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture(); SaveImage(instanced, output->GetWidth(), output->GetHeight(), "instanced"); }
    double difference = 0, reference = 0;
    for (size_t index = 0; index < separate.size(); index += 4)
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const double a = PackedVector::XMConvertHalfToFloat(separate[index + channel]);
            const double b = PackedVector::XMConvertHalfToFloat(instanced[index + channel]);
            difference += std::abs(a - b); reference += std::abs(a);
        }
    const double drift = difference / (std::max)(1.0, reference);
    std::cout << "Instanced/separate HDR relative drift: " << drift << '\n';
    Require(drift < 0.00001, "Instancing changed the rendered poses or instance transforms");
    auto* constants = render->GetCurrentPrimaryFrameConstants();
    auto& visible = constants->CameraVisibilityCommands.at(render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex);
    Require(visible.OpaqueBatches != nullptr, "Opaque pass did not use the instancing path");
    const auto drawCount = ReadBuffer<UINT>(visible.OpaqueDrawCounter->GetResource(), 1)[0];
    Require(drawCount <= visible.OpaqueBatches->Commands().GetElementCount(), "Visible draw counter exceeds its command buffer");
    const auto commands = ReadBuffer<GDX12IndirectDrawArgs>(visible.OpaqueBatches->Commands().GetResource(), drawCount);
    Require(std::any_of(commands.begin(), commands.end(), [](const auto& draw) { return draw.InstanceCount >= 2; }),
        "GPU output contains no actual multi-instance draw");
    UINT expandedCount = 0;
    for (const auto& draw : commands) expandedCount += draw.InstanceCount;
    Require(expandedCount == separateDrawCount && drawCount < separateDrawCount,
        "Batching must preserve every visible instance while reducing draw commands");
    std::cout << "Opaque draw commands: " << separateDrawCount << " separate -> " << drawCount
        << " instanced; visible instances " << expandedCount << '\n';
    auto& noShadowActor = world.GetECS().Get<SkeletalMeshRenderComponent>(actors[0]);
    Require(noShadowActor.GetCastsShadows(), "Skeletal objects must cast shadows by default");
    for (bool casts : {false, true})
    {
        noShadowActor.SetCastsShadows(casts);
        for (int slot = 0; slot < 3; ++slot)
        {
            Capture(app, *render);
            const auto* frame = render->GetCurrentPrimaryFrameConstants();
            const auto transform = render->GetTransformGPUData(actors[0]).CBufferIndex;
            const auto peerTransform = render->GetTransformGPUData(actors[1]).CBufferIndex;
            bool found = false;
            for (UINT i = 0; i < frame->InstanceCache->GetElementCount(); ++i)
            {
                const auto& instance = frame->InstanceCache->GetCPUData()[i];
                if (instance.TransformIndex == peerTransform && instance.SkinOutputVertex != UINT_MAX)
                    Require(instance.CastsShadows != 0, "Object shadow toggle changed its shared-material peer");
                if (instance.TransformIndex != transform || instance.SkinOutputVertex == UINT_MAX) continue;
                found = true;
                Require(bool(instance.CastsShadows) == casts,
                    "Skeletal shadow toggle failed to reach the current frame slot");
            }
            Require(found, "Skeletal shadow toggle lost the object's render instances");
        }
    }
    auto previous = instanced;
    for (const size_t changedActor : {size_t{1}, size_t{3}})
    {
        const auto unchangedActor = actors[changedActor - 1];
        const auto unchangedTime = world.GetECS().Get<SkeletalMeshRenderComponent>(unchangedActor).GetTimeSeconds();
        world.GetECS().Get<SkeletalMeshRenderComponent>(actors[changedActor]).SetTimeSeconds(1.1);
        auto changed = Capture(app, *render);
        const auto changedPixels = ChangedPixels(previous, changed);
        std::cout << (changedActor == 1 ? "Human" : "Fox") << " individual phase changed pixels: " << changedPixels << '\n';
        Require(changedPixels > 20 && world.GetECS().Get<SkeletalMeshRenderComponent>(unchangedActor).GetTimeSeconds() == unchangedTime,
            "Individual animation time must change GPU pixels without changing its peer's clock");
        previous = std::move(changed);
    }
    auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
    CheckDebugMessages(device.Get());
}

int main(int argc, char** argv)
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        CheckBatchCompaction();
        if (argc == 2 && std::strcmp(argv[1], "--batcher-only") == 0) return 0;
        Require(argc == 1, "Usage: InstancingTests.exe [--batcher-only]");
        CheckBistroInstancing();
        std::cout << "Instancing: GPU batching, individual phases, A/B image agreement and clean debug layer passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
