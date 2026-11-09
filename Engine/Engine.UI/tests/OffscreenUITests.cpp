#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.UI/OffscreenUI.h"
#include "Engine.UI/UI_test.h"
#include <d3d12sdklayers.h>
#include <imgui/imgui.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace Engine::UI;
using Pixel = std::array<unsigned char, 4>;
constexpr UINT Width = 64;
constexpr UINT Height = 48;
constexpr UITextureId ImageId{1234};
constexpr Pixel ScreenBackground{51, 102, 153, 255};
constexpr const char* AtlasText[] = {"A", "Wide"};

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class F>
static void Reject(F action, const char* message)
{
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

class TestApp final : public App
{
public:
    TestApp() : App(GetModuleHandleW(nullptr)) {}
};

static GDX12Submission::CompletionPoint Submit(GDX12CommandQueue& queue, GDX12CommandList& commands)
{
    ThrowIfFailed(commands.GetCommandList()->Close());
    ID3D12CommandList* lists[] = {commands.GetCommandList().Get()};
    queue.GetCommandQueue()->ExecuteCommandLists(1, lists);
    return queue.SignalCompletion();
}

static GDX12ResourceHandle MakeTarget(GDX12Device& device, UINT width, UINT height)
{
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, width, height, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const D3D12_CLEAR_VALUE clear{OffscreenFormat, {0, 0, 0, 0}};
    return device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
}

static GDX12ResourceHandle MakeImage(GDX12Device& device, Pixel pixel, DXGI_FORMAT format)
{
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, 1, 1, 1, 1);
    auto image = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(upload->Map(0, &noRead, &mapped));
    std::memcpy(mapped, pixel.data(), pixel.size());
    upload->Unmap(0, nullptr);

    GDX12CommandList commands(&device);
    const CD3DX12_TEXTURE_COPY_LOCATION src(upload.Get(), footprint), dst(image.Get(), 0u);
    commands.GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(image.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)});
    auto& queue = *device.GetCommandQueue();
    queue.CPUWaitForFenceValue(Submit(queue, commands).Value());
    return image;
}

struct Readback
{
    GDX12ResourceHandle Buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
    UINT64 Bytes = 0;

    void Record(GDX12Device& device, GDX12CommandList& commands, ID3D12Resource& source,
        D3D12_RESOURCE_STATES state)
    {
        const auto desc = source.GetDesc();
        device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &Footprint, nullptr, nullptr, &Bytes);
        Buffer = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(Bytes),
            D3D12_RESOURCE_STATE_COPY_DEST);
        commands.ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(&source, state, D3D12_RESOURCE_STATE_COPY_SOURCE)});
        const CD3DX12_TEXTURE_COPY_LOCATION src(&source, 0u), dst(Buffer.Get(), Footprint);
        commands.GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    std::vector<Pixel> Read() const
    {
        const auto& layout = Footprint.Footprint;
        std::vector<Pixel> pixels(static_cast<size_t>(layout.Width) * layout.Height);
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(Bytes)}, noWrite{0, 0};
        ThrowIfFailed(Buffer->Map(0, &range, &mapped));
        for (UINT y = 0; y < layout.Height; ++y)
            std::memcpy(pixels.data() + y * layout.Width,
                static_cast<const unsigned char*>(mapped) + Footprint.Offset + static_cast<size_t>(y) * layout.RowPitch,
                static_cast<size_t>(layout.Width) * sizeof(Pixel));
        Buffer->Unmap(0, &noWrite);
        return pixels;
    }
};

static void DrawFixture(const char* text)
{
    auto& draw = *ImGui::GetForegroundDrawList();
    draw.AddRectFilled({4, 4}, {20, 20}, IM_COL32(255, 0, 0, 128));
    draw.AddRectFilled({12, 12}, {28, 24}, IM_COL32(0, 0, 255, 128));
    draw.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    draw.PushClipRect({32, 4}, {38, 10}, true);
    draw.AddRectFilled({30, 2}, {44, 12}, IM_COL32(0, 255, 0, 255));
    draw.PopClipRect();
    draw.AddText({4, 27}, IM_COL32_WHITE, text);
}

