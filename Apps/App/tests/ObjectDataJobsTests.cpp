#include "App.Base/App.h"
#include "App.Base/Systems/FlyingPropSystem.h"
#include "Engine.Core/IO/TextureLoader.h"
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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
        FlyingPropSystem motion;
        auto* render = Locator.GetModule<RenderModule>().get();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        double motionSeconds = 0, objectSeconds = 0;
        constexpr int warmup = 16, iterations = 256;
        for (int i = -warmup; i < iterations; ++i)
        {
            render->Update();
            const auto start = std::chrono::steady_clock::now();
            const auto moved = motion.Schedule(jobs, world, 1.0f / 60.0f);
            moved.Complete();
            const auto middle = std::chrono::steady_clock::now();
            _gpuData.Schedule(jobs, world, 0, moved).Complete();
            const auto end = std::chrono::steady_clock::now();
            if (i >= 0)
            {
                motionSeconds += std::chrono::duration<double>(middle - start).count();
                objectSeconds += std::chrono::duration<double>(end - middle).count();
            }
        }
        Require(world.GetECS().GetPool<FlyingPropComponent>().Size() == 346, "Benchmark needs the actual Bistro Interior props");
        std::cout << "threads=" << threads << " batch_size=" << jobs.GetDefaultBatchSize() << " props=346 samples=" << iterations
            << " motion_us=" << motionSeconds * 1e6 / iterations
            << " object_data_us=" << objectSeconds * 1e6 / iterations << '\n';
    }
    void Check(unsigned threads)
    {
        JobSystem jobs(threads, GetJobs().GetDefaultBatchSize());
        auto* render = Locator.GetModule<RenderModule>().get();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        Require(ecs.GetPool<FlyingPropComponent>().Size() == 1, "Fixture bottle was not split into one entity");
        const Entity bottle = ecs.GetPool<FlyingPropComponent>().GetEntities().front();
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
    }
protected:
    bool AddModules() override
    {
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
        Locator.RegisterModule(std::make_shared<SceneManagerModule>(_scene));
        return BenchmarkEngine::AddModules();
    }
private:
    std::filesystem::path _scene;
};

int main(int argc, char** argv)
{
    try
    {
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
        obj << "mtllib interior.mtl\nv 0 0 0\nv 2 0 0\nv 0 2 0\ng Wall\nusemtl wall\nf 1 2 3\ng Paris_LiquorBottle_1\n";
        for (const char* name : { "glass", "label", "cap" }) obj << "usemtl " << name << "\nf 1 2 3\n";
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
