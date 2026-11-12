#include "Engine.UI/WorldCanvas.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include <imgui/imgui.h>
#include <cmath>
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
            UINT BlurRadius;
        };
        static_assert(sizeof(EffectConstants) == 6 * sizeof(UINT));

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
        Impl(CanvasId id, std::string title) : Id(id), Title(std::move(title)), Owner(std::this_thread::get_id())
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
        std::thread::id Owner;
        ImGuiContext* Context = nullptr;
        CanvasSize LastSize;
        CanvasQuality LastQuality = CanvasQuality::Low;
        std::uint64_t Revision = 0;
    };

    WorldCanvas::WorldCanvas(CanvasId id, std::string title)
    {
        if (id == CanvasId::Invalid || title.empty()) throw std::invalid_argument("Canvas requires an ID and title");
        _impl = std::make_unique<Impl>(id, std::move(title));
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
        const auto font = ImGui::GetFont();
        draw->AddText(font, size.y * 0.14f, {size.x * 0.12f, size.y * 0.21f}, IM_COL32_WHITE, state.Title.c_str());
        draw->AddText(font, size.y * 0.065f, {size.x * 0.12f, size.y * 0.47f}, IM_COL32(195, 230, 244, 255),
            "RESERVE POWER");
        ImGui::Render();
        auto frame = std::make_shared<const CanvasFrame>(
            CanvasFrame{state.Id, logicalSize, timeSeconds, quality, revision, CaptureDrawData(*ImGui::GetDrawData())});
        state.LastSize = logicalSize;
        state.LastQuality = quality;
        state.Revision = revision;
        return frame;
    }

    struct CanvasEffects::Impl
    {
        struct Recording
        {
            explicit Recording(std::shared_ptr<Impl> renderer)
                : Renderer(std::move(renderer)), Descriptors(&Renderer->Device,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {}

            std::shared_ptr<Impl> Renderer;
            GDX12DescriptorHeap Descriptors;
            GDX12ResourceHandle Scratch[2];
            GDX12ResourceHandle Output;
        };

        explicit Impl(GDX12Device& device) : Device(device)
        {
            ThrowIfFailed(device.GetDevice().As(&Identity));
            D3D12_FEATURE_DATA_FORMAT_SUPPORT format{OffscreenFormat};
            ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
            if (!(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                throw std::runtime_error("Canvas effects require RGBA8 typed UAV stores");

            GDX12RootSignatureDesc root;
            root.Constants = {sizeof(EffectConstants) / sizeof(UINT)};
            root.NumSingleSRVSlots = 1;
            root.NumSingleUAVSlots = 1;
            Root = std::make_unique<GDX12RootSignature>(&device, root);
            constexpr wchar_t file[] = SHADERS_FOLDER L"CanvasEffects.hlsl";
            const D3D_SHADER_MACRO medium[] = {{"CANVAS_MEDIUM", "1"}, {nullptr, nullptr}};
            Gradient[0] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "GenerateGradient", nullptr);
            Gradient[1] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "GenerateGradient", medium);
            Blur = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "BlurHorizontal", nullptr);
            Finish[0] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "FinishCanvas", nullptr);
            Finish[1] = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), file, "FinishCanvas", medium);
        }

        void CheckDevice(ID3D12DeviceChild& object) const
        {
            ComPtr<IUnknown> owner;
            ThrowIfFailed(object.GetDevice(IID_PPV_ARGS(&owner)));
            if (owner.Get() != Identity.Get()) throw std::invalid_argument("Canvas effects require device-local resources and commands");
        }

        GDX12Device& Device;
        ComPtr<IUnknown> Identity;
        std::unique_ptr<GDX12RootSignature> Root;
        ComPtr<ID3D12PipelineState> Gradient[2], Blur, Finish[2];
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
        auto& state = *_impl;
        ComPtr<IUnknown> currentDevice;
        ThrowIfFailed(state.Device.GetDevice().As(&currentDevice));
        if (state.Identity != currentDevice)
            throw std::logic_error("Canvas effects belong to a retired device incarnation");
        if ((commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands.GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) ||
            !output || frame.Id == CanvasId::Invalid || !std::isfinite(frame.TimeSeconds) || frame.TimeSeconds < 0)
            throw std::invalid_argument("Canvas effects require a live frame, UAV and DIRECT or COMPUTE list");
        state.CheckDevice(*commands.GetCommandList().Get());
        state.CheckDevice(*output.Get());
        const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
        const auto grid = profile.EffectsSize;
        const auto desc = output->GetDesc();
        if (desc.Format != OffscreenFormat || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width != grid.Width || desc.Height != grid.Height || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("Canvas effect output must match the profile's RGBA8 UAV/SRV grid");

        auto recording = std::make_shared<Impl::Recording>(_impl);
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

        const auto id = static_cast<std::uint64_t>(frame.Id);
        const EffectConstants constants{grid.Width, grid.Height, static_cast<UINT>(id ^ (id >> 32)), profile.Stars,
            static_cast<float>(std::fmod(frame.TimeSeconds, 240.0)), profile.BlurRadius};
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
        dispatch(state.Gradient[variant], 0);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(3));
        dispatch(state.Blur, 1);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(recording->Scratch[1].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
        commands.SetComputeSRV(0, heap.GetGPUHandle(4));
        dispatch(state.Finish[variant], 2);
        return recording;
    }
}
