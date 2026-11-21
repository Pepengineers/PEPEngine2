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
    int Width, Height, Adapter, AOAdapter, ParticlesAdapter, WaterAdapter;
    CloudPictureMode Mode;
    bool SamePicture(const Settings& other) const {
        return Active == other.Active && Width == other.Width && Height == other.Height &&
            Adapter == other.Adapter && Mode == other.Mode;
    }
};
Settings Requested(int aoAdapter, int particlesAdapter, int waterAdapter) {
    const int adapter = Adapter.GetValue();
    const auto mode = adapter >= 0 ? CrossSync.GetValue() : CloudPictureMode::Local;
    return {Enabled.GetValue(), Width.GetValue(), Height.GetValue(), adapter, aoAdapter, particlesAdapter, waterAdapter, mode};
}

bool SameAdapter(LUID a, LUID b) {
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

std::unique_ptr<GDX12Device> OpenSecondary(GDX12Device& primary, int index) {
    if (index < 0) return {};
    const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
    if (size_t(index) >= adapters.size())
        throw std::invalid_argument("Bistro secondary hardware adapter index is out of range");
    const auto& selected = adapters[index];
    if (SameAdapter(selected.AdapterLuid, primary.GetDevice()->GetAdapterLuid()))
        throw std::invalid_argument("Bistro requires a different physical adapter LUID");
    auto device = std::make_unique<GDX12Device>();
    ThrowIfFailed(device->Initialize(selected.Adapter));
    if (!SameAdapter(device->GetDevice()->GetAdapterLuid(), selected.AdapterLuid))
        throw std::runtime_error("Bistro secondary device LUID differs from the selected DXGI adapter");
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
void SceneCloudPicture::UpdateSettings(std::unique_ptr<Executor>& executor, int aoAdapter, int particlesAdapter, int waterAdapter) {
    auto& p = *_impl;
    const auto next = Requested(aoAdapter, particlesAdapter, waterAdapter);
    const bool samePicture = p.Attempted && next.SamePicture(*p.Attempted);
    if (samePicture && next.AOAdapter == p.Attempted->AOAdapter &&
        next.ParticlesAdapter == p.Attempted->ParticlesAdapter && next.WaterAdapter == p.Attempted->WaterAdapter) return;
    p.Attempted = next;
    const bool remoteCloud = next.Active && next.Adapter >= 0;
    const int sceneAdapter = aoAdapter >= 0 ? aoAdapter : particlesAdapter >= 0 ? particlesAdapter : waterAdapter;
    int adapter = remoteCloud ? next.Adapter : sceneAdapter;
    std::unique_ptr<Executor> replacementExecutor;
    const auto selectAdapter = [&](int index) {
        replacementExecutor.reset();
        if (index == p.ExecutorAdapter) return true;
        const auto reject = [&](const auto& reason) {
            Log::Warning("RenderGraph", "Bistro secondary adapter ", index, " settings rejected: ", reason);
        };
        try {
            auto submit = std::make_shared<QueueSubmission>(*p.Resources.Device,
                OpenSecondary(*p.Resources.Device, index));
            replacementExecutor = std::make_unique<Executor>(submit, 0, UINT(p.Resources.FrameConstants.size()));
            return true;
        } catch (const DxException& error) {
            reject(error.ToString());
        } catch (const std::exception& error) {
            reject(error.what());
        }
        return false;
    };
    const auto createPicture = [&](CloudPictureMode mode) -> std::unique_ptr<CloudPictureTexture> {
        try {
            return std::make_unique<CloudPictureTexture>(p.Resources, next.Width, next.Height,
                replacementExecutor ? replacementExecutor->Submissions() : executor->Submissions(), mode);
        } catch (const DxException& error) {
            Log::Warning("RenderGraph", "CloudPicture settings rejected: ", error.ToString());
        } catch (const std::exception& error) {
            Log::Warning("RenderGraph", "CloudPicture settings rejected: ", error.what());
        }
        return {};
    };

    const bool ready = selectAdapter(adapter);
    const bool keepPicture = samePicture && !replacementExecutor && (!remoteCloud || ready);
    std::unique_ptr<CloudPictureTexture> replacement;
    if (next.Active && !keepPicture) {
        if (!remoteCloud || ready) replacement = createPicture(next.Mode);
        if (remoteCloud && !replacement) {
            if (IsRemote()) {
                if (aoAdapter >= 0 && aoAdapter != p.ExecutorAdapter)
                    Log::Warning("RenderGraph", "Using local AO with the retained CloudPicture adapter");
                if (particlesAdapter >= 0 && particlesAdapter != p.ExecutorAdapter)
                    Log::Warning("RenderGraph", "Using local particles with the retained CloudPicture adapter");
                if (waterAdapter >= 0 && waterAdapter != p.ExecutorAdapter)
                    Log::Warning("RenderGraph", "Using local water with the retained CloudPicture adapter");
                return;
            }
            if (!ready || adapter != sceneAdapter) {
                adapter = sceneAdapter;
                selectAdapter(adapter);
            }
            if (!p.Texture) replacement = createPicture(CloudPictureMode::Local);
        }
    }

    const bool replacePicture = replacement || (!next.Active && p.Texture);
    // A retained remote picture still submits and imports tokens in its original domain.
    if (!replacePicture && IsRemote()) replacementExecutor.reset();

    const int effectiveAdapter = replacementExecutor ? adapter : p.ExecutorAdapter;
    if (aoAdapter >= 0 && aoAdapter != effectiveAdapter)
        Log::Warning("RenderGraph", "AO adapter ", aoAdapter, " unavailable in the Bistro submission domain; using local AO");
    if (particlesAdapter >= 0 && particlesAdapter != effectiveAdapter)
        Log::Warning("RenderGraph", "Particle adapter ", particlesAdapter, " unavailable in the Bistro submission domain; using local simulation");
    if (waterAdapter >= 0 && waterAdapter != effectiveAdapter)
        Log::Warning("RenderGraph", "Water adapter ", waterAdapter, " unavailable in the Bistro submission domain; using local SPH");

    if (!replacePicture && !replacementExecutor) return;
    // The primary terminal also retires any AO exchange attached to this domain.
    if (replacePicture && p.Texture) p.Texture->Stop();
    executor->Drain();
    if (replacePicture) {
        p.CurrentImage = nullptr;
        p.Texture = std::move(replacement);
    }
    if (replacementExecutor) {
        executor = std::move(replacementExecutor);
        p.ExecutorAdapter = adapter;
    }
}
int SceneCloudPicture::SubmissionAdapter() const { return _impl->ExecutorAdapter; }
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
