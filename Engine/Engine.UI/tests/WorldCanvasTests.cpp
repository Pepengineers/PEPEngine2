#include "App.Base/App.h"
#include "BistroUICanvases.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include <d3d12sdklayers.h>
#include <imgui/imgui.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace Engine::UI;
using FramePair = std::array<std::shared_ptr<const CanvasFrame>, 2>;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TestApp final : public App
{
public:
    TestApp() : App(GetModuleHandleW(nullptr)) {}
};

struct Fixtures
{
    FramePair Low;
    FramePair Animated;
    FramePair Medium;
};

static Fixtures BuildFixtures()
{
    const auto low = GetCanvasProfile({1920, 1080}, CanvasQuality::Low);
    const auto medium = GetCanvasProfile({1920, 1080}, CanvasQuality::Medium);
    Require(low.RasterSize == CanvasSize{1920, 1080} && low.EffectsSize == CanvasSize{480, 270} &&
        low.Stars == 20 && low.BlurRadius == 2, "Low does not match the fixed workload");
    Require(medium.RasterSize == CanvasSize{2560, 1440} && medium.EffectsSize == CanvasSize{854, 480} &&
        medium.Stars == 40 && medium.BlurRadius == 5, "Medium does not match the fixed workload");
    const auto odd = GetCanvasProfile({385, 217}, CanvasQuality::Medium);
    Require(odd.RasterSize == CanvasSize{514, 290} && odd.EffectsSize == CanvasSize{172, 97},
        "Odd canvas size was rounded incorrectly");

    auto& quality = ConsoleModule{}.FindConsoleVariable(L"Render.UI.Quality");
    Require(quality.GetType() == EConsoleVariableType::Enumeration && quality.GetEnumCount() == 2 &&
        quality.GetEnumValueName(0) == "Low" && quality.GetEnumValueName(1) == "Medium",
        "Canvas quality CVar is not a two-value enum");
    Require(GetCanvasQuality() == CanvasQuality::Low && !quality.SetFromString("2"),
        "Invalid UI quality accepted or seed did not select Low");

    BistroUICanvases::Posters posters;
    auto* activeContext = ImGui::GetCurrentContext();
    Fixtures fixtures;
    fixtures.Low = posters.BuildFrames(0.0);
    fixtures.Animated = posters.BuildFrames(4.0);
    Require(quality.SetFromString("1"), "Cannot select Medium");
    fixtures.Medium = posters.BuildFrames(0.0);
    Require(quality.SetFromString("0"), "Cannot return to Low");
    const auto reverted = posters.BuildFrames(0.0);
    Require(ImGui::GetCurrentContext() == activeContext, "Display-only layout changed the current ImGui context");

    Require(fixtures.Low[0]->Id == BistroUICanvases::Atrium && fixtures.Low[1]->Id == BistroUICanvases::Terrace &&
        fixtures.Low[0]->TimeSeconds != fixtures.Low[1]->TimeSeconds, "Posters do not have independent IDs and phases");
    for (size_t i = 0; i < fixtures.Low.size(); ++i)
    {
        const auto& initial = *fixtures.Low[i];
        Require(initial.Draw && initial.Quality == CanvasQuality::Low && fixtures.Medium[i]->Draw &&
            fixtures.Medium[i]->Quality == CanvasQuality::Medium &&
            fixtures.Animated[i]->SurfaceRevision == initial.SurfaceRevision &&
            fixtures.Medium[i]->SurfaceRevision > initial.SurfaceRevision &&
            reverted[i]->SurfaceRevision > fixtures.Medium[i]->SurfaceRevision,
            "Quality switching did not publish immutable frames and new revisions");
        Require(fixtures.Medium[i]->TimeSeconds == initial.TimeSeconds && reverted[i]->TimeSeconds == initial.TimeSeconds,
            "Quality switching changed the poster animation clock");
    }
    WorldCanvas resize(CanvasId{3}, "RESIZE");
    const auto before = resize.BuildFrame({384, 216}, 0, CanvasQuality::Low);
    const auto after = resize.BuildFrame({385, 217}, 0, CanvasQuality::Low);
    Require(after->SurfaceRevision > before->SurfaceRevision && before->LogicalSize == CanvasSize{384, 216},
        "Resize did not create a new immutable surface revision");
    return fixtures;
}

