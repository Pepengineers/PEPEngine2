#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "Engine.UI/UI_test.h"
#include <cassert>
#include <stdexcept>
#include <climits>
#include "App.Base/App.h"
#include "Engine.Core/AssetManager.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FrameGenerationPass.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12TextureCopyFromSharedMemoryPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12TextureCopyToSharedMemoryPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SyncPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FluidPass.h"
#include <cmath>
#include "Common/ConsoleVariables.h"

static AutoConsoleVariable<bool> GraphReuseTextures(L"Render.Graph.ReuseTextures", true,
    L"Reuse compatible transient graph textures. Disable to compare output and memory.", EConsoleVariableFlags::Default);

RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _dualGPUMode(false), _window(window), _timer(timer),
    _primaryPipelineFlags(0), _secondaryPipelineFlags(0)
{
    _RPcommonData.GameTimer = _timer;
}

RenderModule::~RenderModule()
{
    if (_primaryDevice && _primaryDevice->GetCommandQueue()) { _primaryDevice->GetCommandQueue()->Flush(); }
    if (_secondaryDevice && _secondaryDevice->GetCommandQueue()) { _secondaryDevice->GetCommandQueue()->Flush(); }
    if (_uiInitialized) Engine::UI::ShutdownUI();

    _primaryGraph.reset();
    for (auto& renderpass : _primaryRenderPasses) { renderpass->ClearDenendencies(); }
    for (auto& renderpass : _secondaryRenderPasses) { renderpass->ClearDenendencies(); }

    _primaryResources.Grass.reset();
    _primaryRenderPasses.clear();
    _secondaryRenderPasses.clear();
    _backBuffer.reset();
    GDX12ShaderCompiler::Shutdown();
    GDX12DeviceFactory::Reset();
    GDX12StreamlineSDK::Get().Shutdown();
}

void RenderModule::Initialize()
{
    Log::Info("Renderer", "Initializing rendering devices and frame resources");
    if (!GDX12StreamlineSDK::Get().Initialize())
        Log::Warning("Renderer", "Streamline unavailable; NVIDIA features will report unsupported and fall back");

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
    _primaryGPUTimer = std::make_unique<GDX12GPUTimer>(_primaryDevice.get());

    if (false)
    {
        _secondaryDevice = std::make_unique<GDX12Device>();
        _secondaryDevice->Role = DEVICE_ROLE_SECONDARY;
        _secondaryDevice->Initialize(GDX12DeviceFactory::GetDeviceDescriptors()[1].Adapter.Get());
        _secondaryResources.Initialize(_secondaryDevice.get());
        _secondaryGPUTimer = std::make_unique<GDX12GPUTimer>(_secondaryDevice.get());
        _dualGPUMode = true;
    }

    Log::Info("Renderer", "Creating back buffers");
    BuildBackBuffer();
    if (_dualGPUMode) { ShareFences(); }
    Log::Info("Renderer", "Configuring render passes and pipeline states");
    ConfigureRenderPipeline();

    Engine::UI::Initialize(_window->GetWindowHandle(), _primaryDevice.get(),
        static_cast<int>(_primaryResources.FrameConstants.size()), _backBuffer->GetFormat());
    _uiInitialized = true;

    SubscribeToSceneManager();
    Log::Info("Renderer", "Rendering initialization complete");
}

void RenderModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
}

void RenderModule::OnResize()
{
    _RPcommonData.RequestHistoryReset = true;
    _primaryDevice->GetCommandQueue()->Flush();
    if (_dualGPUMode) { _secondaryDevice->GetCommandQueue()->Flush(); }

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer->Resize(width, height);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
    if (_RPcommonData.Upscaler) { _RPcommonData.Upscaler->QueryRenderTargetResolution(); }

    for (auto& renderPass : _primaryRenderPasses) { renderPass->Resize(); }
    for (auto& renderPass : _secondaryRenderPasses) { renderPass->Resize(); }
    if (_primaryGraph) RebuildRenderGraph();
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

GPUTexture* RenderModule::CreateTexture(const std::string& name, const Texture* texture, ETextureType type)
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
    { _textures[name]->PrimaryDeviceTexture = CreateDX12Texture(name, &_primaryResources, texture, type); }
    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_MATERIALS)
    { _textures[name]->SecondaryDeviceTexture = CreateDX12Texture(name, &_secondaryResources, texture, type); }

    return _textures[name].get();
}

void RenderModule::WaitForUploads()
{
    if (_primaryTextureCommands) _primaryDevice->GetCommandQueue()->ExecuteCommandList(_primaryTextureCommands);
    if (_secondaryTextureCommands) _secondaryDevice->GetCommandQueue()->ExecuteCommandList(_secondaryTextureCommands);
    _primaryDevice->GetCommandQueue()->Flush();
    if (_dualGPUMode) _secondaryDevice->GetCommandQueue()->Flush();
    _primaryTextureCommands = _secondaryTextureCommands = nullptr;
    _textureUploadResources.clear();
    _textureUploadBytes = 0;
}

void RenderModule::BeginTextureUploadBatch(std::uint64_t budgetBytes)
{
    if (!budgetBytes || _textureUploadBudget) throw std::logic_error("Invalid or nested texture upload batch");
    _textureUploadBudget = budgetBytes;
}

void RenderModule::EndTextureUploadBatch()
{
    WaitForUploads();
    _textureUploadBudget = 0;
}

