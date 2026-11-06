#include "App.Base/App.h"
#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Rendering/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
#include "PlacementTiming.h"
#include "SkinPlacementChecks.h"
#include "GrassPlacementChecks.h"
#include "PreparedGrassViewChecks.h"
static void Set(const wchar_t* name, const char* value)
{
    Require(BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->FindConsoleVariable(name).SetFromString(value), "CVar write failed");
}
class TestEngine final : public App { public: TestEngine() : App(GetModuleHandleW(nullptr)) {} };

class WaterGraphOutput final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Water output check");
        graph.ReadInputs(_inputs);
        graph.Export(_inputs[0]);
        graph.HasSideEffects();
    }
};

static std::vector<float> Read(GDX12Device& device, GDX12Texture& texture, UINT channel = 0)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands); queue->Flush();
    void* mapped = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool rgba32 = desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    const UINT channels = hdr ? 3 : 1;
    std::vector<float> result;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* pixel = static_cast<const unsigned char*>(mapped) + footprint.Offset +
                y * footprint.Footprint.RowPitch + x * (hdr ? 8 : rgba32 ? 16 : 4);
            for (UINT c = 0; c < channels; ++c)
            {
                const float value = hdr ? PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(pixel)[c]) : reinterpret_cast<const float*>(pixel)[channel];
                Require(std::isfinite(value), "Nonfinite GPU output");
                result.push_back(value);
            }
        }
    const D3D12_RANGE noWrite = {};
    readback->Unmap(0, &noWrite);
    return result;
}

#include "ParticlePlacementChecks.h"
#include "ParticleSimulationControlsChecks.h"
#include "WaterCompositionChecks.h"

