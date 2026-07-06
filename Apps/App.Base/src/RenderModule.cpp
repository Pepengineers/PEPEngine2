#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _dualGPUMode(false), _window(window), _timer(timer),
    _primaryPipelineFlags(0), _secondaryPipelineFlags(0)
{
    _RPcommonData.GameTimer = _timer;
}

RenderModule::~RenderModule()
{
    _primaryDevice->GetCommandQueue()->Flush();
    if (_dualGPUMode) { _secondaryDevice->GetCommandQueue()->Flush(); }

    for (auto& renderpass : _primaryRenderPassExecutionList) { renderpass->ClearDenendencies(); }
    for (auto& renderpass : _secondaryRenderPassExecutionList) { renderpass->ClearDenendencies(); }

    GDX12StreamlineSDK::Get().Shutdown();
    GDX12ShaderCompiler::Shutdown();
}

void RenderModule::Initialize()
{
    GDX12StreamlineSDK::Get().Initialize();

#if defined(DEBUG) || defined(_DEBUG)
    // Enable the D3D12 debug layer.
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif

    _primaryDevice = std::make_unique<GDX12Device>();
    _primaryDevice->Role = DEVICE_ROLE_PRIMARY;
    _primaryDevice->Initialize(GDX12DeviceFactory::GetMostPerformantAdapter().Get());
    _primaryResources.Initialize(_primaryDevice.get());

    if (false)
    {
        _secondaryDevice = std::make_unique<GDX12Device>();
        _secondaryDevice->Role = DEVICE_ROLE_SECONDARY;
        _secondaryDevice->Initialize(GDX12DeviceFactory::GetDeviceDescriptors()[1].Adapter.Get());
        _secondaryResources.Initialize(_secondaryDevice.get());
        _dualGPUMode = true;
    }

    BuildBackBuffer();
    if (_dualGPUMode) { ShareFences(); }
    ConfigureRenderPipeline();

    SubscribeToSceneManager();
}

void RenderModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
}

void RenderModule::OnResize()
{
    _primaryDevice->GetCommandQueue()->Flush();

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer->Resize(width, height);
    _depthStencil->Resize(width, height);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
    if (_RPcommonData.Upscaler) { _RPcommonData.Upscaler->QueryRenderTargetResolution(); }

    for (auto& renderPass : _primaryRenderPassExecutionList) { renderPass->Resize(); }
    for (auto& renderPass : _secondaryRenderPassExecutionList) { renderPass->Resize(); }
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

    _materials[name] = std::unique_ptr<GDX12Material>(new GDX12Material());

    _materials[name]->Name = name;
    
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
    {
        _materials[name]->_CBufferIndex = _primaryResources.FrameConstants[0]->MaterialCache->GetElementCount();

        for (auto& constants : _primaryResources.FrameConstants)
        {
            auto& CBuffer = constants->MaterialCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
        }
    }
    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
    {
        _materials[name]->_CBufferIndex = _secondaryResources.FrameConstants[0]->MaterialCache->GetElementCount();

        for (auto& constants : _secondaryResources.FrameConstants)
        {
            auto& CBuffer = constants->MaterialCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
        }
    }

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
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
    { _textures[name]->PrimaryDeviceTexture = CreateDX12Texture(name, &_primaryResources, texture); }
    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
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
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_GEOMETRY) 
    { _primaryResources.GeometryBuffer->AddMesh(mesh, handle); }
    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_GEOMETRY)
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

GDX12FrameConstants* RenderModule::GetCurrentPrimaryFrameConstants()
{
    return _primaryResources.FrameConstants[_primaryResources.CurrFrameConstantsIndex].get();
}

GDX12FrameConstants* RenderModule::GetCurrentSecondaryFrameConstants()
{
    return _secondaryResources.FrameConstants[_secondaryResources.CurrFrameConstantsIndex].get();
}

const GPUMesh* RenderModule::GetPrimaryGPUMesh(MeshHandle handle)
{
    return _primaryResources.GeometryBuffer->GetGPUMeshByHandle(handle);
}

const GPUMesh* RenderModule::GetSecondaryGPUMesh(MeshHandle handle)
{
    return _secondaryResources.GeometryBuffer->GetGPUMeshByHandle(handle);
}

GDX12UploadBuffer<GDX12IndirectDrawArgs>* RenderModule::GetPrimaryIndirectCommandsCache()
{
    return _primaryResources.IndirectCommandsCache.get();
}

GDX12UploadBuffer<GDX12IndirectDrawArgs>* RenderModule::GetSecondaryIndirectCommandsCache()
{
    return _secondaryResources.IndirectCommandsCache.get();
}

uint32_t RenderModule::GetPrimaryPipelineFlags()
{
    return _primaryPipelineFlags;
}

uint32_t RenderModule::GetSecondaryPipelineFlags()
{
    return _secondaryPipelineFlags;
}