GDX12Texture* RenderModule::CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture, ETextureType type)
{
    GDX12TextureDesc desc;
    desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
    desc.Format = desc.SRVDesc.Format = texture->GetFormat();
    desc.Semantic = ConvertTextureSemantic(type);
    desc.Width = texture->GetWidth();
    desc.Height = texture->GetHeight();

    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    uint32_t numMipLevels = texture->GetMipLevels();
    uint32_t numArraySlices = texture->GetArraySize();

    GDX12ResourceHandle textureResource;

    bool convertToSRGB = type == Engine::Core::ETextureType::Color;
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

        textureResource = resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, texDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr);
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

        textureResource = resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, texDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr);
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

    if (_textureUploadBudget && _textureUploadBytes &&
        totalSize > _textureUploadBudget - (std::min)(_textureUploadBytes, _textureUploadBudget))
        WaitForUploads();
    if (_textureUploadBudget && totalSize > _textureUploadBudget)
        OutputDebugStringA("Loading: one texture upload exceeds Loading.TransientBudgetMiB; uploading alone\n");

    auto uploadBuffer = resources->Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);

    void* mappedData = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mappedData));

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
    auto*& pendingCommands = resources == &_primaryResources ? _primaryTextureCommands : _secondaryTextureCommands;
    if (_textureUploadBudget && !pendingCommands) pendingCommands = cmdQueue->GetCommandList();
    auto cmdList = _textureUploadBudget ? pendingCommands : cmdQueue->GetCommandList();
    if (_textureUploadBudget)
    {
        _textureUploadResources.push_back(uploadBuffer);
        _textureUploadResources.push_back(textureResource);
        _textureUploadBytes += totalSize;
    }

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

    if (!_textureUploadBudget)
    {
        cmdQueue->ExecuteCommandList(cmdList);
        cmdQueue->Flush();
    }

    desc.ExternalResource = textureResource;

    resources->Textures[name] = std::make_unique<GDX12Texture>();
    resources->Textures[name]->Initialize(desc);
    // The upload already transitioned every mip to SRV before the wrapper was created.
    resources->Textures[name]->GetResource()->GDX12Resource::SetCurrentState(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    return resources->Textures[name].get();
}

void RenderModule::SubmitMesh(const Mesh* mesh, MeshHandle handle)
{
    if (_primaryPipelineFlags & RENDER_PASS_FLAG_USE_GEOMETRY) 
    {
        _primaryResources.GeometryBuffer->AddMesh(mesh, handle, AssetManager::GetInstance().Meshes().GetContentRevision(handle));
        ++_primaryResources.ReflectionStaticRevision;
    }
    if (_secondaryPipelineFlags & RENDER_PASS_FLAG_USE_GEOMETRY)
    { _secondaryResources.GeometryBuffer->AddMesh(mesh, handle, AssetManager::GetInstance().Meshes().GetContentRevision(handle)); }
}

void RenderModule::SubmitSky(World& world)
{
    _skyWorld = &world;
    GDX12SkyState sky;
    sky.Source = world.GetSkySource();
    auto& ecs = world.GetECS();
    uint64_t revision = world.GetSkyRevision();
    if (sky.Source == SkySource::SkyBox)
    {
        if (!ecs.Has<SkyBoxComponent>(world.GetSkyEntity()))
            throw std::runtime_error("Selected SkyBox component was removed; choose Off or another component");
        const auto& component = ecs.Get<SkyBoxComponent>(world.GetSkyEntity());
        sky.Box = component.GetSettings();
        ValidateSkySettings(sky.Box);
        sky.Quality = GetSkyBoxQuality();
        revision ^= component.GetRevision() * 1099511628211ull;
        if (!sky.Box.Enabled || !sky.Quality) sky.Source = SkySource::Off;
    }
    else if (sky.Source == SkySource::SkyAtmosphere)
    {
        if (!ecs.Has<SkyAtmosphereComponent>(world.GetSkyEntity()))
            throw std::runtime_error("Selected SkyAtmosphere component was removed; choose Off or another component");
        const auto& component = ecs.Get<SkyAtmosphereComponent>(world.GetSkyEntity());
        sky.Atmosphere = component.GetSettings();
        ValidateSkySettings(sky.Atmosphere);
        sky.Quality = GetSkyAtmosphereQuality();
        revision ^= component.GetRevision() * 1099511628211ull;
        if (!sky.Atmosphere.Enabled || !sky.Quality) sky.Source = SkySource::Off;
    }
    uint64_t hash = revision;
    hash = (hash ^ reinterpret_cast<uintptr_t>(&world)) * 1099511628211ull;
    hash = (hash ^ world.GetSkyEntity()) * 1099511628211ull;
    hash = (hash ^ static_cast<UINT>(sky.Source)) * 1099511628211ull;
    hash = (hash ^ sky.Quality) * 1099511628211ull;
    if (_skyHash != hash)
    {
        _skyHash = hash;
        ++_primaryResources.Sky.Revision;
        _RPcommonData.RequestHistoryReset = true;
    }
    sky.Revision = _primaryResources.Sky.Revision;
    _primaryResources.Sky = std::move(sky);
}

void RenderModule::SubmitLights(std::vector<GDX12LocalLightConstants> lights, const GDX12MainConstants& settings)
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    const auto upload = [&](GDX12DeviceResources& resources)
    {
        resources.LocalLights = lights;
        resources.Lighting = settings;
        const auto& sky = _primaryResources.Sky;
        if (sky.Source == SkySource::SkyAtmosphere)
        {
            Vector3 direction = sky.Atmosphere.DirectionToSun;
            direction.Normalize();
            resources.Lighting.LightDirection = -direction;
            resources.Lighting.LightRadiance = sky.Atmosphere.SunIlluminanceLux * sky.Atmosphere.LuminanceScale;
            resources.Lighting.SunShadowFlags = sky.Atmosphere.SunCastsShadows ? 3u : 0u;
            resources.Lighting.SunShadowRevision = static_cast<UINT>(sky.Revision);
        }
        auto& cache = resources.FrameConstants[resources.CurrFrameConstantsIndex]->LocalLightCache;
        cache->Resize((std::max)(static_cast<UINT>(lights.size()), 1u));
        if (!lights.empty()) cache->CopyData(0, lights.data(), static_cast<UINT>(lights.size()));
        resources.UpdateMainCB(width, height, _timer);
    };
    upload(_primaryResources);
    if (_dualGPUMode) upload(_secondaryResources);
}

