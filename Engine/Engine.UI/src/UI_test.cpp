#include "Engine.UI/UI_test.h"
#include "Engine.UI/OffscreenUI.h"
#include "SettingsCache.h"
#include "Engine.Core/ConsoleModule.h"
#include "Common/RuntimePaths.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>
#include <imgui/backends/imgui_impl_dx12.h>
#include <imgui/backends/imgui_impl_win32.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Engine::UI
{
    namespace
    {
        std::unique_ptr<GDX12DescriptorHeap> Heap;
        std::vector<UINT> FreeDescriptors;
        bool FrameBegun = false;
        bool ScreenFrameReady = false;
        bool ApplicationUIInput = false;
        bool DebugReconstructionStatusVisible = true;
        bool SettingsEditingEnabled = true;
        std::map<std::wstring, std::vector<int>> AvailableLevels;
#ifdef _DEBUG
        bool DebugEnabled = true;
#else
        bool DebugEnabled = false;
#endif
        bool Visible = false;
        bool FocusSettings = false;
        std::string LayoutPath;
        std::vector<std::string> AdapterLabels;
        int PrimaryIndex = -1;

        void DrawPhysicsQuality(ConsoleModule& console)
        {
            constexpr const char* tiers[] = {"Off", "Low", "Medium", "High", "Epic"};
            const auto requested = console.FindConsoleVariable(L"App.Physics.Quality").GetInt();
            const auto effective = console.FindConsoleVariable(L"App.Physics.EffectiveQuality").GetInt();
            ImGui::Text("Physics requested: %s; effective: %s", tiers[std::clamp(requested, 0, 4)],
                effective < 0 ? "Legacy" : tiers[std::clamp(effective, 0, 4)]);
            ImGui::TextWrapped("Low: glasses; Medium: +plates; High: +cutlery; Epic: +furniture and movable props.");
            const bool categorized = console.FindConsoleVariable(L"App.Physics.Categorized").GetBool();
            const bool effectiveCategorized = console.FindConsoleVariable(L"App.Physics.EffectiveCategorized").GetBool();
            const int next = console.FindConsoleVariable(L"App.Physics.SimulationEnabled").GetBool() ? requested : 0;
            if (categorized != effectiveCategorized || (categorized && next != effective))
                ImGui::TextColored(ImVec4(1, .7f, .2f, 1), "Restart Bistro required");
            ImGui::TextWrapped("Physics changes require restarting Bistro. Other techniques keep their settings.");
        }

        struct SettingEditor
        {
            std::wstring Name;
            std::string Draft, Current, Error;

            void Select(const IConsoleVariable& variable)
            {
                Name = variable.GetName();
                Draft = Current = variable.GetString();
                Error.clear();
            }

            void Apply(IConsoleVariable& variable, std::string_view value)
            {
                if (!variable.IsValidString(value))
                    Error = "Invalid value. Check the limits above.";
                else if (!variable.SetConfigFromString(value))
                    Error = "Could not save the value. See the engine log.";
                else
                    Select(variable);
            }

            void DrawAdapter(IConsoleVariable& variable)
            {
                const int requested = variable.GetInt();
                const auto local = "Primary GPU (local): " + AdapterLabels.at(PrimaryIndex);
                const bool listed = requested >= 0 && static_cast<size_t>(requested) < AdapterLabels.size();
                const auto preview = requested == -1 ? local : listed ? AdapterLabels[requested]
                    : "Unavailable GPU [" + std::to_string(requested) + "]";
                ImGui::TextUnformatted("Requested GPU");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##adapter", preview.c_str()))
                {
                    const auto option = [&](int index, const std::string& label)
                    {
                        if (ImGui::Selectable(label.c_str(), index == requested) && index != requested)
                            Apply(variable, std::to_string(index));
                        if (index == requested) ImGui::SetItemDefaultFocus();
                    };
                    option(-1, local);
                    for (int i = 0; i < static_cast<int>(AdapterLabels.size()); ++i)
                        option(i, i == PrimaryIndex ? AdapterLabels[i] + " (primary GPU)" : AdapterLabels[i]);
                    ImGui::EndCombo();
                }
                if (requested != -1 && !listed)
                    ImGui::TextWrapped("This index is missing from the renderer hardware list.");
                ImGui::TextWrapped("Configured value: %s. -1 uses the current primary GPU. "
                    "0, 1, ... are absolute indices in the renderer's filtered hardware list.", Current.c_str());
            }

            void Draw(IConsoleVariable& variable)
            {
                const auto name = std::filesystem::path(Name).u8string();
                const auto description = std::filesystem::path(variable.GetDescription()).u8string();
                const bool worldCanvas = Name.rfind(L"App.UI.WorldCanvas.", 0) == 0;
                const bool canvasAdapter = worldCanvas && Name.substr(Name.find_last_of(L'.') + 1) == L"Adapter";
                const auto active = variable.GetString();
                if (Draft == Current) Draft = active;
                Current = active;

                ImGui::PushID(name.c_str());
                ImGui::TextWrapped("%s", canvasAdapter ? description.c_str() : name.c_str());
                if (!description.empty() && !canvasAdapter) ImGui::TextWrapped("%s", description.c_str());
                ImGui::Spacing();
                if (variable.GetFlags() == EConsoleVariableFlags::ReadOnly)
                {
                    ImGui::TextDisabled("Locked during this run");
                    ImGui::TextWrapped("Current value: %s", Current.c_str());
                    if (Name == L"Render.PrimaryAdapter")
                    {
                        ImGui::TextWrapped("Primary GPU: %s", AdapterLabels.at(PrimaryIndex).c_str());
                        ImGui::TextWrapped("Startup hardware index (0, 1, ...). -1 is only valid for technique settings.");
                    }
                }
                else if (Name.substr(Name.find_last_of(L'.') + 1) == L"Adapter")
                    DrawAdapter(variable);
                else if (worldCanvas && Name.substr(Name.find_last_of(L'.') + 1) == L"CrossSync")
                {
                    const int mode = variable.GetInt();
                    const char* labels[] = {"Previous frame (N-1)", "Current frame (N)"};
                    if (ImGui::BeginCombo("##cross-sync", labels[mode == 0 ? 0 : 1]))
                    {
                        for (int i = 0; i < 2; ++i)
                            if (ImGui::Selectable(labels[i], i == mode) && i != mode)
                                Apply(variable, std::to_string(i));
                        ImGui::EndCombo();
                    }
                }
                else if (variable.GetType() == EConsoleVariableType::Boolean)
                {
                    bool enabled = Current == "true";
                    if (ImGui::Checkbox("Enabled", &enabled)) Apply(variable, enabled ? "true" : "false");
                }
                else if (const auto levels = AvailableLevels.find(Name);
                         levels != AvailableLevels.end() || (variable.GetType() == EConsoleVariableType::Enumeration && variable.GetEnumCount() > 0))
                {
                    const int activeValue = variable.GetInt();
                    const auto label = [&](int v)
                    {
                        return variable.GetEnumCount() ? std::string(variable.GetEnumValueName(v)) : std::to_string(v);
                    };
                    if (levels != AvailableLevels.end() && !levels->second.empty())
                        ImGui::Text("VRAM budget allows up to %s", label(levels->second.back()).c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##value", label(activeValue).c_str()))
                    {
                        const auto option = [&](int value)
                        {
                            ImGui::PushID(value);
                            if (ImGui::Selectable(label(value).c_str(), value == activeValue))
                                Apply(variable, std::to_string(value));
                            if (value == activeValue) ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        };
                        if (levels != AvailableLevels.end())
                            for (int level : levels->second) option(level);
                        else
                            for (int i = 0; i < variable.GetEnumCount(); ++i) option(variable.GetEnumFirstValue() + i);
                        ImGui::EndCombo();
                    }
                }
                else
                {
                    ImGui::SetNextItemWidth(-1);
                    const auto format = variable.GetType() == EConsoleVariableType::FloatingPoint
                        ? ImGuiInputTextFlags_CharsScientific : ImGuiInputTextFlags_CharsDecimal;
                    const bool enter = ImGui::InputText("##value", &Draft,
                        format | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsItemEdited()) Error.clear();
                    ImGui::BeginDisabled(Draft == Current);
                    const bool apply = ImGui::Button("Apply");
                    ImGui::SameLine();
                    const bool reset = ImGui::Button("Revert edit");
                    ImGui::EndDisabled();
                    if (reset) Select(variable);
                    else if ((enter || apply) && Draft != Current) Apply(variable, Draft);
                }
                if (!Error.empty())
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", Error.c_str());
                ImGui::PopID();
            }
        };

        void DrawSettings()
        {
            if (!Visible) return;
            if (FocusSettings) { ImGui::SetNextWindowFocus(); FocusSettings = false; }
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(720, 580), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings (F1)", &Visible))
            {
                ImGui::TextWrapped("For changes across Bistro launches, edit Configs/Startup.ini.");
                ConsoleModule console;
                IConsoleVariable* inspector = nullptr;
                if (console.TryFindConsoleVariable(L"Render.Debug.SceneTextures", inspector))
                {
                    bool enabled = inspector->GetBool();
                    ImGui::BeginDisabled(!SettingsEditingEnabled);
                    if (ImGui::Checkbox("Scene textures", &enabled))
                        inspector->SetFromString(enabled ? "true" : "false");
                    ImGui::EndDisabled();
                }
                static ImGuiTextFilter filter;
                filter.Draw("Search", -90);
                static SettingsCache settings;
                static SettingEditor editor;
                uint64_t revision;
                const auto variables = console.GetConsoleVariables({}, &revision);
                settings.Update(variables, revision, filter);

                const auto drawGroup = [&](auto&& self, const SettingsCache::Group& group) -> void
                {
                    for (const auto& setting : group.Variables)
                    {
                        if (console.GetConsoleVariableRevision() != revision) break;
                        auto& variable = *variables.at(setting.VariableIndex);
                        ImGui::PushID(setting.Name.c_str());
                        if (ImGui::Selectable(setting.Label.c_str(), variable.GetName() == editor.Name))
                            editor.Select(variable);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", setting.Name.c_str());
                        ImGui::PopID();
                    }
                    for (const auto& [name, child] : group.Children)
                    {
                        if (filter.IsActive()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                        {
                            self(self, child);
                            ImGui::TreePop();
                        }
                    }
                };
                if (ImGui::BeginChild("Variables", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, 0),
                    ImGuiChildFlags_Borders))
                {
                    ImGui::PushID(filter.IsActive() ? "FilteredSettings" : "SettingsTree");
                    const auto& root = settings.Root();
                    if (root.Children.empty() && root.Variables.empty()) ImGui::TextDisabled("No matching settings");
                    drawGroup(drawGroup, root);
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                if (ImGui::BeginChild("Editor", ImVec2(0, 0), ImGuiChildFlags_Borders))
                {
                    IConsoleVariable* variable = nullptr;
                    if (!editor.Name.empty() && console.TryFindConsoleVariable(editor.Name, variable))
                    {
                        ImGui::BeginDisabled(!SettingsEditingEnabled);
                        editor.Draw(*variable);
                        ImGui::EndDisabled();
                        if (editor.Name == L"App.Physics.Quality") DrawPhysicsQuality(console);
                    }
                    else
                        ImGui::TextDisabled("Select a setting to edit");
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }
        ImDrawData* FinalizeScreenFrame()
        {
            if (!ImGui::GetCurrentContext()) return nullptr;
            if (FrameBegun)
            {
                ImGui::Render();
                FrameBegun = false;
                ScreenFrameReady = true;
            }
            return ScreenFrameReady ? ImGui::GetDrawData() : nullptr;
        }

        class ImGuiContextScope
        {
        public:
            explicit ImGuiContextScope(ImGuiContext* context) : Previous(ImGui::GetCurrentContext())
            { ImGui::SetCurrentContext(context); }
            ~ImGuiContextScope() { ImGui::SetCurrentContext(Previous); }
            ImGuiContextScope(const ImGuiContextScope&) = delete;
            ImGuiContextScope& operator=(const ImGuiContextScope&) = delete;
        private:
            ImGuiContext* Previous;
        };

        struct DrawLists
        {
            explicit DrawLists(const ImDrawData& source)
            {
                Data.Valid = source.Valid;
                Data.DisplayPos = source.DisplayPos;
                Data.DisplaySize = source.DisplaySize;
                Data.FramebufferScale = source.FramebufferScale;
                Data.TotalIdxCount = source.TotalIdxCount;
                Data.TotalVtxCount = source.TotalVtxCount;
                for (const auto* list : source.CmdLists)
                {
                    Owned.emplace_back(list->CloneOutput());
                    Data.CmdLists.push_back(Owned.back().get());
                }
                Data.CmdListsCount = Data.CmdLists.Size;
            }

            struct Deleter { void operator()(ImDrawList* list) const { IM_DELETE(list); } };
            std::vector<std::unique_ptr<ImDrawList, Deleter>> Owned;
            ImDrawData Data;
        };

        struct AtlasSnapshot
        {
            UINT Width = 0;
            UINT Height = 0;
            std::vector<unsigned char> Pixels;
        };
        struct DrawTexture
        {
            UITextureId Id = UITextureId::Invalid;
            std::shared_ptr<const AtlasSnapshot> Atlas;
        };
        struct AtlasCapture
        {
            const ImTextureData* Identity;
            int UniqueID;
            uint64_t Generation;
            std::shared_ptr<const AtlasSnapshot> Snapshot;
        };
        std::vector<AtlasCapture> ScreenAtlasCaptures;
        uint64_t ScreenAtlasGeneration = 0;
        int ScreenAtlasFrame = -1;

        struct AtlasUpload
        {
            GDX12ResourceHandle Texture;
            GDX12ResourceHandle Staging;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        };

        AtlasUpload PrepareAtlas(GDX12Device& device, const AtlasSnapshot& source)
        {
            AtlasUpload upload;
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, source.Width, source.Height, 1, 1);
            upload.Texture = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
            UINT64 uploadBytes = 0;
            device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &upload.Footprint, nullptr, nullptr, &uploadBytes);
            upload.Staging = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(uploadBytes),
                D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            const D3D12_RANGE noRead{0, 0};
            ThrowIfFailed(upload.Staging->Map(0, &noRead, &mapped));
            const auto rowBytes = static_cast<size_t>(source.Width) * 4;
            for (UINT row = 0; row < source.Height; ++row)
                std::memcpy(static_cast<unsigned char*>(mapped) + upload.Footprint.Offset +
                    static_cast<size_t>(row) * upload.Footprint.Footprint.RowPitch,
                    source.Pixels.data() + static_cast<size_t>(row) * rowBytes, rowBytes);
            upload.Staging->Unmap(0, nullptr);
            return upload;
        }
    }

    class DrawData
    {
    public:
        explicit DrawData(const ImDrawData& source) : Lists(source) {}
        DrawLists Lists;
        std::vector<DrawTexture> Textures;
    };

    static std::shared_ptr<const DrawData> CaptureData(const ImDrawData& source, bool reuseScreenAtlases)
    {
        if (!source.Valid) throw std::invalid_argument("UI capture requires finalized ImGui draw data");
        if (!std::isfinite(source.DisplaySize.x) || !std::isfinite(source.DisplaySize.y) ||
            !std::isfinite(source.DisplayPos.x) || !std::isfinite(source.DisplayPos.y))
            throw std::invalid_argument("UI capture requires a finite logical viewport");
        if (source.DisplaySize.x <= 0 || source.DisplaySize.y <= 0) return {};

        auto frame = std::make_shared<DrawData>(source);
        std::vector<AtlasCapture> capturedAtlases;
        std::unordered_map<const ImTextureData*, ImTextureID> managed;
        std::unordered_map<ImTextureID, ImTextureID> external;
        for (auto* list : frame->Lists.Data.CmdLists)
        {
            for (auto& command : list->CmdBuffer)
            {
                if (command.UserCallback)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState &&
                        (!ImGui::GetCurrentContext() || command.UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState))
                        throw std::invalid_argument("UI capture cannot defer custom ImGui callbacks");
                    command.TexRef = {};
                    command.UserCallbackData = nullptr;
                    continue;
                }

                const auto next = static_cast<ImTextureID>(frame->Textures.size() + 1);
                if (const auto* tex = command.TexRef._TexData)
                {
                    const auto [entry, inserted] = managed.emplace(tex, next);
                    if (inserted)
                    {
                        if (!tex->Pixels || tex->Format != ImTextureFormat_RGBA32 || tex->BytesPerPixel != 4 ||
                            tex->Width <= 0 || tex->Height <= 0 ||
                            tex->Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                            tex->Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
                            throw std::invalid_argument("UI capture requires live RGBA8 ImGui texture pixels");
                        const auto cached = std::find_if(ScreenAtlasCaptures.begin(), ScreenAtlasCaptures.end(),
                            [&](const auto& atlas) { return reuseScreenAtlases && atlas.Identity == tex &&
                                atlas.UniqueID == tex->UniqueID && atlas.Generation == ScreenAtlasGeneration &&
                                atlas.Snapshot->Width == tex->Width && atlas.Snapshot->Height == tex->Height; });
                        const auto snapshot = cached != ScreenAtlasCaptures.end() ? cached->Snapshot :
                            std::make_shared<const AtlasSnapshot>(AtlasSnapshot{static_cast<UINT>(tex->Width),
                                static_cast<UINT>(tex->Height), {tex->Pixels, tex->Pixels + tex->GetSizeInBytes()}});
                        frame->Textures.push_back({UITextureId::Invalid, snapshot});
                        if (reuseScreenAtlases)
                            capturedAtlases.push_back({tex, tex->UniqueID, ScreenAtlasGeneration, snapshot});
                    }
                    command.TexRef = ImTextureRef(entry->second);
                }
                else
                {
                    const auto id = command.TexRef._TexID;
                    if (id == ImTextureID_Invalid) throw std::invalid_argument("UI draw has no texture identity");
                    const auto [entry, inserted] = external.emplace(id, next);
                    if (inserted) frame->Textures.push_back({static_cast<UITextureId>(id)});
                    command.TexRef = ImTextureRef(entry->second);
                }
            }
        }
        if (reuseScreenAtlases) ScreenAtlasCaptures = std::move(capturedAtlases);
        return frame;
    }

    std::shared_ptr<const DrawData> CaptureDrawData(const ImDrawData& source)
    {
        return CaptureData(source, false);
    }

    struct OffscreenRenderer::Impl
    {
        static constexpr unsigned TexturesPerSlot = 64;

        struct CachedAtlas
        {
            std::shared_ptr<const AtlasSnapshot> Source;
            GDX12ResourceHandle Texture;
        };

        struct Recording
        {
            std::shared_ptr<Impl> Renderer;
            std::shared_ptr<const DrawData> Frame;
            GDX12ResourceHandle Target;
            std::vector<AtlasUpload> Uploads;
            std::vector<GDX12ResourceHandle> Textures;
        };

        Impl(GDX12Device& device, unsigned slots, bool reuseImmutableAtlases)
            : Device(device), Native(device.GetDevice()),
              OwnerThread(std::this_thread::get_id()),
              SrvHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots * TexturesPerSlot,
                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
              RtvHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, slots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE),
              Slots(slots), ReuseImmutableAtlases(reuseImmutableAtlases)
        {
            ThrowIfFailed(Native.As(&DeviceIdentity));
        }

        void Initialize()
        {
            ImGuiContextScope scope(nullptr);
            Context = ImGui::CreateContext();
            ImGui_ImplDX12_InitInfo info;
            info.Device = Native.Get();
            info.CommandQueue = Device.GetCommandQueue()->GetCommandQueue().Get();
            info.NumFramesInFlight = static_cast<int>(Slots.size());
            info.RTVFormat = OffscreenFormat;
            info.SrvDescriptorHeap = SrvHeap.GetHeap().Get();
            info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE*, D3D12_GPU_DESCRIPTOR_HANDLE*)
            { throw std::logic_error("Offscreen renderer manages its own texture bindings"); };
            info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
            if (!ImGui_ImplDX12_Init(&info) || !ImGui_ImplDX12_CreateDeviceObjects())
                throw std::runtime_error("Cannot initialize offscreen ImGui DX12 backend");
        }

        ~Impl()
        {
            assert(std::this_thread::get_id() == OwnerThread);
            if (!Context) return;
            ImGuiContextScope scope(Context);
            if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext(Context);
        }

        void CheckDevice(ID3D12DeviceChild& object) const
        {
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(object.GetDevice(IID_PPV_ARGS(&device)));
            ComPtr<IUnknown> identity;
            ThrowIfFailed(device.As(&identity));
            if (identity.Get() != DeviceIdentity.Get())
                throw std::invalid_argument("Offscreen UI requires resources and commands on its own device");
        }

        GDX12Device& Device;
        ComPtr<ID3D12Device14> Native;
        ComPtr<IUnknown> DeviceIdentity;
        std::thread::id OwnerThread;
        GDX12DescriptorHeap SrvHeap;
        GDX12DescriptorHeap RtvHeap;
        ImGuiContext* Context = nullptr;
        std::vector<std::weak_ptr<Recording>> Slots;
        bool ReuseImmutableAtlases;
        std::vector<std::shared_ptr<const CachedAtlas>> Atlases;
        // Match the pinned backend's unsigned FrameIndex, including wraparound.
        UINT NextFrame = 0;
    };

    OffscreenRenderer::OffscreenRenderer(GDX12Device& device, unsigned framesInFlight, bool reuseImmutableAtlases)
    {
        if (!device.IsInitialized() || !device.GetDevice() || !device.GetCommandQueue() || !framesInFlight ||
            framesInFlight > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / Impl::TexturesPerSlot)
            throw std::invalid_argument("Offscreen UI requires a live device and a valid frame ring size");
        _impl = std::make_shared<Impl>(device, framesInFlight, reuseImmutableAtlases);
        _impl->Initialize();
    }

    OffscreenRenderer::~OffscreenRenderer() = default;

    std::shared_ptr<const void> OffscreenRenderer::TryRecord(GDX12CommandList& commands, GDX12ResourceHandle target,
        std::shared_ptr<const DrawData> frame, const std::vector<TextureBinding>& textures, bool clearTarget)
    {
        auto& state = *_impl;
        if (std::this_thread::get_id() != state.OwnerThread)
            throw std::logic_error("Offscreen UI recording must run on the ImGui owner thread");
        if (state.Device.GetDevice().Get() != state.Native.Get())
            throw std::logic_error("Offscreen UI renderer belongs to a retired device incarnation");
        if (commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !frame || !target)
            throw std::invalid_argument("Offscreen UI requires a DIRECT list, target, and draw data");
        const auto slot = static_cast<UINT>(state.NextFrame % state.Slots.size());
        if (!state.Slots[slot].expired()) return {};
        if (frame->Textures.size() > Impl::TexturesPerSlot)
            throw std::length_error("Offscreen UI exceeded its per-record texture descriptor range");

        auto* list = commands.GetCommandList().Get();
        state.CheckDevice(*list);
        state.CheckDevice(*target.Get());
        const auto targetDesc = target->GetDesc();
        if (targetDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || targetDesc.Format != OffscreenFormat ||
            targetDesc.SampleDesc.Count != 1 || targetDesc.DepthOrArraySize != 1 || targetDesc.MipLevels != 1 ||
            !(targetDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
            throw std::invalid_argument("Offscreen UI target must be a single-sample RGBA8 UNORM render texture");

        auto recording = std::make_shared<Impl::Recording>();
        recording->Renderer = _impl;
        recording->Frame = std::move(frame);
        recording->Target = std::move(target);
        const auto& captured = *recording->Frame;
        std::unordered_map<UITextureId, const TextureBinding*> bindings;
        for (const auto& binding : textures)
            if (binding.Id == UITextureId::Invalid || !bindings.emplace(binding.Id, &binding).second)
                throw std::invalid_argument("Offscreen UI texture IDs must be nonzero and unique");

        const UINT base = slot * Impl::TexturesPerSlot;
        std::vector<std::shared_ptr<const Impl::CachedAtlas>> atlases;
        for (UINT index = 0; index < captured.Textures.size(); ++index)
        {
            const auto& source = captured.Textures[index];
            ID3D12Resource* resource = nullptr;
            DXGI_FORMAT format = OffscreenFormat;
            if (source.Id == UITextureId::Invalid)
            {
                const auto cached = std::find_if(state.Atlases.begin(), state.Atlases.end(), [&](const auto& atlas) {
                    return atlas->Source == source.Atlas ||
                        (atlas->Source->Width == source.Atlas->Width && atlas->Source->Height == source.Atlas->Height &&
                         atlas->Source->Pixels == source.Atlas->Pixels);
                });
                if (cached != state.Atlases.end())
                {
                    atlases.push_back(*cached);
                    resource = (*cached)->Texture.Get();
                    recording->Textures.push_back((*cached)->Texture);
                }
                else
                {
                    auto& upload = recording->Uploads.emplace_back(PrepareAtlas(state.Device, *source.Atlas));
                    resource = upload.Texture.Get();
                    if (state.ReuseImmutableAtlases)
                        atlases.push_back(std::make_shared<Impl::CachedAtlas>(Impl::CachedAtlas{source.Atlas, upload.Texture}));
                }
            }
            else
            {
                const auto it = bindings.find(source.Id);
                if (it == bindings.end() || !it->second->Resource)
                    throw std::invalid_argument("Offscreen UI texture has no local binding");
                const auto& binding = *it->second;
                resource = binding.Resource.Get();
                state.CheckDevice(*resource);
                const auto desc = resource->GetDesc();
                format = binding.ViewFormat;
                if ((format != OffscreenFormat && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
                    (desc.Format != format && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
                    desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
                    desc.DepthOrArraySize != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                    resource == recording->Target.Get())
                    throw std::invalid_argument("Offscreen UI texture requires a local RGBA8 2D SRV");
                recording->Textures.push_back(binding.Resource);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = resource->GetDesc().MipLevels;
            state.Native->CreateShaderResourceView(resource, &srv, state.SrvHeap.GetCPUHandle(base + index));
        }

        DrawLists playback(captured.Lists.Data);
        ImGuiContextScope scope(state.Context);
        playback.Data.FramebufferScale = {static_cast<float>(targetDesc.Width) / playback.Data.DisplaySize.x,
            static_cast<float>(targetDesc.Height) / playback.Data.DisplaySize.y};
        for (auto* drawList : playback.Data.CmdLists)
            for (auto& command : drawList->CmdBuffer)
                if (command.UserCallback)
                    command.UserCallback = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
                else
                {
                    const auto index = static_cast<UINT>(command.TexRef._TexID - 1);
                    command.TexRef = ImTextureRef(state.SrvHeap.GetGPUHandle(base + index).ptr);
                }

        playback.Data.OwnerViewport = ImGui::GetMainViewport();
        for (const auto& upload : recording->Uploads)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION dest(upload.Texture.Get(), 0u);
            const CD3DX12_TEXTURE_COPY_LOCATION src(upload.Staging.Get(), upload.Footprint);
            list->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
            commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(upload.Texture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)});
        }
        const auto rtv = state.RtvHeap.GetCPUHandle(slot);
        state.Native->CreateRenderTargetView(recording->Target.Get(), nullptr, rtv);
        if (clearTarget)
        {
            const float clear[4]{};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        }
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {state.SrvHeap.GetHeap().Get()};
        list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(&playback.Data, list);
        commands.InvalidateCachedState();
        if (state.ReuseImmutableAtlases) state.Atlases = std::move(atlases);
        state.Slots[slot] = recording;
        ++state.NextFrame;
        return recording;
    }

    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format,
        const std::vector<DeviceDesc>& adapters)
    {
        const auto primary = device->GetDevice()->GetAdapterLuid();
        AdapterLabels.clear();
        PrimaryIndex = -1;
        for (const auto& adapter : adapters)
        {
            const auto index = AdapterLabels.size();
            if (adapter.AdapterLuid.HighPart == primary.HighPart && adapter.AdapterLuid.LowPart == primary.LowPart)
                PrimaryIndex = static_cast<int>(index);
            AdapterLabels.push_back("[" + std::to_string(index) + "] " + std::filesystem::path(adapter.Name).u8string());
        }
        IM_ASSERT(PrimaryIndex >= 0);
        Heap = std::make_unique<GDX12DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        FreeDescriptors.clear();
        for (UINT index = 0; index < Heap->GetNumDescriptors(); ++index) FreeDescriptors.push_back(index);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        if (DebugEnabled)
        {
            const auto configDir = Engine::Core::GetExecutableDirectory() / L"Config";
            std::error_code layoutError;
            std::filesystem::create_directories(configDir, layoutError);
            const auto layoutPath = (configDir / L"ImGui.ini").u8string();
            LayoutPath.assign(reinterpret_cast<const char*>(layoutPath.data()), layoutPath.size());
            io.IniFilename = layoutError ? nullptr : LayoutPath.c_str();
        }
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();
        Visible = DebugEnabled;
        try
        {
            if (!ImGui_ImplWin32_Init(window)) throw std::runtime_error("Cannot initialize ImGui Win32 backend");
            ImGui_ImplDX12_InitInfo info;
            info.Device = device->GetDevice().Get();
            info.CommandQueue = device->GetCommandQueue()->GetCommandQueue().Get();
            info.NumFramesInFlight = framesInFlight;
            info.RTVFormat = format;
            info.SrvDescriptorHeap = Heap->GetHeap().Get();
            info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
            {
                if (FreeDescriptors.empty()) throw std::runtime_error("ImGui descriptor heap exhausted");
                const auto index = FreeDescriptors.back();
                FreeDescriptors.pop_back();
                *cpu = Heap->GetCPUHandle(index);
                *gpu = Heap->GetGPUHandle(index);
            };
            info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
            {
                const auto increment = Heap->GetCPUHandle(1).ptr - Heap->GetCPUHandle(0).ptr;
                FreeDescriptors.push_back(static_cast<UINT>((cpu.ptr - Heap->GetCPUHandle(0).ptr) / increment));
            };
            if (!ImGui_ImplDX12_Init(&info)) throw std::runtime_error("Cannot initialize ImGui DX12 backend");
        }
        catch (...) { ShutdownUI(); throw; }
    }






    void BeginFrame()
    {
        if (!ImGui::GetCurrentContext()) return;
        if (FrameBegun) ImGui::EndFrame();
        ScreenFrameReady = false;
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        FrameBegun = true;
        if (DebugEnabled && SettingsEditingEnabled)
        {
            auto dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;
            if (!Visible) dockFlags |= ImGuiDockNodeFlags_KeepAliveOnly;
            ImGui::DockSpaceOverViewport(0, nullptr, dockFlags);
        }
        DrawSettings();
    }



    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color)
    {
        if (!DebugUIVisible()) return;
        DirectX::XMFLOAT3 corners[8];
        box.GetCorners(corners);
        DirectX::XMFLOAT4 clip[8];
        const auto matrix = DirectX::XMLoadFloat4x4(&viewProjection);
        for (unsigned i = 0; i < 8; ++i)
            DirectX::XMStoreFloat4(&clip[i], DirectX::XMVector4Transform(
                DirectX::XMVectorSet(corners[i].x, corners[i].y, corners[i].z, 1), matrix));
        const auto size = ImGui::GetIO().DisplaySize;
        const auto screen = [size](const DirectX::XMFLOAT4& p)
        { return ImVec2((p.x / p.w + 1) * size.x * 0.5f, (1 - p.y / p.w) * size.y * 0.5f); };
        const auto distance = [](const DirectX::XMFLOAT4& p, unsigned plane)
        {
            switch (plane)
            {
            case 0: return p.w + p.x;
            case 1: return p.w - p.x;
            case 2: return p.w + p.y;
            case 3: return p.w - p.y;
            case 4: return p.z;
            default: return p.w - p.z;
            }
        };
        constexpr unsigned edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        auto* draw = ImGui::GetBackgroundDrawList();
        for (const auto& edge : edges)
        {
            auto a = clip[edge[0]], b = clip[edge[1]];
            bool visible = true;
            for (unsigned plane = 0; plane < 6; ++plane)
            {
                const float da = distance(a, plane), db = distance(b, plane);
                if (da < 0 && db < 0) { visible = false; break; }
                if (da < 0 || db < 0)
                {
                    const float t = da / (da - db);
                    const DirectX::XMFLOAT4 point(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
                        a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t);
                    if (da < 0) a = point; else b = point;
                }
            }
            if (visible && a.w > 0.00001f && b.w > 0.00001f)
                draw->AddLine(screen(a), screen(b), color, 2);
        }
        const auto centre = DirectX::XMVector4Transform(
            DirectX::XMVectorSet(box.Center.x, box.Center.y, box.Center.z, 1), matrix);
        DirectX::XMFLOAT4 point;
        DirectX::XMStoreFloat4(&point, centre);
        bool visible = point.w > 0.00001f;
        for (unsigned plane = 0; plane < 6; ++plane) visible &= distance(point, plane) >= 0;
        if (visible) draw->AddText(screen(point), color, label);
    }

    void Render(GDX12CommandList* commands, GDX12Texture* target)
    {
        auto* drawData = FinalizeScreenFrame();
        if (!drawData) return;
        ScreenFrameReady = false;
        commands->ResourceBarrier({target->GetResource()->GetRenderTargetBarrier()});
        commands->SetRenderTargets({target}, nullptr);
        commands->SetDescriptorHeaps({Heap.get()});
        ImGui_ImplDX12_RenderDrawData(drawData, commands->GetCommandList().Get());
        commands->InvalidateCachedState();
        commands->ResourceBarrier({target->GetResource()->GetPresentBarrier()});
    }

    std::shared_ptr<const DrawData> CaptureScreenFrame()
    {
        const auto* data = FinalizeScreenFrame();
        if (!data) return {};
        const int frame = ImGui::GetFrameCount();
        // ImGui's dynamic atlas marks real pixel writes dirty, including pending WantCreate textures.
        if (ScreenAtlasFrame != frame && !ImGui::GetIO().Fonts->TexIsBuilt) ++ScreenAtlasGeneration;
        ScreenAtlasFrame = frame;
        return data->TotalVtxCount ? CaptureData(*data, true) : nullptr;
    }

    std::shared_ptr<const DrawData> CaptureScreenOverlay(ImDrawList& overlay)
    {
        const auto* frame = ImGui::GetDrawData();
        if (!frame || overlay.VtxBuffer.empty()) return {};
        ImDrawData data;
        data.Valid = true;
        data.DisplayPos = frame->DisplayPos;
        data.DisplaySize = frame->DisplaySize;
        data.FramebufferScale = frame->FramebufferScale;
        data.OwnerViewport = frame->OwnerViewport;
        data.AddDrawList(&overlay);
        return CaptureData(data, true);
    }

    void DrawReconstructionStatus(const GDX12ReconstructionStatus& status, unsigned renderWidth, unsigned renderHeight,
        unsigned outputWidth, unsigned outputHeight)
    {
        if (!DebugReconstructionStatusVisible || !DebugUIVisible()) return;
        ConsoleModule console;
        const auto& provider = console.FindConsoleVariable(L"Render.Upscaler.Provider");
        const auto& quality = console.FindConsoleVariable(L"Render.Upscaler.Quality");
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 16, viewport->WorkPos.y + 16),
            ImGuiCond_FirstUseEver, ImVec2(1, 0));
        if (ImGui::Begin("Reconstruction"))
        {
            const std::string requested(provider.GetEnumValueName(static_cast<int>(status.Requested.Provider)));
            const std::string effective(provider.GetEnumValueName(static_cast<int>(status.Effective)));
            const std::string preset(quality.GetEnumValueName(static_cast<int>(status.Requested.Quality)));
            ImGui::Text("Requested: %s / %s", requested.c_str(), preset.c_str());
            ImGui::Text("Effective: %s", effective.c_str());
            ImGui::Text("Render: %ux%u | Output: %ux%u", renderWidth, renderHeight, outputWidth, outputHeight);
            if (!status.FallbackReason.empty()) ImGui::TextWrapped("Fallback: %s", status.FallbackReason.c_str());
        }
        ImGui::End();
    }

    void ShutdownUI()
    {
        AvailableLevels.clear();
        ScreenAtlasCaptures.clear();
        ScreenAtlasGeneration = 0;
        ScreenAtlasFrame = -1;
        if (!ImGui::GetCurrentContext()) return;
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        FrameBegun = false;
        ScreenFrameReady = false;
        Heap.reset();
        FreeDescriptors.clear();
    }

    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if ((!DebugEnabled && !ApplicationUIInput) || !ImGui::GetCurrentContext()) return false;
        const bool handled = ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
        if (message == WM_KEYUP && wParam == VK_F1) { Visible = !Visible; return true; }
        return handled || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && WantsMouseInput()) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST && WantsKeyboardInput());
    }

    bool WantsKeyboardInput()
    {
        return (ApplicationUIInput || (DebugEnabled && Visible)) && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
    }

    bool WantsMouseInput()
    {
        return (ApplicationUIInput || (DebugEnabled && Visible)) && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    }

    void EnableApplicationUIInput() { ApplicationUIInput = true; }
    void SetDebugReconstructionStatusVisible(bool visible) { DebugReconstructionStatusVisible = visible; }
    void SetAvailableSettingLevels(std::map<std::wstring, std::vector<int>> levels)
    {
        AvailableLevels = std::move(levels);
    }
    void SetSettingsEditingEnabled(bool enabled) { SettingsEditingEnabled = enabled; }
    void HideSettings() { Visible = FocusSettings = false; }
    void OpenSettings() { Visible = FocusSettings = true; }
    void EnableDebugUI() { DebugEnabled = true; }
    bool DebugUIVisible() { return SettingsEditingEnabled && DebugEnabled && FrameBegun && Visible; }
}
