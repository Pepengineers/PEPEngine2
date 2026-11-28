#include "Engine.RendererDX12/RenderPasses/GDX12DLSSUpscalePass.h"
#include "Common/RuntimePaths.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <ngx/nvsdk_ngx_helpers.h>
#include <ShlObj.h>
#include <memory>
#include <mutex>
#include <stdexcept>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace
{
    // NGX's D3D12 entry points share process state. Each pass owns its own parameter map and feature.
    std::mutex NgxMutex;
    std::unordered_map<ID3D12Device*, size_t> NgxDevices;
    constexpr auto EngineVersion = "1.0.0";
    constexpr auto EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;

    std::wstring GetNGXDataDirectory()
    {
        PWSTR path = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> localAppData(path, CoTaskMemFree);
        if (FAILED(result))
            throw std::runtime_error("DLSS NGX could not locate LocalAppData (HRESULT " +
                std::to_string(static_cast<unsigned>(result)) + ")");

        const auto directory = std::filesystem::path(localAppData.get()) / L"PEPEngine2" / L"NGX";
        std::filesystem::create_directories(directory);
        return directory.wstring();
    }

    void CheckNGX(NVSDK_NGX_Result result, const char* operation)
    {
        if (NVSDK_NGX_FAILED(result))
            throw std::runtime_error(std::string("DLSS NGX ") + operation + " failed (" +
                std::to_string(static_cast<unsigned>(result)) + ")");
    }
}

GDX12DLSSUpscalePass::GDX12DLSSUpscalePass(NVSDK_NGX_PerfQuality_Value quality) : _quality(quality)
{
    _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER;
    _numInputs = 3;
    _numOutputs = 1;
    _outputs = {&_output};
}

GDX12DLSSUpscalePass::~GDX12DLSSUpscalePass() { ReleaseNGX(); }

void GDX12DLSSUpscalePass::Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common)
{
    GDX12RenderPass::Setup(resources, common);
    auto* device = resources->Device->GetDevice().Get();
    const auto runtimeDirectory = Engine::Core::GetExecutableDirectory().wstring();
    const auto dataDirectory = GetNGXDataDirectory();
    const wchar_t* paths[] = {runtimeDirectory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Length = _countof(paths);
    info.PathListInfo.Path = paths;
    NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {GDX12StreamlineSDK::ProjectId, EngineType, EngineVersion};
    discovery.ApplicationDataPath = dataDirectory.c_str();
    discovery.FeatureInfo = &info;

    auto* adapter = resources->Device->GetAdapter();
    {
        std::scoped_lock lock(NgxMutex);
        NVSDK_NGX_FeatureRequirement requirement = {};
        CheckNGX(NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, &discovery, &requirement), "adapter query");
        if (requirement.FeatureSupported != 0)
            throw std::runtime_error("DLSS is unavailable on the post adapter (NGX requirements " +
                std::to_string(static_cast<unsigned>(requirement.FeatureSupported)) + ")");

        auto [entry, inserted] = NgxDevices.try_emplace(device, 0);
        if (inserted)
        {
            const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(GDX12StreamlineSDK::ProjectId,
                EngineType, EngineVersion, dataDirectory.c_str(), device, &info, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result)) NgxDevices.erase(entry);
            CheckNGX(result, "device initialization");
        }
        ++entry->second;
        _device = device;
        CheckNGX(NVSDK_NGX_D3D12_GetCapabilityParameters(&_parameters), "capability parameters");
    }
    QueryRenderTargetResolution();
}

void GDX12DLSSUpscalePass::QueryRenderTargetResolution()
{
    UINT width = 0, height = 0, maxWidth, maxHeight, minWidth, minHeight;
    float sharpness = 0.0f;
    std::scoped_lock lock(NgxMutex);
    CheckNGX(NGX_DLSS_GET_OPTIMAL_SETTINGS(_parameters, _commonData->WindowWidth,
        _commonData->WindowHeight, _quality, &width, &height, &maxWidth, &maxHeight,
        &minWidth, &minHeight, &sharpness), "optimal resolution");
    if (!width || !height) throw std::runtime_error("DLSS returned an empty render grid");
    _commonData->DownscaledWidth = width;
    _commonData->DownscaledHeight = height;
}

