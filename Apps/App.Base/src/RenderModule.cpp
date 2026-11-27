#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/AssetManager.h"

#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/GDX12RayTracingScene.h"
#include <algorithm>

namespace
{
    void ConfigureDefaultPipeline(FrameGraph& graph, const FrameGraphInputs& inputs)
    {
        auto culling = graph.AddPrimaryGPUPass<GDX12OcclusionCullingPass>("Occlusion Culling");
        auto shadowCulling = graph.AddPrimaryGPUPass<GDX12ShadowMapCullingPass>("Shadow Map Culling");
        auto shadows = graph.AddPrimaryGPUPass<GDX12ShadowMapPass>("Shadow Maps");
        auto opaque = graph.AddPrimaryGPUPass<GDX12DeferredGeometryPass>("Opaque");
        auto ibl = graph.AddPrimaryGPUPass<GDX12IBLPrecomputePass>("IBL Precompute");
        auto rtao = graph.AddPrimaryGPUPass<GDX12RTAOPass>("Ray Traced Ambient Occlusion");
        auto lighting = graph.AddPrimaryGPUPass<GDX12DeferredLightingPass>("Deferred Lighting");
        auto skyBox = graph.AddPrimaryGPUPass<GDX12SkyBoxPass>("SkyBox");
        auto transparency = graph.AddPrimaryGPUPass<GDX12WBOITTransparencyPass>("Transparency");
        auto composition = graph.AddPrimaryGPUPass<GDX12WBOITCompositionPass>("Composition");
        auto taa = graph.AddPrimaryGPUPass<GDX12TemporalAntiAliasingPass>("Temporal Anti-Aliasing");
        auto clear = graph.AddPrimaryGPUPass<GDX12TextureClearPass>("Back Buffer Clear");
        auto output = graph.AddPrimaryGPUPass<GDX12OutputToScreenPass>("Output To Screen");

        graph.ConnectNodes(shadowCulling.GetOutput("OutDirectionalDrawStream"), shadows.GetInput("InDirectionalDrawStream"));
        graph.ConnectNodes(shadowCulling.GetOutput("OutPointSpotStaticDrawStream"), shadows.GetInput("InPointSpotStaticDrawStream"));
        graph.ConnectNodes(shadowCulling.GetOutput("OutPointSpotDynamicDrawStream"), shadows.GetInput("InPointSpotDynamicDrawStream"));
        graph.ConnectNodes(inputs.BackBuffer, clear.GetInput("InTexture"));
        graph.ConnectNodes(inputs.DepthStencil, clear.GetInput("InDepthBuffer"));
        graph.ConnectNodes(clear.GetOutput("OutClearedTexture"), output.GetInput("InBackBuffer"));
        graph.ConnectNodes(culling.GetOutput("OutOpaqueDrawStream"), opaque.GetInput("InDrawStream"));
        graph.ConnectNodes(culling.GetOutput("OutTransparentDrawStream"), transparency.GetInput("InDrawStream"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), transparency.GetInput("InDepthBuffer"));
        graph.ConnectNodes(inputs.SkyBoxMap, ibl.GetInput("InSkyBoxMap"));
        graph.ConnectNodes(inputs.SkyBoxMap, skyBox.GetInput("InSkyBoxMap"));
        graph.ConnectNodes(lighting.GetOutput("OutColorTexture"), skyBox.GetInput("InColorTexture"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), skyBox.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutVelocityBuffer"), skyBox.GetInput("InVelocityBuffer"));
        graph.ConnectNodes(ibl.GetOutput("OutIrradiance"), lighting.GetInput("InIrradiance"));
        graph.ConnectNodes(ibl.GetOutput("OutPrefilteredEnvironment"), lighting.GetInput("InPrefilteredEnvironment"));
        graph.ConnectNodes(ibl.GetOutput("OutBRDFLut"), lighting.GetInput("InBRDFLut"));
        graph.ConnectNodes(ibl.GetOutput("OutIrradiance"), transparency.GetInput("InIrradiance"));
        graph.ConnectNodes(ibl.GetOutput("OutPrefilteredEnvironment"), transparency.GetInput("InPrefilteredEnvironment"));
        graph.ConnectNodes(ibl.GetOutput("OutBRDFLut"), transparency.GetInput("InBRDFLut"));
        graph.ConnectNodes(opaque.GetOutput("OutAlbedoMetallic"), lighting.GetInput("InAlbedoMetallic"));
        graph.ConnectNodes(opaque.GetOutput("OutNormalRoughness"), lighting.GetInput("InNormalRoughness"));
        graph.ConnectNodes(opaque.GetOutput("OutF0Baked"), lighting.GetInput("InF0Baked"));
        graph.ConnectNodes(opaque.GetOutput("OutEmissive"), lighting.GetInput("InEmissive"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), lighting.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), rtao.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutNormalRoughness"), rtao.GetInput("InNormalRoughness"));
        graph.ConnectNodes(rtao.GetOutput("OutAmbientOcclusion"), lighting.GetInput("InAmbientOcclusion"));
        graph.ConnectNodes(shadows.GetOutput("OutDirectionalMaps"), lighting.GetInput("InDirectionalMaps"));
        graph.ConnectNodes(shadows.GetOutput("OutPointSpotMaps"), lighting.GetInput("InPointSpotMaps"));
        graph.ConnectNodes(shadows.GetOutput("OutDirectionalMaps"), transparency.GetInput("InDirectionalMaps"));
        graph.ConnectNodes(shadows.GetOutput("OutPointSpotMaps"), transparency.GetInput("InPointSpotMaps"));
        graph.ConnectNodes(skyBox.GetOutput("OutColorTexture"), composition.GetInput("InOpaqueTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutAccumulationTexture"), composition.GetInput("InAccumulationTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutRevealageTexture"), composition.GetInput("InRevealageTexture"));
        graph.ConnectNodes(composition.GetOutput("OutColorTexture"), taa.GetInput("InColorTexture"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), taa.GetInput("InDepthBuffer"));
        graph.ConnectNodes(skyBox.GetOutput("OutVelocityBuffer"), taa.GetInput("InVelocityBuffer"));
        graph.ConnectNodes(composition.GetOutput("OutTransparencyMask"), taa.GetInput("InTransparencyMask"));
        graph.ConnectNodes(taa.GetOutput("OutColorTexture"), output.GetInput("InColorTexture"));
    }
}

RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _timer(timer), _window(window)
{
    _RPcommonData.GameTimer = _timer;
}

RenderModule::~RenderModule()
{
    _frameGraph.reset();
    if (_primaryDevice && FAILED(_primaryDevice->TryFlush()))
    { OutputDebugStringA("ERROR: Primary GPU synchronization failed during RenderModule destruction. Check the device removal reason.\n"); }
    if (_secondaryDevice && FAILED(_secondaryDevice->TryFlush()))
    { OutputDebugStringA("ERROR: Secondary GPU synchronization failed during RenderModule destruction. Check the device removal reason.\n"); }

    GDX12ShaderCompiler::Shutdown();
    GDX12DeviceFactory::Reset();
}

void RenderModule::Initialize()
{
#if defined(DEBUG) || defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif

    _primaryDevice = std::make_unique<GDX12Device>();
    _secondaryDevice = std::make_unique<GDX12Device>();
    SetGraphicsPipeline(ConfigureDefaultPipeline);
    _metrics.Initialize();
    SubscribeToSceneManager();
}

void RenderModule::SetGraphicsPipeline(PipelineConfiguration configure)
{
    FrameGraph graph(_primaryDevice.get(), _secondaryDevice.get());
    const FrameGraphInputs inputs{
        graph.ImportTexture("back-buffer", [this]() { return _backBuffer->GetCurrentBuffer(); }),
        graph.ImportTexture("depth-stencil", [this]() { return _depthStencil.get(); }),
        graph.ImportTexturePerDevice("sky-box-map", [this](EDeviceRole role) { return GetSkyBoxMap(role); })
    };
    configure(graph, inputs);
    CompileFrameGraph(graph);
}

void RenderModule::StartBenchmarkRun(const char* name, std::chrono::seconds duration)
{
    _metrics.BeginRun(name, duration);
}

void RenderModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
}

