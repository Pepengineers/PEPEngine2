#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "Engine.UI/UI_test.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <climits>
#include <cstring>
#include <shellapi.h>
#include <string_view>
#pragma comment(lib, "shell32.lib")
#include "App.Base/App.h"
#include "Engine.Core/AssetManager.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "App.Base/Rendering/DefaultRenderPipeline.h"
#include <limits>
#include <cmath>
#include "Common/ConsoleVariables.h"

static AutoConsoleVariable<bool> GraphReuseTextures(L"Render.Graph.ReuseTextures", true,
    L"Reuse compatible transient graph textures. Disable to compare output and memory.", EConsoleVariableFlags::Default);
static AutoConsoleVariable<int> PrimaryAdapter(L"Render.PrimaryAdapter", 0,
    L"Primary adapter in the renderer hardware list. Startup only.", EConsoleVariableFlags::ReadOnly);
RenderModule::GpuOwners::GpuOwners() = default;
RenderModule::GpuOwners::~GpuOwners() = default;

RenderModule::RenderModule(Window* window, GameTimer* timer)
    : RenderModule(window, timer, std::make_unique<DefaultRenderPipeline>()) {}

RenderModule::RenderModule(Window* window, GameTimer* timer, std::unique_ptr<IRenderPipeline> pipeline) :
    _timer(timer), _window(window),
    _primaryPipelineFlags(0)
{
    if (!pipeline) throw std::invalid_argument("Cannot use a null render pipeline");
    _pipeline = std::move(pipeline);
    _RPcommonData.GameTimer = _timer;
}

RenderPipelineContext RenderModule::PipelineContext()
{
    return { *this, _primaryResources, _RPcommonData, _backBuffer.get() };
}

JobHandle RenderPipelineContext::PrepareScene()
{
    Runtime.CollectReflectionScene();
    return Runtime._shadowVisibility.Schedule(*Common.Jobs, Primary.LocalLights,
        *Primary.LocalShadowCasters, Primary.LocalShadowVisibility,
        Primary.StaticMeshShadowCasters, Primary.StaticMeshShadowRevision);
}

void RenderPipelineContext::Resize() { Runtime.OnResize(); }

void RenderModule::RefreshPipelineFlags()
{
    _primaryPipelineFlags = 0;
    for (auto* pass : _pipeline->GetBindings().PrimaryPasses) _primaryPipelineFlags |= pass->GetFlags();
}

RenderModule::~RenderModule()
{
    try
    {
        if (_primaryDevice && _primaryDevice->GetCommandQueue()) DrainFrameWork();
    }
    catch (...)
    {
        QuarantineGpuOwners();
        return;
    }
    if (_uiInitialized) Engine::UI::ShutdownUI();

    _primaryGraph.reset();
    _primaryResources.GeometrySources.clear();
    _pipeline.reset();
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

    int argumentCount = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) throw std::runtime_error("Cannot read renderer startup arguments");
    bool debugLayerRequested = false;
    for (int index = 1; index < argumentCount; ++index)
        if (std::wstring_view(arguments[index]) == L"-d3ddebug") debugLayerRequested = true;
    LocalFree(arguments);
    if (debugLayerRequested)
    {
        ComPtr<ID3D12Debug> debugController;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
        debugController->EnableDebugLayer();
        Log::Info("Renderer", "D3D12 debug layer enabled by -d3ddebug");
    }

    _primaryDevice = std::make_unique<GDX12Device>();
    const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
    const int primaryAdapter = PrimaryAdapter.GetValue();
    if (primaryAdapter < 0 || static_cast<size_t>(primaryAdapter) >= adapters.size())
        throw std::invalid_argument("Primary adapter index out of range");
    ThrowIfFailed(_primaryDevice->Initialize(adapters[primaryAdapter].Adapter));
    _primaryResources.Initialize(_primaryDevice.get());
    _primaryGPUTimer = std::make_unique<GDX12GPUTimer>(_primaryDevice.get());

    Log::Info("Renderer", "Creating back buffers");
    BuildBackBuffer();
    Log::Info("Renderer", "Configuring render passes and pipeline states");
    auto context = PipelineContext();
    _pipeline->Initialize(context);
    if (!_pipeline->GetBindings().FrameGenerationInputs) _backBuffer->SetFrameGenerationSuspended(true);
    RefreshPipelineFlags();
    RebuildRenderGraph();

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