static void TestCPU()
{
    for (UINT quality = 1; quality <= 3; ++quality)
    {
        const auto sky = ResolveSkyAtmosphereSettings({}, quality);
        Require(sky.TransmittanceWidth >= 128 && sky.SkySamples > 0, "Sky quality budgets missing");
        Require(ResolveGrassSettings({}, quality).MaxBlades > 0, "Grass budget missing");
        Require(ResolveWindSettings({}, quality).GridResolution > 0, "Wind grid missing");
        Require(ResolveFluidSettings({}, quality).MaxSubsteps > 0, "Fluid stepping missing");
        Require(ResolveParticleEmitterSettings({}, quality).MaxParticles > 0, "Particle budget missing");
    }
    auto invalid = SkyAtmosphereSettings(); invalid.MieAnisotropy = NAN;
    bool rejected = false;
    try { ValidateSkySettings(invalid); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Nonfinite atmosphere accepted");
    GDX12ParticleEmitterSettings particle; particle.LifetimeMin = -1;
    Require(!IsValidParticleEmitterSettings(particle), "Invalid lifetime accepted");
    particle = {}; particle.CollisionRestitution = 1.01f;
    Require(!IsValidParticleEmitterSettings(particle), "Energy-gaining particle bounce accepted");
    particle = {}; particle.CollisionRadius = 0;
    Require(!IsValidParticleEmitterSettings(particle), "Zero collision radius accepted");
    particle = {}; particle.CollisionThickness = NAN;
    Require(!IsValidParticleEmitterSettings(particle), "Nonfinite collision thickness accepted");
    particle = {}; particle.CollisionBoxCenter.x = NAN;
    Require(!IsValidParticleEmitterSettings(particle), "Nonfinite particle obstacle center accepted");
    particle = {}; particle.CollisionBoxHalfExtents.z = 0;
    Require(!IsValidParticleEmitterSettings(particle), "Empty particle obstacle accepted");
    particle = {}; particle.CollisionBoxYawDegrees = INFINITY;
    Require(!IsValidParticleEmitterSettings(particle), "Nonfinite particle obstacle rotation accepted");
    particle = {}; particle.Geometry = static_cast<ParticleGeometry>(2);
    Require(!IsValidParticleEmitterSettings(particle), "Unknown particle geometry accepted");
    particle.Geometry = ParticleGeometry::Firefly; particle.Blend = ParticleBlend::SortedAlpha;
    Require(!IsValidParticleEmitterSettings(particle), "Unsupported globally sorted mesh particles accepted");
    const auto& firefly = GetFireflyParticleMesh();
    Require(firefly.size() == 120 && firefly.size() <= 600, "Firefly mesh exceeds the vertex budget");
    UINT wings = 0;
    for (size_t vertex = 0; vertex < firefly.size(); vertex += 3)
    {
        const Vector3 normal = (firefly[vertex+1].Position - firefly[vertex].Position).Cross(
            firefly[vertex+2].Position - firefly[vertex].Position);
        Require(normal.LengthSquared() > 1e-10f && normal.Dot(firefly[vertex].Normal) > 0,
            "Firefly triangles must have nonzero area and outward face normals");
    }
    for (const auto& vertex : firefly)
    {
        Require(vertex.Position.Length() <= 0.707107f && std::abs(vertex.Normal.Length() - 1) < 1e-5f,
            "Firefly mesh must fit the shared particle culling sphere and have unit normals");
        if (vertex.Wing != 0) ++wings;
    }
    Require(wings == 48, "Firefly must have two volumetric animated wings");
    auto& assets = Engine::Core::AssetManager::GetInstance();
    const auto* fox = assets.LoadScene(std::filesystem::absolute("Assets/Models/KhronosFox/glTF/Fox.gltf"));
    Require(fox && fox->GetClips().size() == 3 && fox->GetNodeCount() >= 24, "Actual Fox hierarchy/clips failed to import");
    const auto bind = fox->EvaluatePose(-1, 0, false);
    const auto a = fox->EvaluatePose(1, 0, true), b = fox->EvaluatePose(1, 0.3, true);
    Require(bind.size() == fox->GetNodeCount() && a.size() == b.size(), "Incomplete animation pose");
    Require(std::memcmp(a.data(), b.data(), a.size() * sizeof(Matrix)) != 0, "Walk animation does not move");
    std::cout << "CPU component budgets, invalid inputs and actual Fox animation passed.\n";
}

static void TestSkinning(GDX12Device& device, GDX12DeviceResources& resources)
{
    SubMesh source;
    source.Vertices.resize(2);
    for (auto& vertex : source.Vertices) { vertex.Position = Vector3(1, 2, 3); vertex.Normal = Vector3::UnitY; }
    source.Bones.resize(128);
    for (auto& bone : source.Bones) bone.InverseBind = Matrix::Identity;
    source.SkinVertices = { {0, 5}, {5, 0} };
    for (UINT i = 0; i < 5; ++i) source.Influences.push_back({123 + i, 0.2f});
    GDX12Skinning skinning(&device);
    auto mesh = skinning.CreateMesh(source, 1);
    mesh->OutputVertex = resources.GeometryBuffer->AllocateSkinnedVertices(2);
    std::vector<GDX12SkinPalette> palette(129);
    for (auto& bone : palette) { bone.Current = Matrix::CreateTranslation(2, 0, 0); bone.Previous = Matrix::CreateTranslation(1, 0, 0); }
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Vertex) * 2);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue(); auto* commands = queue->GetCommandList();
    skinning.Dispatch(commands, *resources.GeometryBuffer, *mesh, 0, palette);
    auto* vertices = resources.GeometryBuffer->BeginSkinning(commands);
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(vertices, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->GetCommandList()->ResourceBarrier(1, &barrier);
    commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, vertices, size_t(mesh->OutputVertex) * sizeof(Vertex), sizeof(Vertex) * 2);
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(vertices, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->GetCommandList()->ResourceBarrier(1, &barrier);
    resources.GeometryBuffer->EndSkinning(commands);
    queue->ExecuteCommandList(commands); queue->Flush();
    void* mapped = nullptr; const D3D12_RANGE range = {0, sizeof(Vertex) * 2};
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    for (UINT i = 0; i < 2; ++i)
    {
        const auto& vertex = static_cast<const Vertex*>(mapped)[i];
        Require(std::abs(vertex.Position.x - 3) < 0.0001f && std::abs(vertex.PreviousPosition.x - 2) < 0.0001f && std::abs(vertex.Normal.y - 1) < 0.0001f,
            "GPU LBS differs from independent translation oracle");
    }
    const D3D12_RANGE noWrite = {}; readback->Unmap(0, &noWrite);
    resources.GeometryBuffer->ReleaseSkinnedVertices(mesh->OutputVertex, 2);
    std::cout << "GPU skinning: 128 bones, five influences, rigid attachment and previous position passed.\n";
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    ComPtr<ID3D12InfoQueue> diagnosticInfo;
    try
    {
        std::cout.setf(std::ios::unitbuf);
        const std::string_view mode = argc > 1 ? argv[1] : "";
        const bool asyncMode = mode == "--async" || mode == "--async-gpu-validation";
        const bool benchmark = mode == "--benchmark" || mode == "--benchmark-skin" ||
            mode == "--benchmark-grass" || mode == "--benchmark-particles";
        if (!mode.empty() && !benchmark && !asyncMode && mode != "--cpu" && mode != "--gpu-validation")
            throw std::invalid_argument("Unknown SecondGroupTests mode");
        TestEngine engine;
        TestCPU();
        if (argc > 1 && std::string_view(argv[1]) == "--cpu") return 0;
        HANDLE gate = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(gate && WaitForSingleObject(gate, INFINITE) == WAIT_OBJECT_0, "GPU mutex failed");
        ComPtr<ID3D12Debug> debug;
        if (!benchmark) { ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer(); }
        if (mode == "--gpu-validation" || mode == "--async-gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation; ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        LoadPlacementCapturer();
        GDX12Device device; ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        if (benchmark)
            std::cout << "CAPABILITIES UMA=" << device.GetDeviceFeatures().Features.UMA()
                << " GPUUploadHeapSupported=" << device.GetDeviceFeatures().Features.GPUUploadHeapSupported() << '\n';
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        if (!benchmark) ThrowIfFailed(device.GetDevice().As(&info));
        diagnosticInfo = info;
        auto* queue = device.GetCommandQueue();
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = 129; common.WindowHeight = common.DownscaledHeight = 97;
        common.ActiveCameraCBufferIndex = 0;
        const Vector3 eye(0, 30, -150);
        const Matrix view = Matrix::CreateLookAt(eye, Vector3(0, 20, 0), Vector3::UnitY);
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, 129.0f / 97, 1, 10000);
        projection._33 = 1.0f / 9999; projection._43 = 10000.0f / 9999;
        const Matrix vp = view * projection;
        common.CameraView = view;
        for (auto& frame : resources.FrameConstants)
        {
            frame->CameraCB->Resize(1);
            GDX12CameraConstants camera = {};
            XMStoreFloat4x4(&camera.View, view.Transpose()); XMStoreFloat4x4(&camera.ViewProj, vp.Transpose());
            XMStoreFloat4x4(&camera.InvViewProj, vp.Invert().Transpose());
            camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = camera.ViewProj;
            camera.CameraLocation = eye; camera.NearPlane = 1; camera.FarPlane = 10000;
            frame->CameraCB->CopyData(0, camera);
            frame->MaterialCache->Resize(1); frame->MaterialCache->CopyData(0, GDX12MaterialConstants());
        }
        for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
        {
            resources.CurrFrameConstantsIndex = slot;
            auto* commands = queue->GetCommandList();
            resources.PrepareLocalShadowData();
            resources.UploadLocalShadowData(commands);
            resources.FrameConstants[slot]->UploadReflectionProbeData(commands);
            queue->ExecuteCommandList(commands);
        }
        queue->Flush(); resources.CurrFrameConstantsIndex = 0;
        TestSkinning(device, resources);
        if (!benchmark) TestSkinPlacement(device, resources);
        if (benchmark && (mode == "--benchmark" || mode == "--benchmark-skin")) BenchmarkSkinPlacement(device, resources);
        GDX12EnvironmentIBLPass environment; environment.Setup(&resources, nullptr, &common); environment.Initialize();
        const auto execute = [&](GDX12RenderPass& pass) { auto* list = queue->GetCommandList(); pass.Execute(list); queue->ExecuteCommandList(list); queue->Flush(); };
        resources.Sky.Source = SkySource::Off; execute(environment);
        const auto off = Read(device, *environment.GetOutput(3)->GetTexture());
        Require(*std::max_element(off.begin(), off.end()) == 0, "Off environment is not black");
        resources.Sky.Source = SkySource::SkyAtmosphere; resources.Sky.Quality = 1; ++resources.Sky.Revision;
        auto& atmosphere = resources.Sky.Atmosphere;
        atmosphere.IBL.CubeResolution = 32; atmosphere.IBL.FilterSamples = 16;
        execute(environment);
        const auto sky = Read(device, *environment.GetOutput(3)->GetTexture());
        Require(*std::max_element(sky.begin(), sky.end()) > 0, "Atmosphere source is black");
        GDX12CloudComponent clouds; environment.SetCloudSource(&clouds); execute(environment);
        const auto cloud = Read(device, *environment.GetOutput(3)->GetTexture());
        const auto revision = resources.ReflectionEnvironmentRevision;
        common.ResetHistory = common.ResetUpscalerHistory = common.RequestHistoryReset = false;
        clouds.Advance(0.5); execute(environment);
        const auto animated = Read(device, *environment.GetOutput(3)->GetTexture());
        Require(cloud != animated && resources.ReflectionEnvironmentRevision == revision && !common.RequestHistoryReset,
            "Continuous clouds must animate IBL without a temporal discontinuity");
        auto parameters = clouds.GetParameters(); parameters.Implementation = 1; clouds.SetParameters(parameters); execute(environment);
        Require(resources.ReflectionEnvironmentRevision > revision, "Cloud implementation change did not invalidate history");
        environment.SetCloudSource(nullptr); resources.Sky.Source = SkySource::SkyBox; resources.Sky.Quality = 1;
        resources.Sky.Box.IBL.CubeResolution = 32; resources.Sky.Box.IBL.FilterSamples = 16; ++resources.Sky.Revision; execute(environment);
        std::cout << "GPU atmosphere, cloud fBM/ridged, animated IBL, source switching and Off passed.\n";

        GDX12OpaquePass opaque; opaque.Setup(&resources, nullptr, &common); opaque.Initialize();
        auto* depth = opaque.GetOutput(2)->GetTexture(); auto* motion = opaque.GetOutput(1)->GetTexture();
        const auto makeColor = [&](DXGI_FORMAT format, XMFLOAT4 clear)
        {
            auto texture = std::make_unique<GDX12Texture>(); GDX12TextureDesc desc;
            desc.Width = 129; desc.Height = 97; desc.Format = desc.SRVDesc.Format = desc.RTVDesc.Format = format;
            desc.CreateRTV = true; desc.ClearValue = clear; desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
            desc.SRVHeapIndex = resources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Texture2D.MipLevels = 1;
            desc.RTVHeap = resources.RTVHeap.get(); desc.RTVHeapIndex = resources.RTVHeap->GetAvailableIndex();
            desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; texture->Initialize(desc); return texture;
        };
        auto hdr = makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, {0.2f, 0.4f, 0.6f, 1});
        auto accum = makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, {});
        auto reveal = makeColor(DXGI_FORMAT_R16_FLOAT, {1, 1, 1, 1});
        auto* list = queue->GetCommandList();
        list->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier(), motion->GetResource()->GetRenderTargetBarrier()});
        list->ClearDepthStencilView(depth); list->ClearRenderTargetView(motion);
        for (auto* texture : {hdr.get(), accum.get(), reveal.get()})
        { list->ResourceBarrier({texture->GetResource()->GetRenderTargetBarrier()}); list->ClearRenderTargetView(texture); }
        queue->ExecuteCommandList(list); queue->Flush();
        auto grassRenderer = std::make_unique<GDX12GrassRenderer>(&resources);
        GDX12GrassInstance grass; grass.Id = 1; grass.RootsCm = {{0, 0, 0}, {-12, 0, 0}, {12, 0, 0}};
        grass.WidthCm = 12; grass.WindId = 1;
        GDX12WindInstance wind; wind.Id = 1; wind.DeltaSeconds = 1.0f / 60;
        auto controls = GetGrassWindControls();
        list = queue->GetCommandList(); grassRenderer->BeginFrame(common, {grass}, {wind}, controls); grassRenderer->PrepareGPU(list);
        list->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
        list->SetViewport(depth->GetViewport()); list->SetScissorRect(depth->GetScissorRect()); list->SetRenderTargets({}, depth);
        EmitPreparedGrass(*grassRenderer, resources, list, GDX12GrassRenderer::Stage::Depth, vp, eye, resources.FrameConstants[0]->CameraCB->GetElementAddress(0));
        queue->ExecuteCommandList(list); queue->Flush();
        const auto blades = Read(device, *depth);
        Require(*std::max_element(blades.begin(), blades.end()) > 0 && grassRenderer->HasCasters(), "Grass indirect draw produced no depth/casters");
        std::cout << "GPU projected-grid wind and compact grass indirect depth passed.\n";
        if (!benchmark)
        {
            TestGrassPlacement(device, resources, common, depth, vp, eye, *grassRenderer);
            TestPreparedGrassViews(device, resources, common, *depth, vp, eye);
        }
        if (benchmark && (mode == "--benchmark" || mode == "--benchmark-grass")) BenchmarkGrassPlacement(device, resources, common, depth, vp, eye);

        Set(L"Render.Fluid.Enabled", "true");
        Set(L"Render.Fluid.Quality", "2");
        GDX12FluidSurfacePass water; water.Setup(&resources, nullptr, &common); water.SetInputs({depth, motion}); water.Initialize();
        GDX12FluidResolvePass waterResolve(&water);
        waterResolve.Setup(&resources, nullptr, &common);
        waterResolve.SetInputs({ hdr.get(), environment.GetOutput(1) });
        waterResolve.Initialize();
        WaterGraphOutput waterOutput;
        waterOutput.SetInputs({ waterResolve.GetOutput(0), water.GetOutput(0), water.GetOutput(1) });
        GDX12RenderGraph waterGraph(&device);
        waterGraph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Water fixture scene");
        waterGraph.ImportShared(GDX12RenderGraph::SharedResource::ReflectionProbes, "Water fixture probes");
        for (auto* input : std::vector<IRenderPassLink*>{ depth, motion, hdr.get(), environment.GetOutput(1) })
            waterGraph.ImportLink("Water fixture input", input);
        waterGraph.AddPass(water); waterGraph.AddPass(waterResolve); waterGraph.AddPass(waterOutput); waterGraph.Compile(false);
        const auto executeWater = [&]
        {
            water.PrepareFrame();
            waterResolve.PrepareFrame();
            auto* commands = waterGraph.Execute(queue->GetCommandList());
            queue->ExecuteCommandList(commands); queue->Flush();
        };
        const auto requireWaterFallback = [&]
        {
            Require(water.GetOutput(0)->GetTexture() == depth && water.GetOutput(1)->GetTexture() == motion &&
                waterResolve.GetOutput(0)->GetTexture() == hdr.get(), "Inactive water must preserve depth, motion and HDR identity");
        };
        executeWater(); requireWaterFallback();
        GDX12FluidInstance fluid; fluid.Id = 1; fluid.DeltaSeconds = 1.0f / 60;
        GDX12FluidSettings settings; settings.ParticleRadiusCm = 8; settings.SmoothingRadiusCm = 20;
        settings.AccelerationCmPerSecond2 = Vector3::Zero; settings.FilterIterations = 1; settings.FilterRadiusPixels = 2;
        fluid.Component.SetSettings(settings);
        fluid.Component.SetParticles({{{-5,20,0},{}},{{5,20,0},{}},{{0,25,5},{}},{{0,15,-5},{}}});
        water.SetInstances({fluid});
        water.PrepareFrame();
        const auto waterAccesses = water.GetBufferAccesses();
        Require(std::count_if(waterAccesses.begin(), waterAccesses.end(), [](const auto& access)
            { return access.Usage == GDX12RenderGraph::Mode::ReadWrite; }) == 5,
            "Fluid first-frame physical particle/sort/density buffers were not declared before graph execution");
        for (UINT frame = 0; frame < 4; ++frame) executeWater();
        Require(water.GetOutput(0)->GetTexture() != depth, "Active fluid did not publish its depth");
        Require(water.GetOutput(1)->GetTexture() != motion && water.GetOutput(2)->GetTexture() && water.GetOutput(3)->GetTexture(),
            "Active fluid did not publish motion or its internal geometry/owner links");
        const auto waterDepth = Read(device, *water.GetOutput(0)->GetTexture());
        Require(waterDepth != blades, "Water sphere/NRF/merge produced no depth change");
        auto* resolved = waterResolve.GetOutput(0)->GetTexture();
        Require(resolved != hdr.get(), "Water resolve is inactive"); Read(device, *resolved);
        fluid.Component.Reset(); water.SetInstances({fluid}); executeWater();
        for (const char* quality : { "1", "3", "0", "2" })
        {
            Set(L"Render.Fluid.Quality", quality); executeWater();
            if (quality[0] == '0') requireWaterFallback();
            else Require(waterResolve.GetOutput(0)->GetTexture() != hdr.get(), "Fluid quality switch lost water resolve");
        }
        settings.RenderEnabled = false; fluid.Component.SetSettings(settings); water.SetInstances({fluid}); executeWater();
        requireWaterFallback();
        Require(water.GetBufferAccesses().size() >= 6, "Render-disabled fluid lost its simulation state");
        water.SetInstances({}); executeWater(); requireWaterFallback();
        settings.RenderEnabled = true; fluid.Component.SetSettings(settings); water.SetInstances({fluid}); executeWater();
        std::cout << "GPU fluid graph: first-frame buffers, SPH/NRF, resolve, quality, simulation-only, clear and reset passed.\n";

        GDX12ParticlesPass particles; particles.Setup(&resources, nullptr, &common);
        particles.SetInputs({water.GetOutput(0), accum.get(), reveal.get()}); particles.Initialize();
        resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
        Set(L"Render.Particles.Enabled", "true");
        Set(L"Render.Particles.Quality", "2");
        Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true");
        GDX12ParticleSimulationPass simulationPass;
        simulationPass.Setup(&resources, nullptr, &common);
        simulationPass.SetInputs({depth});
        const auto makeParticleGraph = [&](bool draw)
        {
            auto graph = std::make_unique<GDX12RenderGraph>(&device);
            graph->ImportShared(GDX12RenderGraph::SharedResource::Scene, "Fixture scene");
            graph->ImportLink("Opaque depth", depth);
            if (water.GetOutput(0) != depth) graph->ImportLink("Water depth", water.GetOutput(0));
            graph->ImportLink("Accumulation", accum.get());
            graph->ImportLink("Revealage", reveal.get());
            graph->AddPass(simulationPass);
            if (draw) graph->AddPass(particles);
            graph->Compile(false);
            return graph;
        };
        auto particleGraph = makeParticleGraph(true);
        auto simulationOnlyGraph = makeParticleGraph(false);
        bool particleAsync = asyncMode, flushParticles = true, simulationOnly = false;
        const auto prepareParticles = [&]
        {
            resources.ParticleSimulation->BeginFrame(&resources, common);
            particles.BeginFrame();
        };
        const auto executeParticles = [&](GDX12ParticlesPass& pass)
        {
            prepareParticles();
            auto* commands = queue->GetCommandList();
            resources.ParticleSimulation->InitializePools(commands, &resources);
            Require(&pass == &particles, "Unexpected particle fixture pass");
            auto* computeQueue = particleAsync ? device.TryGetComputeQueue() : nullptr;
            Require(!particleAsync || computeQueue, "Async fixture requires a COMPUTE queue");
            auto& graph = simulationOnly ? simulationOnlyGraph : particleGraph;
            commands = graph->Execute(commands, computeQueue);
            queue->ExecuteCommandList(commands);
            resources.FrameConstants[resources.CurrFrameConstantsIndex]->FenceValue = commands->FenceValue;
            Require(graph->GetStatistics().AsyncPasses == (particleAsync ? 1u : 0u), "Particle queue selection failed");
            if (flushParticles) queue->Flush();
        };
        GDX12ParticleEmitter emitter; emitter.Id = 1; emitter.MaxParticles = 64; emitter.BurstTotal = 16;
        emitter.EmissionRate = 0; emitter.World = Matrix::CreateTranslation(0, 30, -40);
        emitter.SpeedMin = emitter.SpeedMax = 0; emitter.SizeStart = emitter.SizeEnd = 40; emitter.Acceleration = Vector3::Zero;
        for (auto blend : {ParticleBlend::WeightedAlpha, ParticleBlend::Additive, ParticleBlend::SortedAlpha})
        {
            emitter.Blend = blend; ++emitter.ResetRevision; resources.ParticleEmitters = {emitter};
            const auto before = Read(device, *hdr);
            executeParticles(particles); list = queue->GetCommandList(); particles.Composite(list, hdr.get());
            queue->ExecuteCommandList(list); queue->Flush();
            const auto after = Read(device, *hdr);
            if (blend == ParticleBlend::WeightedAlpha)
            {
                const auto weighted = Read(device, *accum);
                Require(*std::max_element(weighted.begin(), weighted.end()) > 0, "Weighted particles drew no pixels");
            }
            else Require(before != after, "Additive/sorted particles drew no HDR pixels");
            Require(resources.ParticleMotion, "Particles did not publish motion");
        }
        ComPtr<ID3D12QueryHeap> meshQuery;
        D3D12_QUERY_HEAP_DESC queryDesc = {}; queryDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; queryDesc.Count = 1;
        ThrowIfFailed(device.GetDevice()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&meshQuery)));
        const auto queryHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto queryBuffer = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        ComPtr<ID3D12Resource> meshStats;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&queryHeap, D3D12_HEAP_FLAG_NONE, &queryBuffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&meshStats)));
        emitter.MaxParticles = 16; emitter.BurstTotal = 1; emitter.Geometry = ParticleGeometry::Firefly;
        emitter.ColorStart = emitter.ColorEnd = Vector4(1, 1, 1, 1);
        for (auto blend : {ParticleBlend::WeightedAlpha, ParticleBlend::Additive})
        {
            emitter.Blend = blend; emitter.ResetRevision += 10; resources.ParticleEmitters = {emitter};
            prepareParticles();
            list = queue->GetCommandList();
            resources.ParticleSimulation->InitializePools(list, &resources);
            resources.ParticleSimulation->Execute(list, &resources, common, depth);
            list->GetCommandList()->BeginQuery(meshQuery.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            particles.Execute(list); particles.Composite(list, hdr.get());
            list->GetCommandList()->EndQuery(meshQuery.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
            list->GetCommandList()->ResolveQueryData(meshQuery.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, meshStats.Get(), 0);
            queue->ExecuteCommandList(list); queue->Flush();
            void* mapped = nullptr; ThrowIfFailed(meshStats->Map(0, nullptr, &mapped));
            const auto stats = *static_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mapped);
            meshStats->Unmap(0, nullptr);
            Require(stats.GSInvocations == 0 && stats.IAPrimitives == 80 && stats.VSInvocations >= 120,
                "Mesh particles must draw the 40-triangle model twice (color/motion), with no GS");
            const auto coverage = Read(device, *resources.ParticleMotion, 3);
            Require(*std::max_element(coverage.begin(), coverage.end()) > 0, "Firefly mesh produced no motion/coverage pixels");
        }
        std::cout << "GPU firefly mesh: 120 vertices, 40 triangles, weighted/additive color and motion, zero GS invocations passed.\n";
        // Constant reverse-Z depth is a plane 150 cm from the camera, independent of the shader oracle.
        list = queue->GetCommandList();
        list->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
        list->GetCommandList()->ClearDepthStencilView(depth->GetDSV()->CPUHandle, D3D12_CLEAR_FLAG_DEPTH,
            (10000.0f / 150 - 1) / 9999, 0, 0, nullptr);
        queue->ExecuteCommandList(list); queue->Flush();
        particles.SetInputs({depth, accum.get(), reveal.get()});
        particleGraph = makeParticleGraph(true);
        Vector3 forward(0, -10, 150); forward.Normalize();
        emitter = {}; emitter.Id = 1; emitter.MaxParticles = 16; emitter.BurstTotal = 1;
        emitter.World = Matrix::CreateTranslation(eye + forward * 100);
        emitter.Direction = forward; emitter.ConeAngleDegrees = 0;
        emitter.SpeedMin = emitter.SpeedMax = 100; emitter.Acceleration = Vector3::Zero;
        emitter.SizeStart = emitter.SizeEnd = 10; emitter.EmissionRate = 0;
        emitter.LifetimeMin = emitter.LifetimeMax = 10; emitter.DeltaTime = 0.1f; emitter.Substeps = 4;
        emitter.CollisionRadius = 2; emitter.CollisionRestitution = 1; emitter.SoftDistance = 0;
        emitter.Blend = ParticleBlend::Additive;
        for (int mode = 0; mode < 3; ++mode)
        {
            emitter.DepthCollision = mode != 0;
            emitter.CollisionRestitution = mode == 2 ? 0.0f : 1.0f;
            emitter.ResetRevision = 100 + mode;
            resources.ParticleEmitters = {emitter};
            for (UINT frame = 0; frame < 9; ++frame) executeParticles(particles);
            const auto coverage = Read(device, *resources.ParticleMotion, 3);
            const float alpha = std::accumulate(coverage.begin(), coverage.end(), 0.0f);
            if (mode == 0) Require(alpha == 0, "Noncolliding particles should pass behind the test plane");
            else
            {
                Require(alpha > 0, "Depth-colliding particles disappeared behind the test plane");
                const auto depths = Read(device, *resources.ParticleMotion, 2);
                const float distance = std::accumulate(depths.begin(), depths.end(), 0.0f) / alpha * 10000;
                Require(mode == 1 ? distance > 100 && distance < 130 : std::abs(distance - 147.99f) < 0.2f,
                    "Particle depth collision differs from independent bounce/stop oracle");
            }
        }
        CheckParticleSimulationControls(device, resources, particles, executeParticles, *depth, eye, forward);
        std::vector<float> queuedReference;
        for (bool async : {false, true, false})
        {
            particleAsync = async;
            emitter = {}; emitter.Id = 1901; emitter.MaxParticles = 16; emitter.BurstTotal = 1;
            emitter.ResetRevision = async ? 2 : (queuedReference.empty() ? 1 : 3);
            emitter.EmissionRate = 0; emitter.SpeedMin = emitter.SpeedMax = 10;
            emitter.Direction = forward; emitter.ConeAngleDegrees = 0; emitter.Acceleration = Vector3::Zero;
            emitter.World = Matrix::CreateTranslation(eye + forward * 100);
            emitter.LifetimeMin = emitter.LifetimeMax = 10; emitter.SizeStart = emitter.SizeEnd = 10;
            emitter.SoftDistance = 0; emitter.ColorStart = emitter.ColorEnd = Vector4(0,0,0,1);
            emitter.Blend = ParticleBlend::Additive;
            flushParticles = false;
            for (UINT frame = 0; frame < 10; ++frame)
            {
                resources.CurrFrameConstantsIndex = frame % resources.FrameConstants.size();
                const auto fence = resources.FrameConstants[resources.CurrFrameConstantsIndex]->FenceValue;
                queue->CPUWaitForFenceValue(fence);
                simulationOnly = frame >= 3 && frame <= 6;
                Set(L"Render.Particles.DrawEnabled", simulationOnly ? "false" : "true");
                emitter.DeltaTime = frame ? 0.1f : 0;
                resources.ParticleEmitters = {emitter};
                executeParticles(particles);
            }
            queue->Flush();
            const auto pixels = Read(device, *resources.ParticleMotion, 2);
            if (queuedReference.empty()) queuedReference = pixels;
            else Require(pixels == queuedReference, "Queued particle async/direct result differs");
        }
        std::cout << "Particle direct/async/direct: queued frame slots, simulation-only joins and toggles passed.\n";
        particleAsync = asyncMode; flushParticles = true; simulationOnly = false; resources.CurrFrameConstantsIndex = 0;
        emitter = {}; emitter.Id = 1; emitter.World = Matrix::CreateTranslation(eye + forward * 100);
        emitter.EmissionRate = 0; emitter.SpeedMin = emitter.SpeedMax = 0; emitter.Acceleration = Vector3::Zero;
        emitter.LifetimeMin = emitter.LifetimeMax = 0.15f; emitter.DeltaTime = 0.1f;
        emitter.SizeStart = emitter.SizeEnd = 10; emitter.SizeVariation = 0; emitter.SoftDistance = 0;
        emitter.ColorStart = emitter.ColorEnd = Vector4(0, 0, 0, 1); emitter.Blend = ParticleBlend::Additive;
        for (UINT count : {250000u, 1000000u})
        {
            emitter.MaxParticles = count; emitter.BurstTotal = 0; emitter.ResetRevision = count;
            for (UINT frame = 0; frame < 3; ++frame)
            {
                emitter.BurstTotal += count; resources.ParticleEmitters = {emitter}; executeParticles(particles);
                const auto coverage = Read(device, *resources.ParticleMotion, 3);
                Require(*std::max_element(coverage.begin(), coverage.end()) == static_cast<float>(count),
                    "GPU refill must maintain the exact 250k/1M live count through particle deaths");
            }
        }
        std::cout << "GPU particle refill: exact 250000/1000000 live particles from first frame and after expiry passed.\n";
        resources.ParticleEmitters.clear(); executeParticles(particles); Require(!resources.ParticleMotion, "Removed emitter retained motion");
        Set(L"Render.Fluid.Enabled", "false"); executeWater(); requireWaterFallback();
        std::cout << "GPU particle spawn/indirect/three blend modes/reset/removal and depth collision off/bounce/stop passed.\n";
        if (!benchmark) CheckWaterComposition(device, resources, common, *depth, *motion, environment, makeColor, execute, eye, forward);
        if (benchmark && (mode == "--benchmark" || mode == "--benchmark-particles"))
        {
            Set(L"Render.Particles.Enabled", "true");
            BenchmarkParticlePlacement(device, resources, common, depth, hdr.get(), accum.get(), reveal.get());
        }
        ThrowIfFailed(device.GetDevice()->GetDeviceRemovedReason());
        for (UINT64 i = 0; info && i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        queue->Flush(); grassRenderer.reset(); GDX12ShaderCompiler::Shutdown();
        ReleaseMutex(gate); CloseHandle(gate);
        std::cout << (benchmark ? "Component placement benchmarks passed without the debug layer.\n" :
            "Six-component GPU integration passed with no debug-layer errors.\n");
        return 0;
    }
    catch (const DxException& e) { std::wcerr << e.ToString() << '\n'; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    if (diagnosticInfo)
        for (UINT64 i = 0; i < diagnosticInfo->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0; diagnosticInfo->GetMessage(i, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(diagnosticInfo->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                std::cerr << message->pDescription << '\n';
        }
    return 1;
}
