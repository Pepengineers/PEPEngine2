#pragma once

#include "Common/GameTimer.h"
#include "Common/Module.h"

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/ECS/Event.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "App.Base/Rendering/CrossAdapterSceneReplica.h"
#include "App.Base/Systems/ShadowVisibilitySystem.h"
#include <map>

class SceneManagerModule;
class World;
class GDX12SwapChain;
class GDX12Device;
class GDX12CommandQueue;
class GDX12CommandList;
class GDX12GPUTimer;
struct GDX12GPUTimingTotals;
struct GDX12FrameGenerationStatus;
struct CapturedShadowScene;

struct TransformComponent;
struct CameraComponent;
struct StaticMeshRenderComponent;
class SkeletalMeshRenderComponent;
namespace Engine::Core { class SceneAsset; }

class Window;

using namespace Engine::Core;

struct TransformCompGPUData
{
    UINT CBufferIndex = -1;
    UINT NumFramesDirty = -1;
    Matrix World = Identity4x4();
    Matrix PrevWorld = Identity4x4();
};

namespace Engine::UI { class CrossAdapterUI; struct UIPlacementStatus; }

struct RenderMGPUStatus
{
    bool Requested = false;
    bool SecondaryReady = false;
    std::uint64_t RequestedSecondaryLuid = 0;
    std::uint64_t PrimaryLuid = 0;
    std::uint64_t ActiveSecondaryLuid = 0;
    std::string FallbackReason;
};

class RenderModule final : public Module
{
public:
    RenderModule(Window* window, GameTimer* timer);
    RenderModule(Window* window, GameTimer* timer, std::unique_ptr<IRenderPipeline> pipeline);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize();
    void BeginFrame();
    void SetFrameGenerationSuspended(bool);
    const GDX12ReconstructionStatus& GetReconstructionStatus() const;
    const GDX12FrameGenerationStatus& GetFrameGenerationStatus() const;
    const RenderMGPUStatus& GetMGPUStatus() const { return _mgpuStatus; }
    RenderMGPURuntime* GetMGPURuntime() const { return _gpu ? _mgpuRuntime.get() : nullptr; }
    GDX12Submission::FrameSubmissionId GetFrameSubmissionId() const { return GDX12Submission::FrameSubmissionId{_frameSubmission}; }
    bool ShutdownMGPU() noexcept;
    GDX12Submission::CompletionPoint GetLastLegacyReplacement(const GDX12RenderPass& pass) const;
    GDX12Submission::CompletionPoint GetLastScreenUIFinalize() const { return _screenUIFinalize; }
    Engine::UI::UIPlacementStatus GetUIPlacementStatus(Engine::UI::CanvasId surface) const;
    // Borrowed submission bindings for the renderer's serialized owner.
    RenderMGPUDevices GetMGPUDevices() const
    { return _gpu ? RenderMGPUDevices{ _primaryDevice.get(), _secondaryDevice.get(),
        _primaryTransferQueue.get(), _secondaryTransferQueue.get() } : RenderMGPUDevices{}; }

    bool OwnsMaterial(const GDX12Material* material) const;
    GDX12Material* GetMaterialByName(const std::string& name);

    //returns a pointer to a fully initialized structure that you can specify in components
    GDX12Material* CreateMaterial(const std::string& name);

    GPUTexture* GetTextureByName(const std::string& name);
    //returns a pointer to a fully initialized structure that you can specify in materials
    GPUTexture* CreateTexture(const std::string& name, const Texture* texture, ETextureType type);
    void RetainShadowAlpha(const GPUTexture* texture, const Texture& source);
    CapturedShadowScene CaptureShadowScene(CrossAdapterSceneReplica::FrameStamp stamp,
        const std::vector<GDX12ShadowOutput>& groups,
        const std::vector<CrossAdapterSceneReplica::GrassBatch>& grass, const IRenderGeometrySource* grassSource = nullptr,
        const std::vector<GDX12RenderPrimitiveHandle>& grassPrimitives = {});
    void WaitForUploads();
    D3D12MA::Statistics GetMemoryStatistics() const;
    double GetPrimaryGPUTimeMS();
    GDX12GPUTimingTotals GetPrimaryGPUTimingTotals();
    double GetSkeletalPreparationTimeMS() const { return _skeletalPreparationTimeMS; }
    const GDX12RenderGraph::Statistics& GetRenderGraphStatistics() const;
    double GetGPUFrameTimeMS() const;
    void WriteRenderGraph(std::ostream& output) const;
    void BeginTextureUploadBatch(std::uint64_t budgetBytes);
    void EndTextureUploadBatch();