void RenderModule::QuarantineGpuOwners() noexcept
{
    // shortcut: retain the complete device domain until process exit when no safe drain is known.
    _gpu.release();
    OutputDebugStringA("Renderer GPU retirement failed; retaining device and frame owners until process exit.\n");
    try
    {
        try { throw; }
        catch (const DxException& error) { Log::Error("Renderer", "GPU retirement failed. HRESULT=", error.ErrorCode); }
        catch (const std::exception& error) { Log::Error("Renderer", "GPU retirement failed. ", error.what()); }
        catch (...) { Log::Error("Renderer", "GPU retirement failed with an unknown error."); }
    }
    catch (...) {}
}

void RenderModule::OnResize()
{
    _RPcommonData.RequestHistoryReset = true;
    _primaryDevice->GetCommandQueue()->Flush();

    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer->Resize(width, height);

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
    const bool rebuild = static_cast<bool>(_primaryGraph) || _pipeline->OwnsExecution();
    _primaryGraph.reset();
    auto context = PipelineContext();
    _pipeline->Resize(context);
    RefreshPipelineFlags();
    if (rebuild) RebuildRenderGraph();
}

bool RenderModule::OwnsMaterial(const GDX12Material* material) const
{
    return std::any_of(_materials.begin(), _materials.end(),
        [material](const auto& entry) { return entry.second.get() == material; });
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
    return _textures[name].get();
}

void RenderModule::WaitForUploads()
{
    if (_primaryTextureCommands) _primaryDevice->GetCommandQueue()->ExecuteCommandList(_primaryTextureCommands);
    _primaryDevice->GetCommandQueue()->Flush();
    _primaryTextureCommands = nullptr;
    _textureUploadResources.clear();
    _textureUploadBytes = 0;
}

void RenderModule::DrainFrameWork()
{
    WaitForUploads();
    if (!_pipeline->OwnsExecution())
        if (auto* compute = _primaryDevice->TryGetComputeQueue()) compute->Flush();
}

void RenderModule::ResetWorldHistory()
{
    _RPcommonData.RequestHistoryReset = _RPcommonData.ResetUpscalerHistory = true;
    _pipeline->ResetWorldHistory();
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
    auto*& pendingCommands = _primaryTextureCommands;
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
        ApplyShadowQuality(resources.ShadowQuality, resources.Lighting, resources.LocalLights);
        auto& cache = resources.FrameConstants[resources.CurrFrameConstantsIndex]->LocalLightCache;
        cache->Resize((std::max)(static_cast<UINT>(lights.size()), 1u));
        if (!resources.LocalLights.empty())
            cache->CopyData(0, resources.LocalLights.data(), static_cast<UINT>(resources.LocalLights.size()));
        resources.UpdateMainCB(width, height, _timer);
    };
    upload(_primaryResources);
}

