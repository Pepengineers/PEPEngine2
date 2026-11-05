#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/ConsoleVariables.h"
#include "Common/GameTimer.h"
#include "ffx-api/ffx_framegeneration.h"
#include "ffx-api/dx12/ffx_api_framegeneration_dx12.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xell/xell_d3d12.h"
#ifdef free
#pragma push_macro("free")
#undef free
#define PEP_RESTORE_FREE_FG 1
#endif
#include "nvidia-sdk/sl_dlss_g.h"
#include "nvidia-sdk/sl_reflex.h"
#ifdef PEP_RESTORE_FREE_FG
#pragma pop_macro("free")
#undef PEP_RESTORE_FREE_FG
#endif
#include <atomic>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace
{
    AutoConsoleVariable<GDX12FrameGenerationProvider> Provider(L"Render.FrameGeneration.Provider", GDX12FrameGenerationProvider::Off,
        L"0 Off, 1 AMD, 2 NVIDIA, 3 Intel; unavailable providers remain Off with a reason",
        EConsoleVariableFlags::Default);

    void CheckAMD(const ffxReturnCode_t result, const char* operation)
    {
        if (result != FFX_API_RETURN_OK) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
    }
    void CheckIntel(const int result, const char* operation)
    {
        if (result < 0) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
    }
    void CheckSL(const sl::Result result, const char* operation)
    {
        if (result != sl::Result::eOk) throw std::runtime_error(std::string(operation) + ": " + GDX12StreamlineSDK::ResultToString(result));
    }
    HMODULE LoadRuntime(const wchar_t* name)
    {
        wchar_t exe[32768] = {};
        const auto length = GetModuleFileNameW(nullptr, exe, _countof(exe));
        if (!length || length >= _countof(exe)) throw std::runtime_error("Cannot locate executable directory");
        const auto path = std::filesystem::path(exe).parent_path() / name;
        auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) throw std::runtime_error("Optional FG runtime could not be loaded (" + std::to_string(GetLastError()) + ")");
        return module;
    }
    template<typename T> void Import(const HMODULE module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!function) throw std::runtime_error(std::string("Missing FG export: ") + name);
    }
    template<typename T> void ImportSL(const sl::Feature feature, const char* name, T*& function)
    {
        void* address = nullptr;
        CheckSL(GDX12StreamlineSDK::Get().GetFeatureFunction(feature, name, address), name);
        function = reinterpret_cast<T*>(address);
        if (!function) throw std::runtime_error(std::string("Missing Streamline function: ") + name);
    }
}

GDX12FrameGenerationProvider GetFrameGenerationProvider()
{
    return Provider.GetValue();
}

struct GDX12FrameGeneration::Impl
{
    GDX12Device* Device;
    GDX12FrameGenerationStatus Status;
    uint64_t Restart = 0, History = UINT64_MAX, FrameHistory = 0;
    uint32_t Frame = 0;
    bool Suspended = false, ActiveFrame = false, ResetNext = true, FrameReset = true, NvidiaLoaded = false, NvidiaOn = false, IntelReady = false;
    std::string PendingFailure;
    DXGI_SWAP_CHAIN_DESC1 Desc = {};
    sl::FrameToken* Token = nullptr;
    sl::ViewportHandle Viewport{0};
    PFun_slDLSSGSetOptions* DLSSGSetOptions = nullptr;
    PFun_slDLSSGGetState* DLSSGGetState = nullptr;
    PFun_slReflexSetOptions* ReflexSetOptions = nullptr;
    PFun_slReflexSleep* ReflexSleep = nullptr;
    PFun_slPCLSetMarker* PCLSetMarker = nullptr;
    bool ReflexChecked = false;

    ffxContext AMD = nullptr, AMDSwapChain = nullptr;
    IDXGISwapChain4* AMDChain = nullptr;
    bool AMDConfigured = false;
    std::atomic<uint32_t> AMDError{0};
    std::atomic<uint64_t> AMDGenerated{0};

