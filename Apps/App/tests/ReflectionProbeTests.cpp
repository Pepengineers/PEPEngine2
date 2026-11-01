#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"
#include "App.Base/App.h"
#include "App.Base/LoadingSettings.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <numeric>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Set(const wchar_t* name, const char* value)
{
    Require(BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->FindConsoleVariable(name).SetFromString(value),
        "Valid reflection CVar assignment rejected");
}

static void TestIni()
{
    ConsoleModule console;
    const std::filesystem::path ini = "Build/Review/reflection-settings.ini";
    std::filesystem::create_directories(ini.parent_path());
    {
        std::ofstream file(ini);
        Require(bool(file), "Cannot write reflection INI fixture");
        file << "[ConsoleVariables]\nRender.Reflection.Enabled=false\nRender.Reflection.Quality=1\n";
    }
    Require(console.LoadIni(ini).Error.empty() && !GetReflectionSettings().Enabled && GetReflectionSettings().Quality == 1,
        "Global reflection INI controls did not reach the renderer");
    Require(console.FindConsoleVariable(L"Render.Reflection.Enabled").SetFromString("true"), "Cannot restore fixture Enabled state");
}

static void TestSettings()
{
    auto console = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    const auto reject = [&](const wchar_t* name, const char* value)
    {
        Require(!console->FindConsoleVariable(name).SetFromString(value), "Invalid reflection CVar accepted");
    };
    reject(L"Render.Reflection.Quality", "4");
    GDX12ReflectionProbe author;
    author.Resolution = 32;
    author.FacesPerFrame = 3;
    author.PrefilterSamples = 24;
    Set(L"Render.Reflection.Quality", "1");
    Set(L"Render.Reflection.Quality", "3");
    auto settings = ResolveReflectionCaptureSettings(author, GetReflectionSettings().Quality);
    Require(settings.Resolution == 32 && settings.FacesPerFrame == 3 && settings.PrefilterSamples == 24,
        "Quality changed explicit overrides");
    author.Resolution = author.FacesPerFrame = author.PrefilterSamples = 0;
    const UINT resolutions[] = { 64, 128, 256 };
    const UINT faces[] = { 1, 2, 6 };
    const UINT samples[] = { 32, 64, 128 };
    for (int quality = 1; quality <= 3; ++quality)
    {
        const auto text = std::to_string(quality);
        Set(L"Render.Reflection.Quality", text.c_str());
        settings = ResolveReflectionCaptureSettings(author, GetReflectionSettings().Quality);
        Require(settings.Resolution == resolutions[quality - 1] && settings.FacesPerFrame == faces[quality - 1] &&
            settings.PrefilterSamples == samples[quality - 1], "Incorrect reflection quality preset");
    }
    Set(L"Render.Reflection.Quality", "0");
    Require(!GetReflectionSettings().Enabled, "Off must disable capture");
    Set(L"Render.Reflection.Quality", "2");
    GDX12ReflectionProbe probe;
    Require(IsValidReflectionProbe(probe), "Default author probe invalid");
    probe.BlendDistance = NAN;
    Require(!IsValidReflectionProbe(probe), "NaN author probe accepted");
    probe.BlendDistance = 10;
    probe.Position.x = 1000;
    Require(!IsValidReflectionProbe(probe), "Capture centre outside projection box accepted");
    probe.Position = Vector3::Zero;
    probe.BoxMin = Vector3(-FLT_MAX);
    probe.BoxMax = Vector3(FLT_MAX);
    Require(!IsValidReflectionProbe(probe), "Overflowing probe box span accepted");
    std::cout << "Reflection CVar validation, presets, overrides and author validation passed.\n";
}

class GpuValidationLock
{
public:
    GpuValidationLock()
    {
        _mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(_mutex != nullptr, "Cannot create feature GPU validation mutex");
        const DWORD result = WaitForSingleObject(_mutex, INFINITE);
        Require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED, "Cannot acquire feature GPU validation mutex");
    }
    ~GpuValidationLock() { ReleaseMutex(_mutex); CloseHandle(_mutex); }
private:
    HANDLE _mutex;
};

static std::vector<float> ReadTexture(GDX12Device& device, GDX12Texture& texture, UINT subresource, bool signedValues = false)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), subresource);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped = nullptr;
    D3D12_RANGE range = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool depth = desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT;
    std::vector<float> result;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(mapped) +
                footprint.Offset + y * footprint.Footprint.RowPitch + x * (hdr ? 8 : 4));
            for (UINT c = 0; c < (depth ? 1u : 3u); ++c)
            {
                const float value = depth ? *reinterpret_cast<const float*>(pixel) : hdr ? PackedVector::XMConvertHalfToFloat(pixel[c]) : reinterpret_cast<const unsigned char*>(pixel)[c] / 255.0f;
                Require(std::isfinite(value) && (signedValues || value >= 0), "Nonfinite/negative reflected HDR");
                result.push_back(value);
            }
        }
    const D3D12_RANGE noWrite = {};
    readback->Unmap(0, &noWrite);
    return result;
}

