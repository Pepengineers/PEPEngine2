#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"

RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _dualGPUMode(false), _window(window), _timer(timer), _activeCamera(nullptr)
{
}

RenderModule::~RenderModule()
{
    _primaryDevice->GetCommandQueue()->Flush();

    GDX12ShaderCompiler::Shutdown();
}

void RenderModule::Initialize()
{
#if defined(DEBUG) || defined(_DEBUG)
    // Enable the D3D12 debug layer.
    ComPtr<ID3D12Debug> debugController;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
    debugController->EnableDebugLayer();
#endif

    _primaryDevice = std::make_unique<GDX12Device>();
    _primaryDevice->Initialize(GDX12DeviceFactory::GetMostPerformantAdapter().Get());
    _primaryDevice->Role = DEVICE_ROLE_PRIMARY;
    _primaryResources.Initialize(_primaryDevice.get());

    if (false)
    {
        _secondaryDevice = std::make_unique<GDX12Device>();
        _secondaryDevice->Initialize(GDX12DeviceFactory::GetMostPerformantAdapter().Get());
        _secondaryDevice->Role = DEVICE_ROLE_SECONDARY;
        _secondaryResources.Initialize(_primaryDevice.get());
        _dualGPUMode = true;
    }

    BuildBackBuffer();

    BuildRootSignatures();
    BuildShaders();
    BuildPSOs();

    SubscribeToSceneManager();
}

void RenderModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
}

void RenderModule::OnResize() const
{
    _primaryDevice->GetCommandQueue()->Flush();

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer->Resize(width, height);
    _depthStencil->Resize(width, height);

    _opaqueAccumTexture->Resize(width, height);
    _transparencyAccumTexture->Resize(width, height);
    _transparencyRevealageTexture->Resize(width, height);
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
    _materials[name]->_PrimaryCBufferIndex = _primaryResources.FrameConstants[0]->MaterialCache->GetElementCount();

    for (auto& constants : _primaryResources.FrameConstants)
    {
        auto& CBuffer = constants->MaterialCache;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
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
        std::string errorMsg = "ERROR: Texture with name " + name + " already exists in Textures directory.\n";
        OutputDebugStringA(errorMsg.c_str());
        return nullptr;
    }

    _textures[name] = std::make_unique<GPUTexture>();
    _textures[name]->Name = name;
    _textures[name]->PrimaryDeviceTexture = CreateDX12Texture(name, &_primaryResources, texture);
    if (_secondaryDevice) { _textures[name]->SecondaryDeviceTexture = CreateDX12Texture(name, &_secondaryResources, texture); }
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

    resources->Textures[name] = std::make_unique<GDX12Texture>(desc);

    return resources->Textures[name].get();
}

void RenderModule::SubmitMesh(const Mesh* mesh, MeshHandle handle)
{
    _primaryResources.GeometryBuffer->AddMesh(mesh, handle);
    if (_secondaryDevice) _secondaryResources.GeometryBuffer->AddMesh(mesh, handle);
}