    HMODULE XeFGModule = nullptr, XeLLModule = nullptr;
    xefg_swapchain_handle_t XeFG = nullptr;
    xell_context_handle_t XeLL = nullptr;
#define PEP_XEFG_FUNCTIONS(X) \
    X(xefgSwapChainD3D12CreateContext) X(xefgSwapChainSetLatencyReduction) \
    X(xefgSwapChainD3D12InitFromSwapChainDesc) X(xefgSwapChainD3D12GetSwapChainPtr) \
    X(xefgSwapChainSetEnabled) X(xefgSwapChainD3D12TagFrameResource) X(xefgSwapChainTagFrameConstants) \
    X(xefgSwapChainSetPresentId) X(xefgSwapChainGetLastPresentStatus) X(xefgSwapChainDestroy)
#define PEP_XELL_FUNCTIONS(X) X(xellD3D12CreateContext) X(xellSetSleepMode) X(xellSleep) X(xellAddMarkerData) X(xellDestroyContext)
#define PEP_DECLARE_FN(name) decltype(&name) name##Fn = nullptr;
    PEP_XEFG_FUNCTIONS(PEP_DECLARE_FN)
    PEP_XELL_FUNCTIONS(PEP_DECLARE_FN)
#undef PEP_DECLARE_FN

    explicit Impl(GDX12Device* device) : Device(device) {}
    ~Impl()
    {
        if (!XeFG && XeFGModule) FreeLibrary(XeFGModule);
        if (!XeLL && XeLLModule) FreeLibrary(XeLLModule);
    }

    void Fail(const std::string& reason)
    {
        if (PendingFailure.empty())
        {
            PendingFailure = reason;
            Status.FallbackReason = reason;
            Status.Effective = GDX12FrameGenerationProvider::Off;
            Log::Warning("FrameGeneration", reason, "; falling back to FG Off");
        }
    }

    sl::Result Supported(const sl::Feature feature) const
    {
        auto luid = Device->GetAdapterLuid();
        sl::AdapterInfo adapter{};
        adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
        adapter.deviceLUIDSizeInBytes = sizeof(luid);
        return GDX12StreamlineSDK::Get().IsFeatureSupported(feature, adapter);
    }

    void InitReflex()
    {
        if (ReflexChecked || !GDX12StreamlineSDK::Get().IsEnabled()) return;
        ReflexChecked = true;
        if (Supported(sl::kFeatureReflex) != sl::Result::eOk) return;
        try
        {
            ImportSL(sl::kFeatureReflex, "slReflexSetOptions", ReflexSetOptions);
            ImportSL(sl::kFeatureReflex, "slReflexSleep", ReflexSleep);
            ImportSL(sl::kFeaturePCL, "slPCLSetMarker", PCLSetMarker);
            CheckSL(ReflexSetOptions(sl::ReflexOptions{}), "Reflex Off");
        }
        catch (const std::exception& e)
        {
            ReflexSetOptions = nullptr; ReflexSleep = nullptr; PCLSetMarker = nullptr;
            Log::Warning("FrameGeneration", e.what());
        }
    }

    void NvidiaOptions(const bool enabled, GDX12Texture* depth = nullptr, GDX12Texture* motion = nullptr)
    {
        if (!NvidiaLoaded || !DLSSGSetOptions) return;
        sl::DLSSGOptions options{};
        options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = 1;
        options.numBackBuffers = Desc.BufferCount;
        options.colorWidth = Desc.Width; options.colorHeight = Desc.Height;
        options.colorBufferFormat = Desc.Format;
        if (depth && motion)
        {
            options.mvecDepthWidth = depth->GetWidth(); options.mvecDepthHeight = depth->GetHeight();
            options.depthBufferFormat = depth->GetFormat(); options.mvecBufferFormat = motion->GetFormat();
        }
        CheckSL(DLSSGSetOptions(Viewport, options), "DLSS-G options");
        NvidiaOn = enabled;
    }

