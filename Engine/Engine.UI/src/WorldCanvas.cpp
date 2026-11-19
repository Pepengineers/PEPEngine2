#include "Engine.UI/WorldCanvas.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include <imgui/imgui.h>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>

namespace Engine::UI
{
    namespace
    {
        AutoConsoleVariable<CanvasQuality> QualitySetting(L"Render.UI.Quality", CanvasQuality::Low,
            L"World canvas GPU workload: 0 Low, 1 Medium.");

        class CanvasContextScope
        {
        public:
            explicit CanvasContextScope(ImGuiContext* context) : Previous(ImGui::GetCurrentContext())
            { ImGui::SetCurrentContext(context); }
            ~CanvasContextScope() { ImGui::SetCurrentContext(Previous); }
            CanvasContextScope(const CanvasContextScope&) = delete;
            CanvasContextScope& operator=(const CanvasContextScope&) = delete;
        private:
            ImGuiContext* Previous;
        };

        struct EffectConstants
        {
            UINT Width, Height, Seed, Stars;
            float Time;
            UINT BlurRadius, Style;
        };
        static_assert(sizeof(EffectConstants) == 7 * sizeof(UINT));

        constexpr UINT DivideUp(UINT value, UINT divisor)
        {
            return (value + divisor - 1) / divisor;
        }
    }

    CanvasQuality GetCanvasQuality() { return QualitySetting.GetValue(); }

    CanvasProfile GetCanvasProfile(CanvasSize logicalSize, CanvasQuality quality)
    {
        constexpr auto limit = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        if (!logicalSize.Width || !logicalSize.Height || logicalSize.Width > limit || logicalSize.Height > limit)
            throw std::invalid_argument("Canvas requires a nonempty D3D12 texture grid");
        if (quality != CanvasQuality::Low && quality != CanvasQuality::Medium)
            throw std::invalid_argument("Unknown canvas quality");
        const bool medium = quality == CanvasQuality::Medium;
        const CanvasSize raster = medium
            ? CanvasSize{DivideUp(logicalSize.Width * 4, 3), DivideUp(logicalSize.Height * 4, 3)}
            : logicalSize;
        if (raster.Width > limit || raster.Height > limit)
            throw std::invalid_argument("Canvas quality exceeds the D3D12 texture size limit");
        const UINT divisor = medium ? 3 : 4;
        return {raster, {DivideUp(raster.Width, divisor), DivideUp(raster.Height, divisor)},
            medium ? 40u : 20u, medium ? 5u : 2u};
    }

    struct WorldCanvas::Impl
    {
        Impl(CanvasId id, std::string title, CanvasStyle style)
            : Id(id), Title(std::move(title)), Style(style), Owner(std::this_thread::get_id())
        {
            CanvasContextScope scope(nullptr);
            Context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        }

        ~Impl()
        {
            assert(std::this_thread::get_id() == Owner);
            ImGui::DestroyContext(Context);
        }

        CanvasId Id;
        std::string Title;
        CanvasStyle Style;
        std::thread::id Owner;
        ImGuiContext* Context = nullptr;
        CanvasSize LastSize;
        CanvasQuality LastQuality = CanvasQuality::Low;
        std::uint64_t Revision = 0;
    };

    WorldCanvas::WorldCanvas(CanvasId id, std::string title, CanvasStyle style)
    {
        if (id == CanvasId::Invalid || title.empty()) throw std::invalid_argument("Canvas requires an ID and title");
        if (style != CanvasStyle::Neon && style != CanvasStyle::Graffiti && style != CanvasStyle::Image)
            throw std::invalid_argument("Unknown canvas style");
        _impl = std::make_unique<Impl>(id, std::move(title), style);
    }

    WorldCanvas::~WorldCanvas() = default;