template<typename T> static std::vector<T> ReadUpload(GDX12UploadBuffer<T>& buffer)
{
    void* mapped = nullptr;
    const D3D12_RANGE range = { 0, buffer.GetElementCount() * sizeof(T) };
    ThrowIfFailed(buffer.GetResource().D3DResource->Map(0, &range, &mapped));
    std::vector<T> result(buffer.GetElementCount());
    std::memcpy(result.data(), mapped, range.End);
    const D3D12_RANGE noWrite = {};
    buffer.GetResource().D3DResource->Unmap(0, &noWrite);
    return result;
}

static Vector3 Center(const std::vector<float>& pixels, UINT size)
{
    const size_t index = (size_t(size / 2) * size + size / 2) * 3;
    return Vector3(pixels[index], pixels[index + 1], pixels[index + 2]);
}

class TestEngine final : public App
{
public:
    TestEngine() : App(GetModuleHandleW(nullptr)) {}
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        std::cout.setf(std::ios::unitbuf);
        TestIni();
        TestEngine engine;
        Require(GetLoadingBudgetBytes() > 0, "Invalid inherited loading configuration");
        TestSettings();
        if (argc > 1 && std::strcmp(argv[1], "--cpu") == 0) return 0;
        const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
        GpuValidationLock validationLock;
        ComPtr<ID3D12Debug> debug;
        if (!benchmark)
        {
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            if (!(argc > 1 && std::strcmp(argv[1], "--debug") == 0))
            {
                ComPtr<ID3D12Debug1> validation;
                ThrowIfFailed(debug.As(&validation));
                validation->SetEnableGPUBasedValidation(TRUE);
            }
        }
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        if (!benchmark) ThrowIfFailed(device.GetDevice().As(&info));
        if (info) info->ClearStoredMessages();
        auto* queue = device.GetCommandQueue();
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = 65;
        common.WindowHeight = common.DownscaledHeight = 37;
        resources.Sky.Source = SkySource::SkyBox;
        resources.Sky.Quality = 3;
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common);
        environment.Initialize();
        GDX12LocalShadowPass localShadows;
        localShadows.Setup(&resources, nullptr, &common);
        localShadows.Initialize();
        GDX12ReflectionProbePass reflections;
        reflections.Setup(&resources, nullptr, &common);
        reflections.SetInputs({ environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2),
            environment.GetOutput(3), localShadows.GetOutput(0) });
        reflections.Initialize();
        auto* commands = queue->GetCommandList();
        environment.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();

        SubMesh quad;
        quad.Vertices.resize(4);
        quad.Vertices[0].Position = Vector3(-1, -1, 0);
        quad.Vertices[1].Position = Vector3(1, -1, 0);
        quad.Vertices[2].Position = Vector3(1, 1, 0);
        quad.Vertices[3].Position = Vector3(-1, 1, 0);
        for (auto& vertex : quad.Vertices)
        {
            vertex.Normal = -Vector3::UnitZ;
            vertex.Tangent = Vector3::UnitX;
        }
        quad.Indices = { 2, 1, 0, 3, 2, 0 };
        const BoundingBox unitBounds(XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 0.001f));
        Mesh mesh({ std::move(quad) }, unitBounds);
        resources.GeometryBuffer->AddMesh(&mesh, MeshHandle(0));
        const auto* gpuMesh = resources.GeometryBuffer->GetGPUMeshByHandle(MeshHandle(0));
        const auto& part = gpuMesh->SubMeshes[0];
        resources.IndirectCommandsCache->Resize(3);
        for (UINT i = 0; i < 3; ++i)
        {
            GDX12IndirectDrawArgs draw = {};
            draw.InstanceID = draw.StartInstanceLocation = i;
            draw.InstanceCount = 1;
            draw.IndexCountPerInstance = part.IndexCount;
            draw.StartIndexLocation = part.StartIndexLocation;
            draw.BaseVertexLocation = part.StartVertexLocation;
            resources.IndirectCommandsCache->CopyData(i, draw);
        }
        GDX12Texture white;
        GDX12TextureDesc whiteDesc;
        whiteDesc.Width = whiteDesc.Height = 1;
        whiteDesc.Format = whiteDesc.SRVDesc.Format = whiteDesc.RTVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        whiteDesc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        whiteDesc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        whiteDesc.SRVDesc.Texture2D.MipLevels = 1;
        whiteDesc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        whiteDesc.SRVHeapIndex = resources.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        whiteDesc.CreateRTV = true;
        whiteDesc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        whiteDesc.RTVHeap = resources.RTVHeap.get();
        whiteDesc.RTVHeapIndex = resources.RTVHeap->GetAvailableIndex();
        whiteDesc.ClearValue = { 1, 1, 1, 1 };
        white.Initialize(whiteDesc);
        commands = queue->GetCommandList();
        commands->ResourceBarrier({ white.GetResource()->GetRenderTargetBarrier() });
        commands->ClearRenderTargetView(&white);
        commands->ResourceBarrier({ white.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
        GDX12MaterialConstants material[3];
        for (auto& entry : material)
        {
            entry.Metallic = 0;
            entry.DiffuseIndex = whiteDesc.SRVHeapIndex - Texture2D_StartIndex;
        }
        material[0].EmissiveColor = { 4, 0, 0 };
        material[1].EmissiveColor = { 0, 4, 0 };
        material[2].EmissiveColor = { 0, 0, 4 };
        Vector3 locations[] = { Vector3(0, 0, 100), Vector3(0, 0, 200), Vector3(100, 0, 0) };
        Vector3 scales[] = { Vector3(60), Vector3(60), Vector3(20) };
        bool dynamic[] = { false, true, false };
        resources.Lighting.SunShadowFlags = 0;
        resources.Lighting.LightRadiance = {};
        resources.ReflectionSceneBounds = BoundingBox(XMFLOAT3(0, 0, 100), XMFLOAT3(200, 200, 200));
        for (auto& frame : resources.FrameConstants)
        {
            frame->TransformCache->Resize(3);
            frame->InstanceCache->Resize(3);
            frame->MaterialCache->Resize(3);
        }
        const auto upload = [&]()
        {
            auto& frame = resources.FrameConstants[resources.CurrFrameConstantsIndex];
            frame->MainCB->CopyData(0, resources.Lighting);
            for (UINT i = 0; i < 3; ++i)
            {
                GDX12TransformConstants transform;
                const Matrix world = Matrix::CreateScale(scales[i]) * Matrix::CreateTranslation(locations[i]);
                XMStoreFloat4x4(&transform.WorldMatrix, world.Transpose());
                frame->TransformCache->CopyData(i, transform);
                frame->MaterialCache->CopyData(i, material[i]);
                GDX12InstanceData instance = {};
                instance.TransformIndex = instance.MaterialIndex = i;
                instance.ReflectionDynamic = dynamic[i] ? 1u : 0u;
                BoundingBox bounds;
                unitBounds.Transform(bounds, world);
                instance.BoundingBoxCenter = bounds.Center;
                instance.BoundingBoxExtents = bounds.Extents;
                frame->InstanceCache->CopyData(i, instance);
            }
        };
        GDX12ReflectionProbe probe;
        probe.Id = 1;
        resources.ReflectionProbes = { probe };
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Resolution = 32;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.FacesPerFrame = 1;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 1;
        GDX12GPUTimer timer(&device);
        UINT testFrame = 0;
        const auto render = [&](bool waitForGPU = true)
        {
            ++testFrame;
            resources.CurrFrameConstantsIndex = (resources.CurrFrameConstantsIndex + 1) % static_cast<UINT>(resources.FrameConstants.size());
            auto& frame = resources.FrameConstants[resources.CurrFrameConstantsIndex];
            if (frame->FenceValue) queue->CPUWaitForFenceValue(frame->FenceValue);
            upload();
            auto* list = queue->GetCommandList();
            localShadows.Execute(list);
            timer.Start(list);
            reflections.Execute(list);
            timer.Stop(list);
            queue->ExecuteCommandList(list);
            frame->FenceValue = list->FenceValue;
            timer.OnCommandListExecuted(list->FenceValue);
            if (waitForGPU) queue->Flush();
            if (info)
                for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
                {
                    SIZE_T size = 0;
                    info->GetMessage(i, nullptr, &size);
                    std::vector<unsigned char> storage(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    ThrowIfFailed(info->GetMessage(i, message, &size));
                    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error("Frame " + std::to_string(testFrame) + ": " + message->pDescription);
                }
            return timer.GetTimeMS();
        };
        const auto face = [&](UINT index, UINT mip = 0)
        {
            auto* texture = reflections.GetOutput(0)->GetTexture();
            const UINT mips = texture->GetResource()->D3DResource->GetDesc().MipLevels;
            return ReadTexture(device, *texture, index * mips + mip);
        };
        for (UINT i = 0; i < 5; ++i)
        {
            locations[1].x += 0.01f;
            ++resources.ReflectionDynamicRevision;
            render();
            const auto metadata = ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->ReflectionProbes);
            Require(reflections.GetStatistics().PublishedProbes == 0 && metadata[0].Intensity == 0,
                "Partial cubemap became visible or dynamic motion reset publication cursor");
        }
        render();
        Require(reflections.GetStatistics().PublishedProbes == 1, "Six captured faces must publish a generation");
        const auto original = face(4);
        const auto red = Center(original, 32);
        Require(red.x > 3.5f && red.y < 1.0f, "Static matching depth failed to hide the green dynamic object");
        for (UINT i = 0; i < 6; ++i)
        {
            render();
            Require(reflections.GetStatistics().BakedFaces == 0, "Unchanged static geometry was drawn again");
        }
        locations[1] = Vector3(0, 0, 50);
        ++resources.ReflectionDynamicRevision;
        for (UINT i = 0; i < 5; ++i)
        {
            render();
            Require(face(4) == original, "Published radiance changed before all faces completed");
        }
        render();
        const auto green = Center(face(4), 32);
        Require(green.y > 3.5f && green.x < 1.0f, "Dynamic movement did not update reflected radiance");
        const auto rough = Center(face(4, 5), 1);
        Require(rough.y < green.y * 0.9f, "GGX roughness mips did not spread the reflected emitter");
        std::cout << "Depth/overlay/atomic publication: red=" << red.x << ", green=" << green.y << ", rough=" << rough.y << "\n";

        for (auto& configuredProbe : resources.ReflectionProbes)

            configuredProbe.FacesPerFrame = 6;
        locations[1] = Vector3(0, 0, 200);
        material[0].EmissiveColor = { 0, 0, 6 };
        ++resources.ReflectionStaticRevision;
        render();
        Require(reflections.GetStatistics().BakedFaces == 6 && Center(face(4), 32).z > 5.5f,
            "Material edit did not invalidate baked color");
        render();
        Require(reflections.GetStatistics().BakedFaces == 0, "Dirty material kept invalidating the bake");
        environment.Invalidate();
        commands = queue->GetCommandList();
        environment.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        render();
        Require(reflections.GetStatistics().BakedFaces == 6, "Sky edit did not invalidate the static capture");
        ++resources.ReflectionProbes[0].Revision;
        render();
        Require(reflections.GetStatistics().BakedFaces == 6, "Explicit probe dirty was ignored");
        dynamic[0] = true;
        locations[0].z = 300;
        ++resources.ReflectionStaticRevision;
        ++resources.ReflectionDynamicRevision;
        render();
        Require(Center(face(4), 32).y > 3.5f, "Static-to-dynamic classification retained old baked depth");
        dynamic[0] = false;
        locations[0].z = 100;
        ++resources.ReflectionStaticRevision;
        render();
        Require(Center(face(4), 32).z > 5.5f, "Dynamic-to-static classification failed");
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 0;
        render();
        Require(reflections.GetStatistics().BakedFaces == 0 && Center(face(4), 32).z > 5.5f,
            "Full dynamic mode failed to capture static geometry");
        locations[0].z = 300;
        ++resources.ReflectionStaticRevision;
        render();
        Require(Center(face(4), 32).y > 3.5f, "Full dynamic mode reused stale static radiance");
        locations[0].z = 100;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 1;
        ++resources.ReflectionStaticRevision;
        render();
        Require(reflections.GetStatistics().BakedFaces == 6, "Returning to baked mode failed to rebuild its cache");
        Set(L"Render.Reflection.Enabled", "false");
        render();
        Require(reflections.GetStatistics().CapturedFaces == 0 &&
            ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->ReflectionProbes)[0].Intensity == 0,
            "Disabled probes are not neutral");
        Set(L"Render.Reflection.Enabled", "true");
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.FacesPerFrame = 1;
        render();
        common.ActiveCameraCBufferIndex = 123; // A camera cut cannot reset this camera-independent capture.
        for (UINT i = 0; i < 5; ++i) render();
        Require(reflections.GetStatistics().PublishedProbes == 1, "Camera cut reset the probe generation");
        ++resources.ReflectionProbes[0].Id;
        render();
        Require(ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->ReflectionProbes)[0].Intensity == 0,
            "World/probe replacement exposed the old scene's cubemap");
        for (UINT i = 0; i < 5; ++i) render();
        Require(reflections.GetStatistics().PublishedProbes == 1, "World replacement never published its new generation");

        GDX12RootSignatureDesc sampleRootDesc;
        sampleRootDesc.Constants.push_back(12);
        sampleRootDesc.NumSingleSRVSlots = 14;
        sampleRootDesc.NumSingleUAVSlots = 1;
        sampleRootDesc.StaticSamplers = GetStaticSamplers();
        GDX12RootSignature sampleRoot(&device, sampleRootDesc);
        auto sampleShader = GDX12ShaderCompiler::GetInstance().CompileShader(&device,
            PROJECT_DIR "tests/ReflectionProbeSampling.hlsl", nullptr, "CS", "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC sampleDesc = {};
        sampleDesc.pRootSignature = sampleRoot.GetRootSignature().Get();
        sampleDesc.CS = { sampleShader->GetBufferPointer(), sampleShader->GetBufferSize() };
        ComPtr<ID3D12PipelineState> samplePSO;
        ThrowIfFailed(device.GetDevice()->CreateComputePipelineState(&sampleDesc, IID_PPV_ARGS(&samplePSO)));
        GDX12UploadBuffer<XMFLOAT4> sampleOutput(&device, 1, EBufferType::Default, false);
        sampleOutput.CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(XMFLOAT4));
        ComPtr<ID3D12Resource> sampleReadback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
            &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sampleReadback)));
        const Vector3 global(0.25f, 0.5f, 0.75f);
        const auto sample = [&](Vector3 position, Vector3 direction, float roughness)
        {
            struct { XMFLOAT3 Position; float Roughness; XMFLOAT3 Direction; float Pad;
                XMFLOAT3 Global; float Pad2; } parameters = { position, roughness, direction, 0, global, 0 };
            auto* list = queue->GetCommandList();
            list->SetComputeRootSignature(&sampleRoot);
            list->SetPipelineState(samplePSO);
            list->SetDescriptorHeaps({ resources.SRV_UAV_Heap.get() });
            list->GetCommandList()->SetComputeRoot32BitConstants(0, 12, &parameters, 0);
            list->SetComputeSRV(12, resources.ReflectionRadianceSRV.GPUHandle);
            list->SetComputeSRV(13, resources.FrameConstants[resources.CurrFrameConstantsIndex]->ReflectionProbes->GetSRV()->GPUHandle);
            list->SetComputeUAV(0, sampleOutput.GetUAV()->GPUHandle);
            list->ResourceBarrier({ sampleOutput.GetResource().GetUnorderedAccessBarrier() });
            list->Dispatch(1, 1, 1);
            list->ResourceBarrier({ sampleOutput.GetResource().GetCopySourceBarrier() });
            list->GetCommandList()->CopyResource(sampleReadback.Get(), sampleOutput.GetResource().D3DResource.Get());
            queue->ExecuteCommandList(list);
            queue->Flush();
            void* mapped = nullptr;
            D3D12_RANGE range = { 0, sizeof(XMFLOAT4) };
            ThrowIfFailed(sampleReadback->Map(0, &range, &mapped));
            const auto value = *static_cast<XMFLOAT4*>(mapped);
            const D3D12_RANGE noWrite = {};
            sampleReadback->Unmap(0, &noWrite);
            return Vector3(value.x, value.y, value.z);
        };
        const auto centerSample = sample(Vector3::Zero, Vector3::UnitZ, 0);
        Require(centerSample.z > 5.5f, "Local reflection sampling did not replace global radiance");
        Require(Vector3::Distance(sample(Vector3(501, 0, 0), Vector3::UnitZ, 0), global) < 1e-5f,
            "Outside the probe must return global IBL exactly");
        Require(Vector3::Distance(sample(Vector3(500, 0, 0), Vector3::UnitZ, 0), global) < 1e-5f,
            "Probe edge must fade to global IBL");
        // The box exit at (0,0,500) points back to +Z from the probe, independently of surface offset.
        const Vector3 parallaxPosition(200, 0, 0);
        const Vector3 parallaxRay = Vector3(-200, 0, 500);
        Require(Vector3::Distance(sample(parallaxPosition, parallaxRay, 0), centerSample) < 0.02f,
            "Box projection did not correct parallax");
        Require(sample(Vector3::Zero, Vector3::UnitZ, 1).z < centerSample.z * 0.9f,
            "Runtime roughness lookup ignored prefiltered mips");
        auto second = resources.ReflectionProbes[0];
        second.Id += 100;
        second.Intensity = 2;
        resources.ReflectionProbes.push_back(second);
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.FacesPerFrame = 6;
        render();
        render();
        const auto overlap = sample(Vector3::Zero, Vector3::UnitZ, 0);
        Require(Vector3::Distance(overlap, centerSample * 1.5f) < 0.04f,
            "Overlapping probes were not normalized or double-counted global IBL");
        Set(L"Render.Reflection.Quality", "0");
        render();
        Require(Vector3::Distance(sample(Vector3::Zero, Vector3::UnitZ, 0), global) < 1e-5f,
            "Off must be an exact global IBL fallback");
        Set(L"Render.Reflection.Quality", "2");
        resources.ReflectionProbes.resize(1);
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 0;
        resources.Lighting.SunShadowFlags = 1;
        resources.Lighting.LightDirection = { -1, 0, 1 };
        resources.Lighting.LightRadiance = { 20, 20, 20 };
        material[0].EmissiveColor = {};
        locations[1] = Vector3(0, 0, 300);
        locations[2] = Vector3(100, 0, 0);
        ++resources.ReflectionStaticRevision;
        render();
        const auto shadowed = Center(face(4), 32);
        locations[2].x = 1000;
        ++resources.ReflectionStaticRevision;
        render();
        const auto lit = Center(face(4), 32);
        std::cout << "Off-camera sun caster: shadow=" << shadowed.x << ", lit=" << lit.x << "\n";
        Require(lit.x > shadowed.x + 1, "Probe sun shadows missed an off-camera caster");
        resources.Lighting.SunShadowFlags = 0;
        resources.Lighting.LightRadiance = {};
        material[0].EmissiveColor = { 4, 0, 0 };
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 1;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Resolution = 0;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.FacesPerFrame = 0;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.PrefilterSamples = 0;
        for (int quality = 1; quality <= 3; ++quality)
        {
            const auto qualityText = std::to_string(quality);
            Set(L"Render.Reflection.Quality", qualityText.c_str());
            const auto settings = ResolveReflectionCaptureSettings(resources.ReflectionProbes.front(), GetReflectionSettings().Quality);
            UINT publications = 0;
            for (UINT i = 0; i < 6 / settings.FacesPerFrame; ++i)
            {
                render();
                publications += reflections.GetStatistics().PublishedProbes;
            }
            Require(publications == 1, "Quality budget did not change real GPU capture scheduling");
            Require(reflections.GetOutput(0)->GetTexture()->GetWidth() == settings.Resolution,
                "Quality preset did not change GPU texture resolution");
            double totalMS = 0;
            const UINT iterations = 24;
            for (UINT i = 0; i < iterations; ++i)
            {
                totalMS += render();
                Require(reflections.GetStatistics().BakedFaces == 0, "Steady-state quality benchmark redrew the static bake");
            }
            std::cout << "Quality " << quality << ": " << settings.Resolution << "px, " << settings.FacesPerFrame
                << " faces/frame, " << settings.PrefilterSamples << " samples, textures=" << reflections.GetStatistics().TextureBytes
                << " bytes, capture+GGX GPU mean=" << totalMS / iterations << " ms\n";
            const UINT mips = reflections.GetOutput(0)->GetTexture()->GetResource()->D3DResource->GetDesc().MipLevels;
            for (UINT f = 0; f < 6; ++f)
                for (UINT mip = 0; mip < mips; ++mip) face(f, mip);
        }

        if (argc > 1 && std::strcmp(argv[1], "--capture-gpu-validation") == 0)
        {
            std::cout << "Focused probe capture/cache/shadow/GGX GPU-based validation passed; PBR/XeSS checked separately.\n";
            GDX12ShaderCompiler::Shutdown();
            return 0;
        }

        GDX12XeSSUpscalePass xess;
        xess.Setup(&resources, nullptr, &common);
        GDX12CascadedShadowPass mainShadows;
        mainShadows.Setup(&resources, nullptr, &common);
        mainShadows.Initialize();
        GDX12GPUCullingPass mainCulling;
        mainCulling.Setup(&resources, nullptr, &common);
        GDX12OpaquePass opaque;
        opaque.Setup(&resources, nullptr, &common);
        opaque.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        opaque.Initialize();
        GDX12DeferredLightingPass deferred;
        deferred.Setup(&resources, nullptr, &common);
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common);
        ao.SetInputs({ opaque.GetOutput(2), opaque.GetOutput(3) });
        ao.Initialize();
        Set(L"Render.AO.Enabled", "0");
        deferred.SetInputs({ opaque.GetOutput(0), opaque.GetOutput(3), opaque.GetOutput(4), opaque.GetOutput(5),
            opaque.GetOutput(2), mainShadows.GetOutput(0), environment.GetOutput(0), environment.GetOutput(1),
            environment.GetOutput(2), environment.GetOutput(3), localShadows.GetOutput(0), ao.GetOutput(0) });
        deferred.Initialize();
        GDX12WBOITTransparencyPass transparency;
        transparency.Setup(&resources, nullptr, &common);
        transparency.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        transparency.SetInputs({ opaque.GetOutput(2), mainShadows.GetOutput(0), environment.GetOutput(0),
            environment.GetOutput(1), environment.GetOutput(2), localShadows.GetOutput(0) });
        transparency.Initialize();
        GDX12WBOITCompositionPass composition;
        composition.Setup(&resources, nullptr, &common);
        composition.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        composition.SetInputs({ deferred.GetOutput(0), transparency.GetOutput(0), transparency.GetOutput(1) });
        composition.Initialize();
        xess.SetInputs(std::vector<IRenderPassLink*>{ opaque.GetOutput(2), opaque.GetOutput(1), composition.GetOutput(0) });
        xess.Initialize();
        GDX12Texture display;
        auto displayDesc = whiteDesc;
        displayDesc.Width = common.WindowWidth;
        displayDesc.Height = common.WindowHeight;
        displayDesc.Format = displayDesc.SRVDesc.Format = displayDesc.RTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        displayDesc.SRVHeapIndex = resources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        displayDesc.RTVHeapIndex = resources.RTVHeap->GetAvailableIndex();
        display.Initialize(displayDesc);
        GDX12OutputToScreenPass output;
        output.Setup(&resources, nullptr, &common);
        output.SetInputs({ xess.GetOutput(0), &display });
        output.Initialize();
        for (auto& frame : resources.FrameConstants)
        {
            frame->CameraCB->Resize(1);
            frame->CameraVisibilityCommands.emplace_back();
            auto& visibility = frame->CameraVisibilityCommands[0];
            const auto create = [&](auto& buffer, UINT count)
            {
                using Buffer = typename std::remove_reference_t<decltype(buffer)>::element_type;
                buffer = std::make_unique<Buffer>(&device, count, EBufferType::Default, false);
                buffer->CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            };
            create(visibility.VisibleOpaqueCommandsCache, 3);
            create(visibility.VisibleTransparentCommandsCache, 3);
            create(visibility.OpaqueDrawCounter, 1);
            create(visibility.TransparentDrawCounter, 1);
        }
        common.ActiveCameraCBufferIndex = 0;
        resources.ReflectionProbes[0].Intensity = 3;
        material[0].Metallic = 1;
        material[0].Roughness = 0.1f;
        material[0].EmissiveColor = {};
        const auto surface = [&](bool enabled)
        {
            Set(L"Render.Reflection.Enabled", enabled ? "true" : "false");
            render();
            auto& frame = resources.FrameConstants[resources.CurrFrameConstantsIndex];
            const Matrix view = Matrix::CreateLookAt(Vector3(0, 0, -100), Vector3(0, 0, 100), Vector3::UnitY);
            Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2,
                float(common.WindowWidth) / common.WindowHeight, 1, 10000);
            projection._33 = 1.0f / 9999;
            projection._43 = 10000.0f / 9999;
            GDX12CameraConstants camera = {};
            XMStoreFloat4x4(&camera.View, view.Transpose());
            XMStoreFloat4x4(&camera.ViewProj, (view * projection).Transpose());
            XMStoreFloat4x4(&camera.InvViewProj, (view * projection).Invert().Transpose());
            camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = camera.ViewProj;
            camera.CameraLocation = { 0, 0, -100 };
            camera.NearPlane = 1;
            camera.FarPlane = 10000;
            frame->CameraCB->CopyData(0, camera);
            auto* list = queue->GetCommandList();
            mainShadows.Execute(list);
            mainCulling.Execute(list);
            opaque.Execute(list);
            ao.Execute(list);
            deferred.Execute(list);
            transparency.Execute(list);
            composition.Execute(list);
            xess.Execute(list);
            output.Execute(list);
            queue->ExecuteCommandList(list);
            queue->Flush();
            Require(!common.ResetHistory, "XeSS did not consume the requested temporal reset");
        };
        surface(false);
        const auto opaqueOff = ReadTexture(device, *deferred.GetOutput(0)->GetTexture(), 0);
        // Compare the shared-depth path against the independently rendered full G-buffer.
        const auto referenceDepth = ReadTexture(device, *opaque.GetOutput(2)->GetTexture(), 0);
        const auto referenceNormals = ReadTexture(device, *opaque.GetOutput(3)->GetTexture(), 0, true);
        const auto referenceAlbedo = ReadTexture(device, *opaque.GetOutput(0)->GetTexture(), 0);
        GDX12OpaquePass prepass(GDX12OpaquePass::Stage::DepthPrepass);
        prepass.Setup(&resources, nullptr, &common);
        prepass.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        prepass.Initialize();
        GDX12OpaquePass gbuffer(GDX12OpaquePass::Stage::GBuffer);
        gbuffer.Setup(&resources, nullptr, &common);
        gbuffer.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        gbuffer.SetInputs({ prepass.GetOutput(2) });
        gbuffer.Initialize();
        for (bool early : { true, false })
        {
            common.AOAfterDepthPrepass = early;
            auto* list = queue->GetCommandList();
            auto* normals = prepass.GetOutput(3)->GetTexture();
            list->ResourceBarrier({ normals->GetResource()->GetRenderTargetBarrier() });
            list->ClearRenderTargetView(normals);
            prepass.Execute(list);
            gbuffer.Execute(list);
            queue->ExecuteCommandList(list);
            queue->Flush();
            Require(ReadTexture(device, *prepass.GetOutput(2)->GetTexture(), 0) == referenceDepth,
                "Depth prepass differs from full opaque depth");
            const auto prepassNormals = ReadTexture(device, *normals, 0, true);
            if (early) Require(prepassNormals == referenceNormals, "Early AO normals differ from the G-buffer");
            else Require(std::all_of(prepassNormals.begin(), prepassNormals.end(), [](float v) { return v == 0; }),
                "Late AO wrote normals during the depth-only prepass");
            Require(ReadTexture(device, *gbuffer.GetOutput(3)->GetTexture(), 0, true) == referenceNormals &&
                ReadTexture(device, *gbuffer.GetOutput(0)->GetTexture(), 0) == referenceAlbedo,
                "Equal-depth G-buffer differs from the independently rendered opaque pass");
        }
        std::cout << "Depth-only and depth+normal prepasses match full opaque; late mode leaves normals untouched; Equal G-buffer matches.\n";

        surface(true);
        const auto opaqueOn = ReadTexture(device, *deferred.GetOutput(0)->GetTexture(), 0);
        const auto increase = [](const auto& before, const auto& after)
        {
            float maximum = 0;
            for (size_t i = 0; i < before.size(); ++i) maximum = (std::max)(maximum, after[i] - before[i]);
            return maximum;
        };
        Require(increase(opaqueOff, opaqueOn) > 0.01f, "Deferred opaque PBR did not consume local probe radiance");
        material[0].RenderLayer = 2;
        material[0].Opacity = 0.5f;
        ++resources.ReflectionStaticRevision;
        surface(false);
        const auto glassOff = ReadTexture(device, *transparency.GetOutput(0)->GetTexture(), 0);
        surface(true);
        const auto glassOn = ReadTexture(device, *transparency.GetOutput(0)->GetTexture(), 0);
        Require(increase(glassOff, glassOn) > 0.001f, "Transparent PBR did not consume local probe radiance");
        material[0].RenderLayer = 0;
        material[0].EmissiveColor = { 6, 6, 6 };
        common.ResetHistory = true;
        surface(false);
        const auto hdr = ReadTexture(device, *composition.GetOutput(0)->GetTexture(), 0);
        Require(*std::max_element(hdr.begin(), hdr.end()) > 5.5f, "HDR reflection composition clipped before XeSS");
        const auto upscaled = ReadTexture(device, *xess.GetOutput(0)->GetTexture(), 0);
        const auto ldr = ReadTexture(device, display, 0);
        const size_t centerIndex = (size_t(common.WindowHeight / 2) * common.WindowWidth + common.WindowWidth / 2) * 3;
        const float linear = upscaled[centerIndex] / (1 + upscaled[centerIndex]);
        const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
        Require(std::abs(ldr[centerIndex] - encoded) < 2.0f / 255,
            "Final output must tone map and encode exactly once after HDR XeSS");
        common.WindowWidth = 129;
        common.WindowHeight = 73;
        xess.Resize();
        xess.QueryRenderTargetResolution();
        opaque.Resize();
        ao.Resize();
        deferred.Resize();
        transparency.Resize();
        composition.Resize();
        display.Resize(common.WindowWidth, common.WindowHeight);
        surface(false);
        ReadTexture(device, display, 0);
        std::cout << "Opaque/transparent PBR, FP16 HDR composition, XeSS reset/odd resize and final tone map passed.\n";
        Set(L"Render.Reflection.Enabled", "true");
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 0;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.FacesPerFrame = 1;
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Resolution = 64;
        for (UINT i = 0; i < 6; ++i) render(false);
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Resolution = 128;
        for (UINT i = 0; i < 6; ++i) render(false);
        queue->Flush();
        Require(reflections.GetStatistics().PublishedProbes == 1 && Center(face(4), 128).x > 5.5f,
            "Queued frame slots or resolution replacement corrupted publication");
        for (auto& configuredProbe : resources.ReflectionProbes)
            configuredProbe.Implementation = 1;
        render(false);
        std::cout << "Queued frame-slot publication, resize and mode switch passed.\n";

        auto firstProbe = resources.ReflectionProbes[0];
        firstProbe.Resolution = 32;
        firstProbe.FacesPerFrame = 1;
        firstProbe.PrefilterSamples = 8;
        firstProbe.Implementation = 0;
        auto secondProbe = firstProbe;
        ++secondProbe.Id;
        secondProbe.Resolution = 64;
        secondProbe.FacesPerFrame = 2;
        secondProbe.PrefilterSamples = 16;
        secondProbe.Implementation = 1;
        resources.ReflectionProbes = { firstProbe, secondProbe };
        for (UINT frame = 0; frame < 12; ++frame) render();
        const auto mixedData = ReadUpload(*resources.FrameConstants[resources.CurrFrameConstantsIndex]->ReflectionProbes);
        Require(mixedData[0].Intensity > 0 && mixedData[1].Intensity > 0 &&
            mixedData[0].MinMip == 1 && mixedData[1].MinMip == 0,
            "Different per-probe resolutions did not publish into their own mip ranges");
        Require(Center(face(4, 1), 32).x > 5.5f && Center(face(10, 0), 64).x > 5.5f,
            "Mixed dynamic/baked probes lost their independently sized captured radiance");
        std::cout << "Two real probes with independent resolution/mode/budget/samples published correct radiance.\n";

        // Frame-local metadata must remain neutral after disable in every buffered frame slot.
        Set(L"Render.Reflection.Enabled", "false");
        for (size_t i = 0; i < resources.FrameConstants.size(); ++i) render(false);
        queue->Flush();
        for (const auto& frame : resources.FrameConstants)
            for (const auto& entry : ReadUpload(*frame->ReflectionProbes))
                Require(entry.Intensity == 0, "Disabled probe metadata leaked into another frame slot");
        if (info)
        {
            for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
            {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                std::vector<unsigned char> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                ThrowIfFailed(info->GetMessage(i, message, &size));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error("Frame " + std::to_string(testFrame) + ": " + message->pDescription);
            }
        }
        std::cout << "Reflection GPU depth, radiance, cache, publication, parallax, overlap, roughness, shadows and runtime switches passed.\n";
        GDX12ShaderCompiler::Shutdown();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