void RenderModule::SubmitCameraShadows(const GDX12ShadowConstants& shadows)
{
    _primaryResources.ShadowSettings = shadows;
    GetCurrentPrimaryFrameConstants()->ShadowCB->CopyData(0, shadows);
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
    const bool lastWorld = _worldSubscriptions.size() == 1;
    if (lastWorld) DrainFrameWork();
    struct BatchRelease
    {
        bool& Active;
        ~BatchRelease() { Active = false; }
    } release{_batchWorldRelease};
    _batchWorldRelease = lastWorld;

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
    ecs.ForEach<TransformComponent>([&](Entity entity, TransformComponent&)
    {
        _transformGPUData.erase(entity);
    });
    if (lastWorld)
    {
        _transformCount = _instanceCount = _cameraCount = 0;
        _freeSkeletalInstances.clear();
        _meshSnapshots.clear();
        _staticRendererSnapshots.clear();
        _staticSnapshotInstances.clear();
        _nonStaticMeshPrimitives.clear();
        _staticMeshSnapshotCount = SIZE_MAX;
        _primaryResources.StaticMeshShadowCasters.clear();
        ++_primaryResources.StaticMeshShadowRevision;
        _primaryResources.ReflectionProbes.clear();
        _RPcommonData.ActiveCameraCBufferIndex = UINT_MAX;
        _RPcommonData.RequestHistoryReset = true;
    }
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

const GPUMesh* RenderModule::GetPrimaryGPUMesh(MeshHandle handle)
{
    return _primaryResources.GeometryBuffer->GetGPUMeshByHandle(handle);
}

GDX12UploadBuffer<GDX12IndirectDrawArgs>* RenderModule::GetPrimaryIndirectCommandsCache()
{
    return _primaryResources.IndirectCommandsCache.get();
}

uint32_t RenderModule::GetPrimaryPipelineFlags()
{
    return _primaryPipelineFlags;
}

RenderPipelineCommonData* RenderModule::GetRenderPipelineCommonData()
{
    return &_RPcommonData;
}

bool RenderModule::PrimaryPipelineHasFlag(uint32_t flag)
{
    return _primaryPipelineFlags & flag;
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
    if (!PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS)) return;
    component._CBufferIndex = _cameraCount++;
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_CAMERAS))
    {
        for (auto& constants : _primaryResources.FrameConstants)
        {
            if (component._CBufferIndex < constants->CameraVisibilityCommands.size()) continue;
            constants->CameraCB->Resize(component._CBufferIndex + 1);

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
}

void RenderModule::OnRenderComponentCreated(World&, Entity, StaticMeshRenderComponent& component)
{
    if (!PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_INSTANCES)) return;
    const GPUMesh* mesh = GetPrimaryGPUMesh(component.MeshHandler);
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
    if (!_batchWorldRelease)
    {
        if (_primaryDevice) _primaryDevice->GetCommandQueue()->Flush();
    }
    const GDX12IndirectDrawArgs empty = {};
    for (const auto index : component._CBufferIndices)
    {
        _primaryResources.RemoveMeshPrimitive(index);
        if (_primaryResources.IndirectCommandsCache) _primaryResources.IndirectCommandsCache->CopyData(index, empty);
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
    if (!_gpu) throw std::logic_error("Renderer device domain is quarantined");
    const auto shadowQuality = ReadShadowQuality();
    if (!(shadowQuality == _primaryResources.ShadowQuality))
    {
        DrainFrameWork();
        _primaryResources.ShadowQuality = shadowQuality;
        _RPcommonData.RequestHistoryReset = true;
    }
    GDX12_SCOPED_CPU_EVENT("Frame slot wait and upload");
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    auto context = PipelineContext();
    if (width && height) _pipeline->UpdateFrameSettings(context);
    RefreshPipelineFlags();
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
}

void RenderModule::OnRender()
{
    if (!_gpu) throw std::logic_error("Renderer device domain is quarantined");
    _RPcommonData.Jobs = &App::GetInstance()->GetJobs();
    _backBuffer->GetFrameGeneration().BeginRendering(_RPcommonData);
    _primaryResources.BeginMeshPrimitiveUpdate();
    auto context = PipelineContext();
    _pipeline->PrepareMaterialBindings(context);
    const auto skeletalStarted = std::chrono::steady_clock::now();
    PrepareSkeletalMeshes();
    _skeletalPreparationTimeMS = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - skeletalStarted).count();
    if (PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_MATERIALS))
        _primaryResources.UpdateMaterialCB(_materials);
    CollectParticleEmitters();
    auto primarycmdQueue = _primaryDevice->GetCommandQueue();
    auto primarycmdList = primarycmdQueue->GetCommandList();
    _primaryGPUTimer->Start(primarycmdList);
    // Explicit owners freeze skin work and declare its resources in their plan.
    // Legacy callers retain their existing pre-graph dispatch path.
    if (!_pipeline->OwnsScenePreparation()) DispatchSkeletalMeshes(primarycmdList);
    const bool rebuild = _pipeline->PrepareFrame(context, primarycmdList);
    RefreshPipelineFlags();
    if (rebuild || _graphReuseTextures != GraphReuseTextures.GetValue()) RebuildRenderGraph();

    auto primaryCurrentFrameConsts = GetCurrentPrimaryFrameConstants();
    const auto& bindings = _pipeline->GetBindings();
    auto* compute = bindings.UseAsyncCompute ? _primaryDevice->TryGetComputeQueue() : nullptr;
    uint64_t completion;
    const auto finish = [&](GDX12CommandList* list) {
        if (_uiInitialized && !_pipeline->OwnsScreenUI()) Engine::UI::Render(list, _backBuffer->GetCurrentBuffer());
        _primaryGPUTimer->Stop(list);
    };
    if (_pipeline->OwnsExecution())
        completion = _pipeline->ExecuteFrame(context, primarycmdList, finish);
    else
    {
        primarycmdList = _primaryGraph->Execute(primarycmdList, compute);
        finish(primarycmdList);
        primarycmdQueue->ExecuteCommandList(primarycmdList);
        completion = primarycmdList->FenceValue;
    }
    ++_frameSubmission;
    primaryCurrentFrameConsts->FenceValue = completion;
    _primaryGPUTimer->OnCommandListExecuted(completion);

    _backBuffer->GetFrameGeneration().EndRendering();
    _backBuffer->Present();
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

