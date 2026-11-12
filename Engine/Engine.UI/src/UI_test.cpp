#include "Engine.UI/UI_test.h"
#include "Engine.UI/OffscreenUI.h"
#include "SettingsCache.h"
#include "Engine.Core/ConsoleModule.h"
#include "Common/RuntimePaths.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>
#include <imgui/backends/imgui_impl_dx12.h>
#include <imgui/backends/imgui_impl_win32.h>
#include <charconv>
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
        bool Visible = true;
        bool FrameBegun = false;
        bool ScreenFrameReady = false;
        std::string InvalidVariable;
#ifdef _DEBUG
        std::string LayoutPath;
#endif

        template <class T>
        bool InputNumber(const char* label, ImGuiDataType type, std::string& value)
        {
            T number{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                throw std::runtime_error("Cannot parse console variable for UI");
            const char* format = std::is_floating_point_v<T> ? "%.9g" : nullptr;
            if (!ImGui::InputScalar(label, type, &number, nullptr, nullptr, format))
                return false;
            char buffer[128];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
            if (result.ec != std::errc{}) throw std::runtime_error("Cannot format console variable from UI");
            value.assign(buffer, result.ptr);
            return true;
        }

        void DrawSettings()
        {
            if (!Visible) return;
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(540, 650), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings (F1)", &Visible))
            {
                ImGui::TextWrapped("Changes are saved to Config/Engine.ini automatically. Hover a setting for its description and limits.");
                ImGui::TextWrapped("Readonly settings apply after restart. Other changes apply live. F1 hides/shows the debug panels.");
                static ImGuiTextFilter filter;
                filter.Draw("Search", -90);
                static SettingsCache settings;
                ConsoleModule console;
                uint64_t revision;
                const auto variables = console.GetConsoleVariables({}, &revision);
                settings.Update(variables, revision, filter);
                const auto drawGroup = [&](auto&& self, const SettingsCache::Group& group) -> void
                {
                    for (const auto& setting : group.Variables)
                    {
                        if (console.GetConsoleVariableRevision() != revision) break;
                        auto* variable = variables.at(setting.VariableIndex);
                        const auto& name = setting.Name;
                        const auto& label = setting.Label;
                        auto value = variable->GetConfigString();
                        bool changed = false;
                        const bool readOnly = variable->GetFlags() == EConsoleVariableFlags::ReadOnly;
                        ImGui::PushID(name.c_str());
                        ImGui::SetNextItemWidth(210);
                        if (variable->GetType() == EConsoleVariableType::Boolean)
                        {
                            bool enabled = value == "true";
                            changed = ImGui::Checkbox(label.c_str(), &enabled);
                            value = enabled ? "1" : "0";
                        }
                        else if (variable->GetType() == EConsoleVariableType::Enumeration && variable->GetEnumCount() > 0)
                        {
                            const auto currentValue = std::stoi(value);
                            const std::string preview(variable->GetEnumValueName(currentValue));
                            if (ImGui::BeginCombo(label.c_str(), preview.c_str()))
                            {
                                for (int index = 0; index < variable->GetEnumCount(); ++index)
                                {
                                    const auto itemValue = variable->GetEnumFirstValue() + index;
                                    const std::string item(variable->GetEnumValueName(itemValue));
                                    const bool selected = itemValue == currentValue;
                                    ImGui::PushID(itemValue);
                                    if (ImGui::Selectable(item.c_str(), selected))
                                    { value = std::to_string(itemValue); changed = true; }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                    ImGui::PopID();
                                }
                                ImGui::EndCombo();
                            }
                        }
                        else if (variable->GetType() == EConsoleVariableType::FloatingPoint)
                            changed = InputNumber<double>(label.c_str(), ImGuiDataType_Double, value);
                        else if (variable->GetType() == EConsoleVariableType::UnsignedInteger)
                            changed = InputNumber<uint64_t>(label.c_str(), ImGuiDataType_U64, value);
                        else if (variable->GetType() == EConsoleVariableType::Integer)
                            changed = InputNumber<int64_t>(label.c_str(), ImGuiDataType_S64, value);
                        else changed = ImGui::InputText(label.c_str(), &value, ImGuiInputTextFlags_EnterReturnsTrue);
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(name.c_str());
                            ImGui::TextUnformatted(setting.Description.c_str());
                            if (readOnly)
                            {
                                ImGui::TextUnformatted("Readonly: changes are saved now and apply only after restarting the application.");
                                ImGui::Text("Active value: %s", variable->GetString().c_str());
                            }
                            ImGui::EndTooltip();
                        }
                        if (changed)
                            InvalidVariable = variable->SetConfigFromString(value) ? std::string{} : name;
                        if (InvalidVariable == name)
                            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Invalid value or INI save failed; previous value retained. See tooltip and log.");
                        if (readOnly) { ImGui::SameLine(); ImGui::TextDisabled("(readonly)"); }
                        ImGui::PopID();
                    }
                    for (const auto& [name, child] : group.Children)
                    {
                        if (filter.IsActive()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth))
                        {
                            self(self, child);
                            ImGui::TreePop();
                        }
                    }
                };
                ImGui::PushID(filter.IsActive() ? "FilteredSettings" : "SettingsTree");
                drawGroup(drawGroup, settings.Root());
                ImGui::PopID();
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

        struct DrawTexture
        {
            UITextureId Id = UITextureId::Invalid;
            UINT Width = 0;
            UINT Height = 0;
            std::vector<unsigned char> Pixels;
        };

        struct AtlasUpload
        {
            GDX12ResourceHandle Texture;
            GDX12ResourceHandle Staging;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        };

        AtlasUpload PrepareAtlas(GDX12Device& device, const DrawTexture& source)
        {
            // shortcut: upload each captured atlas, cache immutable generations if profiling shows a cost.
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

    std::shared_ptr<const DrawData> CaptureDrawData(const ImDrawData& source)
    {
        if (!source.Valid) throw std::invalid_argument("UI capture requires finalized ImGui draw data");
        if (!std::isfinite(source.DisplaySize.x) || !std::isfinite(source.DisplaySize.y) ||
            !std::isfinite(source.DisplayPos.x) || !std::isfinite(source.DisplayPos.y))
            throw std::invalid_argument("UI capture requires a finite logical viewport");
        if (source.DisplaySize.x <= 0 || source.DisplaySize.y <= 0) return {};

        auto frame = std::make_shared<DrawData>(source);
        std::unordered_map<const ImTextureData*, ImTextureID> managed;
        std::unordered_map<ImTextureID, ImTextureID> external;
        for (auto* list : frame->Lists.Data.CmdLists)
        {
            for (auto& command : list->CmdBuffer)
            {
                if (command.UserCallback)
                {
                    if (command.UserCallback != ImDrawCallback_ResetRenderState)
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
                        frame->Textures.push_back({UITextureId::Invalid, static_cast<UINT>(tex->Width),
                            static_cast<UINT>(tex->Height), {tex->Pixels, tex->Pixels + tex->GetSizeInBytes()}});
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
        return frame;
    }

    struct OffscreenRenderer::Impl
    {
        static constexpr unsigned TexturesPerSlot = 64;

        struct Recording
        {
            std::shared_ptr<Impl> Renderer;
            std::shared_ptr<const DrawData> Frame;
            GDX12ResourceHandle Target;
            std::vector<AtlasUpload> Uploads;
            std::vector<GDX12ResourceHandle> Textures;
        };

        Impl(GDX12Device& device, unsigned slots)
            : Device(device), Native(device.GetDevice()),
              OwnerThread(std::this_thread::get_id()),
              SrvHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots * TexturesPerSlot,
                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
              RtvHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, slots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE),
              Slots(slots)
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
            ComPtr<IUnknown> identity;
            ThrowIfFailed(object.GetDevice(IID_PPV_ARGS(&identity)));
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
        // Match the pinned backend's unsigned FrameIndex, including wraparound.
        UINT NextFrame = 0;
    };

    OffscreenRenderer::OffscreenRenderer(GDX12Device& device, unsigned framesInFlight)
    {
        if (!device.IsInitialized() || !device.GetDevice() || !device.GetCommandQueue() || !framesInFlight ||
            framesInFlight > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / Impl::TexturesPerSlot)
            throw std::invalid_argument("Offscreen UI requires a live device and a valid frame ring size");
        _impl = std::make_shared<Impl>(device, framesInFlight);
        _impl->Initialize();
    }

    OffscreenRenderer::~OffscreenRenderer() = default;

    std::shared_ptr<const void> OffscreenRenderer::TryRecord(GDX12CommandList& commands, GDX12ResourceHandle target,
        std::shared_ptr<const DrawData> frame, const std::vector<TextureBinding>& textures)
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
        for (UINT index = 0; index < captured.Textures.size(); ++index)
        {
            const auto& source = captured.Textures[index];
            ID3D12Resource* resource = nullptr;
            DXGI_FORMAT format = OffscreenFormat;
            if (source.Id == UITextureId::Invalid)
            {
                auto& upload = recording->Uploads.emplace_back(PrepareAtlas(state.Device, source));
                resource = upload.Texture.Get();
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
        playback.Data.FramebufferScale = {static_cast<float>(targetDesc.Width) / playback.Data.DisplaySize.x,
            static_cast<float>(targetDesc.Height) / playback.Data.DisplaySize.y};
        for (auto* drawList : playback.Data.CmdLists)
            for (auto& command : drawList->CmdBuffer)
                if (!command.UserCallback)
                {
                    const auto index = static_cast<UINT>(command.TexRef._TexID - 1);
                    command.TexRef = ImTextureRef(state.SrvHeap.GetGPUHandle(base + index).ptr);
                }

        ImGuiContextScope scope(state.Context);
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
        const float clear[4]{};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {state.SrvHeap.GetHeap().Get()};
        list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(&playback.Data, list);
        commands.InvalidateCachedState();
        state.Slots[slot] = recording;
        ++state.NextFrame;
        return recording;
    }

    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format)
    {
        Heap = std::make_unique<GDX12DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        FreeDescriptors.clear();
        for (UINT index = 0; index < Heap->GetNumDescriptors(); ++index) FreeDescriptors.push_back(index);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
#ifdef _DEBUG
        const auto configDir = Engine::Core::GetExecutableDirectory() / L"Config";
        std::error_code layoutError;
        std::filesystem::create_directories(configDir, layoutError);
        const auto layoutPath = (configDir / L"ImGui.ini").u8string();
        LayoutPath.assign(reinterpret_cast<const char*>(layoutPath.data()), layoutPath.size());
        io.IniFilename = layoutError ? nullptr : LayoutPath.c_str();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#else
        io.IniFilename = nullptr;
#endif
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        Visible = true;
        InvalidVariable.clear();
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

    static bool SettingsSuspended = false;

    void SuspendSettings(bool suspended) { SettingsSuspended = suspended; }

    int DrawBistroQualityPanel(bool& open, std::string_view status, bool running, bool hasWinner,
        const char* const* techniques, const char* const* currentTiers, size_t techniqueCount, int& technique, int& tier)
    {
        if (!FrameBegun || (!open && !running)) return 0;
        constexpr const char* tiers[] = {"Off", "Low", "Medium", "High", "Epic"};
        int action = 0;
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(460, FLT_MAX));
        if (ImGui::Begin("Bistro quality (F2)", running ? nullptr : &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%.*s", static_cast<int>(status.size()), status.data());
            if (running)
            {
                if (ImGui::Button("Cancel search")) action = 3;
            }
            else
            {
                if (ImGui::Button("Performance (60 FPS)")) action = 1;
                ImGui::SameLine();
                if (ImGui::Button("Quality (28+ FPS)")) action = 2;
                if (hasWinner)
                {
                    if (ImGui::Button("Apply result")) action = 4;
                    ImGui::SameLine();
                    if (ImGui::Button("Save result")) action = 5;
                }
                ImGui::Separator();
                ImGui::Combo("Technique", &technique, techniques, static_cast<int>(techniqueCount));
                ImGui::Text("Current tier: %s", currentTiers[technique]);
                ImGui::Combo("Tier", &tier, tiers, static_cast<int>(std::size(tiers)));
                if (ImGui::Button("Apply preset")) action = 6;
                ImGui::SameLine();
                if (ImGui::Button("Restart Bistro")) action = 7;
                if (ImGui::Button("Load saved profile")) action = 8;
            }
        }
        ImGui::End();
        return action;
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
#if defined(_DEBUG)
        auto dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;
        if (!Visible) dockFlags |= ImGuiDockNodeFlags_KeepAliveOnly;
        ImGui::DockSpaceOverViewport(0, nullptr, dockFlags);
        if (!SettingsSuspended) DrawSettings();
#endif
    }

#ifdef _DEBUG
    bool DebugControlsVisible() { return FrameBegun && Visible && !SettingsSuspended; }

    bool DebugButton(const char* label, std::initializer_list<std::string_view> lines)
    {
        if (!DebugControlsVisible()) return false;
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 16, viewport->WorkPos.y + 16},
            ImGuiCond_FirstUseEver, {1, 0});
        bool clicked = false;
        if (ImGui::Begin("Debug controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            clicked = ImGui::Button(label);
            if (lines.size())
            {
                ImGui::Separator();
                ImGui::TextUnformatted("Placement and freshness (last frame)");
                for (const auto line : lines) ImGui::TextUnformatted(line.data(), line.data() + line.size());
            }
        }
        ImGui::End();
        return clicked;
    }
#endif

    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color)
    {
        if (!FrameBegun || !ImGui::GetCurrentContext()) return;
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
        return data ? CaptureDrawData(*data) : nullptr;
    }

    void ShutdownUI()
    {
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
        if (!ImGui::GetCurrentContext()) return false;
        const bool handled = ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
#if defined(_DEBUG)
        if (message == WM_KEYUP && wParam == VK_F1) { Visible = !Visible; return true; }
#endif
        const auto& io = ImGui::GetIO();
        return handled || (Visible && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && io.WantCaptureMouse) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST && io.WantCaptureKeyboard)));
    }

    bool WantsKeyboardInput()
    {
        return ImGui::GetCurrentContext() && Visible && ImGui::GetIO().WantCaptureKeyboard;
    }
}