RenderPipelineCommonData* RenderModule::GetRenderPipelineCommonData()
{
    return &_RPcommonData;
}

void RenderModule::OnTransformComponentCreated(World& world, Entity entity, TransformComponent& component)
{
    TransformCompGPUData gpuData;
    _transformGPUData[entity] = gpuData;

    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        _transformGPUData[entity].CBufferIndex = _primaryResources.FrameConstants[0]->TransformCache->GetElementCount();
        for (auto& constants : _primaryResources.FrameConstants)
        {
            auto& CBuffer = constants->TransformCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
        }
    }

    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        _transformGPUData[entity].CBufferIndex = _secondaryResources.FrameConstants[0]->TransformCache->GetElementCount();
        for (auto& constants : _secondaryResources.FrameConstants)
        {
            auto& CBuffer = constants->TransformCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
        }
    }
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
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_CAMERAS)
    {
        component._CBufferIndex = _primaryResources.FrameConstants[0]->CameraCB->GetElementCount();

        for (auto& constants : _primaryResources.FrameConstants)
        {
            auto& CBuffer = constants->CameraCB;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);

            constants->CameraVisibilityCommands.push_back(GDX12VisibilityBuffers());

            auto& buffers = constants->CameraVisibilityCommands[component._CBufferIndex];
            buffers.VisibleOpaqueCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_primaryDevice.get(), GetPrimaryIndirectCommandsCache()->GetElementCount(), EBufferType::Default, false);
            buffers.OpaqueDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(_primaryDevice.get(), 1, EBufferType::Default, false);
            buffers.VisibleTransparentCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_primaryDevice.get(), GetPrimaryIndirectCommandsCache()->GetElementCount(), EBufferType::Default, false);
            buffers.TransparentDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(_primaryDevice.get(), 1, EBufferType::Default, false);

            buffers.VisibleOpaqueCommandsCache->CreateUAV(_primaryResources.SRV_UAV_Heap.get(), _primaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.OpaqueDrawCounter->CreateUAV(_primaryResources.SRV_UAV_Heap.get(), _primaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.VisibleTransparentCommandsCache->CreateUAV(_primaryResources.SRV_UAV_Heap.get(), _primaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.TransparentDrawCounter->CreateUAV(_primaryResources.SRV_UAV_Heap.get(), _primaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        }
    }

    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_CAMERAS)
    {
        component._CBufferIndex = _secondaryResources.FrameConstants[0]->CameraCB->GetElementCount();

        for (auto& constants : _secondaryResources.FrameConstants)
        {
            auto& CBuffer = constants->CameraCB;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);

            constants->CameraVisibilityCommands.push_back(GDX12VisibilityBuffers());

            auto& buffers = constants->CameraVisibilityCommands[component._CBufferIndex];
            buffers.VisibleOpaqueCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_secondaryDevice.get(), GetSecondaryIndirectCommandsCache()->GetElementCount(), EBufferType::Default, false);
            buffers.OpaqueDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(_secondaryDevice.get(), 1, EBufferType::Default, false);
            buffers.VisibleTransparentCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_secondaryDevice.get(), GetSecondaryIndirectCommandsCache()->GetElementCount(), EBufferType::Default, false);
            buffers.TransparentDrawCounter = std::make_unique<GDX12UploadBuffer<UINT>>(_secondaryDevice.get(), 1, EBufferType::Default, false);

            buffers.VisibleOpaqueCommandsCache->CreateUAV(_secondaryResources.SRV_UAV_Heap.get(), _secondaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.OpaqueDrawCounter->CreateUAV(_secondaryResources.SRV_UAV_Heap.get(), _secondaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.VisibleTransparentCommandsCache->CreateUAV(_secondaryResources.SRV_UAV_Heap.get(), _secondaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            buffers.TransparentDrawCounter->CreateUAV(_secondaryResources.SRV_UAV_Heap.get(), _secondaryResources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        }
    }
    
}

void RenderModule::OnCameraComponentDestroyed(World& world, Entity entity, CameraComponent& component)
{
}

void RenderModule::OnCameraComponentUpdated(World& world, Entity entity, CameraComponent& component)
{
}

void RenderModule::OnRenderComponentCreated(World& world, Entity entity, StaticMeshRenderComponent& component)
{
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        auto& MeshGPUData = _primaryResources.GeometryBuffer->_meshCache[component.MeshHandler.GetValue()];
        for (int i = 0; i < MeshGPUData->SubMeshes.size(); i++)
        {
            component._CBufferIndices.push_back(_primaryResources.FrameConstants[0]->InstanceCache->GetElementCount());
            _primaryResources.IndirectCommandsCache->Resize(_primaryResources.IndirectCommandsCache->GetElementCount() + 1);

            for (auto& constants : _primaryResources.FrameConstants)
            {
                auto& CBuffer = constants->InstanceCache;
                CBuffer->Resize(CBuffer->GetElementCount() + 1);

                for (auto& buffers : constants->CameraVisibilityCommands)
                {
                    buffers.VisibleOpaqueCommandsCache->Resize(buffers.VisibleOpaqueCommandsCache->GetElementCount() + 1);
                    buffers.VisibleTransparentCommandsCache->Resize(buffers.VisibleTransparentCommandsCache->GetElementCount() + 1);
                }
            }
        }
    }

    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        auto& MeshGPUData = _secondaryResources.GeometryBuffer->_meshCache[component.MeshHandler.GetValue()];
        for (int i = 0; i < MeshGPUData->SubMeshes.size(); i++)
        {
            component._CBufferIndices.push_back(_secondaryResources.FrameConstants[0]->InstanceCache->GetElementCount());
            _secondaryResources.IndirectCommandsCache->Resize(_secondaryResources.IndirectCommandsCache->GetElementCount() + 1);

            for (auto& constants : _secondaryResources.FrameConstants)
            {
                auto& CBuffer = constants->InstanceCache;
                CBuffer->Resize(CBuffer->GetElementCount() + 1);

                for (auto& buffers : constants->CameraVisibilityCommands)
                {
                    buffers.VisibleOpaqueCommandsCache->Resize(buffers.VisibleOpaqueCommandsCache->GetElementCount() + 1);
                    buffers.VisibleTransparentCommandsCache->Resize(buffers.VisibleTransparentCommandsCache->GetElementCount() + 1);
                }
            }
        }
    }
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
    uint16_t width, height;
    _window->GetWindowSize(width, height);

    auto consoleModule = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    IConsoleVariable* ICVNumframes;
    consoleModule->TryFindConsoleVariable(L"Render.NumFrames", ICVNumframes);
    int numFrames = ICVNumframes->GetInt();

    //sync & update Primary Device
    _primaryResources.CurrFrameConstantsIndex = (_primaryResources.CurrFrameConstantsIndex + 1) % numFrames;

    auto cmdQueue = _primaryDevice->GetCommandQueue();
    auto& frameConsts = _primaryResources.FrameConstants[_primaryResources.CurrFrameConstantsIndex];

    if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
    {
        cmdQueue->CPUWaitForFenceValue(frameConsts->FenceValue);
    }

    _primaryResources.UpdateMainCB(width, height, _timer);
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
    { _primaryResources.UpdateMaterialCB(_materials); }

    // same for SecondaryDevice
    if (_dualGPUMode)
    {
        _secondaryResources.CurrFrameConstantsIndex = (_secondaryResources.CurrFrameConstantsIndex + 1) % numFrames;

        auto cmdQueue = _secondaryDevice->GetCommandQueue();
        auto& frameConsts = _secondaryResources.FrameConstants[_secondaryResources.CurrFrameConstantsIndex];

        if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
        {
            cmdQueue->CPUWaitForFenceValue(frameConsts->FenceValue);
        }

        _secondaryResources.UpdateMainCB(width, height, _timer);
        if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS) 
        { _secondaryResources.UpdateMaterialCB(_materials); }
    }
}

