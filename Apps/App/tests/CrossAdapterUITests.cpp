#include "App.Base/App.h"
#include "App.Base/Rendering/CrossAdapterUI.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "BistroUICanvases.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include <d3d12sdklayers.h>
#include <imgui/imgui.h>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace Engine::UI;
using namespace GDX12Submission;

namespace
{
    constexpr CanvasSize ScreenSize{128, 96};
    constexpr CanvasSize PosterSize{384, 216};
    constexpr CanvasId Screen = CanvasId::Invalid;
    constexpr auto Atrium = BistroUICanvases::Atrium;
    constexpr auto Terrace = BistroUICanvases::Terrace;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    class FixtureApp final : public App
    {
    public:
        FixtureApp() : App(GetModuleHandleW(nullptr)) {}
    };

    class Gate
    {
    public:
        Gate(GDX12Device& device, GDX12CommandQueue& queue)
        {
            ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
            ThrowIfFailed(queue.GetCommandQueue()->Wait(_fence.Get(), 1));
        }
        ~Gate() { if (_fence) _fence->Signal(1); }
        void Open() { ThrowIfFailed(_fence->Signal(1)); _fence.Reset(); }
    private:
        ComPtr<ID3D12Fence> _fence;
    };

    void Wait(GDX12CommandQueue& queue, CompletionPoint point)
    {
        Require(point.IsValid() && point.Timeline() == queue.GetTimelineIdentity(), "Wrong UI test timeline");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!point.IsReachedBy(queue.GetCompletionProgress()))
        {
            Require(std::chrono::steady_clock::now() < deadline, "UI GPU point did not complete");
            Sleep(1);
        }
    }

    CompletionPoint Submit(GDX12CommandQueue& queue, GDX12CommandList& commands)
    {
        ThrowIfFailed(commands.GetCommandList()->Close());
        ID3D12CommandList* lists[] = {commands.GetCommandList().Get()};
        queue.GetCommandQueue()->ExecuteCommandLists(1, lists);
        return queue.SignalCompletion();
    }

    void CheckRoutes()
    {
        auto settings = ReadUIOffloadSettings();
        Require(!settings.Enabled && settings.Freshness == UIVisualFreshness::Exact &&
            settings.UpdateHz == 0 && settings.MaxAgeMs == 0 &&
            !RequestsUIOffload(settings, Screen) && !RequestsUIOffload(settings, Atrium),
            "UI offload default is not local");
        settings.Enabled = true;
        Require(RequestsUIOffload(settings, Screen) && RequestsUIOffload(settings, Atrium) &&
            RequestsUIOffload(settings, Terrace), "Whole UI omitted a surface");
        settings.Scope = UIOffloadScope::Canvas;
        for (const auto id : {Atrium, Terrace})
        {
            settings.Selected = id;
            Require(!RequestsUIOffload(settings, Screen) && RequestsUIOffload(settings, id) &&
                !RequestsUIOffload(settings, id == Atrium ? Terrace : Atrium), "Canvas route selected the wrong surfaces");
        }
        ConsoleModule console;
        auto& rate = console.FindConsoleVariable(L"Render.UI.UpdateHz");
        auto& age = console.FindConsoleVariable(L"Render.UI.MaxAgeMs");
        Require(!rate.SetFromString("nan") && !rate.SetFromString("inf") && !rate.SetFromString("-1") &&
            !rate.SetFromString("0.5") && !rate.SetFromString("241") && rate.SetFromString("240"),
            "UI producer cadence did not enforce 0 or 1 through 240 Hz");
        Require(!age.SetFromString("nan") && !age.SetFromString("inf") && !age.SetFromString("-1") &&
            !age.SetFromString("60001") && age.SetFromString("60000"),
            "UI age threshold did not enforce 0 through 60000 ms");
        settings = ReadUIOffloadSettings();
        Require(settings.UpdateHz == 240 && settings.MaxAgeMs == 60000 &&
            rate.SetFromString("0") && age.SetFromString("0"), "UI controls did not round trip");
        Require(UIUpdateDue(8, {}, 2) && UIUpdateDue(8, 8, 0) && !UIUpdateDue(8.25, 8, 2) &&
            UIUpdateDue(8.5, 8, 2), "UI producer limit is not relative to successful admission");
        const auto exactAge = GetUISnapshotAge(4.25, 4, 250);
        const auto overAge = GetUISnapshotAge(4.5, 4, 250);
        Require(exactAge.Milliseconds == 250 && !exactAge.OverAge && overAge.Milliseconds == 500 && overAge.OverAge &&
            !GetUISnapshotAge(4.5, 4, 0).OverAge, "UI age threshold is not diagnostic or uses the wrong clock");
        std::cout << "UI routing, cadence controls and elapsed snapshot age passed\n";
    }

    std::shared_ptr<const DrawData> MakeScreenDraw()
    {
        struct Context
        {
            ImGuiContext* Previous = ImGui::GetCurrentContext();
            ImGuiContext* Own = ImGui::CreateContext();
            ~Context() { ImGui::DestroyContext(Own); ImGui::SetCurrentContext(Previous); }
        } context;
        ImGui::SetCurrentContext(context.Own);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {static_cast<float>(ScreenSize.Width), static_cast<float>(ScreenSize.Height)};
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefaultVector();
        ImGui::NewFrame();
        auto* draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled({4, 4}, {28, 28}, IM_COL32(255, 0, 0, 128));
        draw->AddText({4, 34}, IM_COL32_WHITE, "SCREEN EXACT");
        ImGui::Render();
        return CaptureDrawData(*ImGui::GetDrawData());
    }

    std::shared_ptr<GDX12Resource> MakeTexture(GDX12Device& device, CanvasSize size,
        D3D12_RESOURCE_FLAGS flags, const D3D12_CLEAR_VALUE* clear = nullptr)
    {
        auto output = std::make_shared<GDX12Resource>();
        output->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, size.Width, size.Height, 1, 1, 1, 0, flags),
            D3D12_RESOURCE_STATE_COMMON, clear);
        return output;
    }

    std::shared_ptr<GDX12Resource> MakePoster(GDX12Device& device, const CanvasFrame& frame)
    {
        const D3D12_CLEAR_VALUE clear{OffscreenFormat, {0, 0, 0, 0}};
        return MakeTexture(device, GetCanvasProfile(frame.LogicalSize, frame.Quality).RasterSize,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &clear);
    }

    struct ScreenTarget
    {
        explicit ScreenTarget(GDX12Device& device) : ClearList(&device),
            Rtv(&device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
        {
            const D3D12_CLEAR_VALUE clear{OffscreenFormat, {0.2f, 0.4f, 0.6f, 1}};
            Resource = MakeTexture(device, ScreenSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &clear);
            auto* native = Resource->D3DResource.Get();
            const auto rtv = Rtv.GetCPUHandle();
            device.GetDevice()->CreateRenderTargetView(native, nullptr, rtv);
            ClearList.ResourceBarrier({Resource->GetRenderTargetBarrier()});
            ClearList.GetCommandList()->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
            ClearList.ResourceBarrier({Resource->GetCommonBarrier()});
        }
        GDX12CommandList ClearList;
        GDX12DescriptorHeap Rtv;
        std::shared_ptr<GDX12Resource> Resource;
    };

    ScratchImage Read(GDX12CommandQueue& queue, const GDX12Resource& target)
    {
        ScratchImage image;
        ThrowIfFailed(CaptureTexture(queue.GetCommandQueue().Get(), target.D3DResource.Get(), false, image,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON));
        return image;
    }

    const unsigned char* Pixel(const Image& image, size_t x, size_t y)
    {
        return image.pixels + y * image.rowPitch + 4 * x;
    }

    void CheckScreen(GDX12CommandQueue& direct, const GDX12Resource& target)
    {
        const auto image = Read(direct, target);
        const auto& pixels = *image.GetImage(0, 0, 0);
        const auto check = [&](size_t x, size_t y, const std::array<int, 4>& expected)
        {
            const auto* actual = Pixel(pixels, x, y);
            for (size_t channel = 0; channel < expected.size(); ++channel)
                Require(std::abs(actual[channel] - expected[channel]) <= 2, "Remote screen blend differs from premultiplied golden");
        };
        check(0, 0, {51, 102, 153, 255});
        check(12, 12, {153, 51, 76, 255});
        size_t glyphs = 0;
        for (size_t y = 34; y < 50; ++y)
            for (size_t x = 4; x < 100; ++x)
                if (Pixel(pixels, x, y)[0] > 170) ++glyphs;
        Require(glyphs > 20, "Remote screen lost the font atlas");
    }

    void RenderLocal(GDX12Device& device, CanvasEffects& effects, OffscreenRenderer& raster,
        const CanvasFrame& frame, GDX12Resource& target)
    {
        const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
        auto effect = MakeTexture(device, profile.EffectsSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        GDX12CommandList commands(&device);
        commands.ResourceBarrier({effect->GetTransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)});
        auto effectOwner = effects.Record(commands, frame, effect->D3DResource);
        commands.ResourceBarrier({effect->GetPixelShaderResourceBarrier(), target.GetRenderTargetBarrier()});
        auto rasterOwner = raster.TryRecord(commands, target.D3DResource, frame.Draw, {{CanvasEffectTextureId, effect->D3DResource}});
        Require(effectOwner && rasterOwner, "Local UI recording failed");
        commands.ResourceBarrier({effect->GetCommonBarrier(), target.GetCommonBarrier()});
        auto& queue = *device.GetCommandQueue();
        Wait(queue, Submit(queue, commands));
    }

    void CheckPoster(GDX12CommandQueue& direct, const GDX12Resource& target, CanvasSize expected)
    {
        const auto image = Read(direct, target);
        const auto& pixels = *image.GetImage(0, 0, 0);
        Require(pixels.width == expected.Width && pixels.height == expected.Height && pixels.format == OffscreenFormat,
            "UI material target has the wrong grid");
        size_t bright = 0;
        for (size_t y = 0; y < pixels.height; ++y)
            for (size_t x = 0; x < pixels.width; ++x)
            {
                const auto* pixel = Pixel(pixels, x, y);
                Require(pixel[3] == 255, "UI material target is not opaque");
                if (y < pixels.height / 2 && pixel[0] > 170 && pixel[1] > 170 && pixel[2] > 170) ++bright;
            }
        Require(bright > 20, "UI material target is missing its title");
    }

    void CheckParity(GDX12CommandQueue& direct, const GDX12Resource& remote, const GDX12Resource& local)
    {
        const auto remoteImage = Read(direct, remote), localImage = Read(direct, local);
        const auto& left = *remoteImage.GetImage(0, 0, 0);
        const auto& right = *localImage.GetImage(0, 0, 0);
        Require(left.width == right.width && left.height == right.height, "Placement changed UI raster size");
        size_t different = 0;
        for (size_t y = 0; y < left.height; ++y)
            for (size_t x = 0; x < left.width; ++x)
            {
                const auto* a = Pixel(left, x, y);
                const auto* b = Pixel(right, x, y);
                if (std::abs(a[0] - b[0]) > 3 || std::abs(a[1] - b[1]) > 3 || std::abs(a[2] - b[2]) > 3) ++different;
            }
        Require(different * 100 < left.width * left.height, "Placement changed more than 1% of UI pixels");
    }

    void CheckDebug(GDX12Device& device)
    {
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
        {
            SIZE_T bytes = 0;
            ThrowIfFailed(info->GetMessage(index, nullptr, &bytes));
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(index, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
    }

    void CheckNative(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime)
    {
        auto& direct = *primary.GetCommandQueue();
        auto* compute = secondary.TryGetComputeQueue();
        Require(compute != nullptr, "UI test needs the secondary COMPUTE queue");
        CrossAdapterUI ui(runtime, primary, secondary, {Atrium, Terrace});
        CanvasEffects localEffects(primary);
        OffscreenRenderer localRaster(primary, 2);
        WorldCanvas atrium(Atrium, "ATRIUM"), terrace(Terrace, "TERRACE");
        const auto a = atrium.BuildFrame(PosterSize, 1.0, CanvasQuality::Low);
        const auto b = terrace.BuildFrame({480, 270}, 4.0, CanvasQuality::Low);
        const auto screen = MakeScreenDraw();
        auto aOutput = MakePoster(primary, *a), bOutput = MakePoster(primary, *b);
        ScreenTarget screenTarget(primary);
        std::uint64_t frame = 0;
        const auto begin = [&](UIOffloadSettings settings)
        {
            runtime.Poll();
            ++frame;
            ui.BeginFrame(FrameSubmissionId{frame}, settings, static_cast<double>(frame) * 0.02);
        };
        const UIOffloadSettings wholeExact{true, UIOffloadScope::Whole, {}, UIVisualFreshness::Exact};
        begin(wholeExact);
        const auto aDone = ui.SubmitCanvas(*a, aOutput, direct.SignalCompletion());
        const auto bDone = ui.SubmitCanvas(*b, bOutput, direct.GetLastCompletionPoint());
        const auto screenPrefix = Submit(direct, screenTarget.ClearList);
        Wait(direct, screenPrefix);
        const auto screenDone = ui.SubmitScreen(screen, screenTarget.Resource, screenPrefix);
        Require(aDone && bDone && screenDone, "Whole Exact omitted a UI submission");
        Wait(direct, *screenDone);
        for (const auto id : {Screen, Atrium, Terrace})
            Require(ui.GetStatus(id).ActiveAdapter == secondary.GetSubmissionAdapterId() &&
                ui.GetStatus(id).Displayed.IsValid() && ui.GetStatus(id).FrameAge == 0 &&
                ui.GetStatus(id).FootprintBytes >= ui.GetStatus(id).PayloadBytes &&
                (id == Screen ? ui.GetStatus(id).MaterialCopyBytes == 0 :
                    ui.GetStatus(id).MaterialCopyBytes == ui.GetStatus(id).PayloadBytes),
                "Whole Exact did not display all secondary surfaces");
        CheckScreen(direct, *screenTarget.Resource);
        CheckPoster(direct, *aOutput, PosterSize);
        CheckPoster(direct, *bOutput, {480, 270});
        auto local = MakePoster(primary, *a);
        RenderLocal(primary, localEffects, localRaster, *a, *local);
        CheckParity(direct, *aOutput, *local);

        auto cappedExact = wholeExact;
        cappedExact.UpdateHz = 1;
        begin(cappedExact);
        const auto cappedPrefix = direct.SignalCompletion();
        Require(!ui.SubmitCanvas(*a, aOutput, cappedPrefix) &&
            ui.GetStatus(Atrium).Fallback == "UI producer cadence cap" && !ui.GetStatus(Atrium).Displayed.IsValid(),
            "Capped Exact canvas did not select same-frame local fallback");

        for (const auto selected : {Atrium, Terrace})
        {
            begin({true, UIOffloadScope::Canvas, selected, UIVisualFreshness::Exact});
            const auto prefix = direct.SignalCompletion();
            const auto& requested = selected == Atrium ? a : b;
            const auto& other = selected == Atrium ? b : a;
            const auto& remoteTarget = selected == Atrium ? aOutput : bOutput;
            const auto& localTarget = selected == Atrium ? bOutput : aOutput;
            Require(!ui.SubmitCanvas(*other, localTarget, prefix) && !ui.SubmitScreen(screen, screenTarget.Resource, prefix),
                "Canvas scope submitted an unrelated surface");
            const auto selectedDone = ui.SubmitCanvas(*requested, remoteTarget, prefix);
            Require(selectedDone.has_value(), "Selected canvas did not submit");
            RenderLocal(primary, localEffects, localRaster, *other, *localTarget);
            Require(ui.GetStatus(selected).ActiveAdapter == secondary.GetSubmissionAdapterId() &&
                ui.GetStatus(other->Id).ActiveAdapter == primary.GetSubmissionAdapterId() &&
                ui.GetStatus(Screen).ActiveAdapter == primary.GetSubmissionAdapterId(), "Canvas scope changed unrelated placement");
        }

        const CanvasId unknown{999};
        begin({true, UIOffloadScope::Canvas, unknown, UIVisualFreshness::Exact});
        const auto unknownRoute = ui.GetStatus(unknown);
        Require(unknownRoute.Requested && unknownRoute.ActiveAdapter == primary.GetSubmissionAdapterId() &&
            unknownRoute.Fallback == "Unknown CanvasId" && !ui.GetStatus(Atrium).Requested && !ui.GetStatus(Terrace).Requested,
            "Unknown selected canvas did not preserve primary placement with a diagnostic");

        const UIOffloadSettings wholeLatest{true, UIOffloadScope::Whole, {}, UIVisualFreshness::Latest, 0, 1};
        const auto publishLatest = [&](const auto& canvas, const auto& output)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (bool admit = true;;)
            {
                begin(wholeLatest);
                const auto previousCompute = compute->GetLastCompletionPoint().Value();
                ui.PumpLatest(canvas, admit);
                const auto done = ui.SubmitCanvas(*canvas, output, direct.SignalCompletion());
                if (done)
                {
                    Wait(direct, *done);
                    return ui.GetStatus(canvas->Id).Displayed;
                }
                if (compute->GetLastCompletionPoint().Value() > previousCompute) admit = false;
                Require(std::chrono::steady_clock::now() < deadline, "Latest UI did not publish a completed snapshot");
                Sleep(1);
            }
        };
        const auto shown = publishLatest(a, aOutput);
        Require(shown.IsValid(), "Latest UI version is invalid");
        const auto shownCaptureTime = ui.GetStatus(Atrium).CapturedSeconds;
        {
            Gate blockedEffects(secondary, *compute);
            begin(wholeLatest);
            const auto next = atrium.BuildFrame(PosterSize, 3.0, CanvasQuality::Low);
            const auto before = compute->GetLastCompletionPoint().Value();
            ui.PumpLatest(next, true);
            const auto pendingEffects = compute->GetLastCompletionPoint();
            Require(pendingEffects.Value() > before && !pendingEffects.IsReachedBy(compute->GetCompletionProgress()),
                "Latest compute producer was not gated");
            auto cappedLatest = wholeLatest;
            cappedLatest.UpdateHz = 1;
            begin(cappedLatest);
            ui.PumpLatest(next, true);
            Require(compute->GetLastCompletionPoint().Value() == pendingEffects.Value(),
                "Capped Latest canvas submitted before its next update");
            begin(wholeLatest);
            const auto later = atrium.BuildFrame(PosterSize, 4.0, CanvasQuality::Low);
            ui.PumpLatest(later, true);
            const auto pipelinedEffects = compute->GetLastCompletionPoint();
            Require(pipelinedEffects.Value() > pendingEffects.Value() && !pipelinedEffects.IsReachedBy(compute->GetCompletionProgress()),
                "Uncapped Latest failed to pipeline two invocations on the same stream");
            begin(wholeLatest);
            ui.PumpLatest(later, false);
            const auto useOld = ui.SubmitCanvas(*later, aOutput, direct.SignalCompletion());
            ScreenTarget nextScreen(primary);
            const auto prefix = Submit(direct, nextScreen.ClearList);
            Wait(direct, prefix);
            const auto exactScreen = ui.SubmitScreen(screen, nextScreen.Resource, prefix);
            const auto current = ui.GetStatus(Atrium);
            const double expectedAge = (static_cast<double>(frame) * 0.02 - shownCaptureTime) * 1000;
            Require(useOld && exactScreen && current.Displayed == shown && current.FrameAge > 0 &&
                current.CapturedSeconds == shownCaptureTime && current.Age.OverAge &&
                std::abs(current.Age.Milliseconds - expectedAge) < 1e-9 &&
                ui.GetStatus(Screen).Mode == SnapshotReadMode::ExactVersion && !ui.GetStatus(Screen).Age.OverAge,
                "Latest age changed placement, selected pending data, or changed the screen policy");
            Wait(direct, *exactScreen);
            CheckScreen(direct, *nextScreen.Resource);
            Require(!pipelinedEffects.IsReachedBy(compute->GetCompletionProgress()), "Exact screen waited for optional COMPUTE");
            blockedEffects.Open();
        }

        const auto medium = atrium.BuildFrame({385, 217}, 3.0, CanvasQuality::Medium);
        auto mediumOutput = MakePoster(primary, *medium);
        {
            Gate blockedReader(primary, direct);
            begin(wholeLatest);
            const auto old = ui.SubmitCanvas(*a, aOutput, direct.SignalCompletion());
            Require(old.has_value(), "Could not hold a Latest reader across resize");
            begin(wholeLatest);
            ui.PumpLatest(medium, false);
            Require(!ui.SubmitCanvas(*medium, mediumOutput, direct.GetLastCompletionPoint()) &&
                !ui.GetStatus(Atrium).Displayed.IsValid(), "A stale epoch was used after resize");
            blockedReader.Open();
            Wait(direct, *old);
        }
        CheckPoster(direct, *aOutput, PosterSize);
        const auto resized = publishLatest(medium, mediumOutput);
        Require(resized.Transfer == shown.Transfer && resized.VersionEpoch != shown.VersionEpoch,
            "UI resize changed the stream or reused its epoch");
        CheckPoster(direct, *mediumOutput, {514, 290});
        const auto mediumLocal = MakePoster(primary, *medium);
        RenderLocal(primary, localEffects, localRaster, *medium, *mediumLocal);
        CheckParity(direct, *mediumOutput, *mediumLocal);
        begin({});
        Require(!ui.SubmitCanvas(*medium, mediumOutput, direct.SignalCompletion()) && !ui.GetStatus(Atrium).Requested,
            "Disabling UI offload left an active route");
        runtime.Shutdown();
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const auto option = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
        Require(argc == 1 || option == "--cpu" || option == "--gpu-validation", "Unknown CrossAdapterUITests option");
        CheckRoutes();
        if (option == "--cpu") return 0;
        FixtureApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (option == "--gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device primary, secondary;
        ThrowIfFailed(primary.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        secondary.Role = DEVICE_ROLE_SECONDARY;
        const auto luid = primary.GetAdapterLuid();
        auto& factory = GDX12DeviceFactory::GetFactory();
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter4> candidate;
            const auto result = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            ThrowIfFailed(result);
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(candidate->GetDesc1(&desc));
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
                (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart)) continue;
            if (SUCCEEDED(secondary.Initialize(candidate))) break;
        }
        Require(secondary.IsInitialized(), "Two physical adapters required for the UI transfer fixture");
        GDX12CommandQueue primaryCopy(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12CommandQueue secondaryCopy(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        RenderMGPURuntime runtime({&primary, &secondary, &primaryCopy, &secondaryCopy});
        CheckNative(primary, secondary, runtime);
        CheckDebug(primary);
        CheckDebug(secondary);
        std::cout << "Primary: " << primary.GetDeviceFeatures().Name << "\nSecondary: " << secondary.GetDeviceFeatures().Name << '\n';
        std::cout << "Whole/selected UI, Exact/Latest, screen alpha, parity and epoch retirement passed\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
