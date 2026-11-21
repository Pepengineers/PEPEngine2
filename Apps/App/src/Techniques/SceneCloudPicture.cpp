#include "SceneCloudPicture.h"
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
    L"Cloud Picture GPU in the Render.PrimaryAdapter hardware list. -1 uses the primary GPU.");
AutoConsoleVariable<CloudPictureMode> CrossSync(L"App.CloudPicture.CrossSync", CloudPictureMode::PreviousFrame,
    L"Remote clouds: 0 exact previous (first uses current), 1 exact current, 2 independent GPU1 and latest completed (first can wait for export).");
struct Settings {
    bool Active;
    int Width, Height;
    Id Device;
    CloudPictureMode Mode;
    bool operator==(const Settings& other) const {
        return Active == other.Active && Width == other.Width && Height == other.Height &&
            Device == other.Device && Mode == other.Mode;
    }
};
}
struct SceneCloudPicture::Impl {
    GDX12DeviceResources& Resources;
    std::unique_ptr<CloudPictureTexture> Texture;
    std::optional<Settings> Attempted;
    Id ExecutionDevice = 0;
    GDX12Texture* CurrentImage = nullptr;

    explicit Impl(RenderPipelineContext& c) : Resources(c.Primary) {}
};
SceneCloudPicture::SceneCloudPicture(RenderPipelineContext& c) : _impl(std::make_shared<Impl>(c)) {}
SceneCloudPicture::~SceneCloudPicture() = default;
void SceneCloudPicture::UpdateSettings(Executor& executor) {
    auto& p = *_impl;
    const auto& submit = executor.Submissions();
    const bool enabled = Enabled.GetValue();
    const Id device = enabled ? submit->SelectAdapter(Adapter.GetValue()) : 0;
    const Settings next{enabled, Width.GetValue(), Height.GetValue(), device,
        device ? CrossSync.GetValue() : CloudPictureMode::Local};
    if (p.Attempted && next == *p.Attempted) return;
    p.Attempted = next;

    const auto createPicture = [&](Id target, CloudPictureMode mode) -> std::unique_ptr<CloudPictureTexture> {
        try {
            return std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height, submit, target, mode);
        } catch (const DxException& error) {
            Log::Warning("RenderGraph", "CloudPicture settings rejected: ", error.ToString());
        } catch (const std::exception& error) {
            Log::Warning("RenderGraph", "CloudPicture settings rejected: ", error.what());
        }
        return {};
    };

    auto replacement = next.Active ? createPicture(device, next.Mode) : nullptr;
    Id executionDevice = device;
    if (next.Active && !replacement && device) {
        replacement = createPicture(0, CloudPictureMode::Local);
        executionDevice = 0;
    }
    if (next.Active && !replacement) return;

    if (p.Texture) {
        p.Texture->Stop();
        executor.Drain();
    }
    p.CurrentImage = nullptr;
    p.Texture = std::move(replacement);
    p.ExecutionDevice = executionDevice;
}
Id SceneCloudPicture::ExecutionDevice() const { return _impl->ExecutionDevice; }
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