void RenderModule::SubmitCameraShadows(const GDX12ShadowConstants& shadows)
{
    _primaryResources.ShadowSettings = shadows;
    GetCurrentPrimaryFrameConstants()->ShadowCB->CopyData(0, shadows);
    if (_dualGPUMode)
    {
        _secondaryResources.ShadowSettings = shadows;
        GetCurrentSecondaryFrameConstants()->ShadowCB->CopyData(0, shadows);
    }
}

void RenderModule::SetActiveCamera(CameraComponent* camera)
{
    if (_RPcommonData.ActiveCameraCBufferIndex != camera->_CBufferIndex)
        _RPcommonData.RequestHistoryReset = true;
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

    auto& grassPool = ecs.GetPool<GrassRenderComponent>();
    const auto grassOwner = [this](Entity, GrassRenderComponent& component) { component._renderOwner = ++_grassWindOwner; };
    subscriptions.GrassCreated = grassPool.OnComponentCreated.AddListener(grassOwner);
    subscriptions.GrassUpdated = grassPool.OnComponentUpdated.AddListener(grassOwner);
    ecs.ForEach<GrassRenderComponent>(grassOwner);
    auto& windPool = ecs.GetPool<WindFieldComponent>();
    const auto windOwner = [this](Entity, WindFieldComponent& component) { component._renderOwner = ++_grassWindOwner; };
    subscriptions.WindCreated = windPool.OnComponentCreated.AddListener(windOwner);
    subscriptions.WindUpdated = windPool.OnComponentUpdated.AddListener(windOwner);
    ecs.ForEach<WindFieldComponent>(windOwner);
    auto& skeletalPool = ecs.GetPool<SkeletalMeshRenderComponent>();
    subscriptions.SkeletalDestroyed = skeletalPool.OnComponentDestroyed.AddListener(
        [this, &world](Entity entity, SkeletalMeshRenderComponent&) { ReleaseSkeletalState(world, entity); });
    subscriptions.SkeletalUpdated = skeletalPool.OnComponentUpdated.AddListener(
        [this, &world](Entity entity, SkeletalMeshRenderComponent&) { ReleaseSkeletalState(world, entity); });
    _worldSubscriptions[&world] = subscriptions;
    ++_primaryResources.ReflectionWorldRevision;
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

    auto& skeletalPool = ecs.GetPool<SkeletalMeshRenderComponent>();
    skeletalPool.OnComponentDestroyed.RemoveListener(subscriptions.SkeletalDestroyed);
    skeletalPool.OnComponentUpdated.RemoveListener(subscriptions.SkeletalUpdated);
    ecs.ForEach<SkeletalMeshRenderComponent>([&](Entity entity, SkeletalMeshRenderComponent&)
    {
        ReleaseSkeletalState(world, entity);
    });
    auto& renderPool = ecs.GetPool<StaticMeshRenderComponent>();
    renderPool.OnComponentCreated.RemoveListener(subscriptions.RenderCompCreated);
    renderPool.OnComponentDestroyed.RemoveListener(subscriptions.RenderCompDestroyed);
    renderPool.OnComponentUpdated.RemoveListener(subscriptions.RenderCompUpdated);
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity entity, StaticMeshRenderComponent& component)
    {
        OnRenderComponentDestroyed(world, entity, component);
    });
    ecs.GetPool<GrassRenderComponent>().OnComponentCreated.RemoveListener(subscriptions.GrassCreated);
    ecs.GetPool<GrassRenderComponent>().OnComponentUpdated.RemoveListener(subscriptions.GrassUpdated);
    ecs.GetPool<WindFieldComponent>().OnComponentCreated.RemoveListener(subscriptions.WindCreated);
    ecs.GetPool<WindFieldComponent>().OnComponentUpdated.RemoveListener(subscriptions.WindUpdated);
    ++_primaryResources.ReflectionStaticRevision;
    ++_primaryResources.ReflectionWorldRevision;
    if (_skyWorld == &world)
    {
        _skyWorld = nullptr;
        _skyHash = 0;
        _primaryResources.Sky.Source = SkySource::Off;
        _primaryResources.Sky.Quality = 0;
        ++_primaryResources.Sky.Revision;
        _RPcommonData.RequestHistoryReset = true;
    }
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

bool RenderModule::PrimaryPipelineHasFlag(uint32_t flag)
{
    return _primaryPipelineFlags & flag;
}

bool RenderModule::SecondaryPipelineHasFlag(uint32_t flag)
{
    return _secondaryPipelineFlags & flag;
}

void RenderModule::ReserveTransformCapacity(UINT additionalTransforms)
{
    if (additionalTransforms > UINT_MAX - _transformCount) throw std::overflow_error("Too many transforms");
    const UINT capacity = _transformCount + additionalTransforms;
    const auto grow = [capacity](GDX12DeviceResources& resources)
    {
        if (resources.FrameConstants.front()->TransformCache->GetElementCount() >= capacity) return;
        resources.Device->GetCommandQueue()->Flush();
        for (auto& frame : resources.FrameConstants) frame->TransformCache->Resize(capacity);
    };
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) grow(_primaryResources);
    if (SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) grow(_secondaryResources);
}

void RenderModule::OnTransformComponentCreated(World&, Entity entity, TransformComponent&)
{
    ReserveTransformCapacity(1);
    TransformCompGPUData data;
    data.CBufferIndex = _transformCount++;
    _transformGPUData[entity] = data;
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
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS))
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

    if (SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS))
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

void RenderModule::ReserveInstanceCapacity(UINT additionalInstances)
{
    if (additionalInstances > UINT_MAX - _instanceCount) throw std::overflow_error("Too many render instances");
    EnsureInstanceCapacity(_instanceCount + additionalInstances);
}