void RenderModule::BuildBackBuffer()
{
    uint16_t width, height;
    _window->GetWindowSize(width, height);

    _backBuffer = std::make_unique<GDX12SwapChain>(_primaryDevice.get(), _window->GetWindowHandle(),
        DXGI_FORMAT_R8G8B8A8_UNORM, 2, width, height, _primaryResources.RTVHeap.get());

    _RPcommonData.WindowWidth = width;
    _RPcommonData.WindowHeight = height;
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
    auto* reconstruction = _pipeline->GetBindings().Reconstruction;
    if (!reconstruction) throw std::logic_error("Render pipeline has no reconstruction status");
    return reconstruction->GetStatus();
}

void RenderModule::BeginFrame()
{
    if (_uiInitialized)
    {
        Engine::UI::BeginFrame();
#ifdef _DEBUG
        _pipeline->DrawDebugControls(*this);
#endif
    }
    if (_backBuffer)
    {
        if (_pipeline->GetBindings().FrameGenerationInputs)
            _backBuffer->SetFrameGenerationSuspended(_renderingSuspended || !_pipeline->AllowsFrameGeneration());
        _backBuffer->BeginFrame(_RPcommonData);
    }
}

void RenderModule::SetFrameGenerationSuspended(bool suspended)
{
    if (_renderingSuspended == suspended) return;
    _renderingSuspended = suspended;
    _RPcommonData.RequestHistoryReset = true;
    if (_backBuffer && _pipeline->GetBindings().FrameGenerationInputs)
        _backBuffer->SetFrameGenerationSuspended(suspended || !_pipeline->AllowsFrameGeneration());
}

const GDX12FrameGenerationStatus& RenderModule::GetFrameGenerationStatus() const
{
    if (!_backBuffer) throw std::logic_error("Render pipeline has not been initialized");
    if (!_pipeline->GetBindings().FrameGenerationInputs)
        throw std::logic_error("Render pipeline has no frame generation inputs");
    return _backBuffer->GetFrameGeneration().GetStatus();
}

double RenderModule::GetPrimaryGPUTimeMS()
{
    return _primaryGPUTimer ? _primaryGPUTimer->GetTimeMS() : -1.0;
}

GDX12GPUTimingTotals RenderModule::GetPrimaryGPUTimingTotals()
{
    return _primaryGPUTimer ? _primaryGPUTimer->GetSampleTotals() : GDX12GPUTimingTotals{};
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
    _graphReuseTextures = GraphReuseTextures.GetValue();
    if (_pipeline->OwnsExecution())
    {
        auto context = PipelineContext();
        _pipeline->RebuildExecution(context);
        _RPcommonData.RequestHistoryReset = true;
        return;
    }
    auto graph = std::make_unique<GDX12RenderGraph>(_primaryDevice.get());
    graph->ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene and frame buffers");
    graph->ImportLink("Swapchain", _backBuffer.get());
    _pipeline->BuildGraph(*graph, { _backBuffer.get() });
    graph->Compile(_graphReuseTextures);
    _primaryGraph = std::move(graph);
    _RPcommonData.RequestHistoryReset = true;
}

const GDX12RenderGraph::Statistics& RenderModule::GetRenderGraphStatistics() const
{
    static const GDX12RenderGraph::Statistics empty{};
    return _primaryGraph ? _primaryGraph->GetStatistics() : empty;
}

double RenderModule::GetGPUFrameTimeMS() const
{
    return _primaryGPUTimer->GetTimeMS();
}

void RenderModule::WriteRenderGraph(std::ostream& output) const
{
    if (_pipeline->OwnsExecution()) _pipeline->WriteExecution(output);
    else _primaryGraph->WriteDot(output);
}
