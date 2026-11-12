#include "BistroUIPosters.h"
#include "../../BistroUICanvases.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.UI/OffscreenUI.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <array>
#include <filesystem>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace Engine::UI;
    using Engine::Core::MeshHandle;

    AutoConsoleVariable<bool> AsyncCompute(L"Render.UI.AsyncCompute", false,
        L"Run local Bistro poster effects on the primary COMPUTE queue when available.");

    const std::array<Vector3, BistroUIPosters::Count> Centres = {{
        {-214.84f, 177, -72.78f}, {-174.89f, 177, 5.63f}
    }};
    constexpr const char* Names[] = {"BistroUIPosterAtrium", "BistroUIPosterTerrace"};
    constexpr const char* RasterNames[] = {"UI Atrium Raster", "UI Terrace Raster"};
    constexpr const char* EffectNames[] = {"UI Atrium Effects", "UI Terrace Effects"};

    bool IsBistro(const World& world)
    {
        auto path = world.GetPath().lexically_normal();
        if (path.filename().empty()) path = path.parent_path();
        const auto name = path.filename().wstring();
        return CompareStringOrdinal(name.c_str(), -1, L"Amazon Lumberyard Bistro", -1, TRUE) == CSTR_EQUAL;
    }

    MeshHandle MakePosterMesh(RenderModule& render)
    {
        using namespace Engine::Core;
        auto& registry = AssetManager::GetInstance().Meshes();
        const auto mesh = registry.Register(MeshAssetLocator{
            std::filesystem::path(ASSETS_FOLDER) / "Models/BistroUIPoster.generated"});
        auto* data = registry.GetMesh(mesh);
        if (!data)
        {
            const Vector3 positions[] = {{-0.5f, 0.5f, 0}, {-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}};
            const Vector2 uv[] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
            SubMesh quad;
            for (const bool back : {false, true})
                for (std::size_t i = 0; i < std::size(positions); ++i)
                {
                    Vertex vertex;
                    vertex.Position = vertex.PreviousPosition = positions[i];
                    vertex.Normal = {0, 0, back ? -1.0f : 1.0f};
                    vertex.Tangent = {back ? -1.0f : 1.0f, 0, 0};
                    vertex.TexCoord = {back ? 1 - uv[i].x : uv[i].x, uv[i].y};
                    quad.Vertices.push_back(vertex);
                }
            quad.Indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6};
            quad.Bounds = BoundingBox({0, 0, 0}, {0.5f, 0.5f, 0.002f});
            const auto bounds = quad.Bounds;
            std::vector<SubMesh> parts;
            parts.push_back(std::move(quad));
            data = registry.Cache(mesh, std::make_unique<Mesh>(std::move(parts), bounds,
                std::vector<MeshMaterial>{MeshMaterial{}}));
        }
        render.SubmitMesh(data, mesh);
        return mesh;
    }

    std::unique_ptr<GDX12Texture> MakeMaterialTarget(GDX12DeviceResources& primary, CanvasSize size)
    {
        auto target = std::make_unique<GDX12Texture>();
        GDX12TextureDesc desc;
        desc.Width = size.Width;
        desc.Height = size.Height;
        desc.Format = OffscreenFormat;
        desc.Semantic = ETextureSemantic::Color;
        desc.SRV_UAV_Heap = primary.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        desc.SRVDesc.Format = OffscreenFormat;
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.CreateRTV = true;
        desc.RTVHeap = primary.RTVHeap.get();
        desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
        desc.RTVDesc.Format = OffscreenFormat;
        desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        desc.ClearValue = {0, 0, 0, 0};
        target->Initialize(desc);
        return target;
    }

    class TextureLink final : public IRenderPassLink
    {
    public:
        GDX12Texture* GetTexture() override { return Texture; }
        bool IsInitialized() override { return Texture && Texture->IsInitialized(); }
        GDX12Texture* Texture = nullptr;
    };
}