static void Transition(GDX12CommandList& commands, const GDX12ResourceHandle& resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), before, after)});
}

static GDX12Submission::CompletionPoint Submit(GDX12CommandQueue& queue, GDX12CommandList& commands)
{
    ThrowIfFailed(commands.GetCommandList()->Close());
    ID3D12CommandList* lists[] = {commands.GetCommandList().Get()};
    queue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    return queue.SignalCompletion();
}

struct Images
{
    ScratchImage Effects;
    ScratchImage Raster;
};

struct PendingFrame
{
    PendingFrame(GDX12Device& device, D3D12_COMMAND_LIST_TYPE type)
        : Effects(&device, type), Raster(&device) {}

    GDX12CommandList Effects;
    GDX12CommandList Raster;
    GDX12ResourceHandle EffectOutput;
    GDX12ResourceHandle RasterOutput;
    std::shared_ptr<const void> EffectOwner;
    std::shared_ptr<const void> RasterOwner;
};

static std::unique_ptr<PendingFrame> RecordFrame(GDX12Device& device, GDX12CommandQueue& producer,
    CanvasEffects& effects, OffscreenRenderer& raster, const CanvasFrame& frame)
{
    const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
    const auto make = [&](CanvasSize size, D3D12_RESOURCE_FLAGS flags, const D3D12_CLEAR_VALUE* clear)
    {
        return device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, CD3DX12_RESOURCE_DESC::Tex2D(
            OffscreenFormat, size.Width, size.Height, 1, 1, 1, 0, flags), D3D12_RESOURCE_STATE_COMMON, clear);
    };
    const D3D12_CLEAR_VALUE clear{OffscreenFormat, {0, 0, 0, 0}};
    auto pending = std::make_unique<PendingFrame>(device, producer.GetType());
    auto& effect = pending->EffectOutput;
    auto& target = pending->RasterOutput;
    effect = make(profile.EffectsSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nullptr);
    target = make(profile.RasterSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &clear);
    Transition(pending->Effects, effect, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pending->EffectOwner = effects.Record(pending->Effects, frame, effect);
    Transition(pending->Effects, effect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    Transition(pending->Raster, effect, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(pending->Raster, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pending->RasterOwner = raster.TryRecord(pending->Raster, target, frame.Draw, {{CanvasEffectTextureId, effect}});
    Require(pending->EffectOwner && pending->RasterOwner, "Canvas passes failed to record");
    Transition(pending->Raster, effect, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(pending->Raster, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    return pending;
}

static GDX12Submission::CompletionPoint SubmitFrame(PendingFrame& pending, GDX12CommandQueue& producer,
    GDX12CommandQueue& consumer)
{
    const auto ready = Submit(producer, pending.Effects);
    if (&producer != &consumer) consumer.WaitFor(ready);
    return Submit(consumer, pending.Raster);
}

static Images ReadFrame(GDX12CommandQueue& consumer, const PendingFrame& pending)
{
    Images output;
    for (const auto& pair : {std::make_pair(pending.EffectOutput.Get(), &output.Effects),
        std::make_pair(pending.RasterOutput.Get(), &output.Raster)})
        ThrowIfFailed(CaptureTexture(consumer.GetCommandQueue().Get(), pair.first, false, *pair.second,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON));
    return output;
}

static Images RenderFrame(GDX12Device& device, GDX12CommandQueue& producer, CanvasEffects& effects,
    OffscreenRenderer& raster, const CanvasFrame& frame)
{
    auto& consumer = *device.GetCommandQueue();
    auto pending = RecordFrame(device, producer, effects, raster, frame);
    try
    {
        consumer.CPUWaitForFenceValue(SubmitFrame(*pending, producer, consumer).Value());
    }
    catch (...)
    {
        producer.Flush();
        if (&producer != &consumer) consumer.Flush();
        throw;
    }
    return ReadFrame(consumer, *pending);
}

static std::array<Images, 2> RenderQualityChange(GDX12Device& device, CanvasEffects& effects,
    OffscreenRenderer& raster, const CanvasFrame& low, const CanvasFrame& medium)
{
    auto& direct = *device.GetCommandQueue();
    const auto old = RecordFrame(device, direct, effects, raster, low);
    std::unique_ptr<PendingFrame> next;
    struct Gate
    {
        GDX12CommandQueue& Queue;
        ComPtr<ID3D12Fence> Fence;
        ~Gate()
        {
            if (!Fence) return;
            Fence->Signal(1);
            try { Queue.Flush(); } catch (...) {}
        }
    } gate{direct};
    ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.Fence)));
    ThrowIfFailed(direct.GetCommandQueue()->Wait(gate.Fence.Get(), 1));
    const auto oldDone = SubmitFrame(*old, direct, direct);
    next = RecordFrame(device, direct, effects, raster, medium);
    const auto nextDone = SubmitFrame(*next, direct, direct);
    Require(!oldDone.IsReachedBy(direct.GetCompletionProgress()) && old->RasterOutput.Get() != next->RasterOutput.Get(),
        "Quality resize did not retain distinct surface slots while the old frame was in flight");
    ThrowIfFailed(gate.Fence->Signal(1));
    direct.CPUWaitForFenceValue(nextDone.Value());
    gate.Fence.Reset();
    return {ReadFrame(direct, *old), ReadFrame(direct, *next)};
}

static const unsigned char* Sample(const Image& image, size_t x, size_t y)
{
    return image.pixels + y * image.rowPitch + 4 * x;
}

static void CheckOutput(const Images& images, const CanvasFrame& frame)
{
    const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
    const auto& effect = *images.Effects.GetImage(0, 0, 0);
    const auto& raster = *images.Raster.GetImage(0, 0, 0);
    Require(effect.width == profile.EffectsSize.Width && effect.height == profile.EffectsSize.Height &&
        raster.width == profile.RasterSize.Width && raster.height == profile.RasterSize.Height,
        "Actual GPU grids do not match the profile");
    for (const auto* image : {&effect, &raster})
    {
        Require(image->format == OffscreenFormat, "Canvas output format is not RGBA8 UNORM");
        for (size_t y = 0; y < image->height; ++y)
            for (size_t x = 0; x < image->width; ++x)
                Require(Sample(*image, x, y)[3] == 255, "Material texture is not opaque");
    }
    unsigned char brightestStar = 0;
    for (size_t y = 64 * effect.height / 100; y < 85 * effect.height / 100; ++y)
        for (size_t x = 12 * effect.width / 100; x < 88 * effect.width / 100; ++x)
            brightestStar = (std::max)(brightestStar, Sample(effect, x, y)[1]);
    Require(brightestStar > 145, "Health bar is missing GPU star highlights");
    size_t brightTitle = 0;
    for (size_t y = raster.height / 5; y < 2 * raster.height / 5; ++y)
        for (size_t x = raster.width / 10; x < raster.width / 2; ++x)
        {
            const auto* pixel = Sample(raster, x, y);
            if (pixel[0] > 170 && pixel[1] > 170 && pixel[2] > 170) ++brightTitle;
        }
    Require(brightTitle > 20, "Canvas raster is missing the ImGui title");
    std::cout << "Canvas " << static_cast<std::uint64_t>(frame.Id) << " quality " << static_cast<int>(frame.Quality)
        << " raster " << raster.width << 'x' << raster.height << " effects " << effect.width << 'x' << effect.height
        << " RGBA8 bytes " << raster.width * raster.height * 4 << '\n';
}

static unsigned DifferentSamples(const Image& left, const Image& right)
{
    unsigned changed = 0;
    for (size_t y = 1; y < 20; ++y)
        for (size_t x = 1; x < 20; ++x)
        {
            const auto* a = Sample(left, x * left.width / 20, y * left.height / 20);
            const auto* b = Sample(right, x * right.width / 20, y * right.height / 20);
            const int delta = std::abs(int(a[0]) - b[0]) + std::abs(int(a[1]) - b[1]) + std::abs(int(a[2]) - b[2]);
            if (delta > 12) ++changed;
        }
    return changed;
}

static void RequireIdentical(const ScratchImage& a, const ScratchImage& b)
{
    const auto& left = *a.GetImage(0, 0, 0);
    const auto& right = *b.GetImage(0, 0, 0);
    Require(left.width == right.width && left.height == right.height, "Identical frame inputs changed output extent");
    for (size_t y = 0; y < left.height; ++y)
        Require(std::memcmp(Sample(left, 0, y), Sample(right, 0, y), left.width * 4) == 0,
            "Identical frame inputs changed output between DIRECT and COMPUTE");
}

static void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 index = 0; index < info.GetNumStoredMessages(); ++index)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(index, nullptr, &size));
        std::vector<unsigned char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        ThrowIfFailed(info.GetMessage(index, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        const bool cpuOnly = mode == "--cpu";
        const bool gbv = mode == "--gpu-validation";
        Require(argc == 1 || cpuOnly || gbv, "Unknown WorldCanvasTests option");
        const auto fixtures = BuildFixtures();
        std::cout << "Canvas profile, CVar, revisions and independent frame ownership passed\n";
        if (cpuOnly) return 0;

        TestApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        CanvasEffects effects(device);
        OffscreenRenderer raster(device, 2);
        auto& direct = *device.GetCommandQueue();
        auto* compute = device.TryGetComputeQueue();
        Require(compute != nullptr, "GPU fixture requires a COMPUTE queue");
        auto qualityChange = RenderQualityChange(device, effects, raster, *fixtures.Low[0], *fixtures.Medium[0]);
        std::array<Images, 2> low{std::move(qualityChange[0]), RenderFrame(device, direct, effects, raster, *fixtures.Low[1])};
        for (size_t i = 0; i < low.size(); ++i)
        {
            const auto animated = RenderFrame(device, *compute, effects, raster, *fixtures.Animated[i]);
            const auto medium = i == 0 ? std::move(qualityChange[1]) : RenderFrame(device, *compute, effects, raster, *fixtures.Medium[i]);
            CheckOutput(low[i], *fixtures.Low[i]);
            CheckOutput(animated, *fixtures.Animated[i]);
            CheckOutput(medium, *fixtures.Medium[i]);
            const auto& first = *low[i].Effects.GetImage(0, 0, 0);
            Require(DifferentSamples(first, *animated.Effects.GetImage(0, 0, 0)) > 20,
                "Poster effect did not animate");
            Require(DifferentSamples(first, *medium.Effects.GetImage(0, 0, 0)) > 4,
                "GPU output did not respond to the quality profile");
        }
        Require(DifferentSamples(*low[0].Effects.GetImage(0, 0, 0), *low[1].Effects.GetImage(0, 0, 0)) > 20,
            "Independent poster canvases have indistinguishable effects");
        const auto replay = RenderFrame(device, *compute, effects, raster, *fixtures.Low[0]);
        RequireIdentical(low[0].Effects, replay.Effects);
        RequireIdentical(low[0].Raster, replay.Raster);
        CheckDebug(*info.Get());
        std::cout << "Two animated posters, real GPU LOD and COMPUTE-to-DIRECT composition passed\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
