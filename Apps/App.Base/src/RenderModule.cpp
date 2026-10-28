#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"

#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/RenderPasses/GDX12TextureClearPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FSRUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12NISUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DLSSUpscalePass.h"

RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _window(window), _timer(timer)
{
    _RPcommonData.GameTimer = _timer;
}

RenderModule::~RenderModule()
{
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    _frameGraph.reset();
    if (_primaryDevice) { _primaryDevice->Flush(); }
    if (_secondaryDevice) { _secondaryDevice->Flush(); }

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
    FrameGraph graph(_primaryDevice.get(), _secondaryDevice.get());
    ConfigureRenderPipeline(graph);
    CompileFrameGraph(graph);
    SubscribeToSceneManager();
}

void RenderModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
}

void RenderModule::OnResize()
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _renderingSuspended = !width || !height;
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    if (_renderingSuspended) { return; }

    if (_frameGraph) { _frameGraph->ReleaseFrameResourcesForResize(); }
    _backBuffer->Resize(width, height);
    _depthStencil->Resize(width, height);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
    if (_frameGraph) { _frameGraph->ResizeResources({width, height}); }
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
    { _textures[name]->PrimaryDeviceTexture = CreateDX12Texture(name, &_primaryResources, texture); }
    if (GetDevicePassFlags(DEVICE_ROLE_SECONDARY) & RENDER_PASS_FLAG_USE_MATERIALS)
    { _textures[name]->SecondaryDeviceTexture = CreateDX12Texture(name, &_secondaryResources, texture); }

    return _textures[name].get();
}

GDX12Texture* RenderModule::CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture)
{
    GDX12TextureDesc desc;
    desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
    desc.Format = desc.SRVDesc.Format = texture->GetFormat();
    desc.Semantic = ConvertTextureSemantic(texture->GetType());
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

    // TODO: add IsDiffuseTexture/useSRGB flag to Texture
    bool convertToSRGB = false;
    if (convertToSRGB) { desc.Format = desc.SRVDesc.Format = FormatToSRGB(texture->GetFormat()); }

    if (texture->IsCubeMap())
    {
        // CubeMap: 6 faces, array size is multiple of 6
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.SRVDesc.TextureCube.MipLevels = numMipLevels;
        desc.SRVDesc.TextureCube.MostDetailedMip = 0;
        desc.SRVDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex, TextureCube_RangeLength);

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            desc.Format, desc.Width, desc.Height,
            numArraySlices, numMipLevels, 1, 0,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_DIMENSION_TEXTURE2D);

        resources->Device->GetDevice()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&textureResource));
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

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            desc.Format, desc.Width, desc.Height,
            numArraySlices, numMipLevels);

        resources->Device->GetDevice()->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&textureResource));
    }

    auto& subresources = texture->GetSubresources();
    UINT totalSubresources = numMipLevels * numArraySlices;

    UINT64 totalSize = 0;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(totalSubresources);
    std::vector<UINT> rowCounts(totalSubresources);
    std::vector<UINT64> rowSizes(totalSubresources);

    auto texDesc = textureResource->GetDesc();
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

    auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        textureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cmdList->GetCommandList()->ResourceBarrier(1, &Barrier);

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    desc.ExternalResource = textureResource;

    resources->Textures[name] = std::make_unique<GDX12Texture>();
    resources->Textures[name]->Initialize(desc);

    return resources->Textures[name].get();
}

void RenderModule::SubmitMesh(const Mesh* mesh, MeshHandle handle)
{
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    _meshes[handle.GetValue()] = mesh;
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

TransformCompGPUData& RenderModule::GetTransformGPUData(Entity entity)
{
    return _transformGPUData.at(entity);
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

    transformPool.OnComponentCreated.RemoveListener(subscriptions.TransformCreated);
    transformPool.OnComponentDestroyed.RemoveListener(subscriptions.TransformDestroyed);
    transformPool.OnComponentUpdated.RemoveListener(subscriptions.TransformUpdated);

    cameraPool.OnComponentCreated.RemoveListener(subscriptions.CameraCreated);
    cameraPool.OnComponentDestroyed.RemoveListener(subscriptions.CameraDestroyed);
    cameraPool.OnComponentUpdated.RemoveListener(subscriptions.CameraUpdated);

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

GDX12UploadBuffer<GDX12IndirectDrawArgs>* RenderModule::GetIndirectCommandsCache(EDeviceRole role)
{
    // This upload buffer is shared by all frame slots; callers update it on the CPU.
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    return GetDeviceResources(role).IndirectCommandsCache.get();
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
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    auto& data = _transformGPUData[entity];
    data.CBufferIndex = _transformCount++;
    data.NumFramesDirty = GetFrameSlotCount();
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));
}

void RenderModule::OnTransformComponentDestroyed(World& world, Entity entity, TransformComponent& component)
{
    _transformGPUData.erase(entity);
}

void RenderModule::OnTransformComponentUpdated(World& world, Entity entity, TransformComponent& component)
{
}

