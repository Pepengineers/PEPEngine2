#include "SceneCloudPicture.h"
#include "NativeWork.h"
#include "Rendering/UI/BistroUIPosters.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/ConsoleVariables.h"

namespace Meta {
namespace {
AutoConsoleVariable<bool> Enabled(L"Meta.CloudPicture.Enabled", true, L"Show the MGPU-Noise cloud picture in Bistro.");
AutoConsoleVariable<int> Width(L"Meta.CloudPicture.Width", 1024, L"Cloud picture texture width, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Height(L"Meta.CloudPicture.Height", 512, L"Cloud picture texture height, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Adapter(L"Meta.CloudPicture.Adapter", -1, L"-1 primary; otherwise explicit DXGI hardware adapter index.",
    EConsoleVariableFlags::Default, [](int v) { return v >= -1; });
struct Settings {
    bool Active;
    int Width, Height, Adapter;
    bool operator==(const Settings& other) const {
        return Active == other.Active && Width == other.Width && Height == other.Height && Adapter == other.Adapter;
    }
};
Settings Requested() { return {Enabled.GetValue(), Width.GetValue(), Height.GetValue(), Adapter.GetValue()}; }
}
struct SceneCloudPicture::Impl {
    GDX12DeviceResources& Resources;
    RenderModule& Render;
    std::shared_ptr<SceneManagerModule> Scenes;
    std::unique_ptr<CloudPictureTexture> Texture;
    std::shared_ptr<QueueSubmission> Local;
    Settings Attempted = Requested();
    bool Active = Attempted.Active;
    GDX12Material* Material = nullptr;
    GPUTexture Binding;
    std::vector<GDX12Texture*> SlotBindings;
    World* Scene = nullptr;
    Entity Picture = InvalidEntity;
    ListenerHandle Created = 0, Destroyed = 0;

