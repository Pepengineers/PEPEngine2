#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <random>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void ShadowUploadSlots(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.GetDevice().As(&info))) info->ClearStoredMessages();
    auto& templates = *resources.IndirectCommandsCache;
    for (auto& frame : resources.FrameConstants) frame->InstanceCache->Resize(1);
    {
        GDX12UploadBuffer<UINT> upload(&device, 1, EBufferType::Upload, false);
        GDX12UploadBuffer<UINT> target(&device, 1, EBufferType::Default, false);
        upload.CopyData(0, 123u);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)),
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
        auto* queue = device.GetCommandQueue();
        auto* list = queue->GetCommandList();
        const auto toCopy = target.GetResource().GetCopyDestBarrier();
        list->ResourceBarrier({ target.GetResource().GetCopyDestBarrier(), toCopy });
        list->GetCommandList()->CopyBufferRegion(target.GetResource().D3DResource.Get(), 0,
            upload.GetResource().D3DResource.Get(), 0, sizeof(UINT));
        list->ResourceBarrier({ target.GetResource().GetCopySourceBarrier() });
        list->ResourceBarrier({ target.GetResource().GetCopySourceBarrier() });
        list->ResourceBarrier({});
        list->GetCommandList()->CopyBufferRegion(readback.Get(), 0, target.GetResource().D3DResource.Get(), 0, sizeof(UINT));
        queue->ExecuteCommandList(list); queue->Flush();
        void* data = nullptr;
        ThrowIfFailed(readback->Map(0, nullptr, &data));
        Require(*static_cast<UINT*>(data) == 123u, "Mixed/no-op barrier filtering lost an ordered buffer copy");
        readback->Unmap(0, nullptr);
    }
    GDX12IndirectDrawArgs draw = {};
    draw.IndexCountPerInstance = 3; draw.InstanceCount = 1;
    for (UINT capacity : { 1u, 9u })
    {
        templates.Resize(capacity);
        templates.CopyData(capacity - 1, draw);
        const auto revision = templates.GetRevision();
        templates.CopyData(capacity - 1, draw);
        Require(templates.GetRevision() == revision, "Unchanged templates must not invalidate GPU copies");
        for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
        {
            resources.CurrFrameConstantsIndex = slot;
            auto& frame = *resources.FrameConstants[slot];
            resources.PrepareLocalShadowData();
            Require(frame.ShadowTemplatesDirty, "Template edits/growth must reach every frame slot");
            auto* queue = device.GetCommandQueue();
            auto* list = queue->GetCommandList();
            resources.UploadLocalShadowData(list);
            const auto bytes = UINT64(capacity) * sizeof(draw);
            auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
            auto& gpu = frame.ShadowTemplates->GetResource();
            D3D12_HEAP_PROPERTIES heap = {};
            ThrowIfFailed(gpu.D3DResource->GetHeapProperties(&heap, nullptr));
            Require(heap.Type == D3D12_HEAP_TYPE_DEFAULT, "Shadow templates must reside in DEFAULT memory");
            list->ResourceBarrier({ gpu.GetCopySourceBarrier() });
            list->GetCommandList()->CopyBufferRegion(readback.Get(), 0, gpu.D3DResource.Get(), 0, bytes);
            queue->ExecuteCommandList(list); queue->Flush();
            void* mapped = nullptr;
            ThrowIfFailed(readback->Map(0, nullptr, &mapped));
            Require(std::memcmp(static_cast<GDX12IndirectDrawArgs*>(mapped) + capacity - 1, &draw, sizeof(draw)) == 0,
                "Frame-local template staging lost an edit");
            readback->Unmap(0, nullptr);
            resources.PrepareLocalShadowData();
            Require(!frame.ShadowTemplatesDirty, "Unchanged slots must reuse their DEFAULT templates");
        }
        draw = {}; // Deletion is an empty command and must invalidate every slot too.
        templates.CopyData(capacity - 1, draw);
        for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
        {
            resources.CurrFrameConstantsIndex = slot;
            resources.PrepareLocalShadowData();
            Require(resources.FrameConstants[slot]->ShadowTemplatesDirty, "Deletion must invalidate every slot");
        }
        draw.IndexCountPerInstance = 6; draw.InstanceCount = 1;
    }
    GDX12IndirectDrawArgs first = {}, second = {};
    first.InstanceCount = second.InstanceCount = 1;
    first.IndexCountPerInstance = 3; second.IndexCountPerInstance = 6;
    second.InstanceID = 1;
    templates.CopyData(0, first); templates.CopyData(2, second);
    for (bool swap : { false, true })
        for (auto& constants : resources.FrameConstants)
        {
            resources.CurrFrameConstantsIndex = static_cast<UINT>(&constants - resources.FrameConstants.data());
            constants->InstanceCache->Resize(2);
            GDX12InstanceData instance = {};
            instance.ReflectionDynamic = swap;
            constants->InstanceCache->CopyData(0, instance);
            instance.ReflectionDynamic = !swap;
            constants->InstanceCache->CopyData(1, instance);
            resources.PrepareLocalShadowData();
            const auto& expected = swap ? first : second;
            Require(constants->DynamicShadowCommandCount == 1 &&
                std::memcmp(constants->DynamicShadowTemplatesUpload->GetCPUData(), &expected, sizeof(expected)) == 0,
                "Dynamic template compaction lost a command or a direct classification edit");
        }
    templates.CopyData(0, GDX12IndirectDrawArgs{}); templates.CopyData(2, GDX12IndirectDrawArgs{});
    resources.PrepareLocalShadowData();
    Require(resources.FrameConstants[resources.CurrFrameConstantsIndex]->DynamicShadowCommandCount == 0,
        "Empty dynamic list retained its previous high-water draw count");
}