void RenderModule::EnsureInstanceCapacity(UINT capacity)
{
    const auto grow = [capacity](GDX12DeviceResources& resources)
    {
        if (resources.IndirectCommandsCache->GetElementCount() >= capacity) return;
        resources.Device->GetCommandQueue()->Flush();
        resources.IndirectCommandsCache->Resize(capacity);
        for (auto& frame : resources.FrameConstants)
        {
            frame->InstanceCache->Resize(capacity);
            for (auto& camera : frame->CameraVisibilityCommands)
            {
                camera.VisibleOpaqueCommandsCache->Resize(capacity);
                camera.VisibleTransparentCommandsCache->Resize(capacity);
            }
        }
    };
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) grow(_primaryResources);
    if (SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) grow(_secondaryResources);
}

void RenderModule::OnRenderComponentCreated(World&, Entity, StaticMeshRenderComponent& component)
{
    if (!PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES) &&
        !SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) return;
    const GPUMesh* mesh = PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)
        ? GetPrimaryGPUMesh(component.MeshHandler) : GetSecondaryGPUMesh(component.MeshHandler);
    const size_t count = component.GetSubMeshCount(mesh->SubMeshes.size());
    if (count > UINT_MAX - _instanceCount) throw std::overflow_error("Too many render instances");
    for (size_t i = 0; i < count; ++i)
        if (component.GetSubMeshIndex(i) >= mesh->SubMeshes.size()) throw std::out_of_range("Invalid render submesh index");
    EnsureInstanceCapacity(_instanceCount + static_cast<UINT>(count));
    component._CBufferIndices.reserve(count);
    for (UINT i = 0; i < count; ++i) component._CBufferIndices.push_back(_instanceCount + i);
    _instanceCount += static_cast<UINT>(count);
}

void RenderModule::OnRenderComponentDestroyed(World&, Entity, StaticMeshRenderComponent& component)
{
    // Removed entities must stop participating in every indirect pass, including probes.
    if (_primaryDevice) _primaryDevice->GetCommandQueue()->Flush();
    if (_secondaryDevice) _secondaryDevice->GetCommandQueue()->Flush();
    const GDX12IndirectDrawArgs empty = {};
    for (const auto index : component._CBufferIndices)
    {
        if (_primaryResources.IndirectCommandsCache) _primaryResources.IndirectCommandsCache->CopyData(index, empty);
        if (_secondaryResources.IndirectCommandsCache) _secondaryResources.IndirectCommandsCache->CopyData(index, empty);
    }
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
    GDX12_SCOPED_CPU_EVENT("Frame slot wait and upload");
    if (_timer->DeltaTime() > 0.25f) _RPcommonData.RequestHistoryReset = true;
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    if (width && height && _reconstructionPass && _reconstructionPass->ApplySettings())
    {
        OnResize();
        _primaryPipelineFlags = 0;
        for (const auto& pass : _primaryRenderPasses) _primaryPipelineFlags |= pass->GetFlags();
    }

    auto consoleModule = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    const auto numFrames = consoleModule->FindConsoleVariable(L"Render.NumFrames").GetUInt();

    //sync & update Primary Device
    _primaryResources.CurrFrameConstantsIndex = (_primaryResources.CurrFrameConstantsIndex + 1) % numFrames;

    auto cmdQueue = _primaryDevice->GetCommandQueue();
    auto& frameConsts = _primaryResources.FrameConstants[_primaryResources.CurrFrameConstantsIndex];

    if (frameConsts->FenceValue > cmdQueue->GetFence()->GetCompletedValue())
    {
        cmdQueue->CPUWaitForFenceValue(frameConsts->FenceValue);
    }

    _primaryResources.UpdateMainCB(width, height, _timer);
    // Materials are uploaded once in OnRender, after lazy component resource creation.

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
    }
}

void RenderModule::OnRender()
{
    _RPcommonData.Jobs = &App::GetInstance()->GetJobs();
    _backBuffer->GetFrameGeneration().BeginRendering(_RPcommonData);
    PrepareSkeletalMeshes();
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_MATERIALS))
        _primaryResources.UpdateMaterialCB(_materials);
    if (SecondaryPipelineHasFlag(RENDER_PASS_FLAG_USE_MATERIALS))
        _secondaryResources.UpdateMaterialCB(_materials);
    CollectParticleEmitters();
    if (_graphEarlyAO != (GetAmbientOcclusionExecutionPoint() == 0) ||
        _graphReuseTextures != GraphReuseTextures.GetValue()) RebuildRenderGraph();
    auto primarycmdQueue = _primaryDevice->GetCommandQueue();
    auto primarycmdList = primarycmdQueue->GetCommandList();
    _primaryGPUTimer->Start(primarycmdList);
    DispatchSkeletalMeshes(primarycmdList);
    CollectGrassScene(primarycmdList);
    {
        GDX12_SCOPED_CPU_EVENT("Scene preparation");
        CollectReflectionScene();
        auto visibility = _shadowVisibility.Schedule(App::GetInstance()->GetJobs(), _primaryResources.LocalLights,
            *_primaryResources.LocalShadowCasters, _primaryResources.LocalShadowVisibility);
        try
        {
            _deferredLightingPass->Prepare();
            {
                GDX12_SCOPED_CPU_EVENT("Shadow visibility wait");
                visibility.Complete();
            }
        }
        catch (...)
        {
            try { visibility.Complete(); } catch (...) {}
            throw;
        }
        _localShadowPass->Prepare();
        _reflectionPass->Prepare();
        _primaryResources.PrepareLocalShadowData();
    }

    auto primaryCurrentFrameConsts = GetCurrentPrimaryFrameConstants();
    _primaryResources.UploadLocalShadowData(primarycmdList);
    _primaryGraph->Execute(primarycmdList);
    if (_uiInitialized) Engine::UI::Render(primarycmdList, _backBuffer->GetCurrentBuffer());
    _primaryGPUTimer->Stop(primarycmdList);
    primarycmdQueue->ExecuteCommandList(primarycmdList);
    primaryCurrentFrameConsts->FenceValue = primarycmdList->FenceValue;
    _primaryGPUTimer->OnCommandListExecuted(primarycmdList->FenceValue);

    if (_dualGPUMode)
    {
        auto secondarycmdQueue = _secondaryDevice->GetCommandQueue();
        auto secondarycmdList = secondarycmdQueue->GetCommandList();
        _secondaryGPUTimer->Start(secondarycmdList);

        auto secondaryCurrentFrameConsts = GetCurrentSecondaryFrameConstants();
        _secondaryResources.PrepareLocalShadowData();
        _secondaryResources.UploadLocalShadowData(secondarycmdList);
        secondaryCurrentFrameConsts->UploadReflectionProbeData(secondarycmdList);
        for (auto& renderPass : _secondaryRenderPasses)
        {
            if (renderPass->GetFlagValue(RENDER_PASS_FLAG_SYNC_DEVICES))
            {
                secondarycmdQueue->ExecuteCommandList(secondarycmdList);
                secondarycmdQueue->WaitForOtherFence(secondarycmdList->FenceValue);
                secondarycmdList = secondarycmdQueue->GetCommandList();
            }
            else { renderPass->Execute(secondarycmdList); }
        }
        _secondaryGPUTimer->Stop(secondarycmdList);
        secondarycmdQueue->ExecuteCommandList(secondarycmdList);
        secondaryCurrentFrameConsts->FenceValue = secondarycmdList->FenceValue;
        _secondaryGPUTimer->OnCommandListExecuted(secondarycmdList->FenceValue);
    }

    _backBuffer->GetFrameGeneration().EndRendering();
    _backBuffer->Present();
}