void RenderModule::OnResize()
{
    if (!_frameGraph) { return; }
    ResetMetricsInterval();
    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _renderingSuspended = !width || !height;
    if (_renderingSuspended)
    {
        _frameGraph->WaitForGPUCompletion();
        return;
    }

    _frameGraph->ReleaseFrameSlotResourcesForResize();
    _backBuffer->Resize(width, height);
    _depthStencil->Resize(width, height);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
    _frameGraph->ResizeFrameSlotResources({width, height});
}

GDX12Material* RenderModule::GetMaterialByName(const std::string& name)
{
    auto it = _materials.find(name);
    if (it != _materials.end()) { return it->second.get(); }

    std::string errorMsg = "ERROR: Material " + name + " not found in materials directory.\n";
    OutputDebugStringA(errorMsg.c_str());
    return nullptr;
}

GDX12Material* RenderModule::CreateMaterial(const std::string& name)
{
    if (_materials.find(name) != _materials.end()) 
    {
        std::string errorMsg = "ERROR: Material with name " + name + " already exists in materials directory.\n";
        OutputDebugStringA(errorMsg.c_str());
        return nullptr;
    }

    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }

    auto material = std::unique_ptr<GDX12Material>(new GDX12Material());
    material->Name = name;
    material->_CBufferIndex = static_cast<uint32_t>(_materials.size());
    _materials[name] = std::move(material);
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));

    return _materials[name].get();
}

GPUTexture* RenderModule::GetTextureByName(const std::string& name)
{
    auto it = _textures.find(name);
    if (it != _textures.end()) { return it->second.get(); }

    std::string errorMsg = "ERROR: Texture " + name + " not found in textures directory.\n";
    OutputDebugStringA(errorMsg.c_str());
    return nullptr;
}

GPUTexture* RenderModule::CreateTexture(const std::string& name, const Texture* texture)
{
    if (_textures.find(name) != _textures.end())
    {
        std::string errorMsg = "WARNING: Texture with name " + name + " already exists in Textures directory. Texture creation Skipped\n";
        OutputDebugStringA(errorMsg.c_str());
        return nullptr;
    }

    _textures[name] = std::make_unique<GPUTexture>();
    _textures[name]->Name = name;
    _textures[name]->Source = *texture;
    if (GetDevicePassFlags(DEVICE_ROLE_PRIMARY) & RENDER_PASS_FLAG_USE_MATERIALS)
    { _textures[name]->PrimaryDeviceTexture = CreateDX12Texture(name, &_primaryResources, texture, texture->GetType()); }
    if (GetDevicePassFlags(DEVICE_ROLE_SECONDARY) & RENDER_PASS_FLAG_USE_MATERIALS)
    { _textures[name]->SecondaryDeviceTexture = CreateDX12Texture(name, &_secondaryResources, texture, texture->GetType()); }

    return _textures[name].get();
}

GDX12Texture* RenderModule::GetSkyBoxMap(EDeviceRole role)
{
    auto& resources = GetDeviceResources(role);
    const std::string name = "Environment/kloofendal_48d_partly_cloudy_puresky_4096.dds";
    auto found = resources.Textures.find(name);
    if (found != resources.Textures.end()) { return found->second.get(); }

    const auto* asset = AssetManager::GetInstance().LoadTexture(std::filesystem::path(TEXTURES_FOLDER) / name);
    if (!asset || !asset->IsCubeMap() || asset->GetArraySize() != 6 || asset->GetSubresourceCount() != 6 * asset->GetMipLevels())
    { throw std::runtime_error("Expected a complete environment cubemap: " + name); }
    return CreateDX12Texture(name, &resources, asset, ETextureType::Color);
}