    void Stop()
    {
        if (AMD && AMDConfigured)
        {
            ffxConfigureDescFrameGeneration off{};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = AMDChain;
            off.frameID = Frame;
            CheckAMD(ffxConfigure(&AMD, &off.header), "AMD FG Off");
            AMDConfigured = false;
        }
        if (AMDSwapChain)
        {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            CheckAMD(ffxDispatch(&AMDSwapChain, &wait.header), "AMD wait for presents");
        }
        if (XeFG && IntelReady) CheckIntel(xefgSwapChainSetEnabledFn(XeFG, false), "XeFG Off");
        if (NvidiaOn) NvidiaOptions(false);
        if (ReflexSetOptions) CheckSL(ReflexSetOptions(sl::ReflexOptions{}), "Reflex Off");
        ActiveFrame = false; Token = nullptr; ResetNext = true;
    }

    void DestroyAMD()
    {
        if (AMD) { CheckAMD(ffxDestroyContext(&AMD, nullptr), "Destroy AMD FG"); AMD = nullptr; }
        if (AMDSwapChain) { CheckAMD(ffxDestroyContext(&AMDSwapChain, nullptr), "Destroy AMD swapchain"); AMDSwapChain = nullptr; }
        AMDChain = nullptr;
    }
    void DestroyIntel()
    {
        if (XeFG) { CheckIntel(xefgSwapChainDestroyFn(XeFG), "Destroy XeFG"); XeFG = nullptr; IntelReady = false; }
        if (XeLL) { CheckIntel(xellDestroyContextFn(XeLL), "Destroy XeLL"); XeLL = nullptr; }
    }

    static ffxReturnCode_t DispatchAMD(ffxDispatchDescFrameGeneration* params, void* user) noexcept
    {
        auto& self = *static_cast<Impl*>(user);
        params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        const auto result = ffxDispatch(&self.AMD, &params->header);
        if (result == FFX_API_RETURN_OK) self.AMDGenerated.fetch_add(params->numGeneratedFrames, std::memory_order_relaxed);
        else self.AMDError.store(result, std::memory_order_relaxed);
        return result;
    }

    ComPtr<IDXGISwapChain4> CreateAMD(const HWND window)
    {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = Device->GetDevice().Get();
        ffxCreateContextDescFrameGenerationVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_FRAMEGENERATION_VERSION;
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &version.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        create.displaySize = create.maxRenderSize = { Desc.Width, Desc.Height };
        create.backBufferFormat = ffxApiGetSurfaceFormatDX12(Desc.Format);
        CheckAMD(ffxCreateContext(&AMD, &create.header, nullptr), "Create AMD FG");

        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 swapVersion{};
        swapVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        swapVersion.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        IDXGISwapChain4* chain = nullptr;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 swap{};
        swap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        swap.header.pNext = &swapVersion.header;
        swap.swapchain = &chain; swap.hwnd = window; swap.desc = &Desc;
        swap.dxgiFactory = GDX12DeviceFactory::GetFactory().Get();
        swap.gameQueue = Device->GetCommandQueue()->GetCommandQueue().Get();
        const auto result = ffxCreateContext(&AMDSwapChain, &swap.header, nullptr);
        ComPtr<IDXGISwapChain4> output; output.Attach(chain); AMDChain = chain;
        CheckAMD(result, "Create AMD FG swapchain");
        return output;
    }