void RenderModule::CollectGrassScene(GDX12CommandList* commands)
{
    std::vector<GDX12GrassInstance> grass;
    std::vector<GDX12WindInstance> wind;
    const auto controls = GetGrassWindControls();
    auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    if (controls.GrassEnabled && controls.GrassQuality > 0)
    {
        for (size_t worldIndex = 0; worldIndex < scenes->GetWorldCount(); ++worldIndex)
        {
            auto* world = scenes->GetWorld(worldIndex);
            auto& ecs = world->GetECS();
            ecs.ForEach<GrassRenderComponent>([&](Entity entity, GrassRenderComponent& component)
            {
                if (!component.Enabled || !component.Material || !ecs.Has<TransformComponent>(entity)) return;
                if (!IsValidGrassSettings(component)) throw std::invalid_argument("Invalid GrassRenderComponent");
                if (!GrassBladeCount(ResolveGrassSettings(component, controls.GrassQuality))) return;
                const auto material = std::find_if(_materials.begin(), _materials.end(),
                    [&](const auto& entry) { return entry.second.get() == component.Material; });
                if (material == _materials.end() || component.Material->Type == MaterialType::Transparent)
                    throw std::invalid_argument("Grass requires an owned opaque or masked material");
                if (!component._renderOwner) component._renderOwner = ++_grassWindOwner;
                const auto& transform = ecs.Get<TransformComponent>(entity);
                GDX12GrassInstance instance;
                static_cast<GDX12GrassSettings&>(instance) = component;
                instance.Id = component._renderOwner;
                instance.World = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                    XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                    XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
                instance.MaterialIndex = component.Material->_CBufferIndex;
                instance.ResetRevision = component.ResetRevision;
                instance.MaterialRevision = component.Material->_revision;
                if (component.WindEntity != InvalidEntity && ecs.Has<WindFieldComponent>(component.WindEntity) &&
                    ecs.Has<TransformComponent>(component.WindEntity))
                {
                    auto& field = ecs.Get<WindFieldComponent>(component.WindEntity);
                    if (field.Enabled && controls.WindEnabled && controls.WindQuality > 0)
                    {
                        if (!field._renderOwner) field._renderOwner = ++_grassWindOwner;
                        instance.WindId = field._renderOwner;
                        const auto found = std::find_if(wind.begin(), wind.end(), [&](const auto& item) { return item.Id == instance.WindId; });
                        if (found == wind.end())
                        {
                            GDX12WindInstance snapshot;
                            static_cast<GDX12WindSettings&>(snapshot) = field;
                            snapshot.Id = field._renderOwner;
                            const auto& location = ecs.Get<TransformComponent>(component.WindEntity).Location;
                            snapshot.CenterCm = Vector2(location.x, location.z);
                            snapshot.DeltaSeconds = field.Paused || world->IsPaused() ? 0 :
                                _timer->DeltaTime() * world->GetTimeScale();
                            wind.push_back(std::move(snapshot));
                        }
                    }
                }
                grass.push_back(std::move(instance));
            });
        }
    }
    if (!grass.empty() && !_primaryResources.Grass)
        _primaryResources.Grass = std::make_unique<GDX12GrassRenderer>(&_primaryResources);
    if (_primaryResources.Grass)
    {
        _primaryResources.Grass->BeginFrame(commands, _RPcommonData, std::move(grass), std::move(wind), controls);
        BoundingBox bounds;
        if (_primaryResources.Grass->HasCasters() && _primaryResources.Grass->GetBounds(bounds))
            SubmitCameraShadows(BuildShadowCascades(_RPcommonData.CameraView.Invert(),
                XMConvertToRadians(_RPcommonData.ActiveCameraFOV), GetAspectRatio(),
                _RPcommonData.ActiveCameraNearPlane, _RPcommonData.ActiveCameraFarPlane,
                Vector3(_primaryResources.Lighting.LightDirection), &bounds));
    }
}