struct BistroUIPosters::Impl
{
    struct Frame
    {
        std::unique_ptr<GDX12Texture> Effect, Target;
        std::shared_ptr<const CanvasFrame> Input;
        std::shared_ptr<const void> EffectOwner, RasterOwner;
    };

    struct Surface;
    class Pass final : public GDX12RenderPass
    {
    public:
        Pass(Impl& owner, Surface& surface, bool effects) : Owner(owner), Poster(surface), Effects(effects)
        { _numOutputs = 1; }
        void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override;
        void Execute(GDX12CommandList* commands) override;
        bool IsAsyncComputeEnabled() const override { return Effects && AsyncCompute.GetValue(); }
        IRenderPassLink* GetOutput(UINT index) override;

    private:
        Impl& Owner;
        Surface& Poster;
        bool Effects;
    };

    struct Surface
    {
        Surface(Impl& owner, std::size_t index) : Index(index), EffectPass(owner, *this, true), Raster(owner, *this, false) {}
        const std::size_t Index;
        GDX12Material* Material = nullptr;
        GPUTexture Binding;
        TextureLink EffectLink, TargetLink;
        Frame* Current = nullptr;
        bool SeparateEffects = false;
        Pass EffectPass, Raster;
    };

    explicit Impl(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes)
        : Render(render), Scenes(scenes), Atrium(*this, 0), Terrace(*this, 1)
    {
    }

    ~Impl()
    {
        if (const auto scenes = Scenes.lock())
        {
            scenes->OnWorldCreated.RemoveListener(WorldCreated);
            scenes->OnWorldDestroyed.RemoveListener(WorldDestroyed);
        }
        for (auto* surface : Surfaces())
            if (surface->Material && surface->Material->Diffuse == &surface->Binding)
                surface->Material->Diffuse = nullptr;
    }

    std::array<Surface*, Count> Surfaces() { return {&Atrium, &Terrace}; }

    void Subscribe(const std::shared_ptr<Impl>& owner, SceneManagerModule& scenes)
    {
        const std::weak_ptr<Impl> weak(owner);
        WorldCreated = scenes.OnWorldCreated.AddListener([weak](World& world)
        {
            const auto state = weak.lock();
            if (!state || !IsBistro(world)) return;
            state->Worlds.try_emplace(&world);
            // This factory runs after RenderModule has subscribed to the world's component events.
            auto previous = std::move(world.SceneMeshFactory);
            world.SceneMeshFactory = [weak, &world, previous = std::move(previous)](
                const std::string& name, const StaticMeshRenderComponent& source)
            {
                const bool handled = previous && previous(name, source);
                if (name == "Exterior_exterior")
                    if (const auto owner = weak.lock()) owner->Spawn(world);
                return handled;
            };
        });
        WorldDestroyed = scenes.OnWorldDestroyed.AddListener([weak](World& world)
        {
            if (const auto state = weak.lock()) state->Worlds.erase(&world);
        });
        for (std::size_t i = 0; i < scenes.GetWorldCount(); ++i)
            if (auto* world = scenes.GetWorld(i); world && IsBistro(*world)) Worlds.try_emplace(world);
    }

    void Spawn(World& world)
    {
        const auto found = Worlds.find(&world);
        if (found == Worlds.end()) return;
        auto& entities = found->second;
        if (entities[0] != InvalidEntity && entities[1] != InvalidEntity) return;
        if (!Mesh.IsValid()) Mesh = MakePosterMesh(Render);
        Render.ReserveTransformCapacity(Count);
        Render.ReserveInstanceCapacity(Count);
        auto& ecs = world.GetECS();
        for (auto* surface : Surfaces())
        {
            const auto index = surface->Index;
            if (entities[index] != InvalidEntity) continue;
            if (!surface->Material)
            {
                surface->Material = Render.GetMaterialByName(Names[index]);
                if (!surface->Material) surface->Material = Render.CreateMaterial(Names[index]);
                if (!surface->Material) throw std::runtime_error("Cannot create Bistro poster material");
                surface->Binding.Name = Names[index];
                surface->Material->Diffuse = &surface->Binding;
                surface->Material->Type = MaterialType::Opaque;
                surface->Material->UseBakedLighting = true;
                surface->Material->DirtyFlag = true;
            }
            auto entity = ecs.CreateEntity();
            try
            {
                entity.AddComponent<NameComponent>(Names[index]);
                entity.AddComponent<TransformComponent>(Centres[index], Vector3(0, -63, 0), Vector3(68, 38.25f, 1));
                auto& draw = entity.AddComponent<StaticMeshRenderComponent>(Mesh, std::vector<GDX12Material*>{surface->Material});
                draw.ReflectionDynamic = true;
                entities[index] = entity.GetId();
            }
            catch (...)
            {
                ecs.DestroyEntity(entity.GetId());
                throw;
            }
        }
    }