static void ExpectPixel(Pixel actual, Pixel expected)
{
    for (size_t channel = 0; channel < actual.size(); ++channel)
        if (std::abs(int(actual[channel]) - int(expected[channel])) > 2)
        {
            std::cerr << "RGBA actual:";
            for (const auto value : actual) std::cerr << ' ' << int(value);
            std::cerr << " expected:";
            for (const auto value : expected) std::cerr << ' ' << int(value);
            std::cerr << '\n';
            throw std::runtime_error("Offscreen RGBA8 pixel differs from expected blend or texture sample");
        }
}

static void ExpectComposite(Pixel foreground, Pixel direct)
{
    Pixel composite{};
    for (size_t c = 0; c < composite.size(); ++c)
        composite[c] = static_cast<unsigned char>(foreground[c] +
            std::lround(ScreenBackground[c] * (255 - foreground[3]) / 255.0));
    ExpectPixel(direct, composite);
}

static void CheckGlyph(const std::vector<Pixel>& pixels, UINT scale, const std::vector<Pixel>& direct)
{
    unsigned covered = 0, edge = 0;
    bool bright = false, wide = false;
    for (UINT y = 27 * scale; y < 44 * scale; ++y)
        for (UINT x = 4 * scale; x < 36 * scale; ++x)
        {
            const auto index = y * Width * scale + x;
            const auto& pixel = pixels[index];
            ExpectComposite(pixel, direct[index]);
            if (pixel[3] == 0) continue;
            ++covered;
            if (pixel[3] < 255) ++edge;
            // Filtering near transparent atlas texels can lower RGB as well as coverage.
            Require(pixel[0] <= pixel[3] + 2, "Font edge is outside premultiplied range");
            ExpectPixel(pixel, {pixel[0], pixel[0], pixel[0], pixel[3]});
            bright |= pixel[0] >= 128;
            wide |= x >= 18 * scale;
        }
    Require(covered > 8 && edge > 0 && bright, "Font glyph coverage and fractional edge alpha must survive");
    Require(wide == (scale == 2), "Revised text did not change the glyph coverage");
}

static void GrowAtlas(ImFontAtlas& atlas)
{
    const int revision = atlas.TexData->UniqueID;
    Require(atlas.AddCustomRect(512, 512) != ImFontAtlasRectId_Invalid &&
        atlas.TexData->UniqueID != revision, "Font atlas test did not force a new generation");
}

