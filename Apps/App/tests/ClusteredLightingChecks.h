#pragma once
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.Core/ConsoleModule.h"
#include <algorithm>
#include <cstring>
#include <chrono>

static void CheckLightGridSettings()
{
    ConsoleModule console;
    const auto original = GetLightGridSettings();
    const auto path = std::filesystem::path(SOLUTION_DIR) / "Build/FrustumLightGrid/settings.ini";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream ini(path);
        ini << "[ConsoleVariables]\nRender.Lighting.Cluster.TileSize=48\nRender.Lighting.Cluster.DepthSlices=24\nRender.Lighting.Cluster.FarDepthCm=50000\n";
    }
    Require(console.LoadIni(path).Error.empty(), "Light grid INI failed to load");
    const auto loaded = GetLightGridSettings();
    Require(loaded.TileSize == 48 && loaded.DepthSlices == 24 && loaded.FarDepthCm == 50000, "INI did not configure the light grid");
    for (const auto name : { L"Render.Lighting.Cluster.TileSize", L"Render.Lighting.Cluster.DepthSlices", L"Render.Lighting.Cluster.FarDepthCm" })
    {
        auto& variable = console.FindConsoleVariable(name);
        Require(variable.GetFlags() == EConsoleVariableFlags::ReadOnly, "Light grid setting must be startup-only");
        Require(!variable.SetFromString("0") && !variable.SetFromString("nan"), "Invalid light grid setting accepted");
    }
    Require(console.FindConsoleVariable(L"Render.Lighting.Cluster.TileSize").SetFromString(std::to_string(original.TileSize)), "Cannot restore grid tile size");
    Require(console.FindConsoleVariable(L"Render.Lighting.Cluster.DepthSlices").SetFromString(std::to_string(original.DepthSlices)), "Cannot restore grid slice count");
    Require(console.FindConsoleVariable(L"Render.Lighting.Cluster.FarDepthCm").SetFromString(std::to_string(original.FarDepthCm)), "Cannot restore grid depth");
}

static void CheckLightGridCoverage(JobSystem& oneThread, JobSystem& fourThreads)
{
    Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, 1.7f, 1, 10000);
    projection._31 = 0.017f; projection._32 = -0.011f;
    const Matrix view = Matrix::CreateLookAt(Vector3(100, -30, 400), Vector3(50, 20, 0), Vector3::UnitY);
    const auto inverse = view.Invert();
    std::vector<GDX12LocalLightConstants> lights(257);
    for (UINT i = 0; i < lights.size(); ++i)
    {
        const float z = std::exp2(float(i % 14));
        lights[i].Position = Vector3::Transform(Vector3((int(i % 7) - 3) * z * 0.3f, (int(i % 5) - 2) * z * 0.2f, -z), inverse);
        lights[i].Range = z * 0.75f;
    }
    GDX12LightGridBuildSystem builder;
    GDX12LightGrid serial, parallel;
    for (const auto settings : { GDX12LightGridSettings{ 8, 1, 100 }, { 64, 32, 10000 }, { 96, 63, 500 } })
    {
        builder.Build(serial, projection, view, 1, 0, 641, 377, lights, &oneThread, settings);
        builder.Build(parallel, projection, view, 1, 0, 641, 377, lights, &fourThreads, settings);
        Require(serial.Masks == parallel.Masks, "Grid data changes with thread count");
        for (UINT id = 0; id < lights.size(); ++id)
            for (UINT sample = 0; sample < 128; ++sample)
            {
                const float angle = sample * 2.399963f, nz = 1 - 2 * (sample + 0.5f) / 128;
                const float xy = std::sqrt(1 - nz * nz);
                const Vector3 point = Vector3(lights[id].Position) + Vector3(xy * std::cos(angle), xy * std::sin(angle), nz) * lights[id].Range * 0.999f;
                const Vector3 camera = Vector3::Transform(point, view);
                if (-camera.z < 1) continue;
                const auto ndc = Vector3::Transform(camera, projection);
                if (std::abs(ndc.x) >= 1 || std::abs(ndc.y) >= 1) continue;
                const UINT x = (std::min)(UINT((ndc.x + 1) * 0.5f * 641) / settings.TileSize, serial.Constants.Size.x - 1);
                const UINT y = (std::min)(UINT((1 - ndc.y) * 0.5f * 377) / settings.TileSize, serial.Constants.Size.y - 1);
                const size_t address = ((size_t(serial.DepthSlice(-camera.z)) * serial.Constants.Size.y + y) * serial.Constants.Size.x + x) * serial.Constants.Size.w + id / 32;
                Require((serial.Masks[address] & (1u << (id & 31))) != 0, "A light sphere sample is absent from its grid cell");
            }
    }
    std::cout << "CPU frustum grid: INI/parallel identity, sphere coverage, jitter, moved camera, variable grid, infinite far passed\n";
}