    std::shared_ptr<const CanvasFrame> WorldCanvas::BuildFrame(CanvasSize logicalSize, double timeSeconds, CanvasQuality quality)
    {
        auto& state = *_impl;
        if (state.Owner != std::this_thread::get_id()) throw std::logic_error("Canvas layout requires the ImGui owner thread");
        if (!std::isfinite(timeSeconds) || timeSeconds < 0) throw std::invalid_argument("Canvas animation time must be finite and nonnegative");
        GetCanvasProfile(logicalSize, quality);
        const bool changed = logicalSize != state.LastSize || quality != state.LastQuality;
        if (changed && state.Revision == (std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error("Canvas surface revision exhausted");
        const auto revision = state.Revision + changed;

        CanvasContextScope scope(state.Context);
        const ImVec2 size(static_cast<float>(logicalSize.Width), static_cast<float>(logicalSize.Height));
        ImGui::GetIO().DisplaySize = size;
        ImGui::NewFrame();
        auto* draw = ImGui::GetBackgroundDrawList();
        draw->AddImage(ImTextureRef(static_cast<ImTextureID>(CanvasEffectTextureId)), {0, 0}, size);
        const float time = static_cast<float>(std::fmod(timeSeconds, 240.0));
        const auto font = ImGui::GetFont();
        const auto text = [&](const char* value, float height, ImVec2 position, ImU32 color, bool slanted = false) {
            const int begin = draw->VtxBuffer.Size;
            draw->AddText(font, height, position, color, value);
            if (slanted)
                for (int i = begin; i < draw->VtxBuffer.Size; ++i)
                    draw->VtxBuffer[i].pos.x += (position.y + height * 0.5f - draw->VtxBuffer[i].pos.y) * 0.20f;
        };
        draw->PushClipRect({0, 0}, size, true);
        if (state.Style == CanvasStyle::Neon)
        {
            const float pulse = 0.75f + 0.25f * std::sin(time * 3.2f);
            const ImU32 cyan = IM_COL32(70, 255, 245, static_cast<int>(220 + 35 * pulse));
            const ImU32 pink = IM_COL32(255, 30, 155, 255);
            draw->AddRect({size.x * 0.035f, size.y * 0.06f}, {size.x * 0.965f, size.y * 0.94f}, pink, 7, 0, 3);
            draw->AddRect({size.x * 0.05f, size.y * 0.085f}, {size.x * 0.95f, size.y * 0.915f}, cyan, 5, 0, 1);
            text(state.Title.c_str(), size.y * 0.055f, {size.x * 0.085f, size.y * 0.15f}, cyan);
            char live[32];
            std::snprintf(live, sizeof(live), "LIVE %05.1f", time);
            text(live, size.y * 0.05f, {size.x * 0.72f, size.y * 0.15f}, pink);
            const float stride = size.x * 1.2f;
            const float shift = std::fmod(time * size.x * 0.09f, stride);
            for (int copy = 0; copy < 2; ++copy)
            {
                const ImVec2 p{size.x * 0.08f - shift + copy * stride, size.y * 0.31f};
                text("NEON DISTRICT", size.y * 0.24f, {p.x + 3, p.y + 3}, pink);
                text("NEON DISTRICT", size.y * 0.24f, p, cyan);
            }
            for (int i = 0; i < 12; ++i)
            {
                const float x = size.x * (0.085f + i * 0.069f);
                const float height = size.y * (0.025f + 0.06f * (0.5f + 0.5f * std::sin(time * 4 - i * 0.7f)));
                draw->AddRectFilled({x, size.y * 0.67f - height}, {x + size.x * 0.045f, size.y * 0.67f}, i % 2 ? cyan : pink);
            }
            const char* ticker = "AFTER HOURS // NO SIGNAL LOST // OPEN ALL NIGHT // ";
            const float tickerWidth = font->CalcTextSizeA(size.y * 0.06f, FLT_MAX, 0, ticker).x;
            const float tickerX = -std::fmod(time * size.x * 0.14f, tickerWidth);
            for (int i = 0; i < 3; ++i)
                text(ticker, size.y * 0.06f, {tickerX + i * tickerWidth, size.y * 0.79f}, IM_COL32(240, 235, 255, 255));
        }
        else if (state.Style == CanvasStyle::Graffiti)
        {
            const auto random = [](unsigned v) {
                v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
                return float(v & 65535u) / 65535.f;
            };
            for (unsigned i = 0; i < 90; ++i)
            {
                const ImVec2 p{size.x * random(i * 3 + 13), size.y * random(i * 3 + 14)};
                const float radius = size.y * (0.002f + 0.014f * random(i * 3 + 15));
                draw->AddCircleFilled(p, radius, i % 3 ? IM_COL32(249, 53, 143, 130) : IM_COL32(40, 220, 225, 150), 8);
            }
            const float fontSize = size.y * 0.49f;
            const float width = font->CalcTextSizeA(fontSize, FLT_MAX, 0, "ASTRA").x;
            const ImVec2 p{(size.x - width) * 0.5f, size.y * 0.25f};
            for (int y = -4; y <= 4; y += 2)
                for (int x = -4; x <= 4; x += 2)
                    text("ASTRA", fontSize, {p.x + x + 5, p.y + y + 7}, IM_COL32(10, 8, 22, 255), true);
            text("ASTRA", fontSize, {p.x - 3, p.y - 2}, IM_COL32(45, 250, 245, 255), true);
            text("ASTRA", fontSize, {p.x + 3, p.y + 3}, IM_COL32(255, 42, 160, 255), true);
            for (int x = 0; x < 3; ++x)
                text("ASTRA", fontSize, {p.x + x, p.y}, IM_COL32(255, 233, 113, 255), true);
            for (int i = 0; i < 7; ++i)
            {
                const float x = size.x * (0.18f + i * 0.105f);
                const float end = size.y * (0.75f + 0.09f * (0.5f + 0.5f * std::sin(time * 0.8f + i)));
                draw->AddLine({x, size.y * 0.68f}, {x - size.x * 0.01f, end}, IM_COL32(255, 43, 158, 210), 3);
            }
            draw->AddLine({size.x * 0.12f, size.y * 0.82f}, {size.x * 0.89f, size.y * 0.74f}, IM_COL32(50, 240, 240, 255), 4);
            text("SIGNAL IN THE CONCRETE", size.y * 0.055f, {size.x * 0.15f, size.y * 0.12f}, IM_COL32(255, 230, 170, 240));
            text("ASTRA / AFTER DARK", size.y * 0.05f, {size.x * 0.42f, size.y * 0.86f}, IM_COL32(230, 210, 235, 255));
        }
        draw->PopClipRect();
        ImGui::Render();
        auto frame = std::make_shared<const CanvasFrame>(
            CanvasFrame{state.Id, logicalSize, timeSeconds, quality, revision, CaptureDrawData(*ImGui::GetDrawData()), state.Style});
        state.LastSize = logicalSize;
        state.LastQuality = quality;
        state.Revision = revision;
        return frame;
    }

    struct CanvasEffects::Impl
    {
        explicit Impl(GDX12Device& device) : Device(device)
        {
            ThrowIfFailed(device.GetDevice().As(&Identity));
            D3D12_FEATURE_DATA_FORMAT_SUPPORT format{OffscreenFormat};
            ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
            if (!(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                throw std::runtime_error("Canvas effects require RGBA8 typed UAV stores");

            GDX12RootSignatureDesc root;
            root.Constants = {sizeof(EffectConstants) / sizeof(UINT)};
            root.NumSingleSRVSlots = 2;
            root.NumSingleUAVSlots = 1;
            Root = std::make_unique<GDX12RootSignature>(&device, root);
            constexpr wchar_t file[] = SHADERS_FOLDER L"CanvasEffects.hlsl";
            const D3D_SHADER_MACRO medium[] = {{"CANVAS_MEDIUM", "1"}, {nullptr, nullptr}};
            Gradient[0] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "GenerateGradient", nullptr);
            Gradient[1] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "GenerateGradient", medium);
            Blur = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "BlurHorizontal", nullptr);
            Finish[0] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "FinishCanvas", nullptr);
            Finish[1] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "FinishCanvas", medium);
            GaussianVertical = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "GaussianVertical", nullptr);
            Composite = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "ComposePost", nullptr);
        }

        void CheckDevice(ID3D12DeviceChild& object) const
        {
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(object.GetDevice(IID_PPV_ARGS(&device)));
            ComPtr<IUnknown> owner;
            ThrowIfFailed(device.As(&owner));
            if (owner.Get() != Identity.Get()) throw std::invalid_argument("Canvas effects require device-local resources and commands");
        }

        GDX12Device& Device;
        ComPtr<IUnknown> Identity;
        std::unique_ptr<GDX12RootSignature> Root;
        ComPtr<ID3D12PipelineState> Gradient[2], Blur, Finish[2], GaussianVertical, Composite;
    };

    struct CanvasEffects::Prepared
    {
        explicit Prepared(std::shared_ptr<Impl> renderer)
            : Renderer(std::move(renderer)), Descriptors(&Renderer->Device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {}

        std::shared_ptr<Impl> Renderer;
        GDX12DescriptorHeap Descriptors;
        GDX12ResourceHandle Scratch[2];
        GDX12ResourceHandle Output;
    };

    struct CanvasEffects::PreparedPost
    {
        explicit PreparedPost(std::shared_ptr<Impl> renderer)
            : Renderer(std::move(renderer)), Descriptors(&Renderer->Device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {}

        std::shared_ptr<Impl> Renderer;
        GDX12DescriptorHeap Descriptors;
        GDX12ResourceHandle Raster, Output, Scratch[2];
    };

    CanvasEffects::CanvasEffects(GDX12Device& device)
    {
        if (!device.IsInitialized()) throw std::invalid_argument("Canvas effects require a live device");
        _impl = std::make_shared<Impl>(device);
    }

    CanvasEffects::~CanvasEffects() = default;

    std::shared_ptr<const void> CanvasEffects::Record(GDX12CommandList& commands, const CanvasFrame& frame,
        GDX12ResourceHandle output) const
    {
        return RecordPrepared(commands, frame, Prepare(frame, std::move(output)));
    }

    std::shared_ptr<CanvasEffects::Prepared> CanvasEffects::Prepare(const CanvasFrame& frame,
        GDX12ResourceHandle output) const
    {
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Canvas effects belong to a retired device incarnation");
        if (!output || frame.Id == CanvasId::Invalid || !std::isfinite(frame.TimeSeconds) || frame.TimeSeconds < 0)
            throw std::invalid_argument("Canvas effects require a live frame and UAV");
        state.CheckDevice(*output.Get());
        const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
        const auto grid = profile.EffectsSize;
        const auto desc = output->GetDesc();
        if (desc.Format != OffscreenFormat || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != grid.Width || desc.Height != grid.Height || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("Canvas effect output must match the profile's RGBA8 UAV/SRV grid");

        auto recording = std::make_shared<Prepared>(_impl);
        recording->Output = std::move(output);
        auto& heap = recording->Descriptors;
        auto& device = state.Device;
        const auto scratchDesc = CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, grid.Width, grid.Height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = OffscreenFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        for (UINT index = 0; index < 2; ++index)
        {
            auto& scratch = recording->Scratch[index];
            scratch = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, scratchDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            device.GetDevice()->CreateUnorderedAccessView(scratch.Get(), nullptr, nullptr, heap.GetCPUHandle(index));
            device.GetDevice()->CreateShaderResourceView(scratch.Get(), &srv, heap.GetCPUHandle(3 + index));
        }
        device.GetDevice()->CreateUnorderedAccessView(recording->Output.Get(), nullptr, nullptr, heap.GetCPUHandle(2));
        device.GetDevice()->CreateShaderResourceView(nullptr, &srv, heap.GetCPUHandle(5));
        return recording;
    }

    std::shared_ptr<const void> CanvasEffects::RecordPrepared(GDX12CommandList& commands, const CanvasFrame& frame,
        const std::shared_ptr<Prepared>& recording) const
    {
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Canvas effects belong to a retired device incarnation");
        if (!recording || recording->Renderer != _impl ||
            (commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands.GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
            frame.Id == CanvasId::Invalid || !std::isfinite(frame.TimeSeconds) || frame.TimeSeconds < 0)
            throw std::invalid_argument("Canvas effects require matching prepared resources and a live DIRECT or COMPUTE frame");
        state.CheckDevice(*commands.GetCommandList().Get());
        const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
        const auto grid = profile.EffectsSize;
        const auto desc = recording->Output->GetDesc();
        if (desc.Width != grid.Width || desc.Height != grid.Height)
            throw std::invalid_argument("Prepared canvas effects must match the frame grid");
        auto& heap = recording->Descriptors;
        const auto id = static_cast<std::uint64_t>(frame.Id);
        const EffectConstants constants{grid.Width, grid.Height, static_cast<UINT>(id ^ (id >> 32)), profile.Stars,
            static_cast<float>(std::fmod(frame.TimeSeconds, 240.0)), profile.BlurRadius, static_cast<UINT>(frame.Style)};
        const UINT variant = static_cast<UINT>(frame.Quality);
        commands.SetDescriptorHeaps({&heap});
        commands.SetComputeRootSignature(state.Root.get());
        commands.GetCommandList()->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
        const auto dispatch = [&](const ComPtr<ID3D12PipelineState>& pipeline, UINT uav)
        {
            commands.SetPipelineState(pipeline);
            commands.SetComputeUAV(0, heap.GetGPUHandle(uav));
            commands.Dispatch(DivideUp(grid.Width, 8), DivideUp(grid.Height, 8), 1);
        };
        // Replace the preceding canvas's SRV binding after switching heaps on the same list.
        commands.SetComputeSRV(0, heap.GetGPUHandle(5));
        commands.SetComputeSRV(1, heap.GetGPUHandle(5));
        dispatch(state.Gradient[variant], 0);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(3));
        dispatch(state.Blur, 1);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[1].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(4));
        dispatch(state.Finish[variant], 2);
        commands.ResourceBarrier({
            CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[0].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[1].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        return recording;
    }

    std::shared_ptr<CanvasEffects::PreparedPost> CanvasEffects::PreparePost(const CanvasFrame& frame,
        GDX12ResourceHandle raster, GDX12ResourceHandle output) const
    {
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Canvas effects belong to a retired device incarnation");
        if (!raster || !output || raster.Get() == output.Get() || frame.Id == CanvasId::Invalid ||
            !std::isfinite(frame.TimeSeconds) || frame.TimeSeconds < 0)
            throw std::invalid_argument("Canvas post requires a live frame and separate raster/output textures");
        state.CheckDevice(*raster.Get()); state.CheckDevice(*output.Get());
        const auto grid = GetCanvasProfile(frame.LogicalSize, frame.Quality).RasterSize;
        for (const auto& resource : {raster, output})
        {
            const auto desc = resource->GetDesc();
            if (desc.Format != OffscreenFormat || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                desc.Width != grid.Width || desc.Height != grid.Height || desc.DepthOrArraySize != 1 ||
                desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
                throw std::invalid_argument("Canvas post textures must match the RGBA8 raster grid");
        }
        if (!(output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            throw std::invalid_argument("Canvas post output requires a UAV");
        auto recording = std::make_shared<PreparedPost>(_impl);
        recording->Raster = std::move(raster); recording->Output = std::move(output);
        auto& device = state.Device;
        auto& heap = recording->Descriptors;
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, grid.Width, grid.Height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = OffscreenFormat; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < 2; ++i)
        {
            recording->Scratch[i] = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            device.GetDevice()->CreateUnorderedAccessView(recording->Scratch[i].Get(), nullptr, nullptr, heap.GetCPUHandle(i));
            device.GetDevice()->CreateShaderResourceView(recording->Scratch[i].Get(), &srv, heap.GetCPUHandle(4 + i));
        }
        device.GetDevice()->CreateUnorderedAccessView(recording->Output.Get(), nullptr, nullptr, heap.GetCPUHandle(2));
        device.GetDevice()->CreateShaderResourceView(recording->Raster.Get(), &srv, heap.GetCPUHandle(3));
        return recording;
    }

    std::shared_ptr<const void> CanvasEffects::RecordPost(GDX12CommandList& commands, const CanvasFrame& frame,
        const std::shared_ptr<PreparedPost>& recording) const
    {
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Canvas effects belong to a retired device incarnation");
        if (!recording || recording->Renderer != _impl || frame.Id == CanvasId::Invalid ||
            !std::isfinite(frame.TimeSeconds) || frame.TimeSeconds < 0 ||
            (commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands.GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE))
            throw std::invalid_argument("Canvas post requires matching prepared resources and a live DIRECT/COMPUTE frame");
        state.CheckDevice(*commands.GetCommandList().Get());
        const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
        const auto grid = profile.RasterSize;
        const auto desc = recording->Output->GetDesc();
        if (desc.Width != grid.Width || desc.Height != grid.Height)
            throw std::invalid_argument("Prepared canvas post must match the current raster grid");
        auto& heap = recording->Descriptors;
        const EffectConstants constants{grid.Width, grid.Height, static_cast<UINT>(frame.Id), profile.Stars,
            static_cast<float>(std::fmod(frame.TimeSeconds, 240.0)), profile.BlurRadius * 3 + 2, static_cast<UINT>(frame.Style)};
        commands.SetDescriptorHeaps({&heap}); commands.SetComputeRootSignature(state.Root.get());
        commands.GetCommandList()->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
        const auto dispatch = [&](const ComPtr<ID3D12PipelineState>& pipeline, UINT target) {
            commands.SetPipelineState(pipeline); commands.SetComputeUAV(0, heap.GetGPUHandle(target));
            commands.Dispatch(DivideUp(grid.Width, 8), DivideUp(grid.Height, 8), 1);
        };
        commands.SetComputeSRV(0, heap.GetGPUHandle(3)); commands.SetComputeSRV(1, heap.GetGPUHandle(3));
        dispatch(state.Blur, 0);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(4)); dispatch(state.GaussianVertical, 1);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[1].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(3)); commands.SetComputeSRV(1, heap.GetGPUHandle(5));
        dispatch(state.Composite, 2);
        commands.ResourceBarrier({
            CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[0].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[1].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        return recording;
    }
}