GDX12Texture* RenderModule::CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture, ETextureType type)
{
    GDX12TextureDesc desc;
    desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
    desc.Format = texture->GetFormat();
    if (type == ETextureType::Color) { desc.Format = DirectX::MakeSRGB(desc.Format); }
    if (type == ETextureType::Data) { desc.Format = DirectX::MakeLinear(desc.Format); }
    desc.SRVDesc.Format = desc.Format;
    desc.Semantic = ConvertTextureSemantic(type);
    desc.Width = texture->GetWidth();
    desc.Height = texture->GetHeight();

    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    uint32_t numMipLevels = texture->GetMipLevels();
    uint32_t numArraySlices = texture->GetArraySize();
    desc.MipLevels = numMipLevels;
    desc.ArraySize = numArraySlices;
    desc.IsCube = texture->IsCubeMap() && numArraySlices == 6;

    ComPtr<ID3D12Resource> textureResource = nullptr;
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);

    if (texture->IsCubeMap())
    {
        // CubeMap: 6 faces, array size is multiple of 6
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.SRVDesc.TextureCube.MipLevels = numMipLevels;
        desc.SRVDesc.TextureCube.MostDetailedMip = 0;
        desc.SRVDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex, TextureCube_RangeLength);

    }
    else
    {
        // 2D Texture
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = numMipLevels;
        desc.SRVDesc.Texture2D.MostDetailedMip = 0;
        desc.SRVDesc.Texture2D.PlaneSlice = 0;
        desc.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);

    }

    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, numArraySlices, numMipLevels);
    ThrowIfFailed(resources->Device->GetDevice()->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&textureResource)));

    UINT totalSubresources = numMipLevels * numArraySlices;

    UINT64 totalSize = 0;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(totalSubresources);
    std::vector<UINT> rowCounts(totalSubresources);
    std::vector<UINT64> rowSizes(totalSubresources);

    resources->Device->GetDevice()->GetCopyableFootprints(
        &texDesc, 0, totalSubresources,
        0, footprints.data(), rowCounts.data(), rowSizes.data(), &totalSize);

    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);

    ComPtr<ID3D12Resource> uploadBuffer;
    resources->Device->GetDevice()->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    void* mappedData = nullptr;
    uploadBuffer->Map(0, nullptr, &mappedData);

    for (uint32_t arraySlice = 0; arraySlice < numArraySlices; arraySlice++)
    {
        for (uint32_t mipLevel = 0; mipLevel < numMipLevels; mipLevel++)
        {
            uint32_t subresourceIndex = arraySlice * numMipLevels + mipLevel;
            auto& subresource = texture->GetSubresource(mipLevel, arraySlice);

            uint8_t* dest = static_cast<uint8_t*>(mappedData) + footprints[subresourceIndex].Offset;
            const uint8_t* src = reinterpret_cast<const uint8_t*>(subresource.Data.data());

            size_t srcRowPitch = subresource.RowPitch;
            size_t dstRowPitch = footprints[subresourceIndex].Footprint.RowPitch;
            size_t numRows = rowCounts[subresourceIndex];

            for (size_t row = 0; row < numRows; row++)
            {
                memcpy(dest + row * dstRowPitch, src + row * srcRowPitch, std::min(srcRowPitch, dstRowPitch));
            }
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    auto cmdQueue = resources->Device->GetCommandQueue();
    auto cmdList = cmdQueue->GetCommandList();

    for (uint32_t arraySlice = 0; arraySlice < numArraySlices; arraySlice++)
    {
        for (uint32_t mipLevel = 0; mipLevel < numMipLevels; mipLevel++)
        {
            uint32_t subresourceIndex = arraySlice * numMipLevels + mipLevel;

            D3D12_TEXTURE_COPY_LOCATION destLocation = {};
            destLocation.pResource = textureResource.Get();
            destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destLocation.SubresourceIndex = subresourceIndex;

            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = uploadBuffer.Get();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLocation.PlacedFootprint = footprints[subresourceIndex];

            cmdList->GetCommandList()->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
        }
    }

    constexpr auto shaderReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        textureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        shaderReadState);

    cmdList->GetCommandList()->ResourceBarrier(1, &Barrier);

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    desc.ExternalResource = textureResource;

    resources->Textures[name] = std::make_unique<GDX12Texture>();
    resources->Textures[name]->Initialize(desc);
    resources->Textures[name]->GetResource()->SetCurrentState(shaderReadState);

    return resources->Textures[name].get();
}

void RenderModule::SubmitMesh(const Mesh* mesh, MeshHandle handle)
{
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    if (_meshes.insert_or_assign(handle.GetValue(), mesh).second) { _meshUploadOrder.push_back(handle); }
    if (GetDevicePassFlags(DEVICE_ROLE_PRIMARY) & RENDER_PASS_FLAG_USE_GEOMETRY)
    { _primaryResources.GeometryBuffer->AddMesh(mesh, handle); }
    if (GetDevicePassFlags(DEVICE_ROLE_SECONDARY) & RENDER_PASS_FLAG_USE_GEOMETRY)
    { _secondaryResources.GeometryBuffer->AddMesh(mesh, handle); }
}

void RenderModule::SetActiveCamera(CameraComponent* camera)
{
    _RPcommonData.ActiveCameraCBufferIndex = camera->_CBufferIndex;
    _RPcommonData.ActiveCameraFOV = camera->FOV;
    _RPcommonData.ActiveCameraNearPlane = camera->NearPlane;
    _RPcommonData.ActiveCameraFarPlane = camera->FarPlane;
}

