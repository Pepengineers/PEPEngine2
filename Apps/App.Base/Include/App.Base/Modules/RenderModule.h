#pragma once

#include "App.Base/RenderingMetrics.h"
#include "Common/GameTimer.h"
#include "Common/Module.h"

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"
#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/ECS/Event.h"

#include <vector>

class SceneManagerModule;
class World;
class GDX12SwapChain;
class GDX12Device;

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

struct FrameGraphInputs
{
    OutputPinRef BackBuffer;
    OutputPinRef DepthStencil;
};

class RenderModule final : public Module
{
public:
    using PipelineConfiguration = void (*)(FrameGraph&, const FrameGraphInputs&);

    RenderModule(Window* window, GameTimer* timer);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize();
    void DumpFrameGraphDebugReport() const;
    void ResetMetricsInterval() { _metrics.ResetInterval(); }

    void SetGraphicsPipeline(PipelineConfiguration configure);
    void StartBenchmarkRun(const char* name, std::chrono::seconds duration);
    Event<>& OnBenchmarkRunCompleted() { return _metrics.OnRunCompleted; }

    using MetricsSample = RenderingMetrics::Sample;
    // Read on the application thread. Sampling may invalidate references to individual entries.
    const std::vector<MetricsSample>& GetMetricsHistory() const { return _metrics.GetHistory(); }

    GDX12Material* GetMaterialByName(const std::string& name);

    //returns a pointer to a fully initialized structure that you can specify in components
    GDX12Material* CreateMaterial(const std::string& name);

    GPUTexture* GetTextureByName(const std::string& name);
    //returns a pointer to a fully initialized structure that you can specify in materials
    GPUTexture* CreateTexture(const std::string& name, const Texture* texture);

    //Uploads Mesh geometry to GPU
    void SubmitMesh(const Mesh* mesh, MeshHandle handle);

    void SetActiveCamera(CameraComponent* camera);
    
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
    GDX12FrameConstants* GetCurrentFrameConstants(EDeviceRole role);
    const GPUMesh* GetGPUMesh(EDeviceRole role, MeshHandle handle);
    GDX12UploadBuffer<GDX12IndirectDrawArgs>* GetIndirectCommandsCache(EDeviceRole role);
    uint32_t GetDevicePassFlags(EDeviceRole role) const;
    bool PipelineHasFlag(EDeviceRole role, uint32_t flag) const;
    RenderPipelineCommonData* GetRenderPipelineCommonData();
    uint32_t GetFrameSlotCount() const { return static_cast<uint32_t>(_primaryResources.FrameConstants.size()); }

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    GDX12DeviceResources& GetDeviceResources(EDeviceRole role);
    void BuildBackBuffer(GDX12DeviceResources& presentResources);
    void CompileFrameGraph(FrameGraph& graph);
    void UploadSceneResources(GDX12DeviceResources& resources, uint32_t flags);
    void ResizeSceneBuffers(GDX12DeviceResources& resources, uint32_t flags);
    void MarkSceneDataDirty();
    
    void SubscribeToSceneManager();
    void UnsubscribeFromSceneManager();
    void UnsubscribeFromAllWorlds();

    GDX12Texture* CreateDX12Texture(const std::string& name, GDX12DeviceResources* resources, const Texture* texture);

    GameTimer* _timer;
    Window* _window;
    bool _renderingSuspended = false;
    
    ListenerHandle _worldCreatedListener = 0;
    ListenerHandle _worldDestroyedListener = 0;
    
    std::unordered_map<Entity, TransformCompGPUData> _transformGPUData;
    std::unordered_map<World*, WorldRenderSubscriptions> _worldSubscriptions;
    uint32_t _transformCount = 0;
    uint32_t _cameraCount = 0;
    uint32_t _instanceCount = 0;
    RenderPipelineCommonData _RPcommonData;

    std::unique_ptr<GDX12Device> _primaryDevice;
    std::unique_ptr<GDX12Device> _secondaryDevice;

    GDX12DeviceResources _primaryResources;
    GDX12DeviceResources _secondaryResources;
    std::unordered_map<std::string, std::unique_ptr<GPUTexture>> _textures;
    std::unordered_map<std::string, std::unique_ptr<GDX12Material>> _materials;
    std::unordered_map<uint32_t, const Mesh*> _meshes;

    // The swap chain and display depth belong to the presenting device.
    std::unique_ptr<GDX12SwapChain> _backBuffer;
    std::unique_ptr<GDX12Texture> _depthStencil;
    
    std::unique_ptr<CompiledFrameGraph> _frameGraph;

    RenderingMetrics _metrics;

};
