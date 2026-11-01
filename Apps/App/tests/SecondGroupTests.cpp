#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12GrassRenderer.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FluidPass.h"
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
static void Set(const wchar_t* name, const char* value)
{
    Require(BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->FindConsoleVariable(name).SetFromString(value), "CVar write failed");
}
class TestEngine final : public App { public: TestEngine() : App(GetModuleHandleW(nullptr)) {} };

static std::vector<float> Read(GDX12Device& device, GDX12Texture& texture)
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
    const UINT channels = hdr ? 3 : 1;
    std::vector<float> result;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* pixel = static_cast<const unsigned char*>(mapped) + footprint.Offset +
                y * footprint.Footprint.RowPitch + x * (hdr ? 8 : 4);
            for (UINT c = 0; c < channels; ++c)
            {
                const float value = hdr ? PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(pixel)[c]) : *reinterpret_cast<const float*>(pixel);
                Require(std::isfinite(value), "Nonfinite GPU output");
                result.push_back(value);
            }
        }
    const D3D12_RANGE noWrite = {};
    readback->Unmap(0, &noWrite);
    return result;
}

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
        TestEngine engine;
        TestCPU();
        if (argc > 1 && std::string_view(argv[1]) == "--cpu") return 0;
        HANDLE gate = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(gate && WaitForSingleObject(gate, INFINITE) == WAIT_OBJECT_0, "GPU mutex failed");
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        if (argc > 1 && std::string_view(argv[1]) == "--gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation; ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device device; ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info));
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
        TestSkinning(device, resources);
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
        resources.Grass = std::make_unique<GDX12GrassRenderer>(&resources);
        GDX12GrassInstance grass; grass.Id = 1; grass.RootsCm = {{0, 0, 0}, {-12, 0, 0}, {12, 0, 0}};
        grass.WidthCm = 12; grass.WindId = 1;
        GDX12WindInstance wind; wind.Id = 1; wind.DeltaSeconds = 1.0f / 60;
        auto controls = GetGrassWindControls();
        list = queue->GetCommandList(); resources.Grass->BeginFrame(list, common, {grass}, {wind}, controls);
        list->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
        list->SetViewport(depth->GetViewport()); list->SetScissorRect(depth->GetScissorRect()); list->SetRenderTargets({}, depth);
        resources.Grass->Draw(list, GDX12GrassRenderer::Stage::Depth, vp, eye, resources.FrameConstants[0]->CameraCB->GetElementAddress(0));
        queue->ExecuteCommandList(list); queue->Flush();
        const auto blades = Read(device, *depth);
        Require(*std::max_element(blades.begin(), blades.end()) > 0 && resources.Grass->HasCasters(), "Grass indirect draw produced no depth/casters");
        std::cout << "GPU projected-grid wind and compact grass indirect depth passed.\n";

        GDX12FluidSurfacePass water; water.Setup(&resources, nullptr, &common); water.SetInputs({depth, motion}); water.Initialize();
        execute(water); Require(water.GetOutput(0)->GetTexture() == depth, "Empty fluid must preserve depth identity");
        GDX12FluidInstance fluid; fluid.Id = 1; fluid.DeltaSeconds = 1.0f / 60;
        GDX12FluidSettings settings; settings.ParticleRadiusCm = 8; settings.SmoothingRadiusCm = 20;
        settings.AccelerationCmPerSecond2 = Vector3::Zero; settings.FilterIterations = 1; settings.FilterRadiusPixels = 2;
        fluid.Component.SetSettings(settings);
        fluid.Component.SetParticles({{{-5,20,0},{}},{{5,20,0},{}},{{0,25,5},{}},{{0,15,-5},{}}});
        resources.Fluids = {fluid};
        for (UINT frame = 0; frame < 4; ++frame) execute(water);
        Require(water.GetOutput(0)->GetTexture() != depth, "Active fluid did not publish its depth");
        const auto waterDepth = Read(device, *water.GetOutput(0)->GetTexture());
        Require(waterDepth != blades, "Water sphere/NRF/merge produced no depth change");
        list = queue->GetCommandList(); auto* resolved = water.Resolve(list, hdr.get(), environment.GetOutput(1)->GetTexture());
        queue->ExecuteCommandList(list); queue->Flush();
        Require(resolved != hdr.get(), "Water resolve is inactive"); Read(device, *resolved);
        resources.Fluids[0].Component.Reset(); execute(water);
        std::cout << "GPU fluid hash/sort/SPH, NRF surface, resolve and reset passed.\n";

        GDX12ParticlesPass particles; particles.Setup(&resources, nullptr, &common);
        particles.SetInputs({water.GetOutput(0), accum.get(), reveal.get()}); particles.Initialize();
        GDX12ParticleEmitter emitter; emitter.Id = 1; emitter.MaxParticles = 64; emitter.BurstTotal = 16;
        emitter.EmissionRate = 0; emitter.World = Matrix::CreateTranslation(0, 30, -40);
        emitter.SpeedMin = emitter.SpeedMax = 0; emitter.SizeStart = emitter.SizeEnd = 40; emitter.Acceleration = Vector3::Zero;
        for (auto blend : {ParticleBlend::WeightedAlpha, ParticleBlend::Additive, ParticleBlend::SortedAlpha})
        {
            emitter.Blend = blend; ++emitter.ResetRevision; resources.ParticleEmitters = {emitter};
            const auto before = Read(device, *hdr);
            execute(particles); list = queue->GetCommandList(); particles.Composite(list, hdr.get());
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
        resources.ParticleEmitters.clear(); execute(particles); Require(!resources.ParticleMotion, "Removed emitter retained motion");
        Set(L"Render.Fluid.Enabled", "false"); execute(water);
        Require(water.GetOutput(0)->GetTexture() == depth, "Disabled fluid retained merged depth");
        std::cout << "GPU particle spawn/indirect/three blend modes/reset/removal passed.\n";
        ThrowIfFailed(device.GetDevice()->GetDeviceRemovedReason());
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        queue->Flush(); resources.Grass.reset(); GDX12ShaderCompiler::Shutdown();
        ReleaseMutex(gate); CloseHandle(gate);
        std::cout << "Six-component GPU integration passed with no debug-layer errors.\n";
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