static std::array<std::vector<Pixel>, 2> TestScreen(GDX12Device& device)
{
    struct Screen
    {
        HWND Window = CreateWindowExW(0, L"STATIC", L"Offscreen UI test", WS_POPUP,
            0, 0, Width, Height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Screen() { ShutdownUI(); if (Window) DestroyWindow(Window); }
    } screen;
    Require(screen.Window != nullptr, "Cannot create hidden Win32 UI fixture");
    Initialize(screen.Window, &device, 2, OffscreenFormat);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().Fonts->AddFontDefaultVector();
#ifdef _DEBUG
    Require(HandleMessage(screen.Window, WM_KEYUP, VK_F1, 0), "Screen F1 toggle stopped handling input");
#endif
    HandleMessage(screen.Window, WM_KEYDOWN, 'A', 0x001E0001);
    auto* screenContext = ImGui::GetCurrentContext();
    OffscreenRenderer renderer(device, 2);
    Require(ImGui::GetCurrentContext() == screenContext, "Offscreen initialization replaced the screen context");

    GDX12DescriptorHeap rtv(&device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    std::array<std::vector<Pixel>, 2> references;
    for (UINT scale = 1; scale <= references.size(); ++scale)
    {
        BeginFrame();
        if (scale == 1) Require(ImGui::IsKeyDown(ImGuiKey_A), "Screen Win32 input did not reach ImGui");
        else GrowAtlas(*ImGui::GetIO().Fonts);
        DrawFixture(AtlasText[scale - 1]);
        auto frame = CaptureScreenFrame();
        Require(frame != nullptr, "Screen layout was not captured");
        const auto scaleFloat = static_cast<float>(scale);
        ImGui::GetDrawData()->FramebufferScale = {scaleFloat, scaleFloat};

        GDX12Texture screenTarget;
        GDX12TextureDesc desc{};
        desc.RTVHeap = &rtv;
        desc.Width = Width * scale;
        desc.Height = Height * scale;
        desc.Format = OffscreenFormat;
        desc.CreateSRV = false;
        desc.CreateRTV = true;
        desc.RTVHeapIndex = 0;
        desc.RTVDesc.Format = OffscreenFormat;
        desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        desc.ClearValue = {0.2f, 0.4f, 0.6f, 1};
        screenTarget.Initialize(desc);

        GDX12CommandList commands(&device);
        auto target = MakeTarget(device, desc.Width, desc.Height);
        auto recording = renderer.TryRecord(commands, target, frame);
        Require(recording && ImGui::GetCurrentContext() == screenContext, "Offscreen recording disturbed the screen backend");
        Readback offscreen, direct;
        offscreen.Record(device, commands, *target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands.ResourceBarrier({screenTarget.GetResource()->GetRenderTargetBarrier()});
        commands.ClearRenderTargetView(&screenTarget);
        Render(&commands, &screenTarget);
        direct.Record(device, commands, *screenTarget.GetResource()->D3DResource.Get(), D3D12_RESOURCE_STATE_COMMON);
        auto& queue = *device.GetCommandQueue();
        queue.CPUWaitForFenceValue(Submit(queue, commands).Value());

        const auto foreground = offscreen.Read();
        auto& legacy = references[scale - 1];
        legacy = direct.Read();
        const auto sample = [&](UINT x, UINT y) { return foreground[(y * Width * scale + x) * scale]; };
        ExpectPixel(sample(0, 0), {});
        ExpectPixel(sample(6, 6), {128, 0, 0, 128});
        ExpectPixel(sample(15, 15), {64, 0, 128, 192});
        ExpectPixel(sample(35, 8), {0, 255, 0, 255});
        ExpectPixel(sample(39, 8), {});
        CheckGlyph(foreground, scale, legacy);
        for (size_t i = 0; i < foreground.size(); ++i)
            ExpectComposite(foreground[i], legacy[i]);
    }
    return references;
}

static std::array<std::shared_ptr<const DrawData>, 2> CaptureAtlasRevisions()
{
    struct Context
    {
        ImGuiContext* Value = ImGui::CreateContext();
        ~Context() { ImGui::DestroyContext(Value); }
    } context;
    ImGui::SetCurrentContext(context.Value);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {Width, Height};
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefaultVector();
    std::array<std::shared_ptr<const DrawData>, 2> frames;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        ImGui::NewFrame();
        if (i == 1) GrowAtlas(*io.Fonts);
        DrawFixture(AtlasText[i]);
        ImGui::GetForegroundDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(ImageId)), {45, 4}, {60, 20});
        ImGui::Render();
        frames[i] = CaptureDrawData(*ImGui::GetDrawData());
    }
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddCallback([](const ImDrawList*, const ImDrawCmd*) {}, nullptr);
    ImGui::Render();
    Reject([&] { CaptureDrawData(*ImGui::GetDrawData()); }, "Capture accepted a borrowed custom callback");
    return frames;
}

static void TestInflight(GDX12Device& device, std::array<std::shared_ptr<const DrawData>, 2> frames,
    const std::array<std::vector<Pixel>, 2>& direct)
{
    constexpr auto srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::array<GDX12ResourceHandle, 2> images{
        MakeImage(device, {0, 255, 0, 128}, OffscreenFormat), MakeImage(device, {128, 0, 0, 255}, srgb)};
    auto& queue = *device.GetCommandQueue();
    OffscreenRenderer renderer(device, 2);
    struct Frame
    {
        std::unique_ptr<GDX12CommandList> Commands;
        std::shared_ptr<const void> Owner;
        Readback Output;
        GDX12Submission::CompletionPoint Done;
    } recorded[2];
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
    } gate{queue};
    ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.Fence)));
    ThrowIfFailed(queue.GetCommandQueue()->Wait(gate.Fence.Get(), 1));
    const std::weak_ptr<const DrawData> oldDrawData = frames[0];
    for (UINT i = 0; i < 2; ++i)
    {
        const UINT scale = i + 1;
        auto& output = recorded[i];
        output.Commands = std::make_unique<GDX12CommandList>(&device);
        auto target = MakeTarget(device, Width * scale, Height * scale);
        auto* nativeTarget = target.Get();
        output.Owner = renderer.TryRecord(*output.Commands, std::move(target), std::move(frames[i]),
            {{ImageId, std::move(images[i]), i == 0 ? OffscreenFormat : srgb}});
        Require(output.Owner != nullptr, "Independent target could not be recorded during resize");
        output.Output.Record(device, *output.Commands, *nativeTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
        output.Done = Submit(queue, *output.Commands);
    }
    Require(!oldDrawData.expired() && !recorded[0].Done.IsReachedBy(queue.GetCompletionProgress()),
        "Resize fixture lost its draw data or was not in flight");
    GDX12CommandList retry(&device);
    const auto retryTarget = MakeTarget(device, Width, Height);
    Require(!renderer.TryRecord(retry, retryTarget, oldDrawData.lock()), "Live ring slot was overwritten");
    ThrowIfFailed(gate.Fence->Signal(1));
    queue.CPUWaitForFenceValue(recorded[1].Done.Value());
    gate.Fence.Reset();

    for (UINT i = 0; i < 2; ++i)
    {
        const UINT scale = i + 1;
        const auto pixels = recorded[i].Output.Read();
        const auto sample = [&](UINT x, UINT y) { return pixels[(y * Width * scale + x) * scale]; };
        ExpectPixel(sample(0, 0), {});
        ExpectPixel(sample(8, 8), {128, 0, 0, 128});
        ExpectPixel(sample(35, 8), {0, 255, 0, 255});
        ExpectPixel(sample(39, 8), {});
        ExpectPixel(sample(50, 10), i == 0 ? Pixel{0, 128, 0, 128} : Pixel{55, 0, 0, 255});
        CheckGlyph(pixels, scale, direct[i]);
    }
    auto replay = oldDrawData.lock();
    recorded[0].Owner.reset();
    auto replacement = MakeImage(device, {255, 255, 255, 255}, OffscreenFormat);
    auto recovered = renderer.TryRecord(retry, retryTarget, replay, {{ImageId, replacement}});
    Require(recovered != nullptr, "Retired UI slot was not reusable");
    queue.CPUWaitForFenceValue(Submit(queue, retry).Value());
    replay.reset();
    recovered.reset();
    Require(oldDrawData.expired(), "Retired draw data is still pinned");
}