    ComPtr<IDXGISwapChain4> CreateIntel(const HWND window)
    {
        if (!XeFGModule) XeFGModule = LoadRuntime(L"libxess_fg.dll");
        if (!XeLLModule) XeLLModule = LoadRuntime(L"libxell.dll");
#define PEP_IMPORT_XEFG(name) Import(XeFGModule, #name, name##Fn);
#define PEP_IMPORT_XELL(name) Import(XeLLModule, #name, name##Fn);
        PEP_XEFG_FUNCTIONS(PEP_IMPORT_XEFG)
        PEP_XELL_FUNCTIONS(PEP_IMPORT_XELL)
#undef PEP_IMPORT_XEFG
#undef PEP_IMPORT_XELL
        CheckIntel(xellD3D12CreateContextFn(Device->GetDevice().Get(), &XeLL), "Create XeLL");
        CheckIntel(xefgSwapChainD3D12CreateContextFn(Device->GetDevice().Get(), &XeFG), "Create XeFG");
        CheckIntel(xefgSwapChainSetLatencyReductionFn(XeFG, XeLL), "Connect XeFG/XeLL");
        xefg_swapchain_d3d12_init_params_t init{};
        init.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY;
        init.maxInterpolatedFrames = 1;
        init.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        CheckIntel(xefgSwapChainD3D12InitFromSwapChainDescFn(XeFG, window, &Desc, nullptr,
            Device->GetCommandQueue()->GetCommandQueue().Get(), GDX12DeviceFactory::GetFactory().Get(), &init), "Initialize XeFG swapchain");
        IntelReady = true;
        ComPtr<IDXGISwapChain4> output;
        CheckIntel(xefgSwapChainD3D12GetSwapChainPtrFn(XeFG, IID_PPV_ARGS(&output)), "Get XeFG swapchain");
        xell_sleep_params_t sleep{}; sleep.bLowLatencyMode = true;
        CheckIntel(xellSetSleepModeFn(XeLL, &sleep), "Enable XeLL low latency");
        CheckIntel(xefgSwapChainSetEnabledFn(XeFG, true), "Enable XeFG");
        return output;
    }

    void CreateNvidia()
    {
        auto& sl = GDX12StreamlineSDK::Get();
        const auto support = Supported(sl::kFeatureDLSS_G);
        if (support != sl::Result::eOk && support != sl::Result::eErrorFeatureMissing)
            CheckSL(support, "DLSS-G capability");
        CheckSL(sl.SetFeatureLoaded(sl::kFeatureDLSS_G, true), "Load DLSS-G");
        NvidiaLoaded = true;
        CheckSL(Supported(sl::kFeatureDLSS_G), "DLSS-G capability");
        InitReflex();
        if (!ReflexSetOptions || !ReflexSleep || !PCLSetMarker) throw std::runtime_error("DLSS-G requires supported Reflex/PCL");
        ImportSL(sl::kFeatureDLSS_G, "slDLSSGSetOptions", DLSSGSetOptions);
        ImportSL(sl::kFeatureDLSS_G, "slDLSSGGetState", DLSSGGetState);
        sl::ReflexOptions reflex{}; reflex.mode = sl::ReflexMode::eLowLatency;
        CheckSL(ReflexSetOptions(reflex), "Enable Reflex");
    }

    void Marker(const sl::PCLMarker nvidia, const xell_latency_marker_type_t intel)
    {
        if (Status.Effective == GDX12FrameGenerationProvider::Intel && XeLL)
            CheckIntel(xellAddMarkerDataFn(XeLL, Frame, intel), "XeLL marker");
        if (Token && PCLSetMarker) CheckSL(PCLSetMarker(nvidia, *Token), "PCL marker");
    }
};

GDX12FrameGeneration::GDX12FrameGeneration(GDX12Device* device) : _impl(std::make_unique<Impl>(device)) {}
GDX12FrameGeneration::~GDX12FrameGeneration() = default;

bool GDX12FrameGeneration::NeedsRecreate() const
{
    const auto& p = *_impl;
    return !p.PendingFailure.empty() || GetFrameGenerationProvider() != p.Status.Requested || GetReconstructionSettings().Restart != p.Restart;
}