// Included by the lighting smoke to reuse its HDR readback and populated environment.
static void CheckClusteredLighting(GDX12DeviceResources& resources, RenderPipelineCommonData& common,
    const std::vector<IRenderPassLink*>& sceneInputs, bool benchmark)
{
    auto& device = *resources.Device;
    auto* queue = device.GetCommandQueue();
    auto& frame = resources.FrameConstants[resources.CurrFrameConstantsIndex];
    JobSystem oneThread(1), fourThreads(4);
    CheckLightGridCoverage(oneThread, fourThreads);
    auto* previousJobs = common.Jobs;
    common.Jobs = &fourThreads;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options = {};
    ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
    const bool waves = options.WaveOps && device.GetDeviceFeatures().Features.HighestShaderModel() >= D3D_SHADER_MODEL_6_0;
    std::cout << "Clustered GPU checks: " << device.GetDeviceFeatures().Name << ", WaveOps=" << waves << '\n';

    // Must match the production signature so a test PSO can use the pass's bound inputs.
    GDX12RootSignatureDesc desc;
    desc.NumSingleCBVSlots = 5; desc.NumSingleSRVSlots = 18; desc.NumSingleUAVSlots = 1;
    desc.StaticSamplers = GetStaticSamplers();
    desc.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    desc.StaticSamplers.push_back(shadowSampler);
    GDX12RootSignature root(&device, desc);
    const D3D_SHADER_MACRO referenceDefines[] = { { "DEFERRED_REFERENCE", "1" }, { nullptr, nullptr } };
    const D3D_SHADER_MACRO scalarDefines[] = { { "DEFERRED_WAVE_OPS", "0" }, { nullptr, nullptr } };
    auto reference = GDX12RenderHelpers::CreateComputePipeline(&device, &root, SHADERS_FOLDER "DeferredLighting.hlsl", "CS", referenceDefines);
    auto scalar = GDX12RenderHelpers::CreateComputePipeline(&device, &root, SHADERS_FOLDER "DeferredLighting.hlsl", "CS", scalarDefines);
    const auto baselinePath = std::filesystem::path(SOLUTION_DIR) / "Apps/App/tests/shaders/DepthAdaptiveLighting.hlsl";
    const D3D_SHADER_MACRO baselineDefines[] = { { "DEFERRED_WAVE_OPS", waves ? "1" : "0" }, { nullptr, nullptr } };
    auto baseline = GDX12RenderHelpers::CreateComputePipeline(&device, &root, baselinePath.c_str(), "CS", baselineDefines);
    GDX12LightGridBuildSystem serialBuild, parallelBuild;
    GDX12LightGrid serialGrid, parallelGrid;

    const auto upload = [&](GDX12Texture& texture, const std::vector<float>& pixels, UINT channels)
    {
        const auto textureDesc = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        device.GetDevice()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> staging;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
        void* data;
        ThrowIfFailed(staging->Map(0, nullptr, &data));
        for (UINT y = 0; y < texture.GetHeight(); ++y)
            std::memcpy(static_cast<BYTE*>(data) + footprint.Offset + y * footprint.Footprint.RowPitch,
                pixels.data() + size_t(y) * texture.GetWidth() * channels, size_t(texture.GetWidth()) * channels * sizeof(float));
        staging->Unmap(0, nullptr);
        auto* list = queue->GetCommandList();
        list->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION source(staging.Get(), footprint), target(texture.GetResource()->D3DResource.Get(), 0);
        list->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        list->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(list);
        queue->Flush();
    };
    const auto compare = [](const std::vector<float>& actual, const std::vector<float>& expected)
    {
        Require(actual.size() == expected.size(), "Clustered HDR dimensions differ from reference");
        for (size_t i = 0; i < actual.size(); ++i)
            Require(std::abs(actual[i] - expected[i]) <= 0.002f * (std::max)(1.0f, expected[i]),
                "Clustered lighting dropped or duplicated a light relative to brute force");
    };
    GDX12GPUTimer timer(&device);
    for (const auto size : { XMUINT2(1, 1), XMUINT2(17, 19), XMUINT2(127, 73), XMUINT2(320, 180), XMUINT2(1280, 720) })
    {
        const UINT width = size.x, height = size.y;
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, float(width) / height, 1, 100000);
        projection._33 = 1.0f / 99999; projection._43 = 100000.0f / 99999;
        GDX12CameraConstants camera = {};
        camera.View = Matrix::Identity;
        camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
        camera.InvViewProj = projection.Invert().Transpose();
        camera.NearPlane = 1; camera.FarPlane = 100000;
        frame->CameraCB->CopyData(0, camera);
        common.ActiveCameraCBufferIndex = 0;
        std::array<GDX12Texture, 6> gbuffer;
        for (UINT i = 0; i < gbuffer.size(); ++i)
        {
            GDX12TextureDesc texture;
            texture.Width = width; texture.Height = height;
            texture.Format = texture.SRVDesc.Format = i >= 4 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
            texture.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
            texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            texture.SRVDesc.Texture2D.MipLevels = 1;
            gbuffer[i].Initialize(texture);
        }
        auto inputs = sceneInputs;
        for (UINT i = 0; i < 5; ++i) inputs[i] = &gbuffer[i];
        inputs[11] = &gbuffer[5];
        GDX12DeferredLightingPass clustered;
        clustered.Setup(&resources, nullptr, &common);
        clustered.SetInputs(inputs);
        clustered.Initialize();
        GDX12RenderGraph graph(&device);
        graph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene");
        graph.ImportShared(GDX12RenderGraph::SharedResource::Environment, "Environment");
        graph.ImportShared(GDX12RenderGraph::SharedResource::ReflectionProbes, "Probes");
        for (auto* input : inputs) graph.ImportLink("Input", input);
        graph.ImportLink("HDR", clustered.GetOutput(0));
        graph.AddPass(clustered);
        graph.Compile(false);
        auto* output = clustered.GetOutput(0)->GetTexture();
        const auto bindPrevious = [&](GDX12CommandList* list)
        {
            list->SetComputeRootSignature(&root);
            list->SetPipelineState(baseline.Get());
            list->SetDescriptorHeaps({ resources.SRV_UAV_Heap.get() });
            list->SetComputeRootConstantBufferView(0, frame->MainCB->GetElementAddress(0));
            list->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(common.ActiveCameraCBufferIndex));
            list->SetComputeRootConstantBufferView(2, frame->ShadowCB->GetElementAddress(0));
            list->SetComputeRootConstantBufferView(3, frame->SkyCB->GetElementAddress(0));
            for (UINT i = 0; i < inputs.size(); ++i)
                list->SetComputeSRV(i < 11 ? i : 14, inputs[i]->GetTexture()->GetSRV()->GPUHandle);
            list->SetComputeSRV(11, frame->LocalLightCache->GetSRV()->GPUHandle);
            list->SetComputeSRV(12, resources.ReflectionRadianceSRV.GPUHandle);
            list->SetComputeSRV(13, frame->ReflectionProbes->GetSRV()->GPUHandle);
            list->SetComputeSRV(15, resources.SkyTransmittanceSRV.GPUHandle);
            list->SetComputeSRV(16, resources.SkyViewSRV.GPUHandle);
            list->SetComputeUAV(0, output->GetUAV()->GPUHandle);
        };
        const auto render = [&](ID3D12PipelineState* variant, UINT repeats = 1)
        {
            clustered.Prepare();
            auto* list = queue->GetCommandList();
            graph.Execute(list);
            if (variant) list->SetPipelineState(variant);
            timer.Start(list);
            const auto cpuStart = std::chrono::steady_clock::now();
            for (UINT repeat = 0; repeat < repeats; ++repeat)
            {
                list->ResourceBarrier({ output->GetResource()->GetUAVBarrier() });
                if (!variant) graph.Execute(list);
                else
                {
                    if (variant == baseline.Get()) bindPrevious(list);
                    const UINT groupSize = variant == baseline.Get() ? 16 : 8;
                    list->Dispatch((width + groupSize - 1) / groupSize, (height + groupSize - 1) / groupSize, 1);
                }
            }
            const double cpuMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count() / repeats;
            timer.Stop(list);
            queue->ExecuteCommandList(list);
            timer.OnCommandListExecuted(queue->FenceValue);
            queue->Flush();
            return Vector2(float(cpuMS), float(timer.GetTimeMS() / repeats));
        };
        for (UINT shape = 0; shape < 5; ++shape)
        {
            // Exercise jitter and an infinite reverse-Z far plane with the same culling path.
            projection._31 = shape == 3 ? 0.013f : 0;
            projection._32 = shape == 3 ? -0.017f : 0;
            projection._33 = shape == 4 ? 0 : 1.0f / 99999;
            projection._43 = shape == 4 ? 1 : 100000.0f / 99999;
            camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
            camera.InvViewProj = projection.Invert().Transpose();
            camera.FarPlane = shape == 4 ? 0.0f : 100000.0f;
            frame->CameraCB->CopyData(0, camera);
            common.CameraProjection = projection;
            common.CameraView = Matrix::Identity;
            common.ActiveCameraNearPlane = camera.NearPlane;
            common.ActiveCameraFarPlane = camera.FarPlane;
            std::array<std::vector<float>, 6> pixels;
            for (UINT i = 0; i < pixels.size(); ++i) pixels[i].resize(size_t(width) * height * (i >= 4 ? 1 : 4));
            std::vector<float> depths(size_t(width) * height);
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                {
                    const size_t p = size_t(y) * width + x, c = p * 4;
                    const float z = shape == 1 ? 256.0f : shape == 4 ? (x % 2 ? 1.001f : 80000.0f) :
                        std::exp2(float((x / 2 + y / 3) % 12) + 1);
                    depths[p] = z;
                    pixels[0][c] = 0.7f; pixels[0][c + 1] = 0.4f; pixels[0][c + 2] = 0.2f;
                    pixels[1][c + 2] = 1; pixels[1][c + 3] = 0.5f;
                    pixels[2][c] = pixels[2][c + 1] = pixels[2][c + 2] = 0.04f;
                    pixels[2][c + 3] = p % 11 == 10 ? 1.0f : 0.0f;
                    pixels[4][p] = shape == 0 || (shape == 3 && p % 5 == 0) ? 0 : projection._43 / z - projection._33;
                    pixels[5][p] = 1;
                }
            for (UINT i = 0; i < pixels.size(); ++i) upload(gbuffer[i], pixels[i], i >= 4 ? 1 : 4);
            for (UINT count : { 0u, 1u, 31u, 32u, 33u, 135u, 255u, 256u, 257u, 511u, 512u, 513u, 769u })
            {
                std::vector<GDX12LocalLightConstants> lights((std::max)(1u, count));
                for (UINT i = 0; i < count; ++i)
                {
                    auto& light = lights[i];
                    const UINT x = (i * 73) % width, y = (i * 17) % height;
                    const float z = depths[size_t(y) * width + x];
                    light.Position = { (((x + 0.5f) / width * 2 - 1) + projection._31) * z / projection._11,
                        ((1 - (y + 0.5f) / height * 2) + projection._32) * z, -z * 0.9f };
                    light.Range = z * 0.3f;
                    light.Radiance = { 0.03f, 0.02f, 0.01f };
                    light.Type = i % 2 ? 2u : 1u;
                    light.Direction = { 0, 0, -1 };
                    if (shape == 1) { light.Position = { 0, 0, -128 }; light.Range = 2048; }
                    else if (i % 17 == 0) { light.Position = { 0, 0, 0 }; light.Range = 100000; }
                    else if (i % 7 == 0) light.Position.x += 1000000;
                }
                frame->LocalLightCache->Resize(static_cast<UINT>(lights.size()));
                frame->LocalLightCache->CopyData(0, lights.data(), static_cast<UINT>(lights.size()));
                GDX12MainConstants lighting;
                lighting.RenderTargetSize = { float(width), float(height) };
                lighting.LightRadiance = {}; lighting.SunShadowFlags = 0; lighting.LocalLightCount = count;
                frame->MainCB->CopyData(0, lighting);
                resources.LocalLights.assign(lights.begin(), lights.begin() + count);
                serialBuild.Build(serialGrid, projection, Matrix::Identity, camera.NearPlane, camera.FarPlane,
                    width, height, resources.LocalLights, &oneThread);
                parallelBuild.Build(parallelGrid, projection, Matrix::Identity, camera.NearPlane, camera.FarPlane,
                    width, height, resources.LocalLights, &fourThreads);
                Require(serialGrid.Masks == parallelGrid.Masks, "Parallel grid differs from one-thread grid");
                render(reference.Get());
                const auto expected = ReadHDR(device, *output);
                render(nullptr);
                compare(ReadHDR(device, *output), expected);
                render(scalar.Get());
                compare(ReadHDR(device, *output), expected);
                render(baseline.Get());
                compare(ReadHDR(device, *output), expected);
                if (benchmark && width >= 320 && (count == 135 || count == 512) && (shape == 1 || shape == 2))
                {
                    std::array<std::vector<Vector2>, 3> samples;
                    const std::array<ID3D12PipelineState*, 3> variants = { baseline.Get(), nullptr, nullptr };
                    for (UINT round = 0; round < 6; ++round)
                        for (UINT variant = 0; variant < variants.size(); ++variant)
                        {
                            common.Jobs = variant == 1 ? &oneThread : &fourThreads;
                            const auto ms = render(variants[variant], 16);
                            if (round != 0) samples[variant].push_back(ms);
                        }
                    const auto median = [&](UINT variant, bool gpu)
                    {
                        auto& values = samples[variant];
                        std::sort(values.begin(), values.end(), [gpu](Vector2 a, Vector2 b) { return (gpu ? a.y : a.x) < (gpu ? b.y : b.x); });
                        const auto v = values[values.size() / 2];
                        return gpu ? v.y : v.x;
                    };
                    std::cout << "Lighting " << width << 'x' << height << ' ' << count << " lights " << (shape == 1 ? "dense" : "depth layers")
                        << " median CPU/GPU ms: previous=" << median(0, false) << '/' << median(0, true)
                        << ", frustum 1 thread=" << median(1, false) << '/' << median(1, true)
                        << ", frustum 4 threads=" << median(2, false) << '/' << median(2, true);
                    std::cout << '\n';
                }
            }
        }
        std::cout << "Clustered " << width << 'x' << height << ": sky/flat/layers/mixed/near-far, 0..769 lights match brute force\n";
    }
    common.Jobs = previousJobs;
}