void RenderModule::CollectParticleEmitters()
{
    _primaryResources.ParticleEmitters.clear();
    auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    for (size_t i = 0; i < scenes->GetWorldCount(); ++i)
    {
        auto* world = scenes->GetWorld(i);
        auto& ecs = world->GetECS();
        ecs.ForEach<ParticleEmitterComponent>([&](Entity entity, ParticleEmitterComponent& component)
        {
            if (!component.IsEnabled() || !ecs.Has<TransformComponent>(entity)) return;
            const auto& transform = ecs.Get<TransformComponent>(entity);
            GDX12ParticleEmitter emitter;
            static_cast<GDX12ParticleEmitterSettings&>(emitter) = component.GetSettings();
            emitter.Id = component.GetId();
            emitter.ResetRevision = component.GetResetRevision();
            emitter.BurstTotal = component.GetBurstTotal();
            emitter.Paused = component.IsPaused() || world->IsPaused();
            emitter.DeltaTime = _timer->DeltaTime() * world->GetTimeScale();
            emitter.World = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
            _primaryResources.ParticleEmitters.push_back(emitter);
        });
    }
}

void RenderModule::CollectReflectionScene()
{
    GDX12_SCOPED_CPU_EVENT("Scene snapshot");
    uint64_t staticHash = 14695981039346656037ull;
    uint64_t dynamicHash = staticHash;
    const auto hash = [](uint64_t& result, const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    };
    auto& resources = _primaryResources;
    if (!resources.LocalShadowCasters) resources.LocalShadowCasters.emplace();
    resources.LocalShadowCasters->clear();
    if (resources.Grass) hash(dynamicHash, resources.Grass->Revision());
    hash(staticHash, resources.ReflectionEnvironmentRevision);
    hash(staticHash, resources.Lighting.LightDirection);
    hash(staticHash, resources.Lighting.LightRadiance);
    hash(staticHash, resources.Lighting.SunShadowFlags);
    hash(staticHash, resources.Lighting.SunShadowRevision);
    for (const auto& light : resources.LocalLights) hash(staticHash, light);
    resources.ReflectionProbes.clear();
    resources.Fluids.clear();
    bool hasBounds = false;
    auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    for (size_t worldIndex = 0; worldIndex < scenes->GetWorldCount(); ++worldIndex)
    {
        auto* world = scenes->GetWorld(worldIndex);
        auto& ecs = world->GetECS();
        hash(staticHash, world);
        ecs.ForEach<StaticMeshRenderComponent>([&](Entity entity, StaticMeshRenderComponent& renderer)
        {
            if (!ecs.Has<TransformComponent>(entity)) return;
            uint64_t signature = 14695981039346656037ull;
            hash(signature, world);
            const auto& transform = ecs.Get<TransformComponent>(entity);
            hash(signature, entity);
            hash(signature, renderer.MeshHandler);
            hash(signature, GetPrimaryGPUMesh(renderer.MeshHandler)->ContentRevision);
            hash(signature, transform.Location);
            hash(signature, transform.Rotation);
            hash(signature, transform.Scale);
            for (const auto index : renderer.SubMeshIndices) hash(signature, index);
            for (const auto* material : renderer.Materials)
            {
                hash(signature, material);
                hash(signature, material->_revision);
            }
            hash(renderer._reflectionDynamic ? dynamicHash : staticHash, signature);
            resources.LocalShadowCasters->push_back({ renderer.Bounds, signature, renderer._reflectionDynamic });
            if (hasBounds) BoundingBox::CreateMerged(resources.ReflectionSceneBounds, resources.ReflectionSceneBounds, renderer.Bounds);
            else resources.ReflectionSceneBounds = renderer.Bounds;
            hasBounds = true;
        });
        ecs.ForEach<FluidRenderComponent>([&](Entity entity, FluidRenderComponent& component)
        {
            if (!ecs.Has<TransformComponent>(entity)) return;
            const auto& transform = ecs.Get<TransformComponent>(entity);
            GDX12FluidInstance instance;
            instance.Id = ((resources.ReflectionWorldRevision * 65599 + worldIndex) << 32) | entity;
            instance.Component = component;
            instance.ContainerToWorld = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
            instance.DeltaSeconds = world->IsPaused() ? 0 : _timer->DeltaTime() * world->GetTimeScale();
            ValidateFluidInstance(instance);
            resources.Fluids.push_back(std::move(instance));
        });
        ecs.ForEach<SkeletalMeshRenderComponent>([&](Entity entity, SkeletalMeshRenderComponent&)
        {
            const auto found = _skeletalStates.find({world, entity});
            if (found == _skeletalStates.end()) return;
            uint64_t signature = 14695981039346656037ull;
            hash(signature, world);
            hash(signature, entity);
            hash(signature, found->second.SceneRevision);
            for (const auto& matrix : found->second.Pose) hash(signature, matrix);
            if (ecs.Has<TransformComponent>(entity))
            {
                const auto& transform = ecs.Get<TransformComponent>(entity);
                hash(signature, transform.Location);
                hash(signature, transform.Rotation);
                hash(signature, transform.Scale);
            }
            for (const auto& part : found->second.Parts)
            {
                auto partSignature = signature;
                hash(partSignature, part.MeshRevision);
                hash(partSignature, part.Material);
                if (part.ActiveMaterial) hash(partSignature, part.ActiveMaterial->_revision);
                hash(dynamicHash, partSignature);
                resources.LocalShadowCasters->push_back({ part.Bounds, partSignature, true });
                if (hasBounds) BoundingBox::CreateMerged(resources.ReflectionSceneBounds, resources.ReflectionSceneBounds, part.Bounds);
                else resources.ReflectionSceneBounds = part.Bounds;
                hasBounds = true;
            }
        });
        ecs.ForEach<ReflectionProbeComponent>([&](Entity entity, ReflectionProbeComponent& component)
        {
            if (!component.Enabled || !ecs.Has<TransformComponent>(entity)) return;
            const auto& transform = ecs.Get<TransformComponent>(entity);
            GDX12ReflectionProbe probe;
            probe.Id = ((_primaryResources.ReflectionWorldRevision * 65599 + worldIndex) << 32) | entity;
            probe.Position = transform.Location + component.CaptureOffset;
            probe.BoxMin = transform.Location - component.Extents;
            probe.BoxMax = transform.Location + component.Extents;
            probe.BlendDistance = component.BlendDistance;
            probe.Intensity = component.Intensity;
            probe.NearPlane = component.NearPlane;
            probe.FarPlane = component.FarPlane;
            static_cast<GDX12ReflectionCaptureSettings&>(probe) = component;
            probe.Revision = component.Revision;
            if (!IsValidReflectionProbe(probe)) throw std::invalid_argument("Invalid ReflectionProbeComponent");
            resources.ReflectionProbes.push_back(probe);
        });
    }
    BoundingBox grassBounds;
    if (resources.Grass && resources.Grass->GetBounds(grassBounds))
    {
        resources.LocalShadowCasters->push_back({ grassBounds, resources.Grass->Revision(), true });
        if (hasBounds) BoundingBox::CreateMerged(resources.ReflectionSceneBounds, resources.ReflectionSceneBounds, grassBounds);
        else resources.ReflectionSceneBounds = grassBounds;
    }
    std::sort(resources.ReflectionProbes.begin(), resources.ReflectionProbes.end(),
        [](const auto& a, const auto& b) { return a.Id < b.Id; });
    if (_reflectionStaticHash != staticHash)
    {
        ++resources.ReflectionStaticRevision;
        _reflectionStaticHash = staticHash;
    }
    if (_reflectionDynamicHash != dynamicHash)
    {
        ++resources.ReflectionDynamicRevision;
        _reflectionDynamicHash = dynamicHash;
    }
}