static std::vector<float> CheckReadback(GDX12Device& device, GDX12Texture& texture, UINT subresource, bool lut, bool depth = false, bool expectCaster = true)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = texture.GetResource()->D3DResource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    const auto* data = static_cast<const unsigned char*>(mapped);
    bool foundCaster = false;
    std::vector<float> depths;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* address = data + footprint.Offset + y * footprint.Footprint.RowPitch + x * (depth ? 4 : 8);
            if (depth)
            {
                const float value = *reinterpret_cast<const float*>(address);
                Require(std::isfinite(value) && value >= 0.0f && value <= 1.0f, "Invalid shadow atlas depth");
                foundCaster |= value < 0.999f;
                depths.push_back(value);
                continue;
            }
            const auto* pixel = reinterpret_cast<const uint16_t*>(address);
            float values[4];
            for (UINT channel = 0; channel < 4; ++channel)
            {
                values[channel] = PackedVector::XMConvertHalfToFloat(pixel[channel]);
                Require(std::isfinite(values[channel]) && values[channel] >= 0.0f, "IBL readback contains invalid values");
            }
            if (!lut) Require(values[0] + values[1] + values[2] > 0.01f, "Environment map was not populated");
            else if (x == footprint.Footprint.Width - 1)
                Require(values[0] + values[1] <= 1.02f, "BRDF LUT exceeds unit reflectance at normal incidence");
        }
    D3D12_RANGE noWrite = { 0, 0 };
    readback->Unmap(0, &noWrite);
    Require(!depth || foundCaster == expectCaster, "Shadow depth contains the wrong caster coverage");
    return depths;
}

static std::vector<float> ReadHDR(GDX12Device& device, GDX12Texture& texture)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = texture.GetResource()->D3DResource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    std::vector<float> result;
    result.reserve(texture.GetWidth() * texture.GetHeight() * 3);
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        for (UINT x = 0; x < texture.GetWidth(); ++x)
        {
            const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(mapped) +
                footprint.Offset + y * footprint.Footprint.RowPitch + x * 8);
            for (UINT channel = 0; channel < 3; ++channel)
            {
                const float value = PackedVector::XMConvertHalfToFloat(pixel[channel]);
                Require(std::isfinite(value) && value >= 0.0f, "Deferred HDR contains invalid values");
                result.push_back(value);
            }
        }
    const D3D12_RANGE noWrite = { 0, 0 };
    readback->Unmap(0, &noWrite);
    return result;
}

#include "ClusteredLightingChecks.h"