void GDX12DLSSUpscalePass::Initialize()
{
    const auto format = _inputs[2]->GetTexture()->GetFormat();
    if (format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        throw std::invalid_argument("DLSS requires linear RGBA16F HDR input");
    const auto width = _commonData->WindowWidth, height = _commonData->WindowHeight;
    if (!_output.IsInitialized())
        GDX12RenderHelpers::InitializeComputeTexture(_output, *_resources, width, height, format);
    else
    {
        const auto allocated = _output.GetResource()->D3DResource->GetDesc();
        if (allocated.Width != width || allocated.Height != height) _output.Resize(width, height);
    }
    CreateFeature();
}

void GDX12DLSSUpscalePass::CreateFeature()
{
    NVSDK_NGX_DLSS_Create_Params params = {};
    params.Feature.InWidth = _commonData->DownscaledWidth;
    params.Feature.InHeight = _commonData->DownscaledHeight;
    params.Feature.InTargetWidth = _commonData->WindowWidth;
    params.Feature.InTargetHeight = _commonData->WindowHeight;
    params.Feature.InPerfQualityValue = _quality;
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    auto* queue = _resources->Device->GetCommandQueue();
    GDX12CommandList* commands = nullptr;
    try
    {
        commands = queue->GetCommandList();
        NVSDK_NGX_Handle* feature = nullptr;
        NVSDK_NGX_Result result;
        {
            std::scoped_lock lock(NgxMutex);
            result = NGX_D3D12_CREATE_DLSS_EXT(commands->GetCommandList().Get(), 1, 1, &feature, _parameters, &params);
        }
        if (NVSDK_NGX_FAILED(result)) ThrowIfFailed(_device->GetDeviceRemovedReason());
        CheckNGX(result, "feature creation");
        _feature = feature;
        queue->ExecuteCommandList(commands);
    }
    catch (const DxException& error)
    {
        if (commands && !queue->DiscardCommandList(commands)) throw;
        ReleaseFeature();
        if (error.ErrorCode == DXGI_ERROR_DEVICE_REMOVED || error.ErrorCode == DXGI_ERROR_DEVICE_RESET ||
            error.ErrorCode == DXGI_ERROR_DEVICE_HUNG || error.ErrorCode == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
            throw;
        ThrowIfFailed(_device->GetDeviceRemovedReason());
        throw std::runtime_error("DLSS feature preparation failed: " + WStringToString(error.ToString()));
    }
    catch (...)
    {
        if (commands && queue->DiscardCommandList(commands)) ReleaseFeature();
        throw;
    }
    queue->CPUWaitForFenceValue(commands->FenceValue);
    _historyRevision = UINT64_MAX;
}

void GDX12DLSSUpscalePass::Resize()
{
    ReleaseFeature();
    _output.Resize(_commonData->WindowWidth, _commonData->WindowHeight);
    CreateFeature();
}

void GDX12DLSSUpscalePass::Execute(GDX12CommandList* commands)
{
    auto* depth = _inputs[0]->GetTexture();
    auto* motion = _inputs[1]->GetTexture();
    auto* color = _inputs[2]->GetTexture();
    const UINT width = _commonData->DownscaledWidth, height = _commonData->DownscaledHeight;
    if (color->GetWidth() != width || color->GetHeight() != height ||
        depth->GetWidth() != width || depth->GetHeight() != height ||
        motion->GetWidth() != width || motion->GetHeight() != height)
        throw std::invalid_argument("DLSS color, depth and motion must share the render grid");

    GDX12_SCOPED_EVENT(commands, "DLSS HDR Upscale", Colors::Blue);
    commands->ResourceBarrier({depth->GetResource()->GetNonPixelShaderResourceBarrier(),
        motion->GetResource()->GetNonPixelShaderResourceBarrier(), color->GetResource()->GetNonPixelShaderResourceBarrier(),
        _output.GetResource()->GetUnorderedAccessBarrier()});

    NVSDK_NGX_D3D12_DLSS_Eval_Params params = {};
    params.Feature.pInColor = color->GetResource()->D3DResource.Get();
    params.Feature.pInOutput = _output.GetResource()->D3DResource.Get();
    params.pInDepth = depth->GetResource()->D3DResource.Get();
    params.pInMotionVectors = motion->GetResource()->D3DResource.Get();
    params.InRenderSubrectDimensions = {width, height};
    params.InJitterOffsetX = _commonData->ActiveCameraJitterOffsetX;
    params.InJitterOffsetY = _commonData->ActiveCameraJitterOffsetY;
    // Opaque motion is unjittered prevNDC - currentNDC. NGX expects render pixels, positive Y down.
    params.InMVScaleX = 0.5f * width;
    params.InMVScaleY = -0.5f * height;
    params.InReset = _commonData->ResetUpscalerHistory || _commonData->ResetHistory ||
        _historyRevision != _commonData->HistoryRevision;
    NVSDK_NGX_Result result;
    {
        std::scoped_lock lock(NgxMutex);
        result = NGX_D3D12_EVALUATE_DLSS_EXT(commands->GetCommandList().Get(), _feature, _parameters, &params);
    }
    commands->InvalidateCachedState();
    CheckNGX(result, "evaluation");
    commands->ResourceBarrier({_output.GetResource()->GetUAVBarrier()});
    _historyRevision = _commonData->HistoryRevision;
}

void GDX12DLSSUpscalePass::ReleaseFeature()
{
    if (!_feature) return;
    // The reconstruction owner drains the queues before changing or destroying passes.
    std::scoped_lock lock(NgxMutex);
    NVSDK_NGX_D3D12_ReleaseFeature(_feature);
    _feature = nullptr;
}

void GDX12DLSSUpscalePass::ReleaseNGX()
{
    if (!_device) return;
    ReleaseFeature();
    std::scoped_lock lock(NgxMutex);
    if (_parameters) NVSDK_NGX_D3D12_DestroyParameters(_parameters);
    _parameters = nullptr;
    auto entry = NgxDevices.find(_device);
    // FG can still use the registered device after the last SR pass on it goes away.
    if (--entry->second == 0 && !GDX12StreamlineSDK::Get().IsRegisteredDevice(_device))
    {
        NVSDK_NGX_D3D12_Shutdown1(_device);
        NgxDevices.erase(entry);
    }
    _device = nullptr;
}

void GDX12DLSSUpscalePass::ShutdownStreamlineDevice(ID3D12Device* device)
{
    std::scoped_lock lock(NgxMutex);
    auto entry = NgxDevices.find(device);
    if (entry == NgxDevices.end()) return;
    assert(entry->second == 0);
    // sl.common may already have shut down NGX. This also covers Streamline sessions without NGX.
    NVSDK_NGX_D3D12_Shutdown1(device);
    NgxDevices.erase(entry);
}

void GDX12DLSSUpscalePass::ClearDenendencies()
{
    ReleaseNGX();
    GDX12RenderPass::ClearDenendencies();
}