void RenderModule::OnCameraComponentCreated(World& world, Entity entity, CameraComponent& component)
{
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
}

void RenderModule::OnRenderComponentCreated(World& world, Entity entity, StaticMeshRenderComponent& component)
{
    if (_frameGraph) { _frameGraph->WaitForGPUCompletion(); }
    const auto* mesh = _meshes.at(component.MeshHandler.GetValue());
    component._CBufferIndices.clear();
    for (size_t i = 0; i < mesh->GetSubMeshCount(); i++)
    {
        component._CBufferIndices.push_back(_instanceCount++);
    }
    ResizeSceneBuffers(_primaryResources, GetDevicePassFlags(DEVICE_ROLE_PRIMARY));
    ResizeSceneBuffers(_secondaryResources, GetDevicePassFlags(DEVICE_ROLE_SECONDARY));
}

void RenderModule::OnRenderComponentDestroyed(World& world, Entity entity, StaticMeshRenderComponent& component)
{
}

void RenderModule::OnRenderComponentUpdated(World& world, Entity entity, StaticMeshRenderComponent& component)
{
}

const float RenderModule::GetAspectRatio()
{
    return _window->GetAspectRatio();
}

void RenderModule::OnUpdate()
{
    if (!_frameGraph || _renderingSuspended) { return; }
    _frameGraph->WaitForCurrentFrameSlot();
    const auto slot = _frameGraph->GetCurrentFrameSlotIndex();
    _primaryResources.CurrFrameConstantsIndex = slot;
    _primaryResources.UpdateMainCB(_RPcommonData.WindowWidth, _RPcommonData.WindowHeight, _timer);
    if (PipelineHasFlag(DEVICE_ROLE_PRIMARY, RENDER_PASS_FLAG_USE_MATERIALS))
    { _primaryResources.UpdateMaterialCB(_materials); }

    if (_frameGraph->UsesDevice(DEVICE_ROLE_SECONDARY))
    {
        _secondaryResources.CurrFrameConstantsIndex = slot;
        _secondaryResources.UpdateMainCB(_RPcommonData.WindowWidth, _RPcommonData.WindowHeight, _timer);
        if (PipelineHasFlag(DEVICE_ROLE_SECONDARY, RENDER_PASS_FLAG_USE_MATERIALS))
        { _secondaryResources.UpdateMaterialCB(_materials); }
    }
}

void RenderModule::OnRender()
{
    if (!_frameGraph || _renderingSuspended) { return; }
    _frameGraph->ExecuteFrame(_RPcommonData);
    _backBuffer->Present();
}

void RenderModule::BuildBackBuffer()
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    _renderingSuspended = !width || !height;
    width = (std::max)(uint16_t(1), width);
    height = (std::max)(uint16_t(1), height);

    _backBuffer = std::make_unique<GDX12SwapChain>(_primaryDevice.get(), _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, _primaryResources.RTVHeap.get());

    GDX12TextureDesc desc;
    desc.CreateSRV = false;
    desc.DSVHeap = _primaryResources.DSVHeap.get();

    desc.CreateDSV = true;
    desc.DSVHeapIndex = _primaryResources.DSVHeap->GetAvailableIndex();
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
            "AddPrimaryPass() for these passes or provide a second compatible GPU."); }
        _secondaryDevice->Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(_secondaryDevice->Initialize(secondary->Adapter));
        _secondaryResources.Initialize(_secondaryDevice.get());
    }

    if (!_backBuffer) { BuildBackBuffer(); }

    PrepareSceneResources(_primaryResources, primaryFlags);
    if (dualGPUMode) { PrepareSceneResources(_secondaryResources, secondaryFlags); }

    graph.SetDeviceResources(&_primaryResources, dualGPUMode ? &_secondaryResources : nullptr, &_RPcommonData);
    BindRenderTargets(graph);

    const UInt2 previousRenderSize = {_RPcommonData.DownscaledWidth, _RPcommonData.DownscaledHeight};
    try { _frameGraph = graph.Compile(); }
    catch (...)
    {
        _RPcommonData.DownscaledWidth = previousRenderSize.x;
        _RPcommonData.DownscaledHeight = previousRenderSize.y;
        throw;
    }
    MarkSceneDataDirty();
    OutputDebugStringA(_frameGraph->GetDebugReport().c_str());
}