    bool Prepare(GDX12DeviceResources& primary, UINT slot, double animationSeconds)
    {
        if (slot != primary.CurrFrameConstantsIndex || slot >= primary.FrameConstants.size() || !primary.Device)
            throw std::invalid_argument("Bistro posters require the current primary frame slot");
        const auto completed = primary.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
        if (completed == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        if (primary.FrameConstants[slot]->FenceValue > completed)
            throw std::logic_error("Bistro poster slot is still in flight");

        for (auto& [world, entities] : Worlds)
            if (entities[0] == InvalidEntity || entities[1] == InvalidEntity) Spawn(*world);
        const bool visible = !Worlds.empty();
        bool rebuild = false;
        if (visible)
        {
            if (!Raster)
            {
                auto raster = std::make_unique<OffscreenRenderer>(*primary.Device,
                    static_cast<unsigned>(Count * primary.FrameConstants.size()));
                auto effects = std::make_unique<CanvasEffects>(*primary.Device);
                Slots.resize(primary.FrameConstants.size());
                Raster = std::move(raster);
                Effects = std::move(effects);
            }
            const auto inputs = Posters.BuildFrames(animationSeconds);
            for (auto* surface : Surfaces())
            {
                const auto i = surface->Index;
                const auto& input = inputs[i];
                const auto profile = GetCanvasProfile(input->LogicalSize, input->Quality);
                auto& frame = Slots.at(slot)[i];
                std::unique_ptr<GDX12Texture> target, effect;
                if (!frame.Target || frame.Target->GetWidth() != profile.RasterSize.Width ||
                    frame.Target->GetHeight() != profile.RasterSize.Height)
                    target = MakeMaterialTarget(primary, profile.RasterSize);
                if (!frame.Effect || frame.Effect->GetWidth() != profile.EffectsSize.Width ||
                    frame.Effect->GetHeight() != profile.EffectsSize.Height)
                {
                    effect = std::make_unique<GDX12Texture>();
                    GDX12RenderHelpers::InitializeComputeTexture(*effect, primary, profile.EffectsSize.Width,
                        profile.EffectsSize.Height, OffscreenFormat, false);
                }
                frame.EffectOwner.reset();
                frame.RasterOwner.reset();
                if (target) frame.Target = std::move(target);
                if (effect) frame.Effect = std::move(effect);
                frame.Input = input;
                surface->Current = &frame;
                surface->TargetLink.Texture = frame.Target.get();
                surface->EffectLink.Texture = frame.Effect.get();
                surface->Binding.PrimaryDeviceTexture = frame.Target.get();
                surface->Material->DirtyFlag = true;
            }
        }
        const bool localAsync = AsyncCompute.GetValue();
        for (auto* surface : Surfaces())
        {
            if (!visible)
            {
                surface->Current = nullptr;
                surface->TargetLink.Texture = surface->EffectLink.Texture = nullptr;
                if (surface->Binding.PrimaryDeviceTexture)
                {
                    surface->Binding.PrimaryDeviceTexture = nullptr;
                    surface->Material->DirtyFlag = true;
                }
            }
            const bool separate = visible && localAsync;
            rebuild |= surface->SeparateEffects != separate;
            surface->SeparateEffects = separate;
        }
        return rebuild;
    }

    RenderModule& Render;
    std::weak_ptr<SceneManagerModule> Scenes;
    ListenerHandle WorldCreated = 0, WorldDestroyed = 0;
    std::map<World*, std::array<Entity, Count>> Worlds;
    MeshHandle Mesh;
    BistroUICanvases::Posters Posters;
    Surface Atrium, Terrace;
    std::unique_ptr<OffscreenRenderer> Raster;
    std::unique_ptr<CanvasEffects> Effects;
    std::vector<std::array<Frame, Count>> Slots;
};

void BistroUIPosters::Impl::Pass::RecordRenderGraph(GDX12RenderGraphBuilder& builder)
{
    builder.SetName(Effects ? EffectNames[Poster.Index] : RasterNames[Poster.Index]);
    if (Effects)
    {
        builder.AsyncCompute();
        builder.Write(&Poster.EffectLink, EffectNames[Poster.Index], Lifetime::Dynamic, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return;
    }
    if (Poster.SeparateEffects) builder.Read(&Poster.EffectLink, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    builder.Write(&Poster.TargetLink, RasterNames[Poster.Index], Lifetime::Dynamic, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void BistroUIPosters::Impl::Pass::Execute(GDX12CommandList* commands)
{
    auto* frame = Poster.Current;
    if (!frame) return;
    auto* effect = frame->Effect->GetResource();
    if (Effects)
    {
        frame->EffectOwner = Owner.Effects->Record(*commands, *frame->Input, effect->D3DResource);
        return;
    }
    if (!Poster.SeparateEffects)
    {
        commands->ResourceBarrier({effect->GetTransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        frame->EffectOwner = Owner.Effects->Record(*commands, *frame->Input, effect->D3DResource);
        commands->ResourceBarrier({effect->GetPixelShaderResourceBarrier()});
    }
    frame->RasterOwner = Owner.Raster->TryRecord(*commands, frame->Target->GetResource()->D3DResource,
        frame->Input->Draw, {{CanvasEffectTextureId, effect->D3DResource}});
    if (!frame->RasterOwner) throw std::logic_error("Retired Bistro poster frame has no local raster slot");
}

IRenderPassLink* BistroUIPosters::Impl::Pass::GetOutput(UINT index)
{
    if (index != 0) return nullptr;
    return Effects ? &Poster.EffectLink : &Poster.TargetLink;
}

BistroUIPosters::BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes)
{
    if (!scenes) throw std::invalid_argument("Bistro posters require a scene manager");
    _impl = std::make_shared<Impl>(render, scenes);
    _impl->Subscribe(_impl, *scenes);
}

BistroUIPosters::~BistroUIPosters() = default;

bool BistroUIPosters::PrepareFrame(GDX12DeviceResources& primary, UINT frameSlot, double animationSeconds)
{
    return _impl->Prepare(primary, frameSlot, animationSeconds);
}

GDX12RenderPass& BistroUIPosters::RasterPass(std::size_t index) const
{
    return _impl->Surfaces().at(index)->Raster;
}

IRenderPassLink& BistroUIPosters::Output(std::size_t index) const
{
    return _impl->Surfaces().at(index)->TargetLink;
}

void BistroUIPosters::AddPasses(GDX12RenderGraph& graph) const
{
    for (auto* surface : _impl->Surfaces())
    {
        if (surface->SeparateEffects) graph.AddPass(surface->EffectPass);
        graph.AddPass(surface->Raster);
    }
}

void BistroUIPosters::DeclareMaterialReads(GDX12RenderGraphBuilder& builder) const
{
    for (auto* surface : _impl->Surfaces())
        builder.Read(&surface->TargetLink, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}

bool BistroUIPosters::UsesAsyncCompute() const
{
    return _impl->Atrium.SeparateEffects || _impl->Terrace.SeparateEffects;
}