    //Uploads Mesh geometry to GPU
    void SubmitMesh(const Mesh* mesh, MeshHandle handle);
    void ReserveInstanceCapacity(UINT additionalInstances);
    void ReserveTransformCapacity(UINT additionalTransforms);

    void SetActiveCamera(CameraComponent* camera);
    void SubmitSky(World& world);
    void SubmitLights(std::vector<GDX12LocalLightConstants> lights, const GDX12MainConstants& settings);
    void SubmitCameraShadows(const GDX12ShadowConstants& shadows);
    const GDX12MainConstants& GetLightingSettings() const { return _primaryResources.Lighting; }
    
    struct WorldRenderSubscriptions
    {
        ListenerHandle TransformCreated = 0;
        ListenerHandle TransformDestroyed = 0;
        ListenerHandle TransformUpdated = 0;

        ListenerHandle CameraCreated = 0;
        ListenerHandle CameraDestroyed = 0;
        ListenerHandle CameraUpdated = 0;

        ListenerHandle RenderCompCreated = 0;
        ListenerHandle RenderCompDestroyed = 0;
        ListenerHandle RenderCompUpdated = 0;
        ListenerHandle SkeletalDestroyed = 0;
        ListenerHandle SkeletalUpdated = 0;
    };

    TransformCompGPUData& GetTransformGPUData(Entity entity);

    // this function adds listeners to new world
    void SubscribeToWorld(World& world);
    // unsubscribing if world is removed
    void UnsubscribeFromWorld(World& world);
    
    // TransformComponent events
    void OnTransformComponentCreated(World& world, Entity entity, TransformComponent& component);
    void OnTransformComponentDestroyed(World& world, Entity entity, TransformComponent& component);
    void OnTransformComponentUpdated(World& world, Entity entity, TransformComponent& component);

    // CameraComponent events
    void OnCameraComponentCreated(World& world, Entity entity, CameraComponent& component);
    void OnCameraComponentDestroyed(World& world, Entity entity, CameraComponent& component);
    void OnCameraComponentUpdated(World& world, Entity entity, CameraComponent& component);

    // RenderComponent events
    void OnRenderComponentCreated(World& world, Entity entity, StaticMeshRenderComponent& component);
    void OnRenderComponentDestroyed(World& world, Entity entity, StaticMeshRenderComponent& component);
    void OnRenderComponentUpdated(World& world, Entity entity, StaticMeshRenderComponent& component);

    const float GetAspectRatio();
    GDX12FrameConstants* GetCurrentPrimaryFrameConstants();
    GDX12FrameConstants* GetCurrentSecondaryFrameConstants();
    const GPUMesh* GetPrimaryGPUMesh(MeshHandle handle);
    const GPUMesh* GetSecondaryGPUMesh(MeshHandle handle);
    GDX12UploadBuffer<GDX12IndirectDrawArgs>* GetPrimaryIndirectCommandsCache();
    GDX12UploadBuffer<GDX12IndirectDrawArgs>* GetSecondaryIndirectCommandsCache();

    uint32_t GetPrimaryPipelineFlags();
    uint32_t GetSecondaryPipelineFlags();
    RenderPipelineCommonData* GetRenderPipelineCommonData();
    bool PrimaryPipelineHasFlag(uint32_t flag);
    bool SecondaryPipelineHasFlag(uint32_t flag);

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    friend class BistroRenderGraphTests;