void RenderModule::ResizeSceneBuffers(GDX12DeviceResources& resources, uint32_t flags)
{
    auto grow = [](auto& buffer, uint32_t count)
    {
        if (buffer->GetElementCount() < count) { buffer->Resize(count); }
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
    if (flags & RENDER_PASS_FLAG_USE_INSTANCES) { grow(resources.IndirectCommandsCache, _instanceCount); }
}

void RenderModule::PrepareSceneResources(GDX12DeviceResources& resources, uint32_t flags)
{
    ResizeSceneBuffers(resources, flags);
    if (flags & RENDER_PASS_FLAG_USE_GEOMETRY)
    {
        for (const auto& mesh : _meshes)
        {
            if (!resources.GeometryBuffer->_meshCache.count(mesh.first))
            { resources.GeometryBuffer->AddMesh(mesh.second, MeshHandle(mesh.first)); }
        }
    }
    if (flags & RENDER_PASS_FLAG_USE_MATERIALS)
    {
        for (auto& entry : _textures)
        {
            auto& texture = *entry.second;
            auto*& gpuTexture = resources.Device == _primaryDevice.get()
                ? texture.PrimaryDeviceTexture : texture.SecondaryDeviceTexture;
            if (!gpuTexture) { gpuTexture = CreateDX12Texture(entry.first, &resources, &texture.Source); }
        }
    }
}

void RenderModule::MarkSceneDataDirty()
{
    for (auto& material : _materials) { material.second->DirtyFlag = true; }
    for (auto& transform : _transformGPUData) { transform.second.NumFramesDirty = GetFrameSlotCount(); }
    for (const auto& subscription : _worldSubscriptions)
    {
        auto& ecs = subscription.first->GetECS();
        ecs.ForEach<CameraComponent>([](Entity, CameraComponent& camera) { camera.DirtyFlag = true; });
        ecs.ForEach<StaticMeshRenderComponent>([](Entity, StaticMeshRenderComponent& renderer) { renderer.DirtyFlag = true; });
    }
}

void RenderModule::BindRenderTargets(FrameGraph& graph)
{
    const auto backBuffer = graph.ImportTexture("back-buffer", [this]() { return _backBuffer->GetCurrentBuffer(); });
    const auto depthTarget = graph.ImportTexture("display-depth", [this]() { return _depthStencil.get(); });
    for (const auto& clear : graph.GetPassNodes<GDX12TextureClearPass>())
    {
        graph.ConnectNodes(backBuffer, clear.GetInput(GDX12TextureClearPass::Target));
        graph.ConnectNodes(depthTarget, clear.GetInput(GDX12TextureClearPass::DepthTarget));
    }
    bool hasPresentation = false;
    for (const auto& entry : graph.GetDefinition().Nodes)
    {
        const auto& node = entry.second;
        auto handle = graph.GetNode(node.Id);
        for (const auto& port : node.Description.Outputs)
        {
            if (!HasUsage(port.second.Usages, ResourceUsage::Present)) { continue; }
            hasPresentation = true;
            const auto& target = port.second.AliasInput;
            const auto& connections = graph.GetDefinition().Connections;
            const bool connected = std::any_of(connections.begin(), connections.end(), [&](const Connection& connection)
            { return connection.Target == PinAddress{node.Id, target}; });
            if (!connected) { graph.ConnectNodes(backBuffer, handle.GetInput(target)); }
        }
    }
    if (!hasPresentation) { throw std::invalid_argument("ERROR: Given Render pipeline has no presentation pass. Add GDX12OutputToScreenPass."); }
}

void RenderModule::ConfigureRenderPipeline(FrameGraph& fg)
{
    fg.SetTransportQueuePolicy(TransportQueuePolicy::Auto);

    auto culling = fg.AddPrimaryPass<GDX12GPUCullingPass>("Culling");
    auto opaque = fg.AddPrimaryPass<GDX12OpaquePass>("Opaque");
    auto transparency = fg.AddPrimaryPass<GDX12WBOITTransparencyPass>("Transparency");
    auto composition = fg.AddPrimaryPass<GDX12WBOITCompositionPass>("Composition");

    auto clear = fg.AddPrimaryPass<GDX12TextureClearPass>("Clear");
    auto upscale = fg.AddSecondaryPass<GDX12XeSSUpscalePass>("XeSS");
    auto output = fg.AddPrimaryPass<GDX12OutputToScreenPass>("Output");

    fg.ConnectNodes(clear.GetOutput("Cleared"), output.GetInput("Target"));
    fg.ConnectNodes(culling.GetOutput("OpaqueDrawStream"), opaque.GetInput("DrawStream"));
    fg.ConnectNodes(culling.GetOutput("TransparentDrawStream"), transparency.GetInput("DrawStream"));
    fg.ConnectNodes(opaque.GetOutput("Depth"), transparency.GetInput("Depth"));
    fg.ConnectNodes(opaque.GetOutput("Color"), composition.GetInput("Opaque"));
    fg.ConnectNodes(transparency.GetOutput("Accum"), composition.GetInput("Accum"));
    fg.ConnectNodes(transparency.GetOutput("Revealage"), composition.GetInput("Revealage"));
    fg.ConnectNodes(composition.GetOutput("Color"), upscale.GetInput("Source"));
    fg.ConnectNodes(opaque.GetOutput("Depth"), upscale.GetInput("Depth"));
    fg.ConnectNodes(opaque.GetOutput("Velocity"), upscale.GetInput("Velocity"));
    fg.ConnectNodes(upscale.GetOutput("Color"), output.GetInput("Color"));
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