void RenderModule::OnRender()
{
    auto primarycmdQueue = _primaryDevice->GetCommandQueue();
    auto primarycmdList = primarycmdQueue->GetCommandList();
    auto primaryCurrentFrameConsts = GetCurrentPrimaryFrameConstants();
    for (auto& renderPass : _primaryRenderPassExecutionList) 
    { 
        if (renderPass->GetFlagValue(RENDER_PASS_FLAG_SYNC_DEVICES) && _dualGPUMode)
        {
            primarycmdQueue->ExecuteCommandList(primarycmdList);
            primarycmdQueue->WaitForOtherFence(primarycmdList->FenceValue);
            primarycmdList = primarycmdQueue->GetCommandList();
        }
        else
        {
            renderPass->Execute(primarycmdList);
        }
    }
    primarycmdQueue->ExecuteCommandList(primarycmdList);
    primaryCurrentFrameConsts->FenceValue = primarycmdList->FenceValue;

    if (_dualGPUMode)
    {
        auto secondarycmdQueue = _secondaryDevice->GetCommandQueue();
        auto secondarycmdList = secondarycmdQueue->GetCommandList();
        auto secondaryCurrentFrameConsts = GetCurrentSecondaryFrameConstants();
        for (auto& renderPass : _secondaryRenderPassExecutionList)
        {
            if (renderPass->GetFlagValue(RENDER_PASS_FLAG_SYNC_DEVICES))
            {
                secondarycmdQueue->ExecuteCommandList(secondarycmdList);
                secondarycmdQueue->WaitForOtherFence(secondarycmdList->FenceValue);
                secondarycmdList = secondarycmdQueue->GetCommandList();
            }
            else
            {
                renderPass->Execute(secondarycmdList);
            }
        }
        secondarycmdQueue->ExecuteCommandList(secondarycmdList);
        secondaryCurrentFrameConsts->FenceValue = secondarycmdList->FenceValue;
    }

    _backBuffer->Present();
}

