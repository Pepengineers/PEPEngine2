#include "BistroUIPosters.h"
#include "BistroPaintingSurfaces.h"
#include "BistroUICanvases.h"
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


    constexpr const char* Names[] = {"BistroUINeon", "BistroUIAstra", "BistroUIClouds"};

    bool IsBistro(const World& world)
    {
        auto path = world.GetPath().lexically_normal();
        if (path.filename().empty()) path = path.parent_path();
        const auto name = path.filename().wstring();
        return CompareStringOrdinal(name.c_str(), -1, L"Amazon Lumberyard Bistro", -1, TRUE) == CSTR_EQUAL;
    }

}

namespace
{
    std::unique_ptr<GDX12Texture> MakeMaterialTarget(GDX12DeviceResources& primary, CanvasSize size, bool post = false)
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
        desc.CreateUAV = post;
        if (post) {
            desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.UAVDesc.Format = OffscreenFormat;
            desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        desc.CreateRTV = !post;
        if (!post) {
            desc.RTVHeap = primary.RTVHeap.get();
            desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
            desc.RTVDesc.Format = OffscreenFormat;
            desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
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
        std::unique_ptr<GDX12Texture> Effect, Target, Raw;
        std::shared_ptr<const CanvasFrame> Input;
        std::shared_ptr<CanvasEffects::Prepared> PreparedEffects;
        std::shared_ptr<CanvasEffects::PreparedPost> PreparedPost;
        std::shared_ptr<const void> EffectOwner, RasterOwner, PostOwner;
        bool Published = false;
    };

    struct Surface;
    class Pass final : public GDX12RenderPass
    {
    public:
        Pass(Impl& owner, Surface& surface) : Owner(owner), Poster(surface)
        { _numOutputs = 1; }
        void Execute(GDX12CommandList* commands) override;
        IRenderPassLink* GetOutput(UINT index) override;

    private:
        Impl& Owner;
        Surface& Poster;
    };

    struct Surface
    {
        Surface(Impl& owner, std::size_t index) : Index(index), Raster(owner, *this) {}
        const std::size_t Index;
        GDX12Material* Material = nullptr;
        GPUTexture Binding;
        TextureLink EffectLink, TargetLink, RawLink;
        Frame* Current = nullptr;
        Pass Raster;
    };

    explicit Impl(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas)
        : Render(render), Scenes(scenes), Atrium(*this, 0), Terrace(*this, 1), Clouds(*this, 2), CloudCanvas(cloudCanvas)
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

    std::array<Surface*, Count> Surfaces() { return {&Atrium, &Terrace, &Clouds}; }

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
                if (!handled && name == "Interior_interior")
                    if (const auto owner = weak.lock()) { owner->Spawn(world, source); return true; }
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

    void Spawn(World& world, const StaticMeshRenderComponent& source)
    {
        auto& entities = Worlds.at(&world);
        std::array<GDX12Material*, Count> materials{};
        for (auto* surface : Surfaces()) {
            const auto i = surface->Index;
            if (i == 2 && !CloudCanvas) continue;
            if (!surface->Material) {
                surface->Material = Render.GetMaterialByName(Names[i]);
                if (!surface->Material) surface->Material = Render.CreateMaterial(Names[i]);
                if (!surface->Material) throw std::runtime_error("Cannot create Bistro canvas material");
                surface->Binding.Name = Names[i];
                surface->Material->Diffuse = &surface->Binding;
                surface->Material->Type = MaterialType::Opaque;
                surface->Material->UseBakedLighting = true;
                surface->Material->DirtyFlag = true;
            }
            materials[i] = surface->Material;
        }
        BistroPaintingSurfaces::Install(world, Render, source, materials, entities);
    }

    bool Prepare(GDX12DeviceResources& primary, UINT slot, double animationSeconds)
    {
        if (slot != primary.CurrFrameConstantsIndex || slot >= primary.FrameConstants.size() || !primary.Device)
            throw std::invalid_argument("Bistro posters require the current primary frame slot");
        const auto completed = primary.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
        if (completed == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        if (primary.FrameConstants[slot]->FenceValue > completed)
            throw std::logic_error("Bistro poster slot is still in flight");

        const bool visible = !Worlds.empty();
        bool rebuild = false;
        if (visible)
        {
            if (!Raster[0])
            {
                std::array<std::unique_ptr<OffscreenRenderer>, Count> raster;
                for (auto& renderer : raster)
                    renderer = std::make_unique<OffscreenRenderer>(*primary.Device,
                        static_cast<unsigned>(primary.FrameConstants.size()), true);
                auto effects = std::make_unique<CanvasEffects>(*primary.Device);
                Slots.resize(primary.FrameConstants.size());
                Raster = std::move(raster);
                Effects = std::move(effects);
            }
            const auto inputs = Posters.BuildFrames(animationSeconds);
            for (auto* surface : Surfaces())
            {
                const auto i = surface->Index;
                const bool image = i == 2;
                if (!surface->Material || (image && (!CloudCanvas || !ImageInput))) {
                    Slots.at(slot)[i].Published = false;
                    surface->Current = nullptr;
                    surface->TargetLink.Texture = surface->EffectLink.Texture = surface->RawLink.Texture = nullptr;
                    continue;
                }
                const auto& input = inputs[i];
                const auto profile = GetCanvasProfile(input->LogicalSize, input->Quality);
                auto& frame = Slots.at(slot)[i];
                frame.EffectOwner.reset(); frame.RasterOwner.reset(); frame.PostOwner.reset();
                bool changed = false;
                if (!frame.Target || frame.Target->GetWidth() != profile.RasterSize.Width ||
                    frame.Target->GetHeight() != profile.RasterSize.Height) {
                    frame.Target = MakeMaterialTarget(primary, profile.RasterSize, !image);
                    if (!image) frame.Raw = MakeMaterialTarget(primary, profile.RasterSize);
                    changed = true;
                }
                if (!image) {
                    if (!frame.Effect || frame.Effect->GetWidth() != profile.EffectsSize.Width ||
                        frame.Effect->GetHeight() != profile.EffectsSize.Height) {
                        frame.Effect = std::make_unique<GDX12Texture>();
                        GDX12RenderHelpers::InitializeComputeTexture(*frame.Effect, primary,
                            profile.EffectsSize.Width, profile.EffectsSize.Height, OffscreenFormat, false);
                        frame.PreparedEffects = Effects->Prepare(*input, frame.Effect->GetResource()->D3DResource);
                    }
                    if (changed) frame.PreparedPost = Effects->PreparePost(*input,
                        frame.Raw->GetResource()->D3DResource, frame.Target->GetResource()->D3DResource);
                }
                frame.Input = input;
                surface->Current = &frame;
                surface->TargetLink.Texture = frame.Target.get();
                surface->RawLink.Texture = frame.Raw.get();
                surface->EffectLink.Texture = image ? ImageInput : frame.Effect.get();
                surface->Binding.PrimaryDeviceTexture = frame.Target.get();
                if (changed || !frame.Published) surface->Material->DirtyFlag = true;
                frame.Published = true;
            }
        }
        for (auto* surface : Surfaces())
        {
            if (!visible) {
                surface->Current = nullptr;
                surface->TargetLink.Texture = surface->EffectLink.Texture = surface->RawLink.Texture = nullptr;
            }
            const bool active = visible && surface->Current;
            if (!active && surface->Binding.PrimaryDeviceTexture) {
                surface->Binding.PrimaryDeviceTexture = nullptr;
                surface->Material->DirtyFlag = true;
            }
            for (auto& [world, entities] : Worlds) {
                const auto entity = entities[surface->Index];
                if (entity == InvalidEntity) continue;
                auto& draw = world->GetECS().Get<StaticMeshRenderComponent>(entity);
                if (draw.Visible != active) {
                    draw.Visible = active;
                    auto* commands = primary.IndirectCommandsCache.get();
                    for (const auto index : draw._CBufferIndices) {
                        auto command = commands->GetCPUData()[index];
                        command.InstanceCount = active ? 1 : 0;
                        commands->CopyData(index, command);
                    }
                }
            }
        }
        return rebuild;
    }

    RenderModule& Render;
    std::weak_ptr<SceneManagerModule> Scenes;
    ListenerHandle WorldCreated = 0, WorldDestroyed = 0;
    std::map<World*, std::array<Entity, Count>> Worlds;
    BistroUICanvases::Posters Posters;
    Surface Atrium, Terrace, Clouds;
    bool CloudCanvas = false;
    GDX12Texture* ImageInput = nullptr;
    std::array<std::unique_ptr<OffscreenRenderer>, Count> Raster;
    std::unique_ptr<CanvasEffects> Effects;
    std::vector<std::array<Frame, Count>> Slots;
};



void BistroUIPosters::Impl::Pass::Execute(GDX12CommandList* commands)
{
    auto* frame = Poster.Current;
    if (!frame) return;
    const bool image = Poster.Index == 2;
    auto* effect = Poster.EffectLink.Texture->GetResource();
    if (!image) {
        commands->ResourceBarrier({effect->GetTransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        frame->EffectOwner = Owner.Effects->RecordPrepared(*commands, *frame->Input, frame->PreparedEffects);
        commands->ResourceBarrier({effect->GetPixelShaderResourceBarrier()});
    }
    auto* raster = image ? frame->Target.get() : frame->Raw.get();
    commands->ResourceBarrier({raster->GetResource()->GetRenderTargetBarrier()});
    frame->RasterOwner = Owner.Raster[Poster.Index]->TryRecord(*commands, raster->GetResource()->D3DResource,
        frame->Input->Draw, {{CanvasEffectTextureId, effect->D3DResource}});
    if (!frame->RasterOwner) throw std::logic_error("Retired Bistro canvas frame has no local raster slot");
    if (!image) {
        commands->ResourceBarrier({raster->GetResource()->GetNonPixelShaderResourceBarrier(),
            frame->Target->GetResource()->GetUnorderedAccessBarrier()});
        frame->PostOwner = Owner.Effects->RecordPost(*commands, *frame->Input, frame->PreparedPost);
    }
}

IRenderPassLink* BistroUIPosters::Impl::Pass::GetOutput(UINT index)
{
    if (index != 0) return nullptr;
    return &Poster.TargetLink;
}

BistroUIPosters::BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas)
{
    if (!scenes) throw std::invalid_argument("Bistro posters require a scene manager");
    _impl = std::make_shared<Impl>(render, scenes, cloudCanvas);
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

std::vector<GDX12Texture*> BistroUIPosters::NativeTargets(std::size_t index) const
{
    auto* frame = _impl->Surfaces().at(index)->Current;
    if (!frame) return {};
    if (index == 2) return {_impl->ImageInput, frame->Target.get()};
    return {frame->Effect.get(), frame->Target.get(), frame->Raw.get()};
}






void BistroUIPosters::SetImageInput(GDX12Texture* image) { _impl->ImageInput = image; }