static void CheckShadowVisibilityJobs()
{
    const auto referenceIntersects = [](const XMFLOAT4X4& matrix, const BoundingBox& bounds)
    {
        const XMMATRIX m = XMLoadFloat4x4(&matrix);
        const XMVECTOR planes[] = { m.r[3] + m.r[0], m.r[3] - m.r[0], m.r[3] + m.r[1],
            m.r[3] - m.r[1], m.r[2], m.r[3] - m.r[2] };
        for (const auto plane : planes)
            if (XMVectorGetX(XMVector3Dot(XMLoadFloat3(&bounds.Center), plane)) + XMVectorGetW(plane) +
                XMVectorGetX(XMVector3Dot(XMLoadFloat3(&bounds.Extents), XMVectorAbs(plane))) < 0) return false;
        return true;
    };
    GDX12LocalLightConstants point;
    point.ShadowFirstSlice = 0;
    BuildLocalLightShadowMatrices(point);
    std::vector<GDX12LocalShadowCaster> casters = { { BoundingBox({ 0, -10, 0 }, { 1, 1, 1 }), 1, true } };
    const auto empty = BuildLocalShadowVisibility(point, {});
    const auto faceMask = [&](const GDX12LocalShadowVisibility& visible)
    {
        UINT mask = 0;
        for (UINT face = 0; face < 6; ++face)
            if (visible.AllCasters[face] != empty.AllCasters[face]) mask |= 1u << face;
        return mask;
    };
    auto visible = BuildLocalShadowVisibility(point, casters);
    Require(faceMask(visible) == (1u << 3) && visible.StaticCasters == empty.StaticCasters,
        "A dynamic caster below the point must affect only -Y and leave static signatures unchanged");
    casters[0].Bounds.Center = { 10, 0, 10 };
    Require(faceMask(BuildLocalShadowVisibility(point, casters)) == ((1u << 0) | (1u << 4)),
        "A caster crossing a cube seam must participate in both faces");
    casters[0].Bounds.Center = { 0, 0, 0 };
    Require(faceMask(BuildLocalShadowVisibility(point, casters)) == 63,
        "Bounds surrounding a point light must participate in all faces");
    std::vector<GDX12LocalLightConstants> lights(65, point);
    for (size_t i = 0; i < lights.size(); ++i)
    {
        lights[i].Position = { static_cast<float>(i) * 10, 20, 0 };
        lights[i].Type = i % 2 ? 2 : 1;
        BuildLocalLightShadowMatrices(lights[i]);
    }
    std::mt19937 random(42);
    std::uniform_real_distribution<float> position(-1500, 1500), extent(0, 500);
    for (const auto& light : lights)
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        {
            const GDX12LocalShadowFrustum frustum(light.ShadowViewProj[face]);
            for (UINT i = 0; i < 512; ++i)
            {
                const BoundingBox bounds({ position(random), position(random), position(random) },
                    { extent(random), extent(random), extent(random) });
                Require(LocalShadowRangeIntersects(light, bounds) == BoundingSphere(light.Position, light.Range).Intersects(bounds),
                    "Scalar shadow range disagrees with sphere/box intersection");
                Require(frustum.Intersects(bounds) == referenceIntersects(light.ShadowViewProj[face], bounds),
                    "Prepared point/spot frustum disagrees with the clip-plane reference");
            }
        }
    std::vector<GDX12LocalShadowVisibility> serial, parallel;
    ShadowVisibilitySystem system;
    JobSystem one(1), four(4);
    system.Schedule(one, lights, casters, serial).Complete();
    system.Schedule(four, lights, casters, parallel).Complete();
    for (size_t i = 0; i < lights.size(); ++i)
        Require(serial[i].AllCasters == parallel[i].AllCasters && serial[i].StaticCasters == parallel[i].StaticCasters,
            "Parallel light batches must match serial visibility exactly");
    for (size_t i = 0; i < 128; ++i)
        casters.push_back({ BoundingBox({ position(random), position(random), position(random) },
            { extent(random), extent(random), extent(random) }), i + 2, i % 3 == 0 });
    ShadowVisibilitySystem fourThreadCache;
    const auto checkCache = [&]()
    {
        for (auto* jobs : { &one, &four })
        {
            auto& cache = jobs == &one ? system : fourThreadCache;
            cache.Schedule(*jobs, lights, casters, parallel).Complete();
            for (size_t i = 0; i < lights.size(); ++i)
            {
                const auto reference = BuildLocalShadowVisibility(lights[i], casters);
                Require(reference.StaticCasters == parallel[i].StaticCasters && reference.AllCasters == parallel[i].AllCasters,
                    "Cached static/dynamic visibility differs from the full caster scan");
            }
        }
    };
    checkCache();
    checkCache();
    casters[1].Bounds.Center.x += 500; checkCache();
    ++casters[1].Signature; checkCache(); // Material edit or same-handle geometry reimport.
    casters[1].Dynamic = true; checkCache();
    casters[1].Dynamic = false; checkCache();
    casters.erase(casters.begin() + 1); checkCache();
    std::reverse(casters.begin(), casters.end()); checkCache();
    for (auto& light : lights)
    {
        light.Position.x += 300;
        light.Range *= 0.75f;
        light.OuterCos = 0.8f;
        BuildLocalLightShadowMatrices(light);
    }
    checkCache();
    lights[0].ShadowFirstSlice = UINT_MAX; checkCache();
    lights[0].ShadowFirstSlice = 0; checkCache();
    casters.clear(); checkCache();
    lights.clear();
    system.Schedule(four, lights, casters, parallel).Complete();
    Require(parallel.empty(), "Removing all lights must discard old visibility results");
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        CheckShadowVisibilityJobs();
        CheckLightGridSettings();
        BenchmarkEngine engine;
        Require(!ConsoleModule{}.FindConsoleVariable(L"Render.Lighting.Cluster.TileSize").SetFromString("32"),
            "Light grid configuration changed after startup");
        auto& locator = BenchmarkEngine::GetLocator();
        locator.RegisterModule(std::make_shared<RenderModule>(nullptr, nullptr));
        const std::weak_ptr<RenderModule> original = locator.GetModule<RenderModule>();
        locator.RegisterModule(std::make_shared<RenderModule>(nullptr, nullptr));
        Require(original.expired(), "Replacing an uninitialized renderer must release it safely");
        locator.UnregisterModule<RenderModule>();

        const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (!benchmark) debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        if (!benchmark) ThrowIfFailed(device.GetDevice().As(&info));
        // Existing empty scene caches create null/zero-element descriptors before scene loading.
        if (info) info->ClearStoredMessages();
        ShadowUploadSlots(device);
        auto* uploadQueue = device.GetCommandQueue();
        auto* pendingUpload = uploadQueue->GetCommandList();
        auto* otherUpload = uploadQueue->GetCommandList();
        Require(pendingUpload != otherUpload, "A recording upload list must not be recycled");
        uploadQueue->ExecuteCommandList(otherUpload);
        uploadQueue->Flush();
        otherUpload = uploadQueue->GetCommandList();
        Require(pendingUpload != otherUpload, "A recording list must survive another list's completion");
        uploadQueue->ExecuteCommandList(otherUpload);
        uploadQueue->ExecuteCommandList(pendingUpload);
        uploadQueue->Flush();
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = 64;
        common.WindowHeight = common.DownscaledHeight = 64;
        resources.Sky.Source = SkySource::SkyBox;
        resources.Sky.Quality = 3;
        resources.Sky.Box.IBL.CubeResolution = 1024;
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common);
        environment.Initialize();
        GDX12CascadedShadowPass shadows;
        shadows.Setup(&resources, nullptr, &common);
        shadows.Initialize();
        GDX12LocalShadowPass localShadows;
        localShadows.Setup(&resources, nullptr, &common);
        localShadows.Initialize();
        GDX12OpaquePass opaque;
        opaque.Setup(&resources, nullptr, &common);
        opaque.SetInputs({});
        opaque.Initialize();
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common);
        ao.SetInputs({ opaque.GetOutput(2), opaque.GetOutput(3) });
        ao.Initialize();
        GDX12DeferredLightingPass deferred;
        deferred.Setup(&resources, nullptr, &common);
        const std::vector<IRenderPassLink*> lightingInputs = { opaque.GetOutput(0), opaque.GetOutput(3), opaque.GetOutput(4), opaque.GetOutput(5),
            opaque.GetOutput(2), shadows.GetOutput(0), environment.GetOutput(0), environment.GetOutput(1),
            environment.GetOutput(2), environment.GetOutput(3), localShadows.GetOutput(0), ao.GetOutput(0) };
        deferred.SetInputs(lightingInputs);
        deferred.Initialize();
        GDX12RenderGraph lightingGraph(&device);
        lightingGraph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene");
        lightingGraph.ImportShared(GDX12RenderGraph::SharedResource::Environment, "Atmosphere");
        lightingGraph.ImportShared(GDX12RenderGraph::SharedResource::ReflectionProbes, "Probes");
        for (auto* input : lightingInputs) lightingGraph.ImportLink("Lighting input", input);
        lightingGraph.ImportLink("HDR readback", deferred.GetOutput(0));
        lightingGraph.AddPass(deferred);
        lightingGraph.Compile(false);
        GDX12GPUCullingPass culling;
        culling.Setup(&resources, nullptr, &common);
        GDX12WBOITTransparencyPass transparency;
        transparency.Setup(&resources, nullptr, &common);
        transparency.SetInputs({ opaque.GetOutput(2), shadows.GetOutput(0), environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2), localShadows.GetOutput(0) });
        transparency.Initialize();
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        environment.Execute(commands);
        const auto sourceCube = environment.GetOutput(3)->GetTexture()->GetResource()->D3DResource->GetDesc();
        Require(sourceCube.Width == 1024 && sourceCube.Height == 1024 && sourceCube.DepthOrArraySize == 6 &&
            sourceCube.MipLevels == 11, "Authored source cubemap budget/mips were not preserved");
        Require(environment.GetOutput(3)->GetTexture()->GetFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT,
            "Generated sky cubemap must contain linear HDR radiance");
        queue->ExecuteCommandList(commands);
        queue->Flush();
        CheckReadback(device, *environment.GetOutput(0)->GetTexture(), 0, false);
        CheckReadback(device, *environment.GetOutput(1)->GetTexture(), 0, false);
        CheckReadback(device, *environment.GetOutput(1)->GetTexture(), 7, false);
        CheckReadback(device, *environment.GetOutput(2)->GetTexture(), 0, true);

        SubMesh triangle;
        triangle.Vertices.resize(3);
        triangle.Vertices[0].Position = Vector3(-100, 0, -100);
        triangle.Vertices[1].Position = Vector3(100, 0, -100);
        triangle.Vertices[2].Position = Vector3(0, 0, 100);
        for (auto& vertex : triangle.Vertices)
        {
            vertex.Normal = Vector3::UnitY;
            vertex.Tangent = Vector3::UnitX;
        }
        triangle.Indices = { 0, 2, 1 };
        BoundingBox bounds(XMFLOAT3(0, 0, 0), XMFLOAT3(100, 0.1f, 100));
        Mesh mesh({ std::move(triangle) }, bounds);
        resources.GeometryBuffer->AddMesh(&mesh, MeshHandle(0));
        auto& frame = resources.FrameConstants[0];
        frame->TransformCache->Resize(1);
        frame->TransformCache->CopyData(0, GDX12TransformConstants{});
        frame->MaterialCache->Resize(1);
        GDX12MaterialConstants material;
        material.Metallic = 0.0f;
        frame->MaterialCache->CopyData(0, material);
        frame->InstanceCache->Resize(1);
        GDX12InstanceData instance = {};
        instance.BoundingBoxCenter = bounds.Center;
        instance.BoundingBoxExtents = bounds.Extents;
        frame->InstanceCache->CopyData(0, instance);
        resources.IndirectCommandsCache->Resize(1);
        GDX12IndirectDrawArgs draw = {};
        draw.IndexCountPerInstance = 3;
        draw.InstanceCount = 1;
        resources.IndirectCommandsCache->CopyData(0, draw);
        const Matrix view = Matrix::CreateLookAt(Vector3(0, 300, -400), Vector3::Zero, Vector3::UnitY);
        frame->ShadowCB->CopyData(0, BuildShadowCascades(view.Invert(), XMConvertToRadians(60.0f), 1.0f,
            0.1f, 10000.0f, Vector3(0.35f, -1.0f, 0.25f)));
        const UINT textureSlot = resources.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        GDX12Texture white;
        GDX12TextureDesc whiteDesc;
        whiteDesc.Width = whiteDesc.Height = 1;
        whiteDesc.Format = whiteDesc.SRVDesc.Format = whiteDesc.RTVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        whiteDesc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        whiteDesc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        whiteDesc.SRVDesc.Texture2D.MipLevels = 1;
        whiteDesc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        whiteDesc.SRVHeapIndex = textureSlot;
        whiteDesc.CreateRTV = true;
        whiteDesc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        whiteDesc.RTVHeap = resources.RTVHeap.get();
        whiteDesc.RTVHeapIndex = resources.RTVHeap->GetAvailableIndex();
        whiteDesc.ClearValue = { 1, 1, 1, 1 };
        white.Initialize(whiteDesc);
        material.DiffuseIndex = textureSlot - Texture2D_StartIndex;
        frame->MaterialCache->CopyData(0, material);
        commands = queue->GetCommandList();
        commands->ResourceBarrier({ white.GetResource()->GetRenderTargetBarrier() });
        commands->ClearRenderTargetView(&white);
        commands->ResourceBarrier({ white.GetResource()->GetSRVBarrier() });
        resources.PrepareLocalShadowData();
        resources.UploadLocalShadowData(commands);
        resources.IndirectCommandsCache->CopyData(0, GDX12IndirectDrawArgs{});
        shadows.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        CheckReadback(device, *shadows.GetOutput(0)->GetTexture(), 0, false, true);
        resources.IndirectCommandsCache->CopyData(0, draw);

        GDX12LocalLightConstants point;
        point.Position = { 0, 20, 0 };
        point.EntityId = 1;
        point.ShadowFirstSlice = 0;
        point.ShadowFlags = 1;
        BuildLocalLightShadowMatrices(point);
        GDX12LocalLightConstants spot = point;
        spot.Type = 2;
        spot.EntityId = 2;
        spot.ShadowFirstSlice = 6;
        spot.Position.y = 30;
        BuildLocalLightShadowMatrices(spot);
        ShadowVisibilitySystem shadowVisibility;
        JobSystem shadowJobs(4);
        const auto updateLocal = [&](bool fullScan = false)
        {
            resources.LocalLights = { point, spot };
            frame->LocalLightCache->Resize(2);
            frame->LocalLightCache->CopyData(0, resources.LocalLights.data(), 2);
            if (resources.LocalShadowCasters)
            {
                shadowVisibility.Schedule(shadowJobs, resources.LocalLights, *resources.LocalShadowCasters,
                    resources.LocalShadowVisibility).Complete();
                for (size_t i = 0; i < resources.LocalLights.size(); ++i)
                {
                    const auto reference = BuildLocalShadowVisibility(resources.LocalLights[i], *resources.LocalShadowCasters);
                    Require(reference.StaticCasters == resources.LocalShadowVisibility[i].StaticCasters &&
                        reference.AllCasters == resources.LocalShadowVisibility[i].AllCasters,
                        "GPU shadow readbacks must use the same visibility as the uncached scan");
                    if (fullScan) resources.LocalShadowVisibility[i] = reference;
                }
            }
            else resources.LocalShadowVisibility.clear();
            auto* list = queue->GetCommandList();
            resources.PrepareLocalShadowData();
            resources.UploadLocalShadowData(list);
            auto casterSnapshot = fullScan ? std::move(resources.LocalShadowCasters) : std::nullopt;
            if (fullScan) resources.LocalShadowCasters.reset();
            localShadows.Prepare();
            if (fullScan) resources.LocalShadowCasters = std::move(casterSnapshot);
            auto visibilitySnapshot = std::move(resources.LocalShadowVisibility);
            localShadows.Execute(list);
            resources.LocalShadowVisibility = std::move(visibilitySnapshot);
            queue->ExecuteCommandList(list);
            queue->Flush();
        };
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 7, "Cached point+spot shadows must bake seven views initially");
        Require(localShadows.GetLastFrameStaticRenderedViews() == 7, "Static casters must bake initially");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 0, "Cached shadows must skip rendering on unchanged frames");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        spot.Position.x += 2;
        BuildLocalLightShadowMatrices(spot);
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Moving a spot must only update its own cached map");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        point.ShadowFlags |= 2;
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 6, "EveryFrame point must update all six faces");
        Require(localShadows.GetLastFrameStaticRenderedViews() == 0, "EveryFrame dynamic shadows must reuse unchanged static casters");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 6, "EveryFrame point updates must continue on later frames");
        ++resources.ReflectionStaticRevision;
        updateLocal();
        Require(localShadows.GetLastFrameStaticRenderedViews() == 7, "Changed static casters must invalidate the static depth cache");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        instance.ReflectionDynamic = 1;
        frame->InstanceCache->CopyData(0, instance);
        ++resources.ReflectionStaticRevision;
        ++resources.ReflectionDynamicRevision;
        updateLocal();
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        draw.InstanceCount = 0;
        resources.IndirectCommandsCache->CopyData(0, draw);
        ++resources.ReflectionDynamicRevision;
        updateLocal();
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true, false);

        // Spatial invalidation must retain unrelated lights and remove shadows at the old position.
        point.ShadowFlags = 1;
        spot.Position.x = 3000;
        BuildLocalLightShadowMatrices(spot);
        Require(LocalShadowIntersects(point, bounds), "Point range must intersect the nearby caster");
        Require(!LocalShadowIntersects(spot, bounds), "Distant spot must reject the caster bounds");
        const BoundingBox outsideCone({ 3100, 20, 0 }, { 1, 1, 1 });
        Require(BoundingSphere(spot.Position, spot.Range).Intersects(outsideCone) &&
            !LocalShadowIntersects(spot, outsideCone), "Spot culling must test its frustum, not just range");
        const BoundingBox behindSpot({ 3000, 40, 0 }, { 1, 1, 1 });
        Require(!LocalShadowIntersects(spot, behindSpot), "Spot culling must reject casters behind the light");
        const BoundingBox touchesPoint({ point.Range + 1, 20, 0 }, { 1, 1, 1 });
        Require(LocalShadowIntersects(point, touchesPoint), "Point bounds touching the range must be retained");
        draw.InstanceCount = 1;
        resources.IndirectCommandsCache->CopyData(0, draw);
        resources.LocalShadowCasters.emplace(1, GDX12LocalShadowCaster{ bounds, 1, true });
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 7, "Switching to spatial snapshots must initialize both lights");
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 0, "Unchanged caster snapshots must reuse both lights");
        const auto moveCaster = [&](float x)
        {
            GDX12TransformConstants transform;
            XMStoreFloat4x4(&transform.WorldMatrix, Matrix::CreateTranslation(x, 0, 0).Transpose());
            frame->TransformCache->CopyData(0, transform);
            instance.BoundingBoxCenter.x = x;
            frame->InstanceCache->CopyData(0, instance);
            resources.LocalShadowCasters->front().Bounds.Center.x = x;
            ++resources.ReflectionDynamicRevision;
            updateLocal();
        };
        moveCaster(5);
        Require(localShadows.GetLastFrameRenderedViews() == 5 && localShadows.GetLastFrameStaticRenderedViews() == 0,
            "Moving near the point must preserve the distant spot and static depth caches");
        moveCaster(3000);
        Require(localShadows.GetLastFrameRenderedViews() == 6, "Teleport must invalidate affected faces of both old and new lights");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true, false);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true);
        moveCaster(6000);
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Leaving the spot must only remove its old shadow");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 6, false, true, false);
        moveCaster(0);
        Require(localShadows.GetLastFrameRenderedViews() == 5, "Entering the point must only update its intersected faces");
        ++resources.LocalShadowCasters->front().Signature;
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 5, "Changed caster content must invalidate its faces without movement");
        resources.LocalShadowCasters->front().Dynamic = false;
        instance.ReflectionDynamic = 0;
        frame->InstanceCache->CopyData(0, instance);
        updateLocal();
        Require(localShadows.GetLastFrameStaticRenderedViews() == 5 && localShadows.GetLastFrameRenderedViews() == 5,
            "Reclassifying a caster must update both depth caches of its light");
        resources.LocalShadowCasters->clear();
        draw.InstanceCount = 0;
        resources.IndirectCommandsCache->CopyData(0, draw);
        updateLocal();
        Require(localShadows.GetLastFrameStaticRenderedViews() == 5 && localShadows.GetLastFrameRenderedViews() == 5,
            "Deleting a caster must invalidate its former light even with an empty snapshot");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true, false);
        draw.InstanceCount = 1;
        resources.IndirectCommandsCache->CopyData(0, draw);
        instance.ReflectionDynamic = 1;
        resources.LocalShadowCasters->push_back({ bounds, 1, true });
        const auto placeCaster = [&](const Matrix& world)
        {
            GDX12TransformConstants transform;
            XMStoreFloat4x4(&transform.WorldMatrix, world.Transpose());
            frame->TransformCache->CopyData(0, transform);
            BoundingBox placed;
            bounds.Transform(placed, world);
            instance.BoundingBoxCenter = placed.Center;
            instance.BoundingBoxExtents = placed.Extents;
            frame->InstanceCache->CopyData(0, instance);
            resources.LocalShadowCasters->front().Bounds = placed;
            ++resources.LocalShadowCasters->front().Signature;
            updateLocal();
        };
        placeCaster(Matrix::CreateTranslation(0, -250, 0));
        Require(localShadows.GetLastFrameRenderedViews() == 1, "A caster contained in -Y must update only that face");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
        placeCaster(Matrix::CreateTranslation(5, -250, 0));
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Movement within one face must retain the other five maps");
        placeCaster(Matrix::CreateRotationZ(XM_PIDIV2) * Matrix::CreateTranslation(250, 20, 0));
        Require(localShadows.GetLastFrameRenderedViews() == 2, "Moving between cube faces must update the old and new faces");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true, false);
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 0, false, true);
        resources.LocalShadowCasters->clear();
        draw.InstanceCount = 0;
        resources.IndirectCommandsCache->CopyData(0, draw);
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Deleting the last caster must clear only its former face");
        CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 0, false, true, false);
        frame->TransformCache->CopyData(0, GDX12TransformConstants{});
        instance.BoundingBoxCenter = bounds.Center;
        instance.BoundingBoxExtents = bounds.Extents;
        resources.LocalShadowCasters.reset();
        spot.Position.x = 2;
        BuildLocalLightShadowMatrices(spot);
        draw.InstanceCount = 1;
        resources.IndirectCommandsCache->CopyData(0, draw);
        instance.ReflectionDynamic = 0;
        frame->InstanceCache->CopyData(0, instance);
        ++resources.ReflectionStaticRevision;

        point.Type = 2;
        point.ShadowFlags = 1;
        BuildLocalLightShadowMatrices(point);
        spot.ShadowFirstSlice = UINT_MAX;
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 1, "First cached spot must own the single shadow slice");
        point.ShadowFirstSlice = UINT_MAX;
        spot.ShadowFirstSlice = 0;
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Second spot must replace the slice owner");
        point.ShadowFirstSlice = 0;
        spot.ShadowFirstSlice = UINT_MAX;
        updateLocal();
        Require(localShadows.GetLastFrameRenderedViews() == 1, "Re-enabled cached spot must reclaim and rebake its overwritten slice");
        point.Type = 1;
        point.ShadowFlags = 3;
        BuildLocalLightShadowMatrices(point);
        spot.ShadowFirstSlice = 6;
        updateLocal();

        GDX12CameraConstants camera = {};
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f), 1.0f, 0.1f, 10000.0f);
        const float reverseRange = 0.1f / (10000.0f - 0.1f);
        projection._33 = reverseRange;
        projection._43 = reverseRange * 10000.0f;
        const Matrix viewProjection = view * projection;
        XMStoreFloat4x4(&camera.ViewProj, XMMatrixTranspose(viewProjection));
        camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = camera.ViewProj;
        XMStoreFloat4x4(&camera.View, XMMatrixTranspose(view));
        XMStoreFloat4x4(&camera.InvViewProj, XMMatrixTranspose(viewProjection.Invert()));
        camera.CameraLocation = { 0, 300, -400 };
        camera.NearPlane = 0.1f;
        camera.FarPlane = 10000.0f;
        frame->CameraCB->Resize(1);
        frame->CameraCB->CopyData(0, camera);
        common.ActiveCameraCBufferIndex = 0;
        common.CameraViewToClip = projection;
        common.CameraProjection = projection;
        common.CameraView = view;
        common.ActiveCameraNearPlane = camera.NearPlane;
        common.ActiveCameraFarPlane = camera.FarPlane;
        common.Jobs = &shadowJobs;
        frame->CameraVisibilityCommands.emplace_back();
        auto& visibility = frame->CameraVisibilityCommands[0];
        visibility.VisibleOpaqueCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(&device, 1, EBufferType::Default, false);
        visibility.VisibleTransparentCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(&device, 1, EBufferType::Default, false);
        visibility.OpaqueDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(&device, 1, EBufferType::Default, false);
        visibility.TransparentDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(&device, 1, EBufferType::Default, false);
        visibility.VisibleOpaqueCommandsCache->CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        visibility.VisibleTransparentCommandsCache->CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        visibility.OpaqueDrawCounter->CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        visibility.TransparentDrawCounter->CreateUAV(resources.SRV_UAV_Heap.get(), resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        GDX12MainConstants lighting;
        lighting.RenderTargetSize = { 64, 64 };
        lighting.LightRadiance = {};
        lighting.SunShadowFlags = 0;
        lighting.LocalLightCount = 2;
        frame->MainCB->CopyData(0, lighting);
        auto* aoTexture = ao.GetOutput(0)->GetTexture();
        GDX12DescriptorHeap clearHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        GDX12Descriptor clearAO;
        D3D12_UNORDERED_ACCESS_VIEW_DESC clearView = {};
        clearView.Format = DXGI_FORMAT_R32_FLOAT;
        clearView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        clearAO.InitAsUAV(aoTexture->GetResource()->D3DResource.Get(), nullptr, &clearView, &clearHeap, 0);
        const auto renderLighting = [&](XMFLOAT3 pointRadiance, XMFLOAT3 spotRadiance, float aoOverride = -1, bool poisonUpload = false)
        {
            point.Radiance = pointRadiance;
            spot.Radiance = spotRadiance;
            const GDX12LocalLightConstants lights[] = { point, spot };
            resources.LocalLights = { point, spot };
            frame->LocalLightCache->CopyData(0, lights, 2);
            auto* list = queue->GetCommandList();
            resources.PrepareLocalShadowData();
            resources.UploadLocalShadowData(list);
            if (poisonUpload)
            {
                queue->ExecuteCommandList(list);
                queue->Flush();
                const GDX12LocalLightConstants unlit[2] = {};
                frame->LocalLightCache->CopyData(0, unlit, 2);
                list = queue->GetCommandList();
            }
            frame->UploadReflectionProbeData(list);
            culling.Execute(list);
            opaque.Execute(list);
            ao.Execute(list);
            if (aoOverride >= 0)
            {
                list->ResourceBarrier({ aoTexture->GetResource()->GetUnorderedAccessBarrier() });
                const float values[] = { aoOverride, aoOverride, aoOverride, aoOverride };
                list->ClearUnorderedAccessViewFloat(aoTexture->GetUAV()->GPUHandle, clearAO.CPUHandle,
                    aoTexture->GetResource()->D3DResource.Get(), values);
            }
            deferred.Prepare();
            auto lightSnapshot = std::move(resources.LocalLights);
            lightingGraph.Execute(list);
            resources.LocalLights = std::move(lightSnapshot);
            transparency.Execute(list);
            queue->ExecuteCommandList(list);
            queue->Flush();
            frame->LocalLightCache->CopyData(0, lights, 2);
            return ReadHDR(device, *deferred.GetOutput(0)->GetTexture());
        };
        resources.LocalShadowCasters.emplace(1, GDX12LocalShadowCaster{ bounds, 1, false });
        for (bool dynamic : { false, true })
        {
            resources.LocalShadowCasters->front().Dynamic = dynamic;
            instance.ReflectionDynamic = dynamic;
            frame->InstanceCache->CopyData(0, instance);
            updateLocal();
            const auto cachedDepth = CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true);
            const auto cachedHDR = renderLighting({ 1, 1, 1 }, { 1, 1, 1 });
            localShadows.InvalidateCache();
            updateLocal(true);
            Require(cachedDepth == CheckReadback(device, *localShadows.GetOutput(0)->GetTexture(), 3, false, true) &&
                cachedHDR == renderLighting({ 1, 1, 1 }, { 1, 1, 1 }),
                "Cached visibility changed shadow depth or HDR compared with the full scan");
        }
        resources.LocalShadowCasters.reset();
        instance.ReflectionDynamic = 0;
        frame->InstanceCache->CopyData(0, instance);
        ++resources.ReflectionStaticRevision;
        ++resources.ReflectionDynamicRevision;
        updateLocal();
        const auto ambient = renderLighting({}, {});
        const auto pointLit = renderLighting({ 1, 1, 1 }, {});
        Require(pointLit == renderLighting({ 1, 1, 1 }, {}, -1, true),
            "Deferred lighting must read the published DEFAULT lights snapshot");
        const auto spotLit = renderLighting({}, { 1, 1, 1 });
        float pointIncrease = 0.0f;
        float spotIncrease = 0.0f;
        for (size_t channel = 0; channel < ambient.size(); ++channel)
        {
            pointIncrease = (std::max)(pointIncrease, pointLit[channel] - ambient[channel]);
            spotIncrease = (std::max)(spotIncrease, spotLit[channel] - ambient[channel]);
        }
        Require(pointIncrease > 0.01f, "Deferred point light did not illuminate the G-buffer geometry");
        Require(spotIncrease > 0.01f, "Deferred spot light did not illuminate the G-buffer geometry");
        Require(ambient[0] + ambient[1] + ambient[2] > 0.01f, "Deferred background did not sample the scene cubemap");
        const auto darkAmbient = renderLighting({}, {}, 0);
        const auto darkPoint = renderLighting({ 1, 1, 1 }, {}, 0);
        float ambientReduction = 0;
        for (size_t channel = 0; channel < ambient.size(); ++channel)
        {
            ambientReduction += ambient[channel] - darkAmbient[channel];
            Require(std::abs((pointLit[channel] - ambient[channel]) - (darkPoint[channel] - darkAmbient[channel])) < 0.01f,
                "AO changed direct local-light energy");
        }
        Require(ambientReduction > 0.01f, "AO did not reduce diffuse IBL");
        material.Metallic = 1;
        frame->MaterialCache->CopyData(0, material);
        const auto specularNeutral = renderLighting({}, {}, 1);
        Require(specularNeutral == renderLighting({}, {}, 0), "Scalar AO attenuated specular IBL");
        material.EmissiveColor = { 3, 2, 1 };
        frame->MaterialCache->CopyData(0, material);
        const auto emissiveNeutral = renderLighting({}, {}, 1);
        Require(emissiveNeutral == renderLighting({}, {}, 0), "AO attenuated emissive energy");
        material.Metallic = 0;
        material.EmissiveColor = {};
        material.Flags = 16;
        frame->MaterialCache->CopyData(0, material);
        const auto bakedNeutral = renderLighting({}, {}, 1);
        Require(bakedNeutral == renderLighting({}, {}, 0), "AO multiplied baked color");
        material.Flags = 0;
        frame->MaterialCache->CopyData(0, material);
        std::cout << "AO diffuse/direct/specular/emissive/baked energy invariants passed\n";
        // Force an occluder in every view to verify shadow sampling independently of map generation.
        GDX12Descriptor occludedViews;
        D3D12_DEPTH_STENCIL_VIEW_DESC occludedDSV = {};
        occludedDSV.Format = DXGI_FORMAT_D32_FLOAT;
        occludedDSV.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        occludedDSV.Texture2DArray.ArraySize = 7;
        auto* localMaps = localShadows.GetOutput(0)->GetTexture();
        occludedViews.InitAsDSV(localMaps->GetResource()->D3DResource.Get(), &occludedDSV,
            resources.DSVHeap.get(), resources.DSVHeap->GetAvailableIndex());
        const auto occludeLocalMaps = [&]()
        {
            auto* list = queue->GetCommandList();
            list->ResourceBarrier({ localMaps->GetResource()->GetDepthWriteBarrier() });
            list->GetCommandList()->ClearDepthStencilView(occludedViews.CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
            list->ResourceBarrier({ localMaps->GetResource()->GetSRVBarrier() });
            queue->ExecuteCommandList(list);
            queue->Flush();
        };
        occludeLocalMaps();
        const auto occluded = renderLighting({ 1, 1, 1 }, { 1, 1, 1 });
        float originalEnergy = 0, occludedEnergy = 0;
        for (size_t channel = 0; channel < ambient.size(); ++channel)
        {
            originalEnergy += pointLit[channel] + spotLit[channel] - 2 * ambient[channel];
            occludedEnergy += occluded[channel] - ambient[channel];
        }
        std::cout << "Shadow occlusion energy: " << occludedEnergy << " / " << originalEnergy << '\n';
        Require(occludedEnergy < originalEnergy * 0.05f,
            "Point/spot shadow maps did not suppress direct illumination");
        ++spot.ShadowRevision;
        updateLocal();
        material.RenderLayer = 2;
        material.Opacity = 0.5f;
        frame->MaterialCache->CopyData(0, material);
        renderLighting({}, {});
        const auto glassAmbient = ReadHDR(device, *transparency.GetOutput(0)->GetTexture());
        renderLighting({ 1, 1, 1 }, { 1, 1, 1 });
        const auto glassLit = ReadHDR(device, *transparency.GetOutput(0)->GetTexture());
        float glassIncrease = 0.0f;
        for (size_t channel = 0; channel < glassAmbient.size(); ++channel)
            glassIncrease = (std::max)(glassIncrease, glassLit[channel] - glassAmbient[channel]);
        Require(glassIncrease > 0.001f, "Forward OIT glass did not receive local lighting");
        occludeLocalMaps();
        renderLighting({ 1, 1, 1 }, { 1, 1, 1 });
        const auto glassOccluded = ReadHDR(device, *transparency.GetOutput(0)->GetTexture());
        originalEnergy = occludedEnergy = 0;
        for (size_t channel = 0; channel < glassAmbient.size(); ++channel)
        {
            originalEnergy += glassLit[channel] - glassAmbient[channel];
            occludedEnergy += glassOccluded[channel] - glassAmbient[channel];
        }
        Require(occludedEnergy < originalEnergy * 0.05f, "Forward OIT glass did not receive local shadows");

        CheckClusteredLighting(resources, common, lightingInputs, benchmark);
        for (UINT64 i = 0; info && i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "DX12 lighting smoke: cubemap/IBL, cached point/spot shadows, deferred light readback passed; debug layer=" << !benchmark << '\n';
        GDX12ShaderCompiler::Shutdown();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