TransformCompGPUData& RenderModule::GetTransformGPUData(World& world, Entity entity)
{
    return _transformGPUData.at(&world).at(entity);
}

GDX12RayTracingScene& RenderModule::GetRayTracingScene(EDeviceRole role)
{
    auto& resources = GetDeviceResources(role);
    if (!resources.RayTracingScene)
    { throw std::logic_error("ERROR: The pipeline must declare RENDER_PASS_FLAG_USE_RAYTRACING on this GPU."); }
    return *resources.RayTracingScene;
}

void RenderModule::UpdateRayTracingInstance(const StaticMeshRenderComponent& component, const Matrix& transform)
{
    if (component._CBufferIndices.empty()) { return; }
    for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
        if (PipelineHasFlag(role, RENDER_PASS_FLAG_USE_RAYTRACING))
            GetRayTracingScene(role).SetInstance(component._CBufferIndices.front(), component._boundMeshHandler, transform);
}

void RenderModule::RemoveRayTracingInstance(const StaticMeshRenderComponent& component)
{
    if (component._CBufferIndices.empty()) { return; }
    for (auto* resources : {&_primaryResources, &_secondaryResources})
        if (resources->RayTracingScene)
            resources->RayTracingScene->RemoveInstance(component._CBufferIndices.front());
}

void RenderModule::SubscribeToWorld(World& world)
{
    if (_worldSubscriptions.find(&world) != _worldSubscriptions.end())
    {
        return;
    }

    auto& ecs = world.GetECS();

    auto& transformPool = ecs.GetPool<TransformComponent>();
    auto& cameraPool = ecs.GetPool<CameraComponent>();
    auto& renderCompPool = ecs.GetPool<StaticMeshRenderComponent>();

    WorldRenderSubscriptions subscriptions;
    auto& lightPool = ecs.GetPool<LightComponent>();
    auto markLightDirty = [](Entity, LightComponent& light) { light.DirtyFlag = true; };
    subscriptions.LightCreated = lightPool.OnComponentCreated.AddListener(markLightDirty);
    subscriptions.LightUpdated = lightPool.OnComponentUpdated.AddListener(markLightDirty);

    subscriptions.TransformCreated =
        transformPool.OnComponentCreated.AddListener(
            [this, &world](Entity entity, TransformComponent& component)
            {
                OnTransformComponentCreated(world, entity, component);
            });

    subscriptions.TransformDestroyed =
        transformPool.OnComponentDestroyed.AddListener(
            [this, &world](Entity entity, TransformComponent& component)
            {
                OnTransformComponentDestroyed(world, entity, component);
            });

    subscriptions.TransformUpdated =
        transformPool.OnComponentUpdated.AddListener(
            [this, &world](Entity entity, TransformComponent& component)
            {
                OnTransformComponentUpdated(world, entity, component);
            });

    subscriptions.CameraCreated =
        cameraPool.OnComponentCreated.AddListener(
            [this, &world](Entity entity, CameraComponent& component)
            {
                OnCameraComponentCreated(world, entity, component);
            });

    subscriptions.CameraDestroyed =
        cameraPool.OnComponentDestroyed.AddListener(
            [this, &world](Entity entity, CameraComponent& component)
            {
                OnCameraComponentDestroyed(world, entity, component);
            });

    subscriptions.CameraUpdated =
        cameraPool.OnComponentUpdated.AddListener(
            [this, &world](Entity entity, CameraComponent& component)
            {
                OnCameraComponentUpdated(world, entity, component);
            });

    subscriptions.RenderCompCreated =
        renderCompPool.OnComponentCreated.AddListener(
            [this, &world](Entity entity, StaticMeshRenderComponent& component)
            {
                OnRenderComponentCreated(world, entity, component);
            });

    subscriptions.RenderCompDestroyed =
        renderCompPool.OnComponentDestroyed.AddListener(
            [this, &world](Entity entity, StaticMeshRenderComponent& component)
            {
                OnRenderComponentDestroyed(world, entity, component);
            });

    subscriptions.RenderCompUpdated =
        renderCompPool.OnComponentUpdated.AddListener(
            [this, &world](Entity entity, StaticMeshRenderComponent& component)
            {
                OnRenderComponentUpdated(world, entity, component);
            });

    _worldSubscriptions[&world] = subscriptions;
    ecs.ForEach<TransformComponent>([&](Entity entity, TransformComponent& transform)
    {
        if (!_transformGPUData[&world].count(entity)) { OnTransformComponentCreated(world, entity, transform); }
    });
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity entity, StaticMeshRenderComponent& mesh)
    {
        if (mesh._CBufferIndices.empty()) OnRenderComponentCreated(world, entity, mesh);
        else
        {
            mesh.DirtyFlag = true;
            WriteMeshDrawCommands(mesh);
        }
    });
}