void RenderModule::BuildBackBuffer()
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer = std::make_unique<GDX12SwapChain>(_primaryDevice.get(), _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, _primaryResources.RTVHeap.get());

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
    auto* environmentPass = CreateRenderPass<GDX12EnvironmentIBLPass>();
    auto* cullingPass = CreateRenderPass<GDX12GPUCullingPass>();
    auto* depthPrepass = CreateRenderPass<GDX12OpaquePass>(GDX12OpaquePass::Stage::DepthPrepass);
    auto* shadowPass = CreateRenderPass<GDX12CascadedShadowPass>();
    auto* localShadowPass = CreateRenderPass<GDX12LocalShadowPass>();
    _localShadowPass = localShadowPass;
    auto* reflectionPass = CreateRenderPass<GDX12ReflectionProbePass>();
    _reflectionPass = reflectionPass;
    auto* opaquePass = CreateRenderPass<GDX12OpaquePass>(GDX12OpaquePass::Stage::GBuffer);
    auto* aoPass = CreateRenderPass<GDX12AmbientOcclusionPass>();
    auto* lightingPass = CreateRenderPass<GDX12DeferredLightingPass>();
    _deferredLightingPass = lightingPass;
    auto* ssrPass = CreateRenderPass<GDX12SSSRPass>();
    auto* fluidSurfacePass = CreateRenderPass<GDX12FluidSurfacePass>();
    auto* transparencyPass = CreateRenderPass<GDX12WBOITTransparencyPass>(GDX12WBOITTransparencyPass::WaterPartition::Behind);
    auto* compositionPass = CreateRenderPass<GDX12WBOITCompositionPass>();
    auto* waterResolvePass = CreateRenderPass<GDX12FluidResolvePass>(fluidSurfacePass);
    auto* frontPass = CreateRenderPass<GDX12WBOITTransparencyPass>(GDX12WBOITTransparencyPass::WaterPartition::Front);
    auto* particlesPass = CreateRenderPass<GDX12ParticlesPass>();
    auto* frontCompositionPass = CreateRenderPass<GDX12WBOITCompositionPass>(nullptr, frontPass);
    auto* dofPass = CreateRenderPass<GDX12DepthOfFieldPass>();
    auto* motionBlurPass = CreateRenderPass<GDX12MotionBlurPass>();
    auto* upscalePass = CreateRenderPass<GDX12ReconstructionPass>();
    auto* outputPass = CreateRenderPass<GDX12OutputToScreenPass>();
    auto* frameGenerationPass = CreateRenderPass<GDX12FrameGenerationPass>(_backBuffer.get());
    _reconstructionPass = upscalePass;
    _depthPrepass = depthPrepass;
    _opaquePass = opaquePass;
    _ambientOcclusionPass = aoPass;
    SetupRenderPasses();

    reflectionPass->SetInputs({ environmentPass->GetOutput(0), environmentPass->GetOutput(1),
        environmentPass->GetOutput(2), environmentPass->GetOutput(3), localShadowPass->GetOutput(0) });
    cullingPass->SetInputs({});
    depthPrepass->SetInputs({});
    opaquePass->SetInputs(std::vector<IRenderPassLink*>{ depthPrepass->GetOutput(2) });
    aoPass->SetInputs({ opaquePass->GetOutput(2), opaquePass->GetOutput(3) });
    lightingPass->SetInputs({ opaquePass->GetOutput(0), opaquePass->GetOutput(3), opaquePass->GetOutput(4), opaquePass->GetOutput(5), opaquePass->GetOutput(2), shadowPass->GetOutput(0), environmentPass->GetOutput(0), environmentPass->GetOutput(1), environmentPass->GetOutput(2), environmentPass->GetOutput(3), localShadowPass->GetOutput(0), aoPass->GetOutput(0) });
    fluidSurfacePass->SetInputs({ opaquePass->GetOutput(2), opaquePass->GetOutput(1) });
    for (auto* pass : { transparencyPass, frontPass })
        pass->SetInputs({ opaquePass->GetOutput(2), shadowPass->GetOutput(0), environmentPass->GetOutput(0),
            environmentPass->GetOutput(1), environmentPass->GetOutput(2), localShadowPass->GetOutput(0),
            fluidSurfacePass->GetOutput(0) });
    ssrPass->SetInputs({ lightingPass->GetOutput(0), opaquePass->GetOutput(2), opaquePass->GetOutput(3),
        opaquePass->GetOutput(4), opaquePass->GetOutput(1), environmentPass->GetOutput(1), environmentPass->GetOutput(2) });
    compositionPass->SetInputs({ ssrPass->GetOutput(0), transparencyPass->GetOutput(0), transparencyPass->GetOutput(1), opaquePass->GetOutput(2) });
    waterResolvePass->SetInputs({ compositionPass->GetOutput(0), environmentPass->GetOutput(1) });
    frontCompositionPass->SetInputs({ waterResolvePass->GetOutput(0), frontPass->GetOutput(0), frontPass->GetOutput(1), fluidSurfacePass->GetOutput(0) });
    dofPass->SetInputs({ frontCompositionPass->GetOutput(0), fluidSurfacePass->GetOutput(0) });
    motionBlurPass->SetInputs({ dofPass->GetOutput(0), fluidSurfacePass->GetOutput(0), fluidSurfacePass->GetOutput(1) });
    upscalePass->SetInputs({ fluidSurfacePass->GetOutput(0), motionBlurPass->GetOutput(1), motionBlurPass->GetOutput(0) });
    particlesPass->SetInputs({ fluidSurfacePass->GetOutput(0), frontPass->GetOutput(0), frontPass->GetOutput(1) });
    static_cast<GDX12WBOITCompositionPass*>(frontCompositionPass)->SetParticles(particlesPass);
    outputPass->SetInputs({ upscalePass->GetOutput(0), _backBuffer.get()});
    frameGenerationPass->SetInputs({ fluidSurfacePass->GetOutput(0), motionBlurPass->GetOutput(1), _backBuffer.get() });

    depthPrepass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    opaquePass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    aoPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    lightingPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    transparencyPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    compositionPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    frontPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    frontCompositionPass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);

    InitializeRenderPasses();
    RebuildRenderGraph();
}

