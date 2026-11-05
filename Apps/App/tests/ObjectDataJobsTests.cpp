#include "App.Base/App.h"
#include "Rendering/BistroRenderPipeline.h"
#include "../src/Systems/FlyingPropSystem.h"
#include "../src/BistroScene.h"
#include "Engine.Core/IO/TextureLoader.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <d3d12sdklayers.h>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<typename T>
static std::vector<T> ReadUpload(GDX12UploadBuffer<T>* buffer)
{
    std::vector<T> result(buffer->GetElementCount());
    D3D12_RANGE range = { 0, result.size() * sizeof(T) };
    void* data = nullptr;
    auto& resource = buffer->GetResource().D3DResource;
    ThrowIfFailed(resource->Map(0, &range, &data));
    std::memcpy(result.data(), data, range.End);
    D3D12_RANGE noWrite = { 0, 0 };
    resource->Unmap(0, &noWrite);
    return result;
}

template<typename T>
static std::vector<T> ReadDefault(GDX12UploadBuffer<T>* buffer)
{
    auto& resource = buffer->GetResource();
    D3D12_HEAP_PROPERTIES properties = {};
    ThrowIfFailed(resource.D3DResource->GetHeapProperties(&properties, nullptr));
    Require(properties.Type == D3D12_HEAP_TYPE_DEFAULT, "Scene consumers must read DEFAULT memory");
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(resource.D3DResource->GetDevice(IID_PPV_ARGS(&device)));
    const UINT64 bytes = UINT64(buffer->GetElementCount()) * sizeof(T);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    auto barrier = resource.GetCopySourceBarrier();
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(readback.Get(), 0, resource.D3DResource.Get(), 0, bytes);
    barrier = resource.GetSRVBarrier(); commands->ResourceBarrier(1, &barrier);
    ThrowIfFailed(commands->Close());
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "Readback fence event failed");
    const HRESULT completion = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(completion)) WaitForSingleObject(event, INFINITE);
    CloseHandle(event); ThrowIfFailed(completion);
    std::vector<T> result(buffer->GetElementCount());
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)}, noWrite = {};
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    std::memcpy(result.data(), mapped, range.End);
    readback->Unmap(0, &noWrite);
    return result;
}