    struct SkeletalPart
    {
        MeshHandle Mesh;
        std::uint64_t MeshRevision = 0;
        UINT Node = 0;
        UINT SubMesh = 0;
        UINT Instance = 0;
        UINT Material = 0;
        UINT SourceMaterial = 0;
        UINT Batch = UINT_MAX;
        UINT BatchStart = 0;
        GDX12Material* ImportedMaterial = nullptr;
        GDX12Material* ActiveMaterial = nullptr;
        std::vector<UINT> BoneNodes;
        std::vector<Matrix> InverseBinds;
        DirectX::BoundingBox SourceBounds;
        DirectX::BoundingBox Bounds;
        std::unique_ptr<GDX12SkinMesh> GPU;
        std::vector<GDX12SkinPalette> Palette;
    };
    struct SkeletalState
    {
        SceneHandle Scene;
        std::uint64_t SceneRevision = 0;
        std::uint64_t ComponentRevision = 0;
        std::uint64_t DeformationRevision = 0;
        std::vector<SkeletalPart> Parts;
        std::vector<Matrix> Pose;
        double SinceUpdate = 0;
        bool NeedsSettle = false;
        bool Dispatch = false;
        bool Seen = false;
    };
    void PrepareSkeletalMeshes();
    void UpdateSkeletalPose(World& world, Entity entity, SkeletalMeshRenderComponent& component,
        SkeletalState& state, const SceneAsset& scene, bool animate, double presetRate);
    void DispatchSkeletalMeshes(GDX12CommandList* commands);
    void ReleaseSkeletalState(World& world, Entity entity);
    std::vector<UINT> _freeSkeletalInstances;
    bool _skeletalBatchDirty = true;
    bool _skeletalInstancing = false;

    void BuildBackBuffer();
    void CollectReflectionScene();
    struct MeshSnapshot
    {
        MeshHandle Handle;
        const GPUMesh* Mesh = nullptr;
        uint64_t GeometryRevision = 0, MaterialRevision = 0, Signature = 0;
        const GDX12Material* Material = nullptr;
        UINT SubMesh = UINT_MAX;
        GDX12InstanceData Instance = {};
        XMFLOAT4X4 World = {};
        GDX12RenderPrimitive Primitive;
        GDX12RenderPrimitiveHandle PrimitiveHandle;
        bool Valid = false;
    };
    std::vector<MeshSnapshot> _meshSnapshots;
    size_t _staticMeshSnapshotCount = SIZE_MAX;
    uint64_t _staticMeshSnapshotHash = 14695981039346656037ull;
    GDX12RenderBounds _staticMeshReflectionBounds, _staticMeshCasterBounds;
    std::vector<GDX12LocalShadowCaster> _staticMeshCasters;
    ShadowVisibilitySystem _shadowVisibility;
    void CollectParticleEmitters();
    void InitializeMGPU();
    void QuarantineGpuOwners() noexcept;
    void RetireScreenUIForSwapchainChange();
    friend struct RenderPipelineContext;
    RenderPipelineContext PipelineContext();
    void RefreshPipelineFlags();
    void RebuildRenderGraph();
    void EnsureInstanceCapacity(UINT capacity);
    
    void SubscribeToSceneManager();
    void UnsubscribeFromSceneManager();
    void UnsubscribeFromAllWorlds();

