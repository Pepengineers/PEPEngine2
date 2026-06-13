#pragma once

#include "Common/GameTimer.h"
#include "Common/Module.h"
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12FrameConstants.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12RenderCommandRecorder.h"

#include "Engine.Core/Types/TextureTypes.h"
#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/ECS/Event.h"

class SceneManagerModule;
class World;

struct TransformComponent;
struct CameraComponent;
struct StaticMeshRenderComponent;

class Window;

using namespace Engine::Core;

class RenderModule final : public Module
{
public:
    RenderModule(Window* window, GameTimer* timer);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize() const;

    GDX12Material* GetMaterialByName(const std::string& name);

    //returns a pointer to a fully initialized structure that you can specify in components
    GDX12Material* CreateMaterial(const std::string& name);

    GDX12Texture* GetTextureByName(const std::string& name);
    //returns a pointer to a fully initialized structure that you can specify in materials
    GDX12Texture* CreateTexture(const std::string& name, const Texture* texture);

    //Uploads Mesh geometry to GPU
    void SubmitMesh(const Mesh* mesh, MeshHandle handle);
    
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
    GDX12RenderCommandRecorder* GetCommandRecorder();
    GDX12FrameConstants* GetCurrentFrameConstants();
    const GPUMesh* GetGPUMesh(MeshHandle handle);
    GDX12UploadBuffer<GDX12InstanceData>* GetInstanceCache();
    GDX12UploadBuffer<GDX12IndirectDrawArgs>* GetIndirectCommandsCache();

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void BuildDescHeapsAndBackBuffer();
    void BuildRootSignatures();
    void BuildShaders();
    void BuildPSOs();
    void BuildFrameConstants();

    void UpdateMainCB();
    void UpdateMaterialCB();
    
    void SubscribeToSceneManager();
    void UnsubscribeFromSceneManager();
    void UnsubscribeFromAllWorlds();

    GameTimer* _timer;
    Window* _window;
    
    ListenerHandle _worldCreatedListener = 0;
    ListenerHandle _worldDestroyedListener = 0;
    
    
    std::unordered_map<World*, WorldRenderSubscriptions> _worldSubscriptions;

    GDX12RenderCommandRecorder _commandRecorder;

    std::unique_ptr<GDX12Device> _primaryDevice;
    std::unique_ptr<GDX12Device> _secondaryDevice;
    bool _dualGPUMode;

    std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> _inputLayouts;
    std::unordered_map<std::string, std::unique_ptr<GDX12Material>> _materials;

    // All of class members below should probably be put into DeviceResources class, and made for each device
    // Since all of these resources are currently existing on _primaryDevice only
    std::unique_ptr<GDX12GeometryBuffer> _geometryBuffer;

    // All heaps created in one high-capacity instance
    std::unique_ptr<GDX12DescriptorHeap> _rtvHeap;
    std::unique_ptr<GDX12DescriptorHeap> _srvuavHeap;
    std::unique_ptr<GDX12DescriptorHeap> _dsvHeap;

    std::vector<std::unique_ptr<GDX12FrameConstants>> _frameConstants;
    UINT _currFrameConstantsIndex;

    std::unique_ptr<GDX12UploadBuffer<GDX12InstanceData>> _instanceCache;
    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> _IndirectCommandsCache;
    ComPtr<ID3D12CommandSignature> _commandSignature;

    std::unordered_map<std::string, ComPtr<ID3DBlob>> _shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> _PSOs;
    std::unordered_map<std::string, std::unique_ptr<GDX12RootSignature>> _rootSignatures;

    std::unordered_map<std::string, std::unique_ptr<GDX12Texture>> _textures;

    // These two resources are made on _primaryDevice only
    std::unique_ptr<GDX12SwapChain> _backBuffer;
    std::unique_ptr<GDX12Texture> _depthStencil;
};