class ObjectDataApp final : public App
{
public:
    explicit ObjectDataApp(std::filesystem::path scene) : App(GetModuleHandleW(nullptr)), _scene(std::move(scene))
    {
        auto& setting = GetLocator().GetModule<ConsoleModule>()->FindConsoleVariable(L"Jobs.ThreadCount");
        const int configured = setting.GetInt();
        const unsigned expected = configured > 0 ? static_cast<unsigned>(configured)
            : (std::max)(1u, std::thread::hardware_concurrency());
        Require(GetJobs().GetThreadCount() == expected, "Jobs.ThreadCount must configure the application pool before scene loading");
        Require(!setting.SetFromString("2"), "Jobs.ThreadCount must stay frozen after startup");
        auto& batch = GetLocator().GetModule<ConsoleModule>()->FindConsoleVariable(L"Jobs.BatchSize");
        Require(GetJobs().GetDefaultBatchSize() == static_cast<size_t>(batch.GetInt()),
            "Jobs.BatchSize must configure the application pool before scene loading");
        Require(!batch.SetFromString("32"), "Jobs.BatchSize must stay frozen after startup");
    }
    void Measure(unsigned threads)
    {
        JobSystem jobs(threads, GetJobs().GetDefaultBatchSize());
        auto* render = Locator.GetModule<RenderModule>().get();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        double motionSeconds = 0, objectSeconds = 0;
        constexpr int warmup = 16, iterations = 256;
        for (int i = -warmup; i < iterations; ++i)
        {
            render->Update();
            const auto start = std::chrono::steady_clock::now();
            UpdatePhysics(world, 1.0f / 60.0f);
            const auto middle = std::chrono::steady_clock::now();
            _gpuData.Schedule(jobs, world, 0).Complete();
            const auto end = std::chrono::steady_clock::now();
            if (i >= 0)
            {
                motionSeconds += std::chrono::duration<double>(middle - start).count();
                objectSeconds += std::chrono::duration<double>(end - middle).count();
            }
        }
        Require(world.GetECS().GetPool<RigidBodyComponent>().Size() == 381, "Benchmark needs the actual Bistro physical props");
        std::cout << "object_threads=" << threads << " physics_threads=" << GetJobs().GetThreadCount()
            << " batch_size=" << jobs.GetDefaultBatchSize() << " props=379 samples=" << iterations
            << " physics_us=" << motionSeconds * 1e6 / iterations
            << " object_data_us=" << objectSeconds * 1e6 / iterations << '\n';
    }
    void Check(unsigned threads)
    {
        JobSystem jobs(threads, GetJobs().GetDefaultBatchSize());
        auto* render = Locator.GetModule<RenderModule>().get();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        Entity bottle = InvalidEntity;
        ecs.ForEach<RigidBodyComponent>([&](Entity entity, const RigidBodyComponent& body)
        { if (body.MotionType == PhysicsMotionType::Dynamic) bottle = entity; });
        Require(bottle != InvalidEntity, "Fixture bottle was not split into one entity");
        ecs.RegisterComponent<FlyingPropComponent>();
        if (!ecs.Has<FlyingPropComponent>(bottle)) ecs.Add<FlyingPropComponent>(bottle);
        auto& renderer = ecs.Get<StaticMeshRenderComponent>(bottle);
        Require(renderer.SubMeshIndices.size() == 3, "Bottle material sections were separated");
        _indirect.Schedule(jobs, world, 0, 1).Complete();
        const auto source = ReadUpload(render->GetPrimaryIndirectCommandsCache());
        Require(source.size() == 4, "Static geometry or bottle commands were duplicated/lost");
        for (size_t i = 0; i < source.size(); ++i)
            Require(source[i].InstanceID == i && source[i].StartInstanceLocation == i && source[i].IndexCountPerInstance == 3,
                "Parallel source command population used an incorrect slot");

        for (int frame = 0; frame < 4; ++frame)
        {
            render->Update();
            _lights.Schedule(jobs, world, 0).Complete();
            _gpuData.Schedule(jobs, world, 0, 1).Complete();
        }
        Require(renderer._numFramesDirty == 0, "Initial frame slots did not finish updating");
        Require(renderer._reflectionDynamic, "Flying props must be classified dynamic for reflection capture");
        const auto initialInstances = ReadUpload(render->GetCurrentPrimaryFrameConstants()->InstanceCache.get());
        Require(initialInstances[0].ReflectionDynamic == 0, "The fixture wall must remain static for reflection baking");
        auto* editedMaterial = renderer.Materials.front();
        const float previousRoughness = editedMaterial->Roughness;
        editedMaterial->Roughness = 0.23f;
        editedMaterial->DirtyFlag = true;
        Timer.Reset();
        for (int slot = 0; slot < 3; ++slot)
        {
            Timer.Tick();
            render->BeginFrame();
            render->Update();
            _lights.Schedule(jobs, world, 0).Complete();
            _gpuData.Schedule(jobs, world, 0, 1).Complete();
            render->Render();
            render->WaitForUploads();
            const auto uploadedMaterials = ReadUpload(render->GetCurrentPrimaryFrameConstants()->MaterialCache.get());
            Require(std::abs(uploadedMaterials[editedMaterial->_CBufferIndex].Roughness - 0.23f) < 1e-6f,
                "An actual material edit must reach every prepared frame slot");
            const auto gpuMaterials = ReadDefault(render->GetCurrentPrimaryFrameConstants()->ShadowMaterials.get());
            Require(std::abs(gpuMaterials[editedMaterial->_CBufferIndex].Roughness - 0.23f) < 1e-6f,
                "An actual material edit must reach the DEFAULT snapshot in every frame slot");
        }
        editedMaterial->Roughness = previousRoughness;
        editedMaterial->DirtyFlag = true;
        const auto transformsBeforeGrowth = ReadUpload(render->GetCurrentPrimaryFrameConstants()->TransformCache.get());
        auto probeEntity = ecs.CreateEntity();
        probeEntity.AddComponent<TransformComponent>();
        probeEntity.AddComponent<ReflectionProbeComponent>();
        const auto transformsAfterGrowth = ReadUpload(render->GetCurrentPrimaryFrameConstants()->TransformCache.get());
        Require(transformsAfterGrowth.size() > transformsBeforeGrowth.size() &&
            std::memcmp(transformsBeforeGrowth.data(), transformsAfterGrowth.data(),
                transformsBeforeGrowth.size() * sizeof(transformsBeforeGrowth[0])) == 0,
            "Adding a runtime reflection probe erased existing uploaded transforms");
        auto& flight = ecs.Get<FlyingPropComponent>(bottle);
        flight.Time = flight.Phase = 0;
        flight.Speed = 1;
        auto& transform = ecs.Get<TransformComponent>(bottle);
        transform.Location = Vector3::Zero;
        FlyingPropSystem motion;
        render->Update();
        const auto moved = motion.Schedule(jobs, world, XM_PIDIV2, 1);
        _gpuData.Schedule(jobs, world, 0, 1, moved).Complete();
        Require(std::abs(transform.Location.x - 15) < 0.001f && std::abs(transform.Location.y - 20) < 0.001f &&
            std::abs(transform.Location.z + 15) < 0.001f, "Scheduled flight produced incorrect object displacement");
        for (int frame = 0; frame < 3; ++frame)
        {
            const auto instances = ReadUpload(render->GetCurrentPrimaryFrameConstants()->InstanceCache.get());
            const auto* mesh = render->GetPrimaryGPUMesh(renderer.MeshHandler);
            for (size_t i = 0; i < renderer.SubMeshIndices.size(); ++i)
            {
                const auto& original = mesh->CPUMesh->GetSubMesh(renderer.GetSubMeshIndex(i)).Bounds;
                const auto& actual = instances.at(renderer._CBufferIndices.at(i));
                const auto& part = mesh->SubMeshes.at(renderer.GetSubMeshIndex(i));
                const auto& command = source.at(renderer._CBufferIndices.at(i));
                Require(actual.ReflectionDynamic == 1, "Every moving submesh must reach the dynamic reflection overlay");
                Require(command.StartIndexLocation == part.StartIndexLocation && command.BaseVertexLocation == part.StartVertexLocation &&
                    actual.MaterialIndex == renderer.Materials.at(part.MaterialIndex)->_CBufferIndex &&
                    actual.TransformIndex == render->GetTransformGPUData(bottle).CBufferIndex,
                    "Selected mesh sections lost their draw offsets, material or shared transform");
                Require(std::abs(actual.BoundingBoxCenter.x - original.Center.x - 15) < 0.001f &&
                    std::abs(actual.BoundingBoxCenter.y - original.Center.y - 20) < 0.001f &&
                    std::abs(actual.BoundingBoxCenter.z - original.Center.z + 15) < 0.001f,
                    "Moving an initialized object left stale instance bounds in a frame slot");
            }
            const auto unchanged = ReadUpload(render->GetPrimaryIndirectCommandsCache());
            Require(std::memcmp(source.data(), unchanged.data(), source.size() * sizeof(source[0])) == 0,
                "Movement rewrote shared indirect command templates");
            render->Update();
            _gpuData.Schedule(jobs, world, 0, 1).Complete();
        }
        for (int frame = 0; frame < 3; ++frame)
        {
            render->Update();
            _gpuData.Schedule(jobs, world, 0, 1).Complete();
            const auto transforms = ReadUpload(render->GetCurrentPrimaryFrameConstants()->TransformCache.get());
            const auto& object = transforms.at(render->GetTransformGPUData(bottle).CBufferIndex);
            Require(Matrix(object.WorldMatrix) == Matrix(object.PrevWorldMatrix),
                "Stopped object retained motion in a buffered frame slot");
        }
        auto& camera = ecs.Get<CameraComponent>(world.ActiveCamera);
        camera.ResetHistory = true;
        render->Update();
        _gpuData.Schedule(jobs, world, 0, 1).Complete();
        Require(render->GetRenderPipelineCommonData()->ResetHistory, "Camera cut did not reach the render pipeline");
        const auto cameras = ReadUpload(render->GetCurrentPrimaryFrameConstants()->CameraCB.get());
        Require(Matrix(cameras[camera._CBufferIndex].ViewProjNoJitter) == Matrix(cameras[camera._CBufferIndex].PrevViewProjNoJitter),
            "Camera cut retained its old projection history");
        render->Update();
        _gpuData.Schedule(jobs, world, 0, 1).Complete();
        Require(!render->GetRenderPipelineCommonData()->ResetHistory, "Camera cut reset did not expire");
        camera.FOV += 1;
        render->Update();
        _gpuData.Schedule(jobs, world, 0, 1).Complete();
        Require(render->GetRenderPipelineCommonData()->ResetHistory, "Projection change did not reset temporal history");
        camera.FOV -= 1;
    }
    void RenderBistroFrames()
    {
        auto* render = Locator.GetModule<RenderModule>().get();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        Require(ecs.GetPool<RigidBodyComponent>().Size() == 381, "Bistro has no physical props");
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(render->GetCurrentPrimaryFrameConstants()->CameraCB->GetResource().D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        Timer.Reset();
        // Debug/SDK stalls must not trigger a pause-history reset every frame; jobs use explicit dt below.
        Timer.Stop();
        auto& cameraTransform = ecs.Get<TransformComponent>(world.ActiveCamera);
        auto& camera = ecs.Get<CameraComponent>(world.ActiveCamera);
        const auto setting = [&](const wchar_t* name, const char* value)
        {
            Require(Locator.GetModule<ConsoleModule>()->FindConsoleVariable(name).SetFromString(value), "Bistro runtime setting failed");
        };
        const int upscalers[] = {0, 1, 2, 3, 4, 1};
        bool startedFG[4] = {};
        for (unsigned frame = 0; frame < 36; ++frame)
        {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            std::cout << "Bistro transition frame=" << frame << std::endl;
            Timer.Tick();
            if (frame < 12)
            {
                cameraTransform.Location.x += 0.25f;
                cameraTransform.Rotation.y += 0.05f;
                cameraTransform.DirtyFlag = true;
            }
            setting(L"Render.AO.ExecutionPoint", (frame / 6) % 2 ? "0" : "1");
            setting(L"Render.AO.Implementation", std::to_string((frame / 4) % 4).c_str());
            if (frame == 8) setting(L"Render.MotionBlur.Enabled", "false");
            if (frame == 10) setting(L"Render.MotionBlur.Enabled", "true");
            if (frame == 14) setting(L"Render.MotionBlur.Quality", "1");
            if (frame == 16) GetWindow()->SetWindowSize(1001, 613);
            if (frame == 18) setting(L"Render.MotionBlur.Quality", "3");
            if (frame == 20) camera.ResetHistory = true;
            if (frame == 22) setting(L"Render.MotionBlur.Quality", "2");
            if (frame == 24) setting(L"Render.MotionBlur.MaxRadius", "16");
            if (frame == 26) setting(L"Render.MotionBlur.MaxRadius", "40");
            if (frame == 28) setting(L"Render.MotionBlur.ShutterFraction", "0");
            if (frame == 30) setting(L"Render.MotionBlur.ShutterFraction", "0.5");
            const int upscaler = upscalers[frame / 6];
            const int frameGeneration = frame < 12 ? 0 : frame < 24 ? 1 : 3;
            setting(L"Render.Upscaler.Provider", std::to_string(upscaler).c_str());
            setting(L"Render.Upscaler.Quality", "0");
            setting(L"Render.FrameGeneration.Provider", std::to_string(frameGeneration).c_str());
            if (frame == 9 || frame == 27)
                setting(L"Render.Reconstruction.Restart", std::to_string(frame).c_str());
            // This fixture renders explicitly instead of using App::Update.
            render->SetFrameGenerationSuspended(false);
            render->BeginFrame();
            render->Update();
            const float dt = frame < 24 ? 1.0f / 60 : 0;
            auto work = _movement.Schedule(_jobs, world, dt);
            work = _circle.Schedule(_jobs, world, dt, work);
            work = _spline.Schedule(_jobs, world, dt, work);
            work = _lookAt.Schedule(_jobs, world, dt, work);
            work = _lights.Schedule(_jobs, world, dt, work);
            work.Complete();
            UpdatePhysics(world, dt);
            _gpuData.Schedule(_jobs, world, dt).Complete();
            Render(Timer);
            render->WaitForUploads();
            const auto& sr = render->GetReconstructionStatus();
            const auto& fg = render->GetFrameGenerationStatus();
            Require(static_cast<int>(sr.Requested.Provider) == upscaler &&
                (sr.Effective == sr.Requested.Provider || !sr.FallbackReason.empty()), "Bistro SR mode/fallback mismatch");
            Require(static_cast<int>(fg.Requested) == frameGeneration, "Bistro FG request lost");
            if (frameGeneration && fg.Effective == fg.Requested) startedFG[frameGeneration] = true;
            if (frameGeneration && startedFG[frameGeneration])
            {
                Require(fg.Effective == fg.Requested, "Bistro FG failed after starting");
                if (frame % 6 == 5) Require(fg.GeneratedFrames > 0, "Bistro FG did not generate frames");
            }
            else Require(!frameGeneration || !fg.FallbackReason.empty(), "Bistro FG fallback omitted its reason");
        }
        render->SetFrameGenerationSuspended(true);
        ThrowIfFailed(device->GetDeviceRemovedReason());
        for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
        {
            SIZE_T bytes = 0;
            info->GetMessage(index, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(index, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) throw std::runtime_error(message->pDescription);
        }
        std::cout << "Actual Bistro rendered 36 frames through all SR providers, independent AMD/Intel FG and Reinhard; props="
            << ecs.GetPool<RigidBodyComponent>().Size() - 2
            << ", camera translation/rotation, Off/Low/Mid/High, cut and odd resize passed; debug layer clean.\n";
    }
protected:
    bool AddModules() override
    {
        auto scenes = std::make_shared<SceneManagerModule>(_scene);
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer,
            std::make_unique<BistroRenderPipeline>(scenes)));
        ConfigureBistroProps(*scenes, GetJobs());
        Locator.RegisterModule(scenes);
        return BenchmarkEngine::AddModules();
    }
private:
    std::filesystem::path _scene;
};

