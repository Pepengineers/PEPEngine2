#include "SceneCloudPicture.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/ConsoleVariables.h"
#include <optional>

namespace ConsoleVariableDetail {
template <>
bool ParseValue(std::string_view text, BistroRender::CloudPictureMode& mode) {
    using Mode = BistroRender::CloudPictureMode;
    if (text == "true") mode = Mode::CurrentFrame;
    else if (text == "false") mode = Mode::Independent;
    else {
        int value{};
        if (!ParseValue(text, value)) return false;
        mode = static_cast<Mode>(value);
    }
    return true;
}
}

namespace BistroRender {
namespace {
AutoConsoleVariable<bool> Enabled(L"App.CloudPicture.Enabled", true, L"Show the cloud picture in Bistro.");
AutoConsoleVariable<int> Width(L"App.CloudPicture.Width", 1024, L"Cloud picture texture width, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Height(L"App.CloudPicture.Height", 512, L"Cloud picture texture height, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Adapter(L"App.CloudPicture.Adapter", -1,
    L"Cloud Picture GPU in the Render.PrimaryAdapter hardware list. -1 uses the primary GPU.",
    EConsoleVariableFlags::Default, [](int v) { return v >= -1; });
AutoConsoleVariable<CloudPictureMode> CrossSync(L"App.CloudPicture.CrossSync", CloudPictureMode::PreviousFrame,
    L"Remote clouds: 0 imports the previous frame, 1 the current frame, 2 the latest completed independent export.");
struct Settings {
    bool Active;
    int Width, Height, Adapter;
    CloudPictureMode Mode;
    bool operator==(const Settings& other) const {
        return Active == other.Active && Width == other.Width && Height == other.Height &&
            Adapter == other.Adapter && Mode == other.Mode;
    }
};
Settings Requested() {
    const int adapter = Adapter.GetValue();
    const auto mode = adapter >= 0 ? CrossSync.GetValue() : CloudPictureMode::Local;
    return {Enabled.GetValue(), Width.GetValue(), Height.GetValue(), adapter, mode};
}

bool SameAdapter(LUID a, LUID b) {
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

std::unique_ptr<GDX12Device> OpenSecondary(GDX12Device& primary, int index) {
    if (index < 0) return {};
    const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
    if (size_t(index) >= adapters.size())
        throw std::invalid_argument("Cloud Picture hardware adapter index is out of range");
    const auto& selected = adapters[index];
    if (SameAdapter(selected.AdapterLuid, primary.GetDevice()->GetAdapterLuid()))
        throw std::invalid_argument("Cloud Picture requires a different physical adapter LUID");
    auto device = std::make_unique<GDX12Device>();
    ThrowIfFailed(device->Initialize(selected.Adapter));
    if (!SameAdapter(device->GetDevice()->GetAdapterLuid(), selected.AdapterLuid))
        throw std::runtime_error("Cloud Picture device LUID differs from the selected DXGI adapter");
    return device;
}
}
struct SceneCloudPicture::Impl {
    GDX12DeviceResources& Resources;
    std::unique_ptr<CloudPictureTexture> Texture;
    std::optional<Settings> Attempted;
    int ExecutorAdapter = -1;
    GDX12Texture* CurrentImage = nullptr;

    explicit Impl(RenderPipelineContext& c) : Resources(c.Primary) {}
};
SceneCloudPicture::SceneCloudPicture(RenderPipelineContext& c) : _impl(std::make_shared<Impl>(c)) {}
SceneCloudPicture::~SceneCloudPicture() = default;
void SceneCloudPicture::UpdateSettings(std::unique_ptr<Executor>& executor) {
    auto& p = *_impl;
    const auto next = Requested();
    if (p.Attempted && next == *p.Attempted) return;
    p.Attempted = next;
    const int adapter = next.Active ? next.Adapter : -1;
    std::unique_ptr<Executor> replacementExecutor;
    std::unique_ptr<CloudPictureTexture> replacement;
    const auto reject = [&](const auto& reason) {
        Log::Error("RenderGraph", "CloudPicture adapter ", adapter, " settings rejected: ", reason);
    };
    try {
        if (adapter != p.ExecutorAdapter) {
            auto submit = std::make_shared<QueueSubmission>(*p.Resources.Device,
                OpenSecondary(*p.Resources.Device, adapter));
            replacementExecutor = std::make_unique<Executor>(submit, 0, UINT(p.Resources.FrameConstants.size()));
        }
        if (next.Active) {
            replacement = std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height,
                replacementExecutor ? replacementExecutor->Submissions() : executor->Submissions(), next.Mode);
        }
    } catch (const DxException& error) {
        reject(error.ToString());
    } catch (const std::exception& error) {
        reject(error.what());
    }

    if (next.Active && !replacement) {
        if (p.Texture || adapter < 0) return;
        const auto fallbackFailed = [](const auto& reason) {
            Log::Error("RenderGraph", "CloudPicture local fallback failed. Picture disabled: ", reason);
        };
        try {
            p.Texture = std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height,
                executor->Submissions(), CloudPictureMode::Local);
        } catch (const DxException& error) {
            fallbackFailed(error.ToString());
        } catch (const std::exception& error) {
            fallbackFailed(error.what());
        }
        return;
    }
    // Disabling retires the picture even if the current executor has to stay.
    if (p.Texture) p.Texture->Stop();
    executor->Drain();
    p.CurrentImage = nullptr;
    p.Texture = std::move(replacement);
    if (replacementExecutor) {
        executor = std::move(replacementExecutor);
        p.ExecutorAdapter = adapter;
    }
}
void SceneCloudPicture::Prepare(UINT slot, double time) {
    auto& p = *_impl;
    if (p.Texture) p.Texture->Prepare(slot, time);
    p.CurrentImage = p.Texture ? p.Texture->Texture(slot) : nullptr;
}
GDX12Texture* SceneCloudPicture::Image() const { return _impl->CurrentImage; }
bool SceneCloudPicture::IsRemote() const { return _impl->Texture && _impl->Texture->IsRemote(); }
void SceneCloudPicture::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings) {
    if (_impl->CurrentImage) _impl->Texture->Append(graph, bindings);
}
bool SceneCloudPicture::Record(Id id, ID3D12GraphicsCommandList* cmd) { return _impl->CurrentImage && _impl->Texture->Record(id, cmd); }
std::vector<PacketWait> SceneCloudPicture::Waits(const Schedule& schedule) {
    return _impl->CurrentImage ? _impl->Texture->Waits(schedule) : std::vector<PacketWait>{};
}
void SceneCloudPicture::Submitted(const std::vector<CompletionToken>& tokens) {
    if (_impl->Texture) _impl->Texture->Submitted(tokens);
}
void SceneCloudPicture::Presented() {
    if (_impl->CurrentImage) _impl->Texture->Presented();
}
}