void RenderModule::UnsubscribeFromWorld(World& world)
{
    auto it = _worldSubscriptions.find(&world);

    if (it == _worldSubscriptions.end())
    {
        return;
    }

    const WorldRenderSubscriptions subscriptions = it->second;

    auto& ecs = world.GetECS();
    auto& transformPool = ecs.GetPool<TransformComponent>();
    auto& cameraPool = ecs.GetPool<CameraComponent>();

    auto& meshes = ecs.GetPool<StaticMeshRenderComponent>();
    meshes.OnComponentCreated.RemoveListener(subscriptions.RenderCompCreated);
    meshes.OnComponentDestroyed.RemoveListener(subscriptions.RenderCompDestroyed);
    meshes.OnComponentUpdated.RemoveListener(subscriptions.RenderCompUpdated);
    // This world's components are still alive. Disable all of its commands in one queue wait per adapter.
    std::vector<UINT> instances;
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity, const StaticMeshRenderComponent& mesh)
    {
        RemoveRayTracingInstance(mesh);
        instances.insert(instances.end(), mesh._CBufferIndices.begin(), mesh._CBufferIndices.end());
    });
    DisableMeshInstanceDrawCommands(instances);


    transformPool.OnComponentCreated.RemoveListener(subscriptions.TransformCreated);
    transformPool.OnComponentDestroyed.RemoveListener(subscriptions.TransformDestroyed);
    transformPool.OnComponentUpdated.RemoveListener(subscriptions.TransformUpdated);

    cameraPool.OnComponentCreated.RemoveListener(subscriptions.CameraCreated);
    cameraPool.OnComponentDestroyed.RemoveListener(subscriptions.CameraDestroyed);
    cameraPool.OnComponentUpdated.RemoveListener(subscriptions.CameraUpdated);

    auto& lightPool = ecs.GetPool<LightComponent>();
    lightPool.OnComponentCreated.RemoveListener(subscriptions.LightCreated);
    lightPool.OnComponentUpdated.RemoveListener(subscriptions.LightUpdated);

    _transformGPUData.erase(&world);
    _worldSubscriptions.erase(it);
}

GDX12DeviceResources& RenderModule::GetDeviceResources(EDeviceRole role)
{
    if (role == DEVICE_ROLE_PRIMARY) { return _primaryResources; }
    if (role == DEVICE_ROLE_SECONDARY) { return _secondaryResources; }
    throw std::invalid_argument("ERROR: Given render device role is unsupported. Use DEVICE_ROLE_PRIMARY or DEVICE_ROLE_SECONDARY.");
}

GDX12FrameConstants* RenderModule::GetCurrentFrameConstants(EDeviceRole role)
{
    auto& resources = GetDeviceResources(role);
    return resources.FrameConstants.at(resources.CurrFrameConstantsIndex).get();
}

const GPUMesh* RenderModule::GetGPUMesh(EDeviceRole role, MeshHandle handle)
{
    return GetDeviceResources(role).GeometryBuffer->GetGPUMeshByHandle(handle);
}

uint32_t RenderModule::GetDevicePassFlags(EDeviceRole role) const
{
    return _frameGraph ? _frameGraph->GetDevicePassFlags(role) : RENDER_PASS_FLAG_NONE;
}

bool RenderModule::PipelineHasFlag(EDeviceRole role, uint32_t flag) const
{
    return (GetDevicePassFlags(role) & flag) != 0;
}

RenderPipelineCommonData* RenderModule::GetRenderPipelineCommonData()
{
    return &_RPcommonData;
}

void RenderModule::OnTransformComponentCreated(World& world, Entity entity, TransformComponent& component)
{
    component.DirtyFlag = true;
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    auto& data = _transformGPUData[&world][entity];
    data.CBufferIndex = _transformCount++;
    data.NumFramesDirty = GetFrameSlotCount();
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));
    if (world.GetECS().Has<StaticMeshRenderComponent>(entity))
        world.GetECS().Get<StaticMeshRenderComponent>(entity).DirtyFlag = true;
    if (world.GetECS().Has<CameraComponent>(entity))
        world.GetECS().Get<CameraComponent>(entity).DirtyFlag = true;
    if (world.GetECS().Has<LightComponent>(entity))
        world.GetECS().Get<LightComponent>(entity).DirtyFlag = true;
}

void RenderModule::OnTransformComponentDestroyed(World& world, Entity entity, TransformComponent& component)
{
    if (world.GetECS().Has<StaticMeshRenderComponent>(entity))
    {
        auto& mesh = world.GetECS().Get<StaticMeshRenderComponent>(entity);
        if (mesh._shadowCasterInitialized && GetTransformGPUData(world, entity).World != Matrix::Identity)
            mesh._shadowCasterDynamic = true;
        mesh.DirtyFlag = true;
    }
    _transformGPUData.at(&world).erase(entity);
    if (world.GetECS().Has<CameraComponent>(entity))
        world.GetECS().Get<CameraComponent>(entity).DirtyFlag = true;
    if (world.GetECS().Has<LightComponent>(entity))
        world.GetECS().Get<LightComponent>(entity).DirtyFlag = true;
}

void RenderModule::OnTransformComponentUpdated(World& world, Entity entity, TransformComponent& component)
{
    component.DirtyFlag = true;
}

void RenderModule::OnCameraComponentCreated(World& world, Entity entity, CameraComponent& component)
{
    component.DirtyFlag = true;
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    component._CBufferIndex = _cameraCount++;
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));
}

void RenderModule::OnCameraComponentDestroyed(World& world, Entity entity, CameraComponent& component)
{
}

void RenderModule::OnCameraComponentUpdated(World& world, Entity entity, CameraComponent& component)
{
    component.DirtyFlag = true;
}

void RenderModule::OnRenderComponentCreated(World& world, Entity entity, StaticMeshRenderComponent& component)
{
    // A copied component belongs to a new entity and receives its own draw slots.
    component._CBufferIndices.clear();
    component._boundMeshHandler = {};
    component._shadowCasterInitialized = false;
    component._shadowCasterDynamic = false;
    BindMesh(component);
}

void RenderModule::OnRenderComponentDestroyed(World& world, Entity entity, StaticMeshRenderComponent& component)
{
    RemoveRayTracingInstance(component);
    DisableMeshInstanceDrawCommands(component._CBufferIndices);
}

void RenderModule::OnRenderComponentUpdated(World& world, Entity entity, StaticMeshRenderComponent& component)
{
    if (component.MeshHandler != component._boundMeshHandler)
    {
        RemoveRayTracingInstance(component);
        BindMesh(component);
    }
    component.DirtyFlag = true;
}