    GDX12Texture* CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture, ETextureType type);

    std::uint64_t _textureUploadBudget = 0;
    std::uint64_t _textureUploadBytes = 0;
    GDX12CommandList* _primaryTextureCommands = nullptr;
    GDX12CommandList* _secondaryTextureCommands = nullptr;

    GameTimer* _timer;
    Window* _window;
    
    ListenerHandle _worldCreatedListener = 0;
    ListenerHandle _worldDestroyedListener = 0;
    
    std::unordered_map<Entity, TransformCompGPUData> _transformGPUData;
    std::unordered_map<World*, WorldRenderSubscriptions> _worldSubscriptions;
    struct GpuOwners
    {
        GpuOwners();
        ~GpuOwners();
        RenderPipelineCommonData Common;
        std::unique_ptr<GDX12Device> PrimaryDevice, SecondaryDevice;
        std::unique_ptr<GDX12CommandQueue> PrimaryCopy, SecondaryCopy;
        GDX12DeviceResources Primary, Secondary;
        std::unique_ptr<GDX12GPUTimer> PrimaryTimer;
        std::unordered_map<std::string, std::unique_ptr<GPUTexture>> Textures;
        std::unordered_map<const GPUTexture*, CrossAdapterSceneReplica::AlphaTexture> ShadowAlpha;
        std::unordered_map<std::string, std::unique_ptr<GDX12Material>> Materials;
        std::vector<GDX12ResourceHandle> Uploads;
        std::map<std::pair<World*, Entity>, SkeletalState> SkeletalStates;
        std::unique_ptr<GDX12Skinning> Skinning;
        std::unique_ptr<GDX12SwapChain> BackBuffer;
        std::unique_ptr<IRenderPipeline> Pipeline;
        std::unique_ptr<Engine::UI::CrossAdapterUI> CrossAdapterUI;
        std::unique_ptr<GDX12RenderGraph> PrimaryGraph;
        std::unique_ptr<RenderMGPURuntime> MGPU;
    };
    std::unique_ptr<GpuOwners> _gpu = std::make_unique<GpuOwners>();
    decltype(GpuOwners::Common)& _RPcommonData = _gpu->Common;
    decltype(GpuOwners::PrimaryDevice)& _primaryDevice = _gpu->PrimaryDevice;
    decltype(GpuOwners::SecondaryDevice)& _secondaryDevice = _gpu->SecondaryDevice;
    decltype(GpuOwners::PrimaryCopy)& _primaryTransferQueue = _gpu->PrimaryCopy;
    decltype(GpuOwners::SecondaryCopy)& _secondaryTransferQueue = _gpu->SecondaryCopy;
    decltype(GpuOwners::MGPU)& _mgpuRuntime = _gpu->MGPU;
    decltype(GpuOwners::PrimaryTimer)& _primaryGPUTimer = _gpu->PrimaryTimer;
    decltype(GpuOwners::Primary)& _primaryResources = _gpu->Primary;
    decltype(GpuOwners::Secondary)& _secondaryResources = _gpu->Secondary;
    decltype(GpuOwners::Textures)& _textures = _gpu->Textures;
    decltype(GpuOwners::Materials)& _materials = _gpu->Materials;
    decltype(GpuOwners::Uploads)& _textureUploadResources = _gpu->Uploads;
    decltype(GpuOwners::SkeletalStates)& _skeletalStates = _gpu->SkeletalStates;
    decltype(GpuOwners::Skinning)& _skinning = _gpu->Skinning;
    decltype(GpuOwners::BackBuffer)& _backBuffer = _gpu->BackBuffer;
    decltype(GpuOwners::Pipeline)& _pipeline = _gpu->Pipeline;
    decltype(GpuOwners::PrimaryGraph)& _primaryGraph = _gpu->PrimaryGraph;
    decltype(GpuOwners::CrossAdapterUI)& _crossAdapterUI = _gpu->CrossAdapterUI;
    std::uint64_t _frameSubmission = 0;
    std::vector<std::pair<const GDX12RenderPass*, GDX12Submission::CompletionPoint>> _legacyReplacementReceipts;
    GDX12Submission::CompletionPoint _screenUIFinalize;
    bool _screenCaptureFallbackLogged = false;
    RenderMGPUStatus _mgpuStatus;
    bool _uiInitialized = false;
    bool _renderingSuspended = false;
    double _skeletalPreparationTimeMS = 0;
    bool _graphReuseTextures = true;

    // Flags that are accumulated from every active RenderPass on the device
    // Defines which resources should be uploaded onto respective GPU
    uint32_t _primaryPipelineFlags;
    uint32_t _secondaryPipelineFlags;
    World* _skyWorld = nullptr;
    uint64_t _skyHash = 0;
    uint64_t _reflectionStaticHash = 0;
    uint64_t _reflectionDynamicHash = 0;
    UINT _instanceCount = 0;
    UINT _transformCount = 0;
};