void RenderModule::SetActiveCamera(CameraComponent* camera)
{
    _activeCamera = camera;
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

void RenderModule::OnTransformComponentCreated(World& world, Entity entity, TransformComponent& component)
{
    TransformCompGPUData gpuData;
    gpuData.CBufferIndex = _primaryResources.FrameConstants[0]->TransformCache->GetElementCount();
    _transformGPUData[entity] = gpuData;

    for (auto& constants : _primaryResources.FrameConstants)
    {
        auto& CBuffer = constants->TransformCache;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
    }

    if (_secondaryDevice)
    {

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
    component._CBufferIndex = _primaryResources.FrameConstants[0]->CameraCB->GetElementCount();

    for (auto& constants : _primaryResources.FrameConstants)
    {
        auto& CBuffer = constants->CameraCB;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
    }

    if (_secondaryDevice)
    {

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
    auto& MeshGPUData = _primaryResources.GeometryBuffer->_meshCache[component.MeshHandler.GetValue()];
    
    for (int i = 0; i < MeshGPUData->SubMeshes.size(); i++)
    {
        component._CBufferIndices.push_back(_primaryResources.FrameConstants[0]->InstanceCache->GetElementCount());
        _primaryResources.IndirectCommandsCache->Resize(_primaryResources.IndirectCommandsCache->GetElementCount() + 1);

        for (auto& constants : _primaryResources.FrameConstants)
        {
            auto& CBuffer = constants->InstanceCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
            constants->VisibleOpaqueCommandsCache->Resize(constants->VisibleOpaqueCommandsCache->GetElementCount() + 1);
            constants->VisibleTransparentCommandsCache->Resize(constants->VisibleTransparentCommandsCache->GetElementCount() + 1);
        }

    }

    if (_secondaryDevice)
    {

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
        cmdQueue->WaitForFenceValue(frameConsts->FenceValue);
    }

    _primaryResources.UpdateMainCB(width, height, _timer);
    _primaryResources.UpdateMaterialCB(_materials);

    //SecondaryDevice
    if (_secondaryDevice)
    {
        _secondaryResources.CurrFrameConstantsIndex = (_secondaryResources.CurrFrameConstantsIndex + 1) % numFrames;

        auto cmdQueue = _primaryDevice->GetCommandQueue();
        auto& frameConsts = _secondaryResources.FrameConstants[_secondaryResources.CurrFrameConstantsIndex];

        if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
        {
            cmdQueue->WaitForFenceValue(frameConsts->FenceValue);
        }

        _secondaryResources.UpdateMainCB(width, height, _timer);
        _secondaryResources.UpdateMaterialCB(_materials);
    }
}

void RenderModule::OnRender()
{
    auto cmdQueue = _primaryDevice->GetCommandQueue();

    cmdQueue->Flush();

    auto cmdList = cmdQueue->GetCommandList();
    auto CurrentBackBuffer = _backBuffer->GetCurrentBuffer();
    auto CurrentFrameConsts = GetCurrentPrimaryFrameConstants();

    cmdList->BeginPixEvent("Clear Back Buffer", Colors::Aqua);
    cmdList->SetViewport(_backBuffer->GetViewport());
    cmdList->SetScissorRect(_backBuffer->GetScissorRect());
    cmdList->EnhancedTextureBarrier({ CurrentBackBuffer->GetResource()->GetRenderTargetEnhBarrier() });
    cmdList->ResourceBarrier({ _depthStencil->GetResource()->GetDepthWriteBarrier() });
    cmdList->SetRenderTargets({ CurrentBackBuffer }, _depthStencil.get());
    cmdList->ClearRenderTargetView(CurrentBackBuffer);
    cmdList->ClearDepthStencilView(_depthStencil.get());
    cmdList->EndPixEvent();

    cmdList->BeginPixEvent("GPU Mesh Culling", Colors::Blue);

    cmdList->ResourceBarrier({ 
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().GetUnorderedAccessBarrier(),
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().GetUnorderedAccessBarrier(),
        CurrentFrameConsts->VisibleTransparentCommandsCache->GetResource().GetUnorderedAccessBarrier(),
        CurrentFrameConsts->TransparentDrawCounter->GetResource().GetUnorderedAccessBarrier() });

    cmdList->SetComputeRootSignature(_primaryResources.RootSignatures["BufferClear"].get());
    cmdList->SetPipelineState(_primaryResources.PSOs["BufferClear"]);
    cmdList->SetDescriptorHeaps({ _primaryResources.SRV_UAV_Heap.get() });
    //should probably make this into a foreach or clear multiple counters per dispatch
    cmdList->SetComputeUAV(0, CurrentFrameConsts->OpaqueDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch(1, 1, 1);
    cmdList->SetComputeUAV(0, CurrentFrameConsts->TransparentDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch(1, 1, 1);

    cmdList->SetComputeRootSignature(_primaryResources.RootSignatures["Culling"].get());
    cmdList->SetPipelineState(_primaryResources.PSOs["Culling"]);
    cmdList->SetDescriptorHeaps({ _primaryResources.SRV_UAV_Heap.get() });
    cmdList->SetComputeRootConstantBufferView(0, CurrentFrameConsts->CameraCB->
        GetElementAddress(_activeCamera->_CBufferIndex));
    cmdList->SetComputeSRV(0, CurrentFrameConsts->InstanceCache->GetSRV()->GPUHandle);
    cmdList->SetComputeSRV(1, _primaryResources.IndirectCommandsCache->GetSRV()->GPUHandle);
    cmdList->SetComputeSRV(2, CurrentFrameConsts->MaterialCache->GetSRV()->GPUHandle);
    cmdList->SetComputeUAV(0, CurrentFrameConsts->VisibleOpaqueCommandsCache->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(1, CurrentFrameConsts->OpaqueDrawCounter->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(2, CurrentFrameConsts->VisibleTransparentCommandsCache->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(3, CurrentFrameConsts->TransparentDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch((_primaryResources.IndirectCommandsCache->GetElementCount() + 63) / 64, 1, 1);

    cmdList->ResourceBarrier({
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().GetUAVBarrier(),
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().GetUAVBarrier(),
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->VisibleTransparentCommandsCache->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->TransparentDrawCounter->GetResource().GetIndirectArgsBarrier() });
    cmdList->EndPixEvent();


    cmdList->BeginPixEvent("Opaque Render Pass", Colors::ForestGreen);
    cmdList->SetGraphicsRootSignature(_primaryResources.RootSignatures["OpaquePass"].get());
    cmdList->SetPipelineState(_primaryResources.PSOs["OpaquePass"]);
    cmdList->SetGraphicsRootConstantBufferView(1, CurrentFrameConsts->MainCB->GetElementAddress(0));
    cmdList->SetGraphicsRootConstantBufferView(2, CurrentFrameConsts->CameraCB->
        GetElementAddress(_activeCamera->_CBufferIndex));
    cmdList->EnhancedTextureBarrier({ _opaqueAccumTexture->GetResource()->GetRenderTargetEnhBarrier() });
    cmdList->SetRenderTargets({ _opaqueAccumTexture.get() }, _depthStencil.get());
    cmdList->ClearRenderTargetView(_opaqueAccumTexture.get());
    cmdList->SetGeometryBuffer(_primaryResources.GeometryBuffer.get());
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->SetDescriptorHeaps({ _primaryResources.SRV_UAV_Heap.get() });
    cmdList->SetGraphicsSRV(0, CurrentFrameConsts->MaterialCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(1, CurrentFrameConsts->TransformCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(2, CurrentFrameConsts->InstanceCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(3, _primaryResources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    cmdList->ExecuteIndirect(_primaryResources.CommandSignatures["OpaquePass"].Get(), _primaryResources.IndirectCommandsCache->GetElementCount(),
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().D3DResource.Get(), 0,
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().D3DResource.Get(), 0);
    cmdList->EnhancedTextureBarrier({ _opaqueAccumTexture->GetResource()->GetPixelShaderResourceEnhBarrier() });
    cmdList->EndPixEvent();

    cmdList->BeginPixEvent("Transparent Render Pass", Colors::Aqua);
    cmdList->SetGraphicsRootSignature(_primaryResources.RootSignatures["OpaquePass"].get());
    cmdList->SetPipelineState(_primaryResources.PSOs["TransparentPass"]);
    cmdList->SetGraphicsRootConstantBufferView(1, CurrentFrameConsts->MainCB->GetElementAddress(0));
    cmdList->SetGraphicsRootConstantBufferView(2, CurrentFrameConsts->CameraCB->
        GetElementAddress(_activeCamera->_CBufferIndex));
    cmdList->EnhancedTextureBarrier({ _transparencyAccumTexture->GetResource()->GetRenderTargetEnhBarrier(),
    _transparencyRevealageTexture->GetResource()->GetRenderTargetEnhBarrier() });

    cmdList->SetRenderTargets({ _transparencyAccumTexture.get(), _transparencyRevealageTexture.get() }, 
        _depthStencil.get());
    cmdList->ClearRenderTargetView(_transparencyAccumTexture.get());
    cmdList->ClearRenderTargetView(_transparencyRevealageTexture.get());

    cmdList->SetGeometryBuffer(_primaryResources.GeometryBuffer.get());
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->SetDescriptorHeaps({ _primaryResources.SRV_UAV_Heap.get() });
    cmdList->SetGraphicsSRV(0, CurrentFrameConsts->MaterialCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(1, CurrentFrameConsts->TransformCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(2, CurrentFrameConsts->InstanceCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(3, _primaryResources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    cmdList->ExecuteIndirect(_primaryResources.CommandSignatures["OpaquePass"].Get(), _primaryResources.IndirectCommandsCache->GetElementCount(),
        CurrentFrameConsts->VisibleTransparentCommandsCache->GetResource().D3DResource.Get(), 0,
        CurrentFrameConsts->TransparentDrawCounter->GetResource().D3DResource.Get(), 0);
    cmdList->EnhancedTextureBarrier({ _transparencyAccumTexture->GetResource()->GetPixelShaderResourceEnhBarrier(),
    _transparencyRevealageTexture->GetResource()->GetPixelShaderResourceEnhBarrier() });
    cmdList->EndPixEvent();


    cmdList->BeginPixEvent("Composition Render Pass", Colors::Bisque);
    cmdList->SetGraphicsRootSignature(_primaryResources.RootSignatures["CompositionPass"].get());
    cmdList->SetPipelineState(_primaryResources.PSOs["CompositionPass"]);
    cmdList->SetRenderTargets({ CurrentBackBuffer }, _depthStencil.get());
    cmdList->SetDescriptorHeaps({ _primaryResources.SRV_UAV_Heap.get() });
    cmdList->SetGraphicsSRV(0, _opaqueAccumTexture->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(1, _transparencyAccumTexture->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(2, _transparencyRevealageTexture->GetSRV()->GPUHandle);
    cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
    cmdList->EndPixEvent();
    
    cmdList->EnhancedTextureBarrier({ CurrentBackBuffer->GetResource()->GetPresentEnhBarrier() });
    cmdList->ResourceBarrier({ _depthStencil->GetResource()->GetCommonBarrier() });

    cmdQueue->ExecuteCommandList(cmdList);
    CurrentFrameConsts->FenceValue = cmdList->FenceValue;

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

    _depthStencil = std::make_unique<GDX12Texture>(desc);

    //MOVE THESE TO RENDER PASSES
    GDX12TextureDesc desc2;
    desc2.Format = desc2.RTVDesc.Format = desc2.SRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc2.Width = width;
    desc2.Height = height;

    desc2.CreateSRV = true;
    desc2.SRV_UAV_Heap = _primaryResources.SRV_UAV_Heap.get();
    desc2.SRVHeapIndex = _primaryResources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc2.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc2.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc2.SRVDesc.Texture2D.MipLevels = 1;
    desc2.SRVDesc.Texture2D.MostDetailedMip = 0;
    desc2.SRVDesc.Texture2D.PlaneSlice = 0;
    desc2.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    desc2.CreateRTV = true;
    desc2.RTVHeap = _primaryResources.RTVHeap.get();
    desc2.RTVHeapIndex = _primaryResources.RTVHeap->GetAvailableIndex();
    desc2.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    desc2.RTVDesc.Texture2D.PlaneSlice = 0;
    desc2.RTVDesc.Texture2D.MipSlice = 0;

    _opaqueAccumTexture = std::make_unique<GDX12Texture>(desc2);

    desc2.SRVHeapIndex = _primaryResources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc2.RTVHeapIndex = _primaryResources.RTVHeap->GetAvailableIndex();
    _transparencyAccumTexture = std::make_unique<GDX12Texture>(desc2);

    desc2.Format = desc2.RTVDesc.Format = desc2.SRVDesc.Format = DXGI_FORMAT_R16_FLOAT;
    desc2.SRVHeapIndex = _primaryResources.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc2.RTVHeapIndex = _primaryResources.RTVHeap->GetAvailableIndex();
    _transparencyRevealageTexture = std::make_unique<GDX12Texture>(desc2);
}

void RenderModule::BuildRootSignatures()
{
    GDX12RootSignatureDesc desc;
    desc.NumSingleCBVSlots = 2;
    desc.NumSingleSRVSlots = 3;
    desc.StaticSamplers = GetStaticSamplers();
    desc.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    desc.Constants.push_back(1);
    _primaryResources.RootSignatures["OpaquePass"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc);

    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> args;
    D3D12_INDIRECT_ARGUMENT_DESC argConst = {};
    argConst.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    argConst.Constant.DestOffsetIn32BitValues = 0;
    argConst.Constant.Num32BitValuesToSet = 1;
    args.push_back(argConst);
    D3D12_INDIRECT_ARGUMENT_DESC argDraw = {};
    argDraw.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    args.push_back(argDraw);
    D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
    cmdSigDesc.ByteStride = sizeof(GDX12IndirectDrawArgs);
    cmdSigDesc.NumArgumentDescs = args.size();
    cmdSigDesc.pArgumentDescs = args.data();
    cmdSigDesc.NodeMask = 0;

    _primaryDevice->GetDevice()->CreateCommandSignature(&cmdSigDesc,
        _primaryResources.RootSignatures["OpaquePass"]->GetRootSignature().Get(),
        IID_PPV_ARGS(&_primaryResources.CommandSignatures["OpaquePass"]));

    GDX12RootSignatureDesc desc3;
    desc3.NumSingleCBVSlots = 1;
    desc3.NumSingleSRVSlots = 3;
    desc3.NumSingleUAVSlots = 4;
    _primaryResources.RootSignatures["Culling"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc3);

    GDX12RootSignatureDesc desc4;
    desc4.NumSingleUAVSlots = 1;
    _primaryResources.RootSignatures["BufferClear"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc4);

    GDX12RootSignatureDesc desc5;
    desc5.NumSingleSRVSlots = 3;
    desc5.StaticSamplers = GetStaticSamplers();
    _primaryResources.RootSignatures["CompositionPass"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc5);
}

void RenderModule::BuildShaders()
{
    auto& Compiler = GDX12ShaderCompiler::GetInstance();

    _primaryResources.Shaders["CullingCS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Culling.hlsl", nullptr, "CS", "cs");
    _primaryResources.Shaders["BufferClearCS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "BufferClear.hlsl", nullptr, "CS", "cs");

    _primaryResources.Shaders["OpaquePassVS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
    _primaryResources.Shaders["OpaquePassPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "PS", "ps");

    _primaryResources.Shaders["TransparentPassVS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "VS", "vs");
    _primaryResources.Shaders["TransparentPassPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "PS", "ps");

    _primaryResources.Shaders["VS_FSQuad"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
    _primaryResources.Shaders["CompositionPassPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");
}

void RenderModule::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};

    desc.InputLayout = { _primaryResources.InputLayouts["Default"].data(), (UINT)_primaryResources.InputLayouts["Default"].size() };
    desc.pRootSignature = _primaryResources.RootSignatures["OpaquePass"]->GetRootSignature().Get();
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    //reversed-Z
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = _backBuffer->GetFormat();
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.DSVFormat = _depthStencil->GetFormat();

    desc.VS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["OpaquePassVS"]->GetBufferPointer()),
        _primaryResources.Shaders["OpaquePassVS"]->GetBufferSize()
    };
    desc.PS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["OpaquePassPS"]->GetBufferPointer()),
        _primaryResources.Shaders["OpaquePassPS"]->GetBufferSize()
    };
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_primaryResources.PSOs["OpaquePass"])));

    desc.InputLayout = { nullptr, 0 };
    desc.pRootSignature = _primaryResources.RootSignatures["CompositionPass"]->GetRootSignature().Get();
    desc.DepthStencilState.DepthEnable = false;
    desc.DepthStencilState.StencilEnable = false;
    desc.VS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["VS_FSQuad"]->GetBufferPointer()),
        _primaryResources.Shaders["VS_FSQuad"]->GetBufferSize()
    };
    desc.PS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["CompositionPassPS"]->GetBufferPointer()),
        _primaryResources.Shaders["CompositionPassPS"]->GetBufferSize()
    };
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_primaryResources.PSOs["CompositionPass"])));

    desc.InputLayout = { _primaryResources.InputLayouts["Default"].data(), (UINT)_primaryResources.InputLayouts["Default"].size() };
    desc.pRootSignature = _primaryResources.RootSignatures["OpaquePass"]->GetRootSignature().Get();
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // Accumulation
    desc.BlendState.RenderTarget[0].BlendEnable = true;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Revealage
    desc.BlendState.RenderTarget[1].BlendEnable = true;
    desc.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Disable depth write
    desc.DepthStencilState.DepthEnable = true;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.DepthStencilState.StencilEnable = false;

    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = _transparencyAccumTexture->GetFormat();
    desc.RTVFormats[1] = _transparencyRevealageTexture->GetFormat();
    desc.DSVFormat = _depthStencil->GetFormat();
    desc.VS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["TransparentPassVS"]->GetBufferPointer()),
        _primaryResources.Shaders["TransparentPassVS"]->GetBufferSize()
    };
    desc.PS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["TransparentPassPS"]->GetBufferPointer()),
        _primaryResources.Shaders["TransparentPassPS"]->GetBufferSize()
    };
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_primaryResources.PSOs["TransparentPass"])));


    D3D12_COMPUTE_PIPELINE_STATE_DESC desc2 = {};
    desc2.pRootSignature = _primaryResources.RootSignatures["Culling"]->GetRootSignature().Get();
    desc2.CS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["CullingCS"]->GetBufferPointer()),
        _primaryResources.Shaders["CullingCS"]->GetBufferSize()
    };

    ThrowIfFailed(_primaryDevice->GetDevice()->CreateComputePipelineState(
        &desc2, IID_PPV_ARGS(&_primaryResources.PSOs["Culling"])));

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc3 = {};
    desc3.pRootSignature = _primaryResources.RootSignatures["BufferClear"]->GetRootSignature().Get();
    desc3.CS =
    {
        reinterpret_cast<BYTE*>(_primaryResources.Shaders["BufferClearCS"]->GetBufferPointer()),
        _primaryResources.Shaders["BufferClearCS"]->GetBufferSize()
    };

    ThrowIfFailed(_primaryDevice->GetDevice()->CreateComputePipelineState(
        &desc3, IID_PPV_ARGS(&_primaryResources.PSOs["BufferClear"])));
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