void RenderModule::BindMesh(StaticMeshRenderComponent& component)
{
    const auto* mesh = _meshes.at(component.MeshHandler.GetValue());
    for (const auto& subMesh : mesh->GetSubMeshes())
    {
        if (subMesh.MaterialIndex >= component.Materials.size() || !component.Materials[subMesh.MaterialIndex])
            throw std::invalid_argument("Missing material slot for the mesh. Set Materials before notifying the component update.");
    }

    // Growing the shared command and instance buffers requires all previous users to finish.
    component._CBufferIndices.reserve(mesh->GetSubMeshCount());
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    DisableMeshInstanceDrawCommands(component._CBufferIndices);
    component._CBufferIndices.clear();
    // Keep submesh indices contiguous and in mesh order for DXR InstanceID + GeometryIndex lookup.
    for (size_t i = 0; i < mesh->GetSubMeshCount(); ++i)
        component._CBufferIndices.push_back(_instanceCount++);
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));
    component._boundMeshHandler = component.MeshHandler;
    component.DirtyFlag = true;
    WriteMeshDrawCommands(component);
}

void RenderModule::WriteMeshDrawCommands(const StaticMeshRenderComponent& component)
{
    for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
    {
        if (!PipelineHasFlag(role, RENDER_PASS_FLAG_USE_INSTANCES)) continue;
        auto& resources = GetDeviceResources(role);
        resources.SetMeshDrawCommands(component._CBufferIndices, resources.GeometryBuffer->GetGPUMeshByHandle(component._boundMeshHandler));
    }
}

void RenderModule::DisableMeshInstanceDrawCommands(const std::vector<UINT>& instances)
{
    for (UINT instance : instances) _RPcommonData.ShadowSystem.RemoveCaster(instance);
    for (auto* resources : {&_primaryResources, &_secondaryResources})
        resources->SetMeshDrawCommands(instances, nullptr);
}

const float RenderModule::GetAspectRatio()
{
    return _window->GetAspectRatio();
}

void RenderModule::OnUpdate()
{
    if (!_frameGraph || _renderingSuspended) { return; }
    _frameGraph->WaitForCurrentFrameSlotResources();
    const auto frameSlotIndex = _frameGraph->GetCurrentFrameSlotResourcesIndex();
    _primaryResources.CurrFrameConstantsIndex = frameSlotIndex;
    _primaryResources.UpdateMainCB(_RPcommonData.WindowWidth, _RPcommonData.WindowHeight, _timer);

    if (_frameGraph->UsesDevice(DEVICE_ROLE_SECONDARY))
    {
        _secondaryResources.CurrFrameConstantsIndex = frameSlotIndex;
        _secondaryResources.UpdateMainCB(_RPcommonData.WindowWidth, _RPcommonData.WindowHeight, _timer);
    }
    UpdateMaterials();
}

void RenderModule::UpdateMaterials()
{
    const bool primary = PipelineHasFlag(DEVICE_ROLE_PRIMARY, RENDER_PASS_FLAG_USE_MATERIALS);
    const bool secondary = PipelineHasFlag(DEVICE_ROLE_SECONDARY, RENDER_PASS_FLAG_USE_MATERIALS);
    if (!primary && !secondary) return;

    for (auto& [name, material] : _materials)
    {
        if (material->DirtyFlag)
        {
            // Material type changes can add or remove shadow casters without changing the mesh.
            for (const auto& [world, subscriptions] : _worldSubscriptions)
            {
                world->GetECS().ForEach<StaticMeshRenderComponent>([&](Entity, StaticMeshRenderComponent& mesh)
                {
                    if (std::find(mesh.Materials.begin(), mesh.Materials.end(), material.get()) != mesh.Materials.end())
                        mesh.DirtyFlag = true;
                });
            }
            material->_numFramesDirty = GetFrameSlotCount();
            material->DirtyFlag = false;
        }
        if (!material->_numFramesDirty) continue;
        if (primary) _primaryResources.UpdateMaterialCB(*material);
        if (secondary) _secondaryResources.UpdateMaterialCB(*material);
        --material->_numFramesDirty;
    }
}

void RenderModule::PreRender()
{
    auto& shadows = _RPcommonData.ShadowSystem;
    const auto directionalArraySize = shadows.DirectionalArraySize;
    const auto pointSpotArraySize = shadows.PointSpotArraySize;
    shadows.BuildViews(_lightingGrid.Lights);
    if (shadows.DirectionalArraySize != directionalArraySize || shadows.PointSpotArraySize != pointSpotArraySize)
        _frameGraph->RequestFrameSlotResourcesRebuild();
    for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
    {
        if (!PipelineHasFlag(role, RENDER_PASS_FLAG_USE_LIGHTING)) continue;
        auto* frame = GetCurrentFrameConstants(role);
        frame->UploadLightingClusters(_lightingGrid);
        if (!shadows.Views.empty())
        {
            const auto count = static_cast<UINT>(shadows.Views.size());
            frame->ShadowViews->Resize(count);
            frame->ShadowViews->CopyData(0, shadows.Views.data(), count);
        }
        if (!shadows.LightUpdateFlags.empty())
        {
            const auto count = static_cast<UINT>(shadows.LightUpdateFlags.size());
            frame->ShadowCacheUpdateFlags->Resize(count);
            frame->ShadowCacheUpdateFlags->CopyData(0, shadows.LightUpdateFlags.data(), count);
        }
    }
}

void RenderModule::OnRender()
{
    if (!_frameGraph || _renderingSuspended) { return; }
    PreRender();
    _frameGraph->ExecuteFrame();
    _backBuffer->Present();
    _metrics.RecordPresentedFrame(*_frameGraph);
}