int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--render-bistro")
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            ObjectDataApp app(std::filesystem::absolute(argv[2]));
            Require(app.Initialize(), "Bistro render smoke initialization failed");
            ShowWindow(app.GetWindow()->GetWindowHandle(), SW_SHOWNOACTIVATE);
            app.RenderBistroFrames();
            return 0;
        }
        if (argc == 2 && (std::string_view(argv[1]) == "--check-job-threads" || std::string_view(argv[1]) == "--check-job-settings"))
        {
            ObjectDataApp app({});
            std::cout << "Configured job pool: " << app.GetJobs().GetThreadCount()
                << " threads, default batch=" << app.GetJobs().GetDefaultBatchSize() << ".\n";
            return 0;
        }
        if (argc == 2)
        {
            ObjectDataApp app(std::filesystem::absolute(argv[1]));
            Require(app.Initialize(), "Bistro CPU benchmark initialization failed");
            ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
            app.Measure(1);
            app.Measure(4);
            app.Measure(app.GetJobs().GetThreadCount());
            return 0;
        }
        const auto scene = std::filesystem::absolute("Build/tests/object-data/Amazon Lumberyard Bistro");
        std::filesystem::create_directories(scene / "Interior");
        const auto bitmapPath = scene / "metadata.bmp";
        char bitmap[58] = {};
        bitmap[0] = 'B'; bitmap[1] = 'M'; bitmap[2] = 58; bitmap[10] = 54; bitmap[14] = 40;
        bitmap[18] = bitmap[22] = bitmap[26] = 1; bitmap[28] = 24; bitmap[34] = 4;
        std::ofstream image(bitmapPath, std::ios::binary);
        image.write(bitmap, sizeof(bitmap));
        image.close();
        const auto estimate = Engine::Core::TextureLoader::EstimateTransientBytes(bitmapPath);
        Require(estimate == 32 && Engine::Core::TextureLoader::EstimateTransientBytes(bitmapPath) == estimate,
            "Repeated WIC metadata reads must keep their COM factory usable");
        std::ofstream materials(scene / "Interior/interior.mtl");
        for (const char* name : { "wall", "glass", "label", "cap" }) materials << "newmtl " << name << "\nKd 0.8 0.7 0.6\n";
        materials.close();
        std::ofstream obj(scene / "Interior/interior.obj");
        obj << "mtllib interior.mtl\nv 0 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\ng Wall\nusemtl wall\nf 1 2 3\ng Paris_LiquorBottle_1\n";
        for (const char* name : { "glass", "label", "cap" }) obj << "usemtl " << name << "\nf " << (std::string_view(name) == "glass" ? "1 2 3" : std::string_view(name) == "label" ? "1 4 2" : "1 3 4") << "\n";
        obj.close();
        ObjectDataApp app(scene);
        Require(app.Initialize(), "GPU object-data fixture initialization failed");
        ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
        app.Check(1);
        app.Check(4);
        std::cout << "Actual object jobs: selected indirect slots, all frame-slot bounds and immutable source templates passed (1/4 threads).\n";
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
