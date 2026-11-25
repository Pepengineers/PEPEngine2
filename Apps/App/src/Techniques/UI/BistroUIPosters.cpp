#include "BistroUIPosters.h"
#include "BistroPaintingSurfaces.h"
#include "BistroUICanvases.h"
#include "Techniques/NativeWork.h"
#include "Engine.Core/Logging/Logger.h"
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
    using namespace BistroRender;

    const auto AdapterVariables = [] {
        std::array<std::unique_ptr<AutoConsoleVariable<int>>, BistroUICanvases::Posters.size()> variables;
        for (size_t index = 0; index < variables.size(); ++index) {
            const auto& poster = BistroUICanvases::Posters[index];
            if (!poster.Adapter) continue;
            variables[index] = std::make_unique<AutoConsoleVariable<int>>(poster.Adapter, -1, poster.Description,
                EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
        }
        return variables;
    }();
    const auto CrossSyncVariables = [] {
        std::array<std::unique_ptr<AutoConsoleVariable<int>>, BistroUICanvases::Posters.size()> variables;
        for (size_t index = 0; index < variables.size(); ++index) {
            const auto& poster = BistroUICanvases::Posters[index];
            if (!poster.CrossSync) continue;
            variables[index] = std::make_unique<AutoConsoleVariable<int>>(poster.CrossSync, 1,
                std::wstring(poster.Description) + L" remote display: 0 exact previous (first uses current), 1 exact current. Local uses current.",
                EConsoleVariableFlags::Default, [](int value) { return value == 0 || value == 1; });
        }
        return variables;
    }();

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

    constexpr State CopyRead = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr State CopyWrite = D3D12_RESOURCE_STATE_COPY_DEST;

    ID3D12Resource* Native(GDX12Texture& texture) { return texture.GetResource()->D3DResource.Get(); }

    Access Bind(Graph& graph, std::vector<ID3D12Resource*>& bindings, ID3D12Resource* resource,
                const char* name, Mode usage, State state)
    {
        return {NativeWork::ResourceFor(graph, bindings, resource, name), usage, state};
    }

    struct SharedImage
    {
        QueueSubmission::TransferBuffer Buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
        UINT64 PayloadBytes = 0;

        void Prepare(QueueSubmission& submission, Id device, const D3D12_RESOURCE_DESC& desc)
        {
            if (Buffer.Resources[0] && Layout.Footprint.Width == desc.Width && Layout.Footprint.Height == desc.Height &&
                Layout.Footprint.Format == desc.Format) return;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT primary{}, remote{};
            UINT64 bytes = 0, remoteBytes = 0;
            UINT rows = 0;
            UINT64 rowBytes = 0;
            submission.Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &primary, &rows, &rowBytes, &bytes);
            submission.Device(device).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &remote, nullptr, nullptr, &remoteBytes);
            if (!bytes || bytes == UINT64_MAX || bytes != remoteBytes || primary.Footprint.RowPitch != remote.Footprint.RowPitch)
                throw std::runtime_error("World canvas requires matching adapter texture footprints");
            Buffer = submission.CreateTransferBuffer(device, bytes);
            Layout = primary;
            PayloadBytes = rows * rowBytes;
        }

        Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                  const std::string& name, size_t endpoint, GDX12Texture& texture, bool exportImage, Queue queue)
        {
            auto* image = Native(texture);
            auto* buffer = Buffer.Resources.at(endpoint).Get();
            const CD3DX12_TEXTURE_COPY_LOCATION imageLocation(image, 0u), bufferLocation(buffer, Layout);
            const auto source = exportImage ? imageLocation : bufferLocation;
            const auto dest = exportImage ? bufferLocation : imageLocation;
            const Id copy = work.Add(graph, name,
                {Bind(graph, bindings, source.pResource, "canvas transfer source", Mode::Read, CopyRead),
                 Bind(graph, bindings, dest.pResource, "canvas transfer destination", Mode::Write, CopyWrite)},
                [source, dest](auto* cmd) { cmd->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr); }, {}, queue);
            graph.Operations[copy].TransferBytes = PayloadBytes;
            return copy;
        }
    };

    struct DrawFrame
    {
        std::unique_ptr<GDX12Texture> Effect, Target, Raw;
        std::shared_ptr<const CanvasFrame> Input;
        std::shared_ptr<CanvasEffects::Prepared> PreparedEffects;
        std::shared_ptr<CanvasEffects::PreparedPost> PreparedPost;
        std::shared_ptr<const void> EffectOwner, RasterOwner, PostOwner;

        void Retire()
        {
            EffectOwner.reset();
            RasterOwner.reset();
            PostOwner.reset();
        }

        void PrepareTarget(GDX12DeviceResources& device, CanvasSize size)
        {
            if (!Target || Target->GetWidth() != size.Width || Target->GetHeight() != size.Height) {
                Target = MakeMaterialTarget(device, size, true);
                PreparedPost.reset();
            }
        }
    };

    struct CanvasRenderer
    {
        CanvasRenderer(GDX12Device& device, UINT slots) : Raster(device, slots, true) {}
        OffscreenRenderer Raster;
        std::unique_ptr<CanvasEffects> Effects;

        void Prepare(GDX12DeviceResources& device, DrawFrame& frame, const std::shared_ptr<const CanvasFrame>& input)
        {
            frame.Input = input;
            const auto profile = GetCanvasProfile(input->LogicalSize, input->Quality);
            frame.PrepareTarget(device, profile.RasterSize);
            if (!Effects) Effects = std::make_unique<CanvasEffects>(*device.Device);
            if (!frame.Effect || frame.Effect->GetWidth() != profile.EffectsSize.Width ||
                frame.Effect->GetHeight() != profile.EffectsSize.Height) {
                frame.Effect = std::make_unique<GDX12Texture>();
                GDX12RenderHelpers::InitializeComputeTexture(*frame.Effect, device,
                    profile.EffectsSize.Width, profile.EffectsSize.Height, OffscreenFormat, false);
                frame.PreparedEffects = Effects->Prepare(*input, frame.Effect->GetResource()->D3DResource);
            }
            if (!frame.PreparedPost) {
                frame.Raw = MakeMaterialTarget(device, profile.RasterSize);
                frame.PreparedPost = Effects->PreparePost(*input, frame.Raw->GetResource()->D3DResource,
                    frame.Target->GetResource()->D3DResource);
            }
        }

        void Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                    DrawFrame& frame, const std::string& name)
        {
            auto* effect = frame.Effect->GetResource();
            auto* output = frame.Target->GetResource();
            auto* raster = frame.Raw->GetResource();
            std::vector<Access> accesses{
                Bind(graph, bindings, effect->D3DResource.Get(), "canvas effect/image",
                    Mode::ReadWrite, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
                Bind(graph, bindings, raster->D3DResource.Get(), "canvas raster",
                    Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Bind(graph, bindings, output->D3DResource.Get(), "canvas post",
                    Mode::Write, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)};
            work.AddOwned(graph, name, std::move(accesses), bindings,
                [this, &frame, effect, raster, output](auto* cmd) {
                    cmd->ResourceBarrier({effect->GetUnorderedAccessBarrier()});
                    frame.EffectOwner = Effects->RecordPrepared(*cmd, *frame.Input, frame.PreparedEffects);
                    cmd->ResourceBarrier({effect->GetPixelShaderResourceBarrier()});
                    frame.RasterOwner = Raster.TryRecord(*cmd, raster->D3DResource, frame.Input->Draw,
                        {{CanvasEffectTextureId, effect->D3DResource}});
                    if (!frame.RasterOwner) throw std::logic_error("Retired world canvas has no raster slot");
                    cmd->ResourceBarrier({raster->GetNonPixelShaderResourceBarrier(), output->GetUnorderedAccessBarrier()});
                    frame.PostOwner = Effects->RecordPost(*cmd, *frame.Input, frame.PreparedPost);
                }, "World canvas cannot submit private queue signals");
        }
    };

    bool DeviceLost(HRESULT code)
    {
        return code == DXGI_ERROR_DEVICE_REMOVED || code == DXGI_ERROR_DEVICE_RESET ||
            code == DXGI_ERROR_DEVICE_HUNG || code == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
}

struct BistroUIPosters::Impl
{
    struct Remote
    {
        struct Slot
        {
            DrawFrame Draw;
            SharedImage Output;
            NativeWork Work;
            Schedule Plan;
            std::vector<ID3D12Resource*> Bindings;
            CompletionToken Ready, Reader;
            uint64_t Version = 0;
        };
        std::shared_ptr<QueueSubmission> Submit;
        Id Device;
        GDX12DeviceResources Resources;
        CanvasRenderer Draw;
        std::vector<Slot> Slots;
        Executor Recorder;
        Id Latest = Invalid;
        size_t Next = 0;
        uint64_t Ordered = 0, SurfaceRevision = 0;
        int Mode = -1;

        Remote(Executor& primary, Id device, UINT count)
            : Submit(primary.Submissions()), Device(device), Draw(Submit->Device(device), count),
              Slots(count), Recorder(Submit, device, count, 1)
        {
            Resources.Device = &Submit->Device(device);
            Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Resources.Device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Texture2D_StartIndex + count * 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            Resources.RTVHeap = std::make_unique<GDX12DescriptorHeap>(Resources.Device,
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count * 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        }

        Id Prepare(int mode, const std::shared_ptr<const CanvasFrame>& input, const std::string& title)
        {
            if (Mode != mode || SurfaceRevision != input->SurfaceRevision) {
                Mode = mode;
                SurfaceRevision = input->SurfaceRevision;
                Latest = Invalid;
                Ordered = 0;
            }
            const Id index = Id(Next++ % Slots.size());
            if (index == Latest) throw std::logic_error("Reusing the previous world canvas publication");
            Recorder.Begin(index);
            auto& slot = Slots.at(index);
            // One extra slot covers consumption in the frame after submission, even with one primary slot.
            if (slot.Reader && !Submit->Complete(slot.Reader))
                throw std::logic_error("World canvas transfer slot still in use by GPU0");
            slot.Draw.Retire();
            slot.Ready = slot.Reader = {};
            slot.Bindings.clear();
            slot.Work.Commands.clear();
            Draw.Prepare(Resources, slot.Draw, input);
            slot.Output.Prepare(*Submit, Device, Native(*slot.Draw.Target)->GetDesc());
            Graph graph;
            graph.Device = Device;
            graph.FrameBoundary = false;
            Draw.Append(graph, slot.Bindings, slot.Work, slot.Draw, title + " remote raster");
            slot.Output.Append(graph, slot.Bindings, slot.Work, title + " export final texture",
                1, *slot.Draw.Target, true, Queue::Direct);
            slot.Plan = Compile(graph);
            Recorder.Reserve(slot.Plan);
            slot.Version = ++Ordered;
            return index;
        }

        void Execute(UINT index)
        {
            auto& slot = Slots.at(index);
            Recorder.Execute(slot.Plan, slot.Bindings,
                [&](Id op, uint32_t, auto* cmd) { slot.Work.Record(op, cmd); });
            slot.Ready = Recorder.Completions().back();
        }
    };

    struct Frame
    {
        DrawFrame Local;
        std::shared_ptr<Remote> Lane;
        GDX12Texture* MaterialTexture = nullptr;
        uint64_t Version = 0;
        Id Order = Invalid, Display = Invalid;
        Id Output = Invalid, Import = Invalid, ImportPacket = Invalid, Consumer = Invalid;
    };

    struct Surface
    {
        explicit Surface(size_t index)
            : Index(index), Definition(BistroUICanvases::Posters[index]), Canvas(Definition.Id, Definition.Title, Definition.Style) {}
        const size_t Index;
        const BistroUICanvases::Poster& Definition;
        WorldCanvas Canvas;
        GDX12Material* Material = nullptr;
        GPUTexture Binding;
        std::unique_ptr<CanvasRenderer> Local;
        std::shared_ptr<Remote> Lane;
        Frame* Current = nullptr;
        int FailedAdapter = -1;
        Id ReportedDevice = Invalid;
        unsigned Reports = 0;
    };

    explicit Impl(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas)
        : Render(render), Scenes(scenes), Atrium(0), Terrace(1), Clouds(2), CloudCanvas(cloudCanvas) {}

    ~Impl()
    {
        if (const auto scenes = Scenes.lock()) {
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
            const auto& poster = surface->Definition;
            if (poster.Style == CanvasStyle::Image && !CloudCanvas) continue;
            if (!surface->Material) {
                surface->Material = Render.GetMaterialByName(poster.Material);
                if (!surface->Material) surface->Material = Render.CreateMaterial(poster.Material);
                if (!surface->Material) throw std::runtime_error("Cannot create Bistro canvas material");
                surface->Binding.Name = poster.Material;
                surface->Material->Diffuse = &surface->Binding;
                surface->Material->Type = MaterialType::WorldCanvas;
                surface->Material->UseBakedLighting = true;
                surface->Material->DirtyFlag = true;
            }
            materials[i] = surface->Material;
        }
        BistroPaintingSurfaces::Install(world, Render, source, materials, entities);
    }


    void Prepare(GDX12DeviceResources& primary, Executor& executor, uint64_t version, double seconds, GDX12Texture* cloudImage)
    {
        Slot = primary.CurrFrameConstantsIndex;
        const auto completed = primary.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
        if (completed == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        if (primary.FrameConstants.at(Slot)->FenceValue > completed)
            throw std::logic_error("Bistro poster slot is still in flight");
        if (Slots.empty()) Slots.resize(primary.FrameConstants.size());
        const auto quality = GetCanvasQuality();
        for (auto* surface : Surfaces())
        {
            auto& frame = Slots.at(Slot)[surface->Index];
            frame.Local.Retire();
            frame.Lane.reset();
            frame.Order = frame.Display = Invalid;
            frame.Output = frame.Import = frame.ImportPacket = frame.Consumer = Invalid;
            surface->Current = nullptr;
            const auto& poster = surface->Definition;
            const bool image = poster.Style == CanvasStyle::Image;
            const bool active = !Worlds.empty() && surface->Material && (!image || (CloudCanvas && cloudImage));
            const auto* oldTarget = frame.MaterialTexture;
            frame.MaterialTexture = active && image ? cloudImage : nullptr;
            if (active && !image) {
                const auto input = surface->Canvas.BuildFrame(poster.Size, seconds, quality);
                if (input->Id != poster.Id) throw std::logic_error("World canvas identity mismatch");
                const auto profile = GetCanvasProfile(input->LogicalSize, input->Quality);
                frame.Local.PrepareTarget(primary, profile.RasterSize);
                const int requested = AdapterVariables[surface->Index]->GetValue();
                if (requested != surface->FailedAdapter) surface->FailedAdapter = -1;
                const Id device = requested >= 0 && requested != surface->FailedAdapter ?
                    executor.Submissions()->SelectAdapter(requested) : 0;
                if (surface->Lane && surface->Lane->Device != device) surface->Lane.reset();
                if (device) {
                    const auto fallback = [&](const auto& error) {
                        executor.Submissions()->CheckHealth();
                        Log::Warning("RenderGraph", "Using local canvas ", poster.Title, ": ", error);
                        surface->FailedAdapter = requested;
                        surface->Lane.reset();
                    };
                    try {
                        if (!surface->Lane) surface->Lane = std::make_shared<Remote>(executor, device, UINT(Slots.size() + 1));
                        auto& lane = *surface->Lane;
                        frame.Order = lane.Prepare(CrossSyncVariables[surface->Index]->GetValue(), input, poster.Title);
                        frame.Display = lane.Mode == 0 && lane.Latest != Invalid ? lane.Latest : frame.Order;
                        lane.Latest = frame.Order;
                        frame.Lane = surface->Lane;
                    } catch (const DxException& e) {
                        if (DeviceLost(e.ErrorCode)) throw;
                        fallback(e.ToString());
                    } catch (const std::exception& e) {
                        fallback(e.what());
                    }
                }
                if (frame.Lane) {
                    frame.Lane->Execute(frame.Order);
                } else {
                    if (!surface->Local) surface->Local = std::make_unique<CanvasRenderer>(*primary.Device, UINT(Slots.size()));
                    surface->Local->Prepare(primary, frame.Local, input);
                }
                frame.MaterialTexture = frame.Local.Target.get();
            } else surface->Lane.reset();
            auto* target = frame.MaterialTexture;
            if (active) {
                frame.Version = version;
                surface->Current = &frame;
            }
            if ((active && oldTarget != target) || (!active && surface->Binding.PrimaryDeviceTexture))
                surface->Material->DirtyFlag = true;
            surface->Binding.PrimaryDeviceTexture = target;
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
    }

    void AppendImport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, Surface& surface)
    {
        auto& frame = *surface.Current;
        auto& display = frame.Lane->Slots.at(frame.Display);
        frame.Import = display.Output.Append(graph, bindings, work, std::string(surface.Definition.Title) + " import final texture",
            0, *frame.Local.Target, false, Queue::Copy);
    }

    std::vector<CanvasRead> Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work)
    {
        std::vector<CanvasRead> reads;
        for (auto* surface : Surfaces()) {
            if (!surface->Current) continue;
            auto& frame = *surface->Current;
            auto& target = *frame.MaterialTexture;
            if (frame.Lane) {
                AppendImport(graph, bindings, work, *surface);
            } else if (surface->Definition.Style != CanvasStyle::Image) {
                surface->Local->Append(graph, bindings, work, frame.Local, std::string(surface->Definition.Title) + " local raster");
            }
            const auto access = Bind(graph, bindings, Native(target), "canvas material texture",
                Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            frame.Output = access.Resource;
            reads.push_back({access, surface->Material->_CBufferIndex, target.GetSRV()->HeapIndex - Texture2D_StartIndex, &target});
        }
        return reads;
    }

    void AddWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
    {
        for (auto* surface : Surfaces()) {
            if (!surface->Current) continue;
            auto& frame = *surface->Current;
            for (Id i = 0; i < schedule.Packets.size(); ++i) {
                const auto& packet = schedule.Packets[i];
                if (frame.Import != Invalid && packet.Operation == frame.Import) frame.ImportPacket = i;
                if (packet.Name == "world-space canvases over G-buffer" &&
                    std::any_of(packet.Accesses.begin(), packet.Accesses.end(), [&](const auto& access) {
                        return access.Resource == frame.Output && access.Usage == Mode::Read;
                    })) frame.Consumer = i;
            }
            if (!frame.Lane) continue;
            if (frame.ImportPacket == Invalid) throw std::logic_error("Missing world canvas import");
            waits.push_back({frame.ImportPacket, frame.Lane->Slots.at(frame.Display).Ready});
        }
    }

    void Submitted(uint64_t version, const std::vector<CompletionToken>& completions)
    {
        for (auto* surface : Surfaces()) {
            if (!surface->Current) continue;
            const auto& frame = *surface->Current;
            const auto imported = frame.ImportPacket < completions.size() ? completions[frame.ImportPacket] : CompletionToken{};
            const auto* order = frame.Lane ? &frame.Lane->Slots.at(frame.Order) : nullptr;
            auto* display = frame.Lane ? &frame.Lane->Slots.at(frame.Display) : nullptr;
            if (display && imported) display->Reader = imported;
            if (frame.Consumer >= completions.size() || !completions[frame.Consumer]) continue;
            const Id device = frame.Lane ? frame.Lane->Device : 0;
            if (surface->ReportedDevice != device) {
                surface->ReportedDevice = device;
                surface->Reports = 0;
            }
            if (order && order->Version == 1) surface->Reports = 0;
            if (surface->Reports >= 3) continue;
            ++surface->Reports;
            Log::Info("RenderGraph", "World canvas frame=", version, " id=", uint64_t(surface->Definition.Id),
                " title=", surface->Definition.Title, " GPU=", Submissions->AdapterLabel(device),
                " material=", surface->Definition.Material, " materialIndex=", surface->Material->_CBufferIndex,
                " textureIndex=", frame.MaterialTexture->GetSRV()->HeapIndex - Texture2D_StartIndex,
                " crossSync=", frame.Lane ? frame.Lane->Mode : -1,
                " ordered=", order ? order->Version : frame.Version,
                " shown=", display ? display->Version : frame.Version,
                " exportFence=", display ? display->Ready.Value : 0, " importFence=", imported.Value,
                " orderedExportFence=", order ? order->Ready.Value : 0,
                " consumer=world-space canvases over G-buffer consumerFence=", completions[frame.Consumer].Value);
        }
    }

    RenderModule& Render;
    std::weak_ptr<SceneManagerModule> Scenes;
    ListenerHandle WorldCreated = 0, WorldDestroyed = 0;
    std::map<World*, std::array<Entity, Count>> Worlds;
    Surface Atrium, Terrace, Clouds;
    bool CloudCanvas;
    UINT Slot = 0;
    std::shared_ptr<QueueSubmission> Submissions;
    std::vector<std::array<Frame, Count>> Slots;
};

BistroUIPosters::BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas)
{
    if (!scenes) throw std::invalid_argument("Bistro posters require a scene manager");
    _impl = std::make_shared<Impl>(render, scenes, cloudCanvas);
    _impl->Subscribe(_impl, *scenes);
}

BistroUIPosters::~BistroUIPosters() = default;

void BistroUIPosters::PrepareFrame(GDX12DeviceResources& primary, Executor& executor, uint64_t frame,
                                   double animationSeconds, GDX12Texture* image)
{
    _impl->Submissions = executor.Submissions();
    _impl->Prepare(primary, executor, frame, animationSeconds, image);
}

std::vector<CanvasRead> BistroUIPosters::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work)
{
    return _impl->Append(graph, bindings, work);
}

void BistroUIPosters::AddWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
    _impl->AddWaits(schedule, waits);
}

void BistroUIPosters::Submitted(uint64_t frame, const std::vector<CompletionToken>& completions)
{
    _impl->Submitted(frame, completions);
}

std::array<BistroRender::Id, BistroUIPosters::Count> BistroUIPosters::ExecutionDevices() const
{
    std::array<BistroRender::Id, Count> devices{};
    for (const auto* surface : _impl->Surfaces())
    {
        const auto* frame = surface->Current;
        devices[surface->Index] = frame ? (frame->Lane ? frame->Lane->Device : 0) : BistroRender::Invalid;
    }
    return devices;
}
