#include "SceneCloudPicture.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/ConsoleVariables.h"
#include <optional>

namespace BistroRender {
namespace {
AutoConsoleVariable<bool> Enabled(L"App.CloudPicture.Enabled", true, L"Show the MGPU-Noise cloud picture in Bistro.");
AutoConsoleVariable<int> Width(L"App.CloudPicture.Width", 1024, L"Cloud picture texture width, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Height(L"App.CloudPicture.Height", 512, L"Cloud picture texture height, independent of window/SR.",
    EConsoleVariableFlags::Default, [](int v) { return v > 0 && v <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; });
AutoConsoleVariable<int> Adapter(L"App.CloudPicture.Adapter", -1, L"-1 primary; otherwise explicit DXGI hardware adapter index.",
    EConsoleVariableFlags::Default, [](int v) { return v >= -1; });
struct Settings {
    bool Active;
    int Width, Height, Adapter;
    bool operator==(const Settings& other) const {
        return Active == other.Active && Width == other.Width && Height == other.Height && Adapter == other.Adapter;
    }
};
Settings Requested() { return {Enabled.GetValue(), Width.GetValue(), Height.GetValue(), Adapter.GetValue()}; }
}
struct SceneCloudPicture::Impl {
    GDX12DeviceResources& Resources;
    std::unique_ptr<CloudPictureTexture> Texture;
    std::optional<Settings> Attempted;
    bool Active = false;
    GDX12Texture* CurrentImage = nullptr;

    explicit Impl(RenderPipelineContext& c) : Resources(c.Primary) {}
};
SceneCloudPicture::SceneCloudPicture(RenderPipelineContext& c) : _impl(std::make_shared<Impl>(c)) {}
SceneCloudPicture::~SceneCloudPicture() = default;
bool SceneCloudPicture::UpdateSettings(const std::function<std::shared_ptr<QueueSubmission>(int)>& select,
                                       const std::function<void()>& drain) {
    auto& p = *_impl;
    const auto next = Requested();
    if (p.Attempted && next == *p.Attempted) return false;
    p.Attempted = next;
    if (!next.Active) { p.Active = false; return false; }
    std::unique_ptr<CloudPictureTexture> replacement;
    try {
        auto submit = select(next.Adapter);
        replacement = std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height,
                                                            submit, submit->HasSecondary());
    } catch (const std::exception& error) {
        Log::Error("RenderGraph", "Cloud picture settings rejected; retaining last valid generation: ", error.what());
        return false;
    }
    drain();
    if (p.Texture) p.Texture->Drain();
    p.Texture = std::move(replacement);
    p.Active = true;
    return true;
}
void SceneCloudPicture::Prepare(UINT slot, double time) {
    auto& p = *_impl;
    if (p.Active) p.Texture->Prepare(slot, time);
    p.CurrentImage = p.Active ? p.Texture->Texture(slot) : nullptr;
}
GDX12Texture* SceneCloudPicture::Image() const { return _impl->CurrentImage; }
void SceneCloudPicture::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings) {
    if (_impl->Active) _impl->Texture->Append(graph, bindings);
}
bool SceneCloudPicture::Record(Id id, ID3D12GraphicsCommandList* cmd) { return _impl->Active && _impl->Texture->Record(id, cmd); }
std::vector<PacketWait> SceneCloudPicture::Waits(const Schedule& schedule) const {
    return _impl->Active ? _impl->Texture->Waits(schedule) : std::vector<PacketWait>{};
}
void SceneCloudPicture::RetireFrame(const Schedule& schedule, const std::vector<CompletionToken>& tokens, bool success) {
    if (_impl->Active) _impl->Texture->RetireFrame(schedule, tokens, success);
}
void SceneCloudPicture::Drain() { if (_impl->Texture) _impl->Texture->Drain(); }
}
