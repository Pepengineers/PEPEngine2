п»ї#pragma once

#include "Common/GameTimer.h"
#include "Common/Module.h"

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/ECS/Event.h"

class SceneManagerModule;
class World;
class GDX12SwapChain;
class GDX12Device;
class GDX12CommandList;
class GDX12GPUTimer;

struct TransformComponent;
struct CameraComponent;
struct StaticMeshRenderComponent;

class Window;

using namespace Engine::Core;

struct TransformCompGPUData
{
    UINT CBufferIndex = -1;
    UINT NumFramesDirty = -1;
    Matrix World = Identity4x4();
    Matrix PrevWorld = Identity4x4();
};

class RenderModule final : public Module
{
public:
    RenderModule(Window* window, GameTimer* timer);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize();

    GDX12Material* GetMaterialByName(const std::string& name);

    //returns a pointer to a fully initialized structure that you can specify in components
    GDX12Material* CreateMaterial(const std::string& name);

    GPUTexture* GetTextureByName(const std::string& name);
    //returns a pointer to a fully initialized structure that you can specify in materials
    GPUTexture* CreateTexture(const std::string& name, const Texture* texture, ETextureType type);
    void WaitForUploads();
    void BeginTextureUploadBatch(std::uint64_t budgetBytes);
    void EndTextureUploadBatch();

    //Uploads Mesh geometry to GPU
    void SubmitMesh(const Mesh* mesh, MeshHandle handle);
    void ReserveInstanceCapacity(UINT additionalInstances);
    void ReserveTransformCapacity(UINT additionalTransforms);

    void SetActiveCamera(CameraComponent* camera);
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
        ListenerHandle GrassCreated = 0;
        ListenerHandle GrassUpdated = 0;
        ListenerHandle WindCreated = 0;
        ListenerHandle WindUpdated = 0;
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
    void BuildBackBuffer();
    void CollectReflectionScene();
    void CollectGrassScene(GDX12CommandList* commands);
    void ShareFences();
    void ConfigureRenderPipeline();
    void SetupRenderPasses();
    void InitializeRenderPasses();
    void EnsureInstanceCapacity(UINT capacity);
    
    void SubscribeToSceneManager();
    void UnsubscribeFromSceneManager();
    void UnsubscribeFromAllWorlds();

    GDX12Texture* CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture, ETextureType type);

    std::uint64_t _textureUploadBudget = 0;
    std::uint64_t _textureUploadBytes = 0;
    GDX12CommandList* _primaryTextureCommands = nullptr;
    GDX12CommandList* _secondaryTextureCommands = nullptr;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> _textureUploadResources;

    GameTimer* _timer;
    Window* _window;
    
    ListenerHandle _worldCreatedListener = 0;
    ListenerHandle _worldDestroyedListener = 0;
    
    std::unordered_map<Entity, TransformCompGPUData> _transformGPUData;
    std::unordered_map<World*, WorldRenderSubscriptions> _worldSubscriptions;
    RenderPipelineCommonData _RPcommonData;

    std::unique_ptr<GDX12Device> _primaryDevice;
    std::unique_ptr<GDX12Device> _secondaryDevice;
    bool _dualGPUMode;
    std::unique_ptr<GDX12GPUTimer> _primaryGPUTimer;
    std::unique_ptr<GDX12GPUTimer> _secondaryGPUTimer;

    GDX12DeviceResources _primaryResources;
    GDX12DeviceResources _secondaryResources;
    std::unordered_map<std::string, std::unique_ptr<GPUTexture>> _textures;
    std::unordered_map<std::string, std::unique_ptr<GDX12Material>> _materials;

    // These two resources are made on _primaryDevice only
    std::unique_ptr<GDX12SwapChain> _backBuffer;
    std::unique_ptr<GDX12Texture> _depthStencil;
    
    GDX12RenderPass* _depthPrepass = nullptr;
    GDX12RenderPass* _opaquePass = nullptr;
    GDX12RenderPass* _ambientOcclusionPass = nullptr;

    // Sorted in execution order
    std::vector<std::unique_ptr<GDX12RenderPass>> _primaryRenderPassExecutionList;
    std::vector<std::unique_ptr<GDX12RenderPass>> _secondaryRenderPassExecutionList;

    // Flags that are accumulated from every active RenderPass on the device
    // Defines which resources should be uploaded onto respective GPU
    uint32_t _primaryPipelineFlags;
    uint32_t _secondaryPipelineFlags;
    uint64_t _grassWindOwner = 0;
    uint64_t _reflectionStaticHash = 0;
    uint64_t _reflectionDynamicHash = 0;
    UINT _instanceCount = 0;
    UINT _transformCount = 0;
};