static void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

static void TestForeignTexture(GDX12Device& primary, const std::shared_ptr<const DrawData>& frame)
{
    auto& factory = GDX12DeviceFactory::GetFactory();
    ComPtr<IDXGIAdapter4> foreignAdapter;
    const auto primaryLuid = primary.GetAdapterLuid();
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter4> candidate;
        const auto result = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(result);
        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(candidate->GetDesc1(&desc));
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            (desc.AdapterLuid.HighPart == primaryLuid.HighPart && desc.AdapterLuid.LowPart == primaryLuid.LowPart))
            continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr)))
        {
            foreignAdapter = candidate;
            break;
        }
    }
    if (!foreignAdapter) ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&foreignAdapter)));
    GDX12Device foreign;
    foreign.Role = DEVICE_ROLE_SECONDARY;
    ThrowIfFailed(foreign.Initialize(foreignAdapter));
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(foreign.GetDevice().As(&info));
    OffscreenRenderer renderer(foreign, 1);
    GDX12CommandList commands(&foreign);
    auto target = MakeTarget(foreign, Width, Height);
    const auto wrongImage = MakeImage(primary, {0, 0, 0, 255}, OffscreenFormat);
    Reject([&] { auto unused = renderer.TryRecord(commands, target, frame, {{ImageId, wrongImage}}); },
        "Secondary renderer accepted primary image binding");
    const auto image = MakeImage(foreign, {255, 0, 255, 255}, OffscreenFormat);
    auto owner = renderer.TryRecord(commands, target, frame, {{ImageId, image}});
    Require(owner != nullptr, "Foreign binding rejection left a partial recording");
    Readback output;
    output.Record(foreign, commands, *target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto& queue = *foreign.GetCommandQueue();
    queue.CPUWaitForFenceValue(Submit(queue, commands).Value());
    ExpectPixel(output.Read()[10 * Width + 50], {255, 0, 255, 255});
    CheckDebug(*info.Get());
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const bool gbv = argc == 2 && std::string_view(argv[1]) == "--gpu-validation";
        Require(argc == 1 || gbv, "Unknown OffscreenUITests option");
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
        std::cout << "Checking screen fallback and alpha composition\n";
        const auto direct = TestScreen(device);
        auto frames = CaptureAtlasRevisions();
        std::cout << "Checking device-local texture bindings\n";
        TestForeignTexture(device, frames[0]);
        std::cout << "Checking atlas revisions, resize and retirement\n";
        TestInflight(device, std::move(frames), direct);
        CheckDebug(*info.Get());
        std::cout << "Offscreen UI passed Win32 fallback, premultiplied composition, fonts, texture remap, resize and retirement.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