ComPtr<IDXGISwapChain4> GDX12FrameGeneration::CreateSwapChain(const HWND window, DXGI_SWAP_CHAIN_DESC1& desc, const bool applySettings)
{
    auto& p = *_impl;
    p.Desc = desc; p.Status.Requested = applySettings ? GetFrameGenerationProvider() : GDX12FrameGenerationProvider::Off;
    p.Restart = applySettings ? GetReconstructionSettings().Restart : 0;
    p.Status.Effective = GDX12FrameGenerationProvider::Off;
    p.Status.GeneratedFrames = 0; p.AMDGenerated = 0; p.AMDError = 0;
    p.ResetNext = true; p.History = UINT64_MAX;
    ++p.Status.Generation;
    const auto pending = std::move(p.PendingFailure); p.PendingFailure.clear();
    p.Status.FallbackReason = pending;
    ComPtr<IDXGISwapChain4> chain;
    try
    {
        p.InitReflex();
        if (pending.empty())
        {
            switch (p.Status.Requested)
            {
            case GDX12FrameGenerationProvider::AMD: chain = p.CreateAMD(window); break;
            case GDX12FrameGenerationProvider::Intel: chain = p.CreateIntel(window); break;
            case GDX12FrameGenerationProvider::NVIDIA: p.CreateNvidia(); break;
            case GDX12FrameGenerationProvider::Off: break;
            }
            if (chain)
            {
                auto hooked = GDX12StreamlineSDK::Get().CreateProxySwapChain(chain);
                if (GDX12StreamlineSDK::Get().IsEnabled() && hooked.Get() == chain.Get())
                    throw std::runtime_error("Streamline cannot hook this FG swapchain combination");
                chain = std::move(hooked);
            }
            else chain = GDX12DeviceFactory::CreateSwapChain(p.Device, desc, window);
            if (p.Status.Requested == GDX12FrameGenerationProvider::NVIDIA)
            {
                p.NvidiaOptions(true);
                sl::DLSSGState state{};
                CheckSL(p.DLSSGGetState(p.Viewport, state, nullptr), "DLSS-G state");
                if (!state.numFramesToGenerateMax || desc.Width < state.minWidthOrHeight || desc.Height < state.minWidthOrHeight)
                    throw std::runtime_error("DLSS-G does not support this output size or frame multiplier");
            }
            p.Status.Effective = p.Status.Requested;
        }
    }
    catch (const std::exception& e)
    {
        p.Status.FallbackReason = e.what();
        ComPtr<IDXGISwapChain4> native;
        DestroySwapChain(chain, native);
        Log::Warning("FrameGeneration", "FG fallback to Off: ", p.Status.FallbackReason);
    }
    if (!chain) chain = GDX12DeviceFactory::CreateSwapChain(p.Device, desc, window);
    if (p.Suspended) p.Stop();
    return chain;
}

void GDX12FrameGeneration::DestroySwapChain(ComPtr<IDXGISwapChain4>& proxy, ComPtr<IDXGISwapChain4>& native)
{
    auto& p = *_impl;
    p.Stop();
    p.DestroyAMD();
    proxy.Reset(); native.Reset();
    p.DestroyIntel();
    if (p.NvidiaLoaded)
    {
        CheckSL(GDX12StreamlineSDK::Get().SetFeatureLoaded(sl::kFeatureDLSS_G, false), "Unload DLSS-G");
        p.NvidiaLoaded = false; p.DLSSGSetOptions = nullptr; p.DLSSGGetState = nullptr;
    }
    p.Status.Effective = GDX12FrameGenerationProvider::Off;
}

void GDX12FrameGeneration::SetSuspended(const bool suspended)
{
    auto& p = *_impl;
    if (p.Suspended == suspended) return;
    p.Suspended = p.Status.Suspended = suspended;
    if (suspended) p.Stop();
    p.ResetNext = true;
}

void GDX12FrameGeneration::WaitForIdle() { _impl->Stop(); }

void GDX12FrameGeneration::BeginFrame(RenderPipelineCommonData& common)
{
    auto& p = *_impl;
    common.StreamlineFrame = nullptr;
    if (p.Suspended) return;
    ++p.Frame;
    p.Token = nullptr; p.ActiveFrame = true;
    try
    {
        if (GDX12StreamlineSDK::Get().IsEnabled())
        {
            CheckSL(GDX12StreamlineSDK::Get().GetNewFrameToken(p.Token, &p.Frame), "Frame token");
            common.StreamlineFrame = p.Token;
        }
        if (p.Status.Effective == GDX12FrameGenerationProvider::Intel)
        {
            if (p.ResetNext) CheckIntel(p.xefgSwapChainSetEnabledFn(p.XeFG, true), "Resume XeFG");
            CheckIntel(p.xellSleepFn(p.XeLL, p.Frame), "XeLL sleep");
        }
        else if (p.Token && p.ReflexSleep)
        {
            if (p.Status.Effective == GDX12FrameGenerationProvider::NVIDIA && !p.NvidiaOn)
            {
                sl::ReflexOptions options{}; options.mode = sl::ReflexMode::eLowLatency;
                CheckSL(p.ReflexSetOptions(options), "Resume Reflex"); p.NvidiaOptions(true);
            }
            CheckSL(p.ReflexSleep(*p.Token), "Reflex sleep");
        }
        p.Marker(sl::PCLMarker::eSimulationStart, XELL_SIMULATION_START);
    }
    catch (const std::exception& e) { p.Fail(e.what()); p.Stop(); common.StreamlineFrame = nullptr; }
}