void RenderModule::SetupRenderPasses()
{
    for (auto& pass : _primaryRenderPasses)
    {
        pass->Setup(&_primaryResources, &_secondaryResources, &_RPcommonData);
        _primaryPipelineFlags |= pass->GetFlags();
    }

    for (auto& pass : _secondaryRenderPasses)
    {
        pass->Setup(&_secondaryResources, &_primaryResources, &_RPcommonData);
        _secondaryPipelineFlags |= pass->GetFlags();
    }
}

void RenderModule::InitializeRenderPasses()
{
    std::vector<GDX12RenderPass*> allRenderPasses;
    for (auto& pass : _primaryRenderPasses)
        allRenderPasses.push_back(pass.get());
    for (auto& pass : _secondaryRenderPasses)
        allRenderPasses.push_back(pass.get());
    GDX12RenderPass::InitializeAll(std::move(allRenderPasses));
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

const GDX12ReconstructionStatus& RenderModule::GetReconstructionStatus() const
{
    if (!_reconstructionPass) throw std::logic_error("Render pipeline has not been initialized");
    return _reconstructionPass->GetStatus();
}

void RenderModule::BeginFrame()
{
    if (_uiInitialized) Engine::UI::BeginFrame();
    if (_backBuffer) _backBuffer->BeginFrame(_RPcommonData);
}

void RenderModule::SetFrameGenerationSuspended(bool suspended)
{
    if (!_backBuffer || _backBuffer->GetFrameGeneration().GetStatus().Suspended == suspended) return;
    _backBuffer->SetFrameGenerationSuspended(suspended);
    _RPcommonData.RequestHistoryReset = true;
}

const GDX12FrameGenerationStatus& RenderModule::GetFrameGenerationStatus() const
{
    if (!_backBuffer) throw std::logic_error("Render pipeline has not been initialized");
    return _backBuffer->GetFrameGeneration().GetStatus();
}

D3D12MA::Statistics RenderModule::GetMemoryStatistics() const
{
    D3D12MA::TotalStatistics statistics = {};
    if (_primaryDevice && _primaryDevice->GetMemoryAllocator())
        _primaryDevice->GetMemoryAllocator()->CalculateStatistics(&statistics);
    return statistics.Total.Stats;
}

void RenderModule::RebuildRenderGraph()
{
    _primaryDevice->GetCommandQueue()->Flush();
    _graphEarlyAO = GetAmbientOcclusionExecutionPoint() == 0;
    _graphReuseTextures = GraphReuseTextures.GetValue();
    _RPcommonData.AOAfterDepthPrepass = _graphEarlyAO;
    _ambientOcclusionPass->SetInputs({ _depthPrepass->GetOutput(2),
        (_graphEarlyAO ? _depthPrepass : _opaquePass)->GetOutput(3) });
    auto graph = std::make_unique<GDX12RenderGraph>(_primaryDevice.get());
    graph->ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene and frame buffers");
    graph->ImportLink("Swapchain", _backBuffer.get());
    for (const auto& pass : _primaryRenderPasses)
    {
        if (_graphEarlyAO && pass.get() == _ambientOcclusionPass) continue;
        graph->AddPass(*pass);
        if (_graphEarlyAO && pass.get() == _depthPrepass) graph->AddPass(*_ambientOcclusionPass);
    }
    graph->Compile(_graphReuseTextures);
    _primaryGraph = std::move(graph);
    _RPcommonData.RequestHistoryReset = true;
}

const GDX12RenderGraph::Statistics& RenderModule::GetRenderGraphStatistics() const
{
    return _primaryGraph->GetStatistics();
}

void RenderModule::WriteRenderGraph(std::ostream& output) const
{
    _primaryGraph->WriteDot(output);
}