void RenderModule::DumpFrameGraphDebugReport() const
{
    if (_frameGraph) { OutputDebugStringA(_frameGraph->GetDebugReport().c_str()); }
}

void RenderModule::BuildBackBuffer(GDX12DeviceResources& presentResources)
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    _renderingSuspended = !width || !height;
    width = (std::max)(uint16_t(1), width);
    height = (std::max)(uint16_t(1), height);

    _backBuffer = std::make_unique<GDX12SwapChain>(presentResources.Device, _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, presentResources.RTVHeap.get());

    GDX12TextureDesc desc;
    desc.CreateSRV = false;
    desc.DSVHeap = presentResources.DSVHeap.get();

    desc.CreateDSV = true;
    desc.DSVHeapIndex = presentResources.DSVHeap->GetAvailableIndex();
    desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Width = width;
    desc.Height = height;
    desc.ClearValue = { 0.f, 0.f, 0.f, 0.f };

    desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    desc.DSVDesc.Texture2D.MipSlice = 0;

    _depthStencil = std::make_unique<GDX12Texture>();
    _depthStencil->Initialize(desc);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
}

void RenderModule::CompileFrameGraph(FrameGraph& graph)
{
    std::optional<EDeviceRole> presentRole;
    for (const auto& [id, node] : graph.GetDefinition().Nodes)
    {
        for (const auto& [pin, port] : node.Description.Outputs)
        {
            if (!HasUsage(port.Usages, ResourceUsage::Present)) { continue; }
            if (presentRole && *presentRole != node.DeviceRole)
            { throw std::invalid_argument("ERROR: All presentation passes must use the same GPU as the swap chain."); }
            presentRole = node.DeviceRole;
        }
    }
    if (!presentRole) { throw std::invalid_argument("ERROR: Given Render pipeline has no presentation pass. Add GDX12OutputToScreenPass to the pipeline."); }

    const auto shadowPasses = graph.GetPassNodes<GDX12ShadowMapPass>();
    UINT directionalResolution = ShadowMapSystem::DefaultDirectionalLightShadowMapResolution;
    UINT pointSpotResolution = ShadowMapSystem::DefaultPointSpotLightShadowMapResolution;
    if (!shadowPasses.empty())
    {
        directionalResolution = shadowPasses.front().GetParameter<UINT>("DirectionalLightShadowMapResolution");
        pointSpotResolution = shadowPasses.front().GetParameter<UINT>("PointSpotLightShadowMapResolution");
        GDX12ShadowMapPass::ValidateShadowMapResolutions(directionalResolution, pointSpotResolution);
        for (const auto& pass : shadowPasses)
        {
            if (pass.GetParameter<UINT>("DirectionalLightShadowMapResolution") != directionalResolution ||
                pass.GetParameter<UINT>("PointSpotLightShadowMapResolution") != pointSpotResolution)
                throw std::invalid_argument("ERROR: All Shadow Map passes must use the same resolutions because they share ShadowMapSystem projections.");
        }
    }

    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    const auto primaryFlags = graph.GetDevicePassFlags(DEVICE_ROLE_PRIMARY);
    const auto secondaryFlags = graph.GetDevicePassFlags(DEVICE_ROLE_SECONDARY);
    if ((primaryFlags | secondaryFlags) & RENDER_PASS_FLAG_USE_STREAMLINE)
    {
        auto& sdk = GDX12StreamlineSDK::Get();
        sdk.Initialize();
        if (_primaryDevice->IsInitialized()) { sdk.CreateProxyDevice(_primaryDevice->GetDevice(), true); }
    }

    if (!_primaryDevice->IsInitialized())
    {
        _primaryDevice->Role = DEVICE_ROLE_PRIMARY;
        ThrowIfFailed(_primaryDevice->Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        _primaryResources.Initialize(_primaryDevice.get());
    }

    const bool dualGPUMode = graph.UsesDevice(DEVICE_ROLE_SECONDARY);
    if (dualGPUMode && !_secondaryDevice->IsInitialized())
    {
        auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
        auto secondary = std::find_if(adapters.begin(), adapters.end(), [this](const auto& adapter)
        {
            DXGI_ADAPTER_DESC3 desc = {};
            ThrowIfFailed(adapter.Adapter->GetDesc3(&desc));
            const auto primaryLuid = _primaryDevice->GetAdapterLuid();
            return desc.AdapterLuid.LowPart != primaryLuid.LowPart || desc.AdapterLuid.HighPart != primaryLuid.HighPart;
        });
        if (secondary == adapters.end()) { throw std::runtime_error("ERROR: Given FrameGraph assigns passes to the secondary GPU, but no second adapter is available. Use "
            "AddPrimaryGPUPass() for these passes or provide a second compatible GPU."); }
        _secondaryDevice->Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(_secondaryDevice->Initialize(secondary->Adapter));
        _secondaryResources.Initialize(_secondaryDevice.get());
    }

    auto& presentResources = GetDeviceResources(*presentRole);
    if (!_backBuffer || _backBuffer->GetCurrentBuffer()->GetDevice() != presentResources.Device)
    {
        _frameGraph.reset();
        _backBuffer.reset();
        BuildBackBuffer(presentResources);
    }

    UploadSceneResources(_primaryResources, primaryFlags);
    if (dualGPUMode) { UploadSceneResources(_secondaryResources, secondaryFlags); }
    else { _secondaryResources.RayTracingScene.reset(); }
    graph.SetDeviceResources(&_primaryResources, dualGPUMode ? &_secondaryResources : nullptr, &_RPcommonData);

    _RPcommonData.ShadowSystem.DirectionalLightShadowMapResolution = directionalResolution;
    _RPcommonData.ShadowSystem.PointSpotLightShadowMapResolution = pointSpotResolution;
    _frameGraph = graph.Compile();
    MarkSceneDataDirty();
}

void RenderModule::ResizeSceneBuffers(GDX12DeviceResources& resources, uint32_t flags)
{
    auto grow = [](auto& buffer, uint32_t count)
    {
        if (buffer->GetElementCount() >= count) { return false; }
        buffer->Resize(count);
        return true;
    };
    for (auto& frame : resources.FrameConstants)
    {
        if (flags & RENDER_PASS_FLAG_USE_MATERIALS)
        { grow(frame->MaterialCache, static_cast<uint32_t>(_materials.size())); }
        if (flags & RENDER_PASS_FLAG_USE_CAMERAS) { grow(frame->CameraCB, _cameraCount); }
        if (flags & RENDER_PASS_FLAG_USE_INSTANCES)
        {
            grow(frame->TransformCache, _transformCount);
            grow(frame->InstanceCache, _instanceCount);
        }
    }
    if (flags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        const UINT oldCount = resources.IndirectCommandsCache->GetElementCount();
        const bool commandsResized = grow(resources.IndirectCommandsCache, _instanceCount);
        for (UINT i = oldCount; i < _instanceCount; ++i)
            resources.IndirectCommandsCache->CopyData(i, GDX12IndirectDrawArgs{});
        if (commandsResized && _frameGraph) { _frameGraph->RequestFrameSlotResourcesRebuild(); }
    }
}

void RenderModule::UploadSceneResources(GDX12DeviceResources& resources, uint32_t flags)
{
    ResizeSceneBuffers(resources, flags);
    if (flags & RENDER_PASS_FLAG_USE_GEOMETRY)
    {
        for (const auto handle : _meshUploadOrder)
        {
            if (!resources.GeometryBuffer->_meshCache.count(handle.GetValue()))
            { resources.GeometryBuffer->AddMesh(_meshes.at(handle.GetValue()), handle); }
        }
    }
    // Populate existing meshes when this adapter becomes an instance consumer.
    if ((flags & RENDER_PASS_FLAG_USE_INSTANCES) && !PipelineHasFlag(resources.Device->Role, RENDER_PASS_FLAG_USE_INSTANCES))
    {
        for (const auto& [world, subscriptions] : _worldSubscriptions)
        {
            world->GetECS().ForEach<StaticMeshRenderComponent>([&](Entity, const StaticMeshRenderComponent& mesh)
            {
                resources.SetMeshDrawCommands(mesh._CBufferIndices, resources.GeometryBuffer->GetGPUMeshByHandle(mesh._boundMeshHandler));
            });
        }
    }
    if (flags & RENDER_PASS_FLAG_USE_MATERIALS)
    {
        for (auto& entry : _textures)
        {
            auto& texture = *entry.second;
            auto*& gpuTexture = resources.Device == _primaryDevice.get()
                ? texture.PrimaryDeviceTexture : texture.SecondaryDeviceTexture;
            if (!gpuTexture) { gpuTexture = CreateDX12Texture(entry.first, &resources, &texture.Source, texture.Source.GetType()); }
        }
    }
    if (!(flags & RENDER_PASS_FLAG_USE_RAYTRACING)) { resources.RayTracingScene.reset(); }
}

void RenderModule::MarkSceneDataDirty()
{
    for (auto& material : _materials) { material.second->DirtyFlag = true; }
    for (auto& [world, transforms] : _transformGPUData)
        for (auto& [entity, transform] : transforms) { transform.NumFramesDirty = GetFrameSlotCount(); }
    for (const auto& subscription : _worldSubscriptions)
    {
        auto& ecs = subscription.first->GetECS();
        ecs.ForEach<CameraComponent>([](Entity, CameraComponent& camera) { camera.DirtyFlag = true; });
        ecs.ForEach<LightComponent>([](Entity, LightComponent& light) { light.DirtyFlag = true; });
        ecs.ForEach<StaticMeshRenderComponent>([](Entity, StaticMeshRenderComponent& renderer) { renderer.DirtyFlag = true; });
    }
}

void RenderModule::SubscribeToSceneManager()
{
    auto sceneManager = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();

    if (sceneManager)
    {
        _worldCreatedListener =
            sceneManager->OnWorldCreated.AddListener(
                [this](World& world)
                {
                    SubscribeToWorld(world);
                });

        _worldDestroyedListener =
            sceneManager->OnWorldDestroyed.AddListener(
                [this](World& world)
                {
                    UnsubscribeFromWorld(world);
                });

        // just in case
        for (size_t i = 0; i < sceneManager->GetWorldCount(); ++i)
        {
            World* world = sceneManager->GetWorld(i);

            if (world)
            {
                SubscribeToWorld(*world);
            }
        }
    }
}

void RenderModule::UnsubscribeFromSceneManager()
{
    auto sceneManager = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();

    if (sceneManager)
    {
        if (_worldCreatedListener != 0)
        {
            sceneManager->OnWorldCreated.RemoveListener(_worldCreatedListener);
            _worldCreatedListener = 0;
        }

        if (_worldDestroyedListener != 0)
        {
            sceneManager->OnWorldDestroyed.RemoveListener(_worldDestroyedListener);
            _worldDestroyedListener = 0;
        }
    }

    UnsubscribeFromAllWorlds();
}

void RenderModule::UnsubscribeFromAllWorlds()
{
    std::vector<World*> worlds;
    worlds.reserve(_worldSubscriptions.size());

    for (const auto& pair : _worldSubscriptions)
    {
        worlds.push_back(pair.first);
    }

    for (World* world : worlds)
    {
        if (world)
        {
            UnsubscribeFromWorld(*world);
        }
    }
}

bool RenderModule::ShouldTick()
{
    return true;
}

bool RenderModule::ShouldRender()
{
    return _frameGraph && !_renderingSuspended && _RPcommonData.ActiveCameraCBufferIndex != UINT(-1);
}