void GDX12FrameGeneration::BeginRendering(RenderPipelineCommonData& common)
{
    auto& p = *_impl;
    if (!p.ActiveFrame) return;
    p.FrameHistory = common.HistoryRevision;
    p.FrameReset = p.ResetNext || p.History != common.HistoryRevision || common.ResetHistory;
    try
    {
        p.Marker(sl::PCLMarker::eSimulationEnd, XELL_SIMULATION_END);
        p.Marker(sl::PCLMarker::eRenderSubmitStart, XELL_RENDERSUBMIT_START);
        if (p.Token)
        {
            sl::Constants constants{};
            std::memcpy(&constants.cameraViewToClip, &common.CameraViewToClip, sizeof(constants.cameraViewToClip));
            std::memcpy(&constants.clipToCameraView, &common.ClipToCameraView, sizeof(constants.clipToCameraView));
            std::memcpy(&constants.clipToPrevClip, &common.ClipToPrevClip, sizeof(constants.clipToPrevClip));
            std::memcpy(&constants.prevClipToClip, &common.PrevClipToClip, sizeof(constants.prevClipToClip));
            const auto camera = common.CameraView.Invert();
            const auto eye = camera.Translation(), right = camera.Right(), up = camera.Up(), forward = camera.Forward();
            constants.cameraPos = {eye.x, eye.y, eye.z}; constants.cameraRight = {right.x, right.y, right.z};
            constants.cameraUp = {up.x, up.y, up.z}; constants.cameraFwd = {forward.x, forward.y, forward.z};
            constants.cameraNear = common.ActiveCameraNearPlane; constants.cameraFar = common.ActiveCameraFarPlane;
            constants.cameraFOV = XMConvertToRadians(common.ActiveCameraFOV);
            constants.cameraAspectRatio = static_cast<float>(common.WindowWidth) / common.WindowHeight;
            constants.jitterOffset = { common.ActiveCameraJitterOffsetX, common.ActiveCameraJitterOffsetY };
            constants.mvecScale = { 0.5f, -0.5f };
            constants.depthInverted = sl::Boolean::eTrue; constants.cameraMotionIncluded = sl::Boolean::eTrue;
            constants.motionVectors3D = sl::Boolean::eFalse; constants.orthographicProjection = sl::Boolean::eFalse;
            constants.motionVectorsDilated = sl::Boolean::eFalse; constants.motionVectorsJittered = sl::Boolean::eFalse;
            constants.reset = p.FrameReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            CheckSL(GDX12StreamlineSDK::Get().SetConstants(constants, *p.Token, p.Viewport), "Common frame constants");
        }
    }
    catch (const std::exception& e) { p.Fail(e.what()); p.Stop(); common.StreamlineFrame = nullptr; }
}

