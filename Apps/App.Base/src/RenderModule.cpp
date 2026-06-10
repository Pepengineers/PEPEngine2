#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "App.Base/Modules/SceneManagerModule.h"

static UINT _numFrameConstants = 3;

static AutoConsoleVariableRef NumFrameConstantVariable(
    L"Render.NumFrames",
    _numFrameConstants,
    L"How many deferred frames was rendered");


RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _dualGPUMode(false), _window(window),
    _currFrameConstantsIndex(0), _timer(timer)
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

    // TODO: Add find other adapter and use cvar
    // if (secondaryDeviceAdapter)
    // {
    // 	_secondaryDevice = std::make_unique<GDX12Device>();
    // 	_secondaryDevice->Initialize(secondaryDeviceAdapter.Get());
    //
    // 	_dualGPUMode = true;
    // }

    _inputLayouts["Default"] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    BuildDescHeapsAndBackBuffer();
    BuildRootSignatures();
    BuildShaders();
    BuildPSOs();
    BuildFrameConstants();

    _geometryBuffer = std::make_unique<GDX12GeometryBuffer>(_primaryDevice.get());
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
    _materials[name]->_CBufferIndex = _frameConstants[0]->MaterialCache->GetElementCount();

    for (auto& constants : _frameConstants)
    {
        auto& CBuffer = constants->MaterialCache;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
    }

    return _materials[name].get();
}

GDX12Texture* RenderModule::GetTextureByName(const std::string& name)
{
    auto it = _textures.find(name);
    if (it != _textures.end()) { return it->second.get(); }

    std::string errorMsg = "ERROR: Texture " + name + " not found in textures directory.\n";
    OutputDebugStringA(errorMsg.c_str());
    return nullptr;
}

GDX12Texture* RenderModule::CreateTexture(const std::string& name, const Texture* texture)
{
    if (_textures.find(name) != _textures.end())
    {
        std::string errorMsg = "ERROR: Texture with name " + name + " already exists in Textures directory.\n";
        OutputDebugStringA(errorMsg.c_str());
        return nullptr;
    }

    GDX12TextureDesc desc;
    desc.SRV_UAV_Heap = _srvuavHeap.get();
    desc.Format = desc.SRVDesc.Format = texture->GetFormat();
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
        desc.SRVHeapIndex = _srvuavHeap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            desc.Format, desc.Width, desc.Height,
            numArraySlices, numMipLevels, 1, 0,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_DIMENSION_TEXTURE2D);

        _primaryDevice->GetDevice()->CreateCommittedResource(
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
        desc.SRVHeapIndex = _srvuavHeap->GetAvailableIndex(TextureCube_StartIndex, TextureCube_RangeLength);

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            desc.Format, desc.Width, desc.Height,
            numArraySlices, numMipLevels);

        _primaryDevice->GetDevice()->CreateCommittedResource(
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
    _primaryDevice->GetDevice()->GetCopyableFootprints(
        &texDesc, 0, totalSubresources,
        0, footprints.data(), rowCounts.data(), rowSizes.data(), &totalSize);

    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);

    ComPtr<ID3D12Resource> uploadBuffer;
    _primaryDevice->GetDevice()->CreateCommittedResource(
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

    auto cmdQueue = _primaryDevice->GetCommandQueue();
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

    _textures[name] = std::make_unique<GDX12Texture>(desc);

    return _textures[name].get();
}