void RenderModule::BuildBackBuffer()
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);

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

void RenderModule::ShareFences()
{
    HANDLE primaryhandle;
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateSharedHandle(
        _primaryDevice->GetCommandQueue()->GetFence().Get(),
        nullptr, GENERIC_ALL, nullptr, &primaryhandle));

    ThrowIfFailed(_secondaryDevice->GetDevice()->OpenSharedHandle(
        primaryhandle,
        IID_PPV_ARGS(&_secondaryDevice->GetCommandQueue()->GetOtherFence())));

    HANDLE secondaryhandle;
    ThrowIfFailed(_secondaryDevice->GetDevice()->CreateSharedHandle(
        _secondaryDevice->GetCommandQueue()->GetFence().Get(),
        nullptr, GENERIC_ALL, nullptr, &secondaryhandle));

    ThrowIfFailed(_primaryDevice->GetDevice()->OpenSharedHandle(
        secondaryhandle,
        IID_PPV_ARGS(&_primaryDevice->GetCommandQueue()->GetOtherFence())));
}

void RenderModule::ConfigureRenderPipeline()
{
    // Setup render passes you would like to execute
    // Add in execution order
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12BackBufferClearPass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12GPUCullingPass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12OpaquePass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12WBOITTransparencyPass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12WBOITCompositionPass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12DLSSUpscalePass>());
    _primaryRenderPassExecutionList.push_back(std::make_unique<GDX12OutputToScreenPass>());

    SetupRenderPasses();

    // Link inputs & outputs for each pass that needs it
    GDX12RenderPass* clearPass = _primaryRenderPassExecutionList[0].get();
    GDX12RenderPass* cullingPass = _primaryRenderPassExecutionList[1].get();
    GDX12RenderPass* opaquePass = _primaryRenderPassExecutionList[2].get();
    GDX12RenderPass* transparencyPass = _primaryRenderPassExecutionList[3].get();
    GDX12RenderPass* compositionPass = _primaryRenderPassExecutionList[4].get();
    GDX12RenderPass* upscalePass = _primaryRenderPassExecutionList[5].get();
    GDX12RenderPass* outputPass = _primaryRenderPassExecutionList[6].get();

    clearPass->SetInputs({ _backBuffer.get(), _depthStencil.get() });
    cullingPass->SetInputs({});
    opaquePass->SetInputs({});
    transparencyPass->SetInputs({ opaquePass->GetOutputs()[2] });
    compositionPass->SetInputs({ opaquePass->GetOutputs()[0], transparencyPass->GetOutputs()[0], transparencyPass->GetOutputs()[1] });
    upscalePass->SetInputs({ opaquePass->GetOutputs()[2], opaquePass->GetOutputs()[1], compositionPass->GetOutputs()[0] });
    outputPass->SetInputs({ upscalePass->GetOutputs()[0], _backBuffer.get()});

    opaquePass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    transparencyPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    compositionPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);

    InitializeRenderPasses();
}

void RenderModule::SetupRenderPasses()
{
    for (auto& pass : _primaryRenderPassExecutionList)
    {
        pass->Setup(&_primaryResources, &_secondaryResources, &_RPcommonData);
        _primaryPipelineFlags |= pass->GetFlags();
    }

    for (auto& pass : _secondaryRenderPassExecutionList)
    {
        pass->Setup(&_secondaryResources, &_primaryResources, &_RPcommonData);
        _secondaryPipelineFlags |= pass->GetFlags();
    }
}

void RenderModule::InitializeRenderPasses()
{
    std::vector<GDX12RenderPass*> allRenderPasses;
    for (auto& pass : _primaryRenderPassExecutionList)
        allRenderPasses.push_back(pass.get());
    for (auto& pass : _secondaryRenderPassExecutionList)
        allRenderPasses.push_back(pass.get());
    const int MAX_ITERATIONS = 100;
    int iteration = 0;
    while (!allRenderPasses.empty())
    {
        if (iteration > MAX_ITERATIONS)
        {
            OutputDebugStringA("ERROR: RenderPass linking failed after 100 attempts. This can be caused by wrong render pass inputs\n");
            break;
        }
        for (auto it = allRenderPasses.begin(); it != allRenderPasses.end();)
        {
            GDX12RenderPass* renderPass = *it;
            if (renderPass->ValidateInputs())
            {
                renderPass->Initialize();
                it = allRenderPasses.erase(it);
            }
            else { ++it; }
        }
        iteration++;
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
    return true;
}