void GDX12FrameGeneration::Prepare(GDX12CommandList* commands, GDX12Texture* depth, GDX12Texture* motion, RenderPipelineCommonData& common)
{
    auto& p = *_impl;
    if (!p.ActiveFrame || p.Suspended || p.Status.Effective == GDX12FrameGenerationProvider::Off) return;
    try
    {
        if (!depth || !motion || depth->GetWidth() != motion->GetWidth() || depth->GetHeight() != motion->GetHeight())
            throw std::invalid_argument("FG depth and motion must share the render grid");
        // SDK descriptors below require exact compute-read state, without read-only DSV bits.
        if (depth->GetResource()->GetCurrentState() & D3D12_RESOURCE_STATE_DEPTH_READ)
            commands->ResourceBarrier({ depth->GetResource()->GetCommonBarrier() });
        commands->ResourceBarrier({ depth->GetResource()->GetNonPixelShaderResourceBarrier(), motion->GetResource()->GetNonPixelShaderResourceBarrier() });
        if (p.Status.Effective == GDX12FrameGenerationProvider::AMD)
        {
            ffxConfigureDescFrameGeneration config{};
            config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            config.swapChain = p.AMDChain; config.frameGenerationEnabled = true;
            config.frameGenerationCallback = &Impl::DispatchAMD; config.frameGenerationCallbackUserContext = &p;
            config.generationRect = {0, 0, static_cast<int32_t>(common.WindowWidth), static_cast<int32_t>(common.WindowHeight)}; config.frameID = p.Frame;
            CheckAMD(ffxConfigure(&p.AMD, &config.header), "AMD FG configure"); p.AMDConfigured = true;
            ffxDispatchDescFrameGenerationPrepareV2 prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prepare.frameID = p.Frame; prepare.commandList = commands->GetCommandList().Get();
            prepare.renderSize = {depth->GetWidth(), depth->GetHeight()};
            prepare.jitterOffset = {common.ActiveCameraJitterOffsetX, common.ActiveCameraJitterOffsetY};
            prepare.motionVectorScale = {depth->GetWidth() * 0.5f, -static_cast<float>(depth->GetHeight()) * 0.5f};
            prepare.frameTimeDelta = (std::max)(0.01f, common.GameTimer->DeltaTime() * 1000.0f);
            prepare.reset = p.FrameReset;
            prepare.cameraNear = common.ActiveCameraFarPlane; prepare.cameraFar = common.ActiveCameraNearPlane;
            prepare.cameraFovAngleVertical = XMConvertToRadians(common.ActiveCameraFOV); prepare.viewSpaceToMetersFactor = 0.01f;
            prepare.depth = ffxApiGetResourceDX12(depth->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            prepare.motionVectors = ffxApiGetResourceDX12(motion->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            const auto camera = common.CameraView.Invert();
            const auto position = camera.Translation(); auto right = camera.Right(), up = camera.Up(), forward = camera.Forward();
            right.Normalize(); up.Normalize(); forward.Normalize();
            std::memcpy(prepare.cameraPosition, &position, sizeof(prepare.cameraPosition));
            std::memcpy(prepare.cameraRight, &right, sizeof(prepare.cameraRight));
            std::memcpy(prepare.cameraUp, &up, sizeof(prepare.cameraUp));
            std::memcpy(prepare.cameraForward, &forward, sizeof(prepare.cameraForward));
            CheckAMD(ffxDispatch(&p.AMD, &prepare.header), "AMD FG prepare");
        }
        else if (p.Status.Effective == GDX12FrameGenerationProvider::Intel)
        {
            xefg_swapchain_d3d12_resource_data_t tag{};
            tag.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW; tag.resourceSize = { depth->GetWidth(), depth->GetHeight() };
            tag.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            tag.type = XEFG_SWAPCHAIN_RES_DEPTH; tag.pResource = depth->GetResource()->D3DResource.Get();
            CheckIntel(p.xefgSwapChainD3D12TagFrameResourceFn(p.XeFG, commands->GetCommandList().Get(), p.Frame, &tag), "XeFG depth");
            tag.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR; tag.pResource = motion->GetResource()->D3DResource.Get();
            CheckIntel(p.xefgSwapChainD3D12TagFrameResourceFn(p.XeFG, commands->GetCommandList().Get(), p.Frame, &tag), "XeFG motion");
            xefg_swapchain_frame_constant_data_t constants{};
            std::memcpy(constants.viewMatrix, &common.CameraView, sizeof(constants.viewMatrix));
            std::memcpy(constants.projectionMatrix, &common.CameraViewToClip, sizeof(constants.projectionMatrix));
            constants.jitterOffsetX = common.ActiveCameraJitterOffsetX; constants.jitterOffsetY = common.ActiveCameraJitterOffsetY;
            constants.motionVectorScaleX = constants.motionVectorScaleY = 1;
            constants.frameRenderTime = (std::max)(0.01f, common.GameTimer->DeltaTime() * 1000.0f); constants.resetHistory = p.FrameReset;
            CheckIntel(p.xefgSwapChainTagFrameConstantsFn(p.XeFG, p.Frame, &constants), "XeFG constants");
        }
        else
        {
            p.NvidiaOptions(true, depth, motion);
            sl::Resource depthResource{ sl::ResourceType::eTex2d, depth->GetResource()->D3DResource.Get(), nullptr, nullptr, static_cast<uint32_t>(depth->GetResource()->GetCurrentState()) };
            sl::Resource motionResource{ sl::ResourceType::eTex2d, motion->GetResource()->D3DResource.Get(), nullptr, nullptr, static_cast<uint32_t>(motion->GetResource()->GetCurrentState()) };
            sl::Extent extent{}; extent.width = depth->GetWidth(); extent.height = depth->GetHeight();
            const sl::ResourceTag tags[] = {
                {&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
                {&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent} };
            CheckSL(GDX12StreamlineSDK::Get().SetTagForFrame(*p.Token, p.Viewport, tags, _countof(tags), commands->GetCommandList().Get()), "DLSS-G tags");
        }
        commands->InvalidateCachedState();
    }
    catch (const std::exception& e) { commands->InvalidateCachedState(); p.Fail(e.what()); }
}

void GDX12FrameGeneration::EndRendering()
{
    auto& p = *_impl;
    if (!p.ActiveFrame) return;
    if (!p.PendingFailure.empty()) { p.Stop(); return; }
    try { p.Marker(sl::PCLMarker::eRenderSubmitEnd, XELL_RENDERSUBMIT_END); }
    catch (const std::exception& e) { p.Fail(e.what()); p.Stop(); }
}

void GDX12FrameGeneration::BeforePresent()
{
    auto& p = *_impl;
    if (!p.ActiveFrame) return;
    try
    {
        if (p.Status.Effective == GDX12FrameGenerationProvider::Intel)
            CheckIntel(p.xefgSwapChainSetPresentIdFn(p.XeFG, p.Frame), "XeFG present ID");
        p.Marker(sl::PCLMarker::ePresentStart, XELL_PRESENT_START);
    }
    catch (const std::exception& e) { p.Fail(e.what()); p.Stop(); }
}

void GDX12FrameGeneration::AfterPresent()
{
    auto& p = *_impl;
    if (!p.ActiveFrame) return;
    try
    {
        p.Marker(sl::PCLMarker::ePresentEnd, XELL_PRESENT_END);
        if (p.Status.Effective == GDX12FrameGenerationProvider::AMD)
        {
            CheckAMD(p.AMDError.load(std::memory_order_relaxed), "AMD frame generation callback");
            p.Status.GeneratedFrames = p.AMDGenerated.load(std::memory_order_relaxed);
        }
        else if (p.Status.Effective == GDX12FrameGenerationProvider::Intel)
        {
            xefg_swapchain_present_status_t status{};
            CheckIntel(p.xefgSwapChainGetLastPresentStatusFn(p.XeFG, &status), "XeFG present status");
            CheckIntel(status.frameGenResult, "XeFG interpolation");
            if (status.framesPresented > 1) p.Status.GeneratedFrames += status.framesPresented - 1;
        }
        else if (p.Status.Effective == GDX12FrameGenerationProvider::NVIDIA)
        {
            sl::DLSSGState state{};
            CheckSL(p.DLSSGGetState(p.Viewport, state, nullptr), "DLSS-G present status");
            if (state.status != sl::DLSSGStatus::eOk) throw std::runtime_error("DLSS-G runtime status " + std::to_string(static_cast<uint32_t>(state.status)));
            if (state.numFramesActuallyPresented > 1) p.Status.GeneratedFrames += state.numFramesActuallyPresented - 1;
        }
    }
    catch (const std::exception& e) { p.Fail(e.what()); p.Stop(); }
    p.History = p.FrameHistory; p.ResetNext = false; p.ActiveFrame = false;
}

const GDX12FrameGenerationStatus& GDX12FrameGeneration::GetStatus() const { return _impl->Status; }