    explicit Impl(RenderPipelineContext& c) : Resources(c.Primary), Render(c.Runtime),
        Scenes(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()), SlotBindings(c.Primary.FrameConstants.size()) {
        if (Active) Texture = std::make_unique<CloudPictureTexture>(Resources, Attempted.Width, Attempted.Height, Attempted.Adapter);
        else Local = std::make_shared<QueueSubmission>(*Resources.Device);
    }
    ~Impl() {
        Scenes->OnWorldCreated.RemoveListener(Created);
        Scenes->OnWorldDestroyed.RemoveListener(Destroyed);
        if (Material && Material->Diffuse == &Binding) Material->Diffuse = nullptr;
    }
    void Subscribe(const std::shared_ptr<Impl>& owner) {
        const std::weak_ptr<Impl> weak(owner);
        Created = Scenes->OnWorldCreated.AddListener([weak](World& world) {
            auto path = world.GetPath().lexically_normal();
            if (path.filename().empty()) path = path.parent_path();
            if (path.filename() != L"Amazon Lumberyard Bistro") return;
            auto previous = std::move(world.SceneMeshFactory);
            world.SceneMeshFactory = [weak, &world, previous = std::move(previous)](const std::string& name,
                const StaticMeshRenderComponent& source) {
                const bool handled = previous && previous(name, source);
                if (name == "Exterior_exterior") if (const auto state = weak.lock()) state->Spawn(world);
                return handled;
            };
        });
        Destroyed = Scenes->OnWorldDestroyed.AddListener([weak](World& world) {
            if (const auto state = weak.lock(); state && state->Scene == &world) {
                state->Scene = nullptr; state->Picture = InvalidEntity;
            }
        });
    }
    void Spawn(World& world) {
        if (Scene == &world && Picture != InvalidEntity) return;
        if (!Material) {
            Material = Render.GetMaterialByName("BistroCloudPicture");
            if (!Material) Material = Render.CreateMaterial("BistroCloudPicture");
            if (!Material) throw std::runtime_error("Cannot create cloud picture material");
            Binding.Name = "BistroCloudPicture";
            Material->Diffuse = &Binding;
            Material->Type = MaterialType::Opaque;
            Material->UseBakedLighting = true;
            Material->DirtyFlag = true;
        }
        const auto mesh = BistroUIPosters::CreateSurfaceMesh(Render);
        Render.ReserveTransformCapacity(1);
        Render.ReserveInstanceCapacity(1);
        auto& ecs = world.GetECS();
        auto entity = ecs.CreateEntity();
        try {
            entity.AddComponent<NameComponent>("BistroCloudPicture");
            entity.AddComponent<TransformComponent>(Vector3(-194.86f, 230, -33.58f), Vector3(0, -63, 0), Vector3(100, 50, 1));
            auto& draw = entity.AddComponent<StaticMeshRenderComponent>(mesh, std::vector<GDX12Material*>{Material});
            draw.Visible = Active;
            draw.ReflectionDynamic = true;
            Scene = &world; Picture = entity.GetId();
        } catch (...) { ecs.DestroyEntity(entity.GetId()); throw; }
        Log::Info("MetaRenderGraph", "Cloud picture surface center=(-194.86,230,-33.58)cm yaw=-63 size=100x50cm");
    }
    void Visibility(bool visible) {
        if (!Scene || Picture == InvalidEntity) return;
        auto& draw = Scene->GetECS().Get<StaticMeshRenderComponent>(Picture);
        if (draw.Visible == visible) return;
        draw.Visible = visible;
        auto& commands = *Resources.IndirectCommandsCache;
        for (const auto index : draw._CBufferIndices) {
            auto command = commands.GetCPUData()[index];
            command.InstanceCount = visible ? 1 : 0;
            commands.CopyData(index, command);
        }
    }
};
SceneCloudPicture::SceneCloudPicture(RenderPipelineContext& c) : _impl(std::make_shared<Impl>(c)) { _impl->Subscribe(_impl); }
SceneCloudPicture::~SceneCloudPicture() = default;
bool SceneCloudPicture::UpdateSettings(const std::function<void()>& drain) {
    auto& p = *_impl;
    const auto next = Requested();
    if (next == p.Attempted) return false;
    p.Attempted = next;
    if (!next.Active) { p.Active = false; return false; }
    std::unique_ptr<CloudPictureTexture> replacement;
    try {
        replacement = std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height, next.Adapter);
    } catch (const std::exception& error) {
        Log::Error("MetaRenderGraph", "Cloud picture settings rejected; retaining last valid generation: ", error.what());
        return false;
    }
    drain();
    if (p.Texture) p.Texture->Drain();
    p.Texture = std::move(replacement);
    p.Active = true;
    return true;
}
const std::shared_ptr<QueueSubmission>& SceneCloudPicture::Submissions() const {
    return _impl->Texture ? _impl->Texture->Submissions() : _impl->Local;
}
void SceneCloudPicture::Prepare(UINT slot, double time) {
    auto& p = *_impl;
    p.Visibility(p.Active);
    if (p.Active) p.Texture->Prepare(slot, time);
    if (p.Material) {
        auto* texture = p.Active ? p.Texture->Texture(slot) : nullptr;
        p.Binding.PrimaryDeviceTexture = texture;
        if (p.SlotBindings.at(slot) != texture) {
            p.SlotBindings[slot] = texture;
            p.Material->DirtyFlag = true;
        }
    }
}
void SceneCloudPicture::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings) {
    auto& p = *_impl;
    if (!p.Active || !p.Material || !p.Binding.PrimaryDeviceTexture) return;
    const Id consumers = Id(graph.Operations.size());
    p.Texture->Append(graph, bindings);
    const auto id = NativeWork::ResourceFor(graph, bindings, p.Binding.PrimaryDeviceTexture->GetResource()->D3DResource.Get(), "cloud picture material");
    for (Id op = 0; op < consumers; ++op)
        if (graph.Operations[op].Name == "scene GBuffer ExecuteIndirect")
            graph.Operations[op].Accesses.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
}
bool SceneCloudPicture::Record(Id id, ID3D12GraphicsCommandList* cmd) { return _impl->Active && _impl->Texture->Record(id, cmd); }
std::vector<PacketWait> SceneCloudPicture::Waits(const Schedule& schedule) const {
    return _impl->Active ? _impl->Texture->Waits(schedule) : std::vector<PacketWait>{};
}
void SceneCloudPicture::RetireFrame(const Schedule& schedule, const std::vector<CompletionToken>& tokens, bool success) {
    if (_impl->Active) _impl->Texture->RetireFrame(schedule, tokens, success);
}
void SceneCloudPicture::Drain() { if (_impl->Texture) _impl->Texture->Drain(); }
}