void RenderModule::SubmitMesh(const Mesh* mesh, MeshHandle handle)
{
    _geometryBuffer->AddMesh(mesh, handle);
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

GDX12FrameConstants* RenderModule::GetCurrentFrameConstants()
{
    return _frameConstants[_currFrameConstantsIndex].get();
}

const GPUMesh* RenderModule::GetGPUMesh(MeshHandle handle)
{
    return _geometryBuffer->GetGPUMeshByHandle(handle);
}

GDX12UploadBuffer<GDX12IndirectDrawArgs>* RenderModule::GetIndirectCommandsCache()
{
    return _IndirectCommandsCache.get();
}

void RenderModule::OnTransformComponentCreated(World& world, Entity entity, TransformComponent& component)
{
    TransformCompGPUData gpuData;
    gpuData.CBufferIndex = _frameConstants[0]->TransformCache->GetElementCount();
    _transformGPUData[entity] = gpuData;

    for (auto& constants : _frameConstants)
    {
        auto& CBuffer = constants->TransformCache;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
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
    component._CBufferIndex = _frameConstants[0]->CameraCB->GetElementCount();

    for (auto& constants : _frameConstants)
    {
        auto& CBuffer = constants->CameraCB;
        CBuffer->Resize(CBuffer->GetElementCount() + 1);
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
    auto& MeshGPUData = _geometryBuffer->_meshCache[component.MeshHandler.GetValue()];
    
    for (int i = 0; i < MeshGPUData->SubMeshes.size(); i++)
    {
        component._CBufferIndices.push_back(_frameConstants[0]->InstanceCache->GetElementCount());
        _IndirectCommandsCache->Resize(_IndirectCommandsCache->GetElementCount() + 1);

        for (auto& constants : _frameConstants)
        {
            auto& CBuffer = constants->InstanceCache;
            CBuffer->Resize(CBuffer->GetElementCount() + 1);
            constants->VisibleOpaqueCommandsCache->Resize(constants->VisibleOpaqueCommandsCache->GetElementCount() + 1);
            constants->VisibleTransparentCommandsCache->Resize(constants->VisibleTransparentCommandsCache->GetElementCount() + 1);
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

GDX12RenderCommandRecorder* RenderModule::GetCommandRecorder()
{
    return &_commandRecorder;
}

void RenderModule::OnUpdate()
{
    _currFrameConstantsIndex = (_currFrameConstantsIndex + 1) % NumFrameConstantVariable.GetValue();

    auto cmdQueue = _primaryDevice->GetCommandQueue();
    auto& frameConsts = _frameConstants[_currFrameConstantsIndex];

    if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
    {
        cmdQueue->WaitForFenceValue(frameConsts->FenceValue);
    }

    UpdateMainCB();
    UpdateMaterialCB();
}

void RenderModule::OnRender()
{
    auto cmdQueue = _primaryDevice->GetCommandQueue();

    cmdQueue->Flush();

    auto cmdList = cmdQueue->GetCommandList();
    auto CurrentBackBuffer = _backBuffer->GetCurrentBuffer();
    auto& CurrentFrameConsts = _frameConstants[_currFrameConstantsIndex];

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

    cmdList->SetComputeRootSignature(_rootSignatures["BufferClear"].get());
    cmdList->SetPipelineState(_PSOs["BufferClear"]);
    cmdList->SetDescriptorHeaps({ _srvuavHeap.get() });
    //should probably make this into a foreach
    cmdList->SetComputeUAV(0, CurrentFrameConsts->OpaqueDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch(1, 1, 1);
    cmdList->SetComputeUAV(0, CurrentFrameConsts->TransparentDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch(1, 1, 1);

    cmdList->SetComputeRootSignature(_rootSignatures["Culling"].get());
    cmdList->SetPipelineState(_PSOs["Culling"]);
    cmdList->SetDescriptorHeaps({ _srvuavHeap.get() });
    cmdList->SetComputeRootConstantBufferView(0, CurrentFrameConsts->CameraCB->
        GetElementAddress(_commandRecorder._cameraCBIndex));
    cmdList->SetComputeSRV(0, CurrentFrameConsts->InstanceCache->GetSRV()->GPUHandle);
    cmdList->SetComputeSRV(1, _IndirectCommandsCache->GetSRV()->GPUHandle);
    cmdList->SetComputeSRV(2, CurrentFrameConsts->MaterialCache->GetSRV()->GPUHandle);
    cmdList->SetComputeUAV(0, CurrentFrameConsts->VisibleOpaqueCommandsCache->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(1, CurrentFrameConsts->OpaqueDrawCounter->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(2, CurrentFrameConsts->VisibleTransparentCommandsCache->GetUAV()->GPUHandle);
    cmdList->SetComputeUAV(3, CurrentFrameConsts->TransparentDrawCounter->GetUAV()->GPUHandle);
    cmdList->Dispatch((_IndirectCommandsCache->GetElementCount() + 63) / 64, 1, 1);

    cmdList->ResourceBarrier({
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().GetUAVBarrier(),
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().GetUAVBarrier(),
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->VisibleTransparentCommandsCache->GetResource().GetIndirectArgsBarrier(),
        CurrentFrameConsts->TransparentDrawCounter->GetResource().GetIndirectArgsBarrier() });
    cmdList->EndPixEvent();


    cmdList->BeginPixEvent("Opaque Render Pass", Colors::ForestGreen);
    cmdList->SetGraphicsRootSignature(_rootSignatures["OpaquePass"].get());
    cmdList->SetGraphicsRootConstantBufferView(1, CurrentFrameConsts->MainCB->GetElementAddress(0));
    cmdList->SetGraphicsRootConstantBufferView(2, CurrentFrameConsts->CameraCB->
        GetElementAddress(_commandRecorder._cameraCBIndex));
    cmdList->SetPipelineState(_PSOs["OpaquePass"]);
    cmdList->SetGeometryBuffer(_geometryBuffer.get());
    cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->SetDescriptorHeaps({ _srvuavHeap.get() });
    cmdList->SetGraphicsSRV(0, CurrentFrameConsts->MaterialCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(1, CurrentFrameConsts->TransformCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(2, CurrentFrameConsts->InstanceCache->GetSRV()->GPUHandle);
    cmdList->SetGraphicsSRV(3, _srvuavHeap->GetGPUHandle(Texture2D_StartIndex));
    cmdList->ExecuteIndirect(_commandSignatures["OpaquePass"].Get(), _IndirectCommandsCache->GetElementCount(),
        CurrentFrameConsts->VisibleOpaqueCommandsCache->GetResource().D3DResource.Get(), 0,
        CurrentFrameConsts->OpaqueDrawCounter->GetResource().D3DResource.Get(), 0);
    cmdList->EndPixEvent();
    
    cmdList->EnhancedTextureBarrier({ CurrentBackBuffer->GetResource()->GetPresentEnhBarrier() });
    cmdList->ResourceBarrier({ _depthStencil->GetResource()->GetCommonBarrier() });

    cmdQueue->ExecuteCommandList(cmdList);
    CurrentFrameConsts->FenceValue = cmdList->FenceValue;

    _backBuffer->Present();
}

void RenderModule::BuildDescHeapsAndBackBuffer()
{
    _rtvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    
    _srvuavHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

    _dsvHeap = std::make_unique<GDX12DescriptorHeap>(_primaryDevice.get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1000,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer = std::make_unique<GDX12SwapChain>(_primaryDevice.get(), _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, _rtvHeap.get());

    GDX12TextureDesc desc;
    desc.CreateSRV = false;
    desc.DSVHeap = _dsvHeap.get();

    desc.CreateDSV = true;
    desc.DSVHeapIndex = _dsvHeap->GetAvailableIndex();
    desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Width = width;
    desc.Height = height;
    desc.ClearValue = { 0.f, 0.f, 0.f, 0.f };

    desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    desc.DSVDesc.Texture2D.MipSlice = 0;

    _depthStencil = std::make_unique<GDX12Texture>(desc);
}

void RenderModule::BuildRootSignatures()
{
    GDX12RootSignatureDesc desc;
    desc.NumSingleCBVSlots = 2;
    desc.NumSingleSRVSlots = 3;
    desc.StaticSamplers = GetStaticSamplers();
    desc.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    desc.Constants.push_back(1);
    _rootSignatures["OpaquePass"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc);

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
        _rootSignatures["OpaquePass"]->GetRootSignature().Get(),
        IID_PPV_ARGS(&_commandSignatures["OpaquePass"]));

    GDX12RootSignatureDesc desc3;
    desc3.NumSingleCBVSlots = 1;
    desc3.NumSingleSRVSlots = 3;
    desc3.NumSingleUAVSlots = 4;
    _rootSignatures["Culling"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc3);

    GDX12RootSignatureDesc desc4;
    desc4.NumSingleUAVSlots = 1;
    _rootSignatures["BufferClear"] = std::make_unique<GDX12RootSignature>(_primaryDevice.get(), desc4);
}

void RenderModule::BuildShaders()
{
    auto& Compiler = GDX12ShaderCompiler::GetInstance();

    _shaders["CullingCS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Culling.hlsl", nullptr, "CS", "cs");
    _shaders["BufferClearCS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "BufferClear.hlsl", nullptr, "CS", "cs");

    _shaders["OpaquePassVS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "VS", "vs");
    _shaders["OpaquePassPS"] = Compiler.CompileShader(_primaryDevice.get(), SHADERS_FOLDER "Test.hlsl", nullptr, "PS", "ps");
}

void RenderModule::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};

    desc.InputLayout = { _inputLayouts["Default"].data(), (UINT)_inputLayouts["Default"].size() };
    desc.pRootSignature = _rootSignatures["OpaquePass"]->GetRootSignature().Get();
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
        reinterpret_cast<BYTE*>(_shaders["OpaquePassVS"]->GetBufferPointer()),
        _shaders["OpaquePassVS"]->GetBufferSize()
    };
    desc.PS =
    {
        reinterpret_cast<BYTE*>(_shaders["OpaquePassPS"]->GetBufferPointer()),
        _shaders["OpaquePassPS"]->GetBufferSize()
    };
    ThrowIfFailed(_primaryDevice->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_PSOs["OpaquePass"])));


    D3D12_COMPUTE_PIPELINE_STATE_DESC desc2 = {};
    desc2.pRootSignature = _rootSignatures["Culling"]->GetRootSignature().Get();
    desc2.CS =
    {
        reinterpret_cast<BYTE*>(_shaders["CullingCS"]->GetBufferPointer()),
        _shaders["CullingCS"]->GetBufferSize()
    };

    ThrowIfFailed(_primaryDevice->GetDevice()->CreateComputePipelineState(
        &desc2, IID_PPV_ARGS(&_PSOs["Culling"])));

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc3 = {};
    desc3.pRootSignature = _rootSignatures["BufferClear"]->GetRootSignature().Get();
    desc3.CS =
    {
        reinterpret_cast<BYTE*>(_shaders["BufferClearCS"]->GetBufferPointer()),
        _shaders["BufferClearCS"]->GetBufferSize()
    };

    ThrowIfFailed(_primaryDevice->GetDevice()->CreateComputePipelineState(
        &desc3, IID_PPV_ARGS(&_PSOs["BufferClear"])));
}

void RenderModule::BuildFrameConstants()
{
    for (int i = 0; i < NumFrameConstantVariable.GetValue(); i++)
    {
        _frameConstants.push_back(std::make_unique<GDX12FrameConstants>(_primaryDevice.get()));

        _frameConstants[i]->MaterialCache->CreateSRV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->TransformCache->CreateSRV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->InstanceCache->CreateSRV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->VisibleOpaqueCommandsCache->CreateUAV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->OpaqueDrawCounter->CreateUAV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->VisibleTransparentCommandsCache->CreateUAV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
        _frameConstants[i]->TransparentDrawCounter->CreateUAV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
    }

    _IndirectCommandsCache = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_primaryDevice.get(), 0, EBufferType::Upload, false);
    _IndirectCommandsCache->CreateSRV(_srvuavHeap.get(), _srvuavHeap->GetAvailableIndex(ConstantsResources));
}

void RenderModule::UpdateMainCB()
{
    auto& frameRes = _frameConstants[_currFrameConstantsIndex];

    GDX12MainConstants mainConstants;

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    mainConstants.RenderTargetSize = { static_cast<float>(width), static_cast<float>(height) };
    mainConstants.TotalTime = _timer->TotalTime();
    mainConstants.DeltaTime = _timer->DeltaTime();

    frameRes->MainCB->CopyData(0, mainConstants);
}

void RenderModule::UpdateMaterialCB()
{
    auto currMaterialCB = _frameConstants[_currFrameConstantsIndex]->MaterialCache.get();
    for (auto& i : _materials)
    {
        GDX12Material* material = i.second.get();

        if (material->DirtyFlag)
        { 
            material->DirtyFlag = false;
            material->_numFramesDirty = _numFrameConstants;
        }

        if (material->_numFramesDirty > 0)
        {
            GDX12MaterialConstants materialConstants;
            materialConstants.Roughness = material->Roughness;
            materialConstants.Metallic = material->Metallic;
            materialConstants.RenderLayer = UINT(material->Type);
            
            if (material->Diffuse)
            {
                materialConstants.DiffuseIndex = material->Diffuse->GetSRV()->HeapIndex - Texture2D_StartIndex;
            }
            if (material->Normal)
            {
                materialConstants.NormalIndex = material->Normal->GetSRV()->HeapIndex - Texture2D_StartIndex;
            }
            if (material->Displacement)
            {
                materialConstants.DisplacementIndex = material->Displacement->GetSRV()->HeapIndex - Texture2D_StartIndex;
            }

            currMaterialCB->CopyData(material->_CBufferIndex, materialConstants);
            material->_numFramesDirty--;
        }
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
