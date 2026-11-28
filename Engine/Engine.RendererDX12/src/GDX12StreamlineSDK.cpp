#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DLSSUpscalePass.h"

#include <filesystem>

#ifdef free
#pragma push_macro("free")
#undef free
#define PEP_RESTORE_FREE_MACRO_STREAMLINE_CPP 1
#endif
#include "nvidia-sdk/sl_security.h"
#ifdef PEP_RESTORE_FREE_MACRO_STREAMLINE_CPP
#pragma pop_macro("free")
#undef PEP_RESTORE_FREE_MACRO_STREAMLINE_CPP
#endif

GDX12StreamlineSDK& GDX12StreamlineSDK::Get()
{
    static GDX12StreamlineSDK instance;
    return instance;
}

bool GDX12StreamlineSDK::Initialize()
{
    if (_initialized) { return true; }

    _runtimeDirectory = GetRuntimeDirectory();
    LoadInterposer();
    if (!_interposerModule) return false;
    LoadFunctions();
    if (!_slInit || !_slShutdown || !_slUpgradeInterface || !_slGetNativeInterface || !_slSetD3DDevice ||
        !_slIsFeatureSupported || !_slGetFeatureFunction || !_slSetFeatureLoaded ||
        !_slSetTagForFrame || !_slSetConstants || !_slGetNewFrameToken) return false;

    const wchar_t* pluginPaths[] = { _runtimeDirectory.c_str() };

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eOff;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = static_cast<uint32_t>(std::size(pluginPaths));
    preferences.pathToLogsAndData = _runtimeDirectory.c_str();
    preferences.logMessageCallback = &GDX12StreamlineSDK::StreamlineLog;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking
        | sl::PreferenceFlags::eUseManualHooking
        | sl::PreferenceFlags::eUseDXGIFactoryProxy
        | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = _countof(features);
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0.0";
    preferences.projectId = ProjectId;
    preferences.renderAPI = sl::RenderAPI::eD3D12;

    const sl::Result initResult = _slInit(preferences, sl::kSDKVersion);
    if (initResult != sl::Result::eOk)
    {
        LogFailure("slInit", initResult);
        Shutdown();
        return false;
    }

    _initialized = true;
    Log("initialized in manual hooking mode.");
    return true;
}

void GDX12StreamlineSDK::Shutdown()
{
    if (_initialized && _slShutdown)
    {
        const sl::Result shutdownResult = _slShutdown();
        if (shutdownResult != sl::Result::eOk) { LogFailure("slShutdown", shutdownResult); }
    }

    if (_mainDevice) GDX12DLSSUpscalePass::ShutdownStreamlineDevice(_mainDevice.Get());
    _initialized = false;
    _mainDevice.Reset();

    _slInit = nullptr;
    _slShutdown = nullptr;
    _slUpgradeInterface = nullptr;
    _slGetNativeInterface = nullptr;
    _slSetD3DDevice = nullptr;
    _slIsFeatureSupported = nullptr;
    _slGetFeatureFunction = nullptr;
    _slSetFeatureLoaded = nullptr;
    _slSetTagForFrame = nullptr;
    _slSetConstants = nullptr;
    _slGetNewFrameToken = nullptr;

    // Proxy COM objects can outlive slShutdown; keep their implementation loaded until process exit.
}

bool GDX12StreamlineSDK::IsEnabled() const
{
    return _initialized;
}

std::string GDX12StreamlineSDK::ResultToString(const sl::Result result)
{
    const char* name = sl::getResultAsStr(result);
    return name ? name : "UNKNOWN_RESULT";
}

ComPtr<IDXGIFactory7> GDX12StreamlineSDK::CreateProxyFactory(const ComPtr<IDXGIFactory7>& nativeFactory) const
{
    const ComPtr<IDXGIFactory7> resolvedNativeFactory = UnwrapInterface(nativeFactory);
    if (resolvedNativeFactory && resolvedNativeFactory.Get() != nativeFactory.Get())
    { return nativeFactory; }

    return UpgradeInterface(nativeFactory, "slUpgradeInterface(IDXGIFactory7)");
}

ComPtr<IDXGISwapChain4> GDX12StreamlineSDK::CreateProxySwapChain(const ComPtr<IDXGISwapChain4>& swapChain) const
{
    return UpgradeInterface(swapChain, "slUpgradeInterface(IDXGISwapChain4)");
}

ComPtr<ID3D12Device14> GDX12StreamlineSDK::CreateProxyDevice(const ComPtr<ID3D12Device14>& nativeDevice, const bool setAsMainDevice)
{
    if (!_initialized || !nativeDevice)
    {
        return nativeDevice;
    }

    const ComPtr<ID3D12Device14> resolvedNativeDevice = UnwrapInterface(nativeDevice);
    const bool inputAlreadyProxy = resolvedNativeDevice && resolvedNativeDevice.Get() != nativeDevice.Get();
    const ComPtr<ID3D12Device14>& deviceToRegister = inputAlreadyProxy ? resolvedNativeDevice : nativeDevice;

    if (setAsMainDevice && !_mainDevice)
    {
        const sl::Result setDeviceResult = _slSetD3DDevice(deviceToRegister.Get());
        if (setDeviceResult != sl::Result::eOk)
        {
            LogFailure("slSetD3DDevice", setDeviceResult);
            return nativeDevice;
        }

        _mainDevice = deviceToRegister;
        // Loading state changes require a device and must precede swapchain creation.
        SetFeatureLoaded(sl::kFeatureDLSS_G, false);
    }

    if (inputAlreadyProxy)
    {
        return nativeDevice;
    }

    return UpgradeInterface(nativeDevice, "slUpgradeInterface(ID3D12Device14)");
}

ComPtr<IDXGIFactory7> GDX12StreamlineSDK::GetNativeFactory(const ComPtr<IDXGIFactory7>& factory) const
{
    return UnwrapInterface(factory);
}

ComPtr<ID3D12Device14> GDX12StreamlineSDK::GetNativeDevice(const ComPtr<ID3D12Device14>& device) const
{
    return UnwrapInterface(device);
}

ComPtr<ID3D12CommandQueue> GDX12StreamlineSDK::GetNativeCommandQueue(const ComPtr<ID3D12CommandQueue>& commandQueue) const
{
    return UnwrapInterface(commandQueue);
}

ComPtr<IDXGISwapChain4> GDX12StreamlineSDK::GetNativeSwapChain(const ComPtr<IDXGISwapChain4>& swapChain) const
{
    return UnwrapInterface(swapChain);
}

ComPtr<ID3D12Resource> GDX12StreamlineSDK::GetNativeResource(const ComPtr<ID3D12Resource>& resource) const
{
    return UnwrapInterface(resource);
}

sl::Result GDX12StreamlineSDK::IsFeatureSupported(const sl::Feature feature, ID3D12Device14& device) const
{
    if (!_mainDevice) return sl::Result::eErrorInvalidState;
    // Streamline evaluates features on the device registered with slSetD3DDevice.
    if (_mainDevice.Get() != &device) return sl::Result::eErrorAdapterNotSupported;
    auto luid = device.GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    return _slIsFeatureSupported(feature, adapter);
}

sl::Result GDX12StreamlineSDK::GetFeatureFunction(const sl::Feature feature, const char* name, void*& function) const
{
    function = nullptr;
    return _initialized && _slGetFeatureFunction ? _slGetFeatureFunction(feature, name, function) : sl::Result::eErrorInvalidState;
}

sl::Result GDX12StreamlineSDK::SetFeatureLoaded(const sl::Feature feature, const bool loaded) const
{
    return _initialized && _slSetFeatureLoaded ? _slSetFeatureLoaded(feature, loaded) : sl::Result::eErrorInvalidState;
}

sl::Result GDX12StreamlineSDK::GetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) const
{
    token = nullptr;
    return _initialized && _slGetNewFrameToken ? _slGetNewFrameToken(token, frameIndex) : sl::Result::eErrorInvalidState;
}

sl::Result GDX12StreamlineSDK::SetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, const uint32_t numTags, sl::CommandBuffer* cmdBuffer) const
{
    return _initialized && _slSetTagForFrame ? _slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer) : sl::Result::eErrorInvalidState;
}

sl::Result GDX12StreamlineSDK::SetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) const
{
    return _initialized && _slSetConstants ? _slSetConstants(values, frame, viewport) : sl::Result::eErrorInvalidState;
}

void GDX12StreamlineSDK::StreamlineLog(const sl::LogType type, const char* message)
{
    const char* level = "INFO";
    if (type == sl::LogType::eWarn) { level = "WARNING"; }
    else if (type == sl::LogType::eError) { level = "ERROR"; }

    std::string formattedMessage = std::string("Streamline ") + '[' + level + "] " + (message ? message : "<null>") + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

std::wstring GDX12StreamlineSDK::GetRuntimeDirectory() const
{
    wchar_t executablePath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);

    return std::filesystem::path(std::wstring(executablePath, executablePath + pathLength)).parent_path().wstring();
}

void GDX12StreamlineSDK::LoadInterposer()
{
    if (_interposerModule) return;
    const std::filesystem::path interposerPath = std::filesystem::path(_runtimeDirectory) / L"sl.interposer.dll";
    if (!std::filesystem::exists(interposerPath)) { LogWarning("sl.interposer.dll was not found"); return; }
    if (!sl::security::verifyEmbeddedSignature(interposerPath.c_str())) 
    { LogWarning("sl.interposer.dll signature check failed"); return; }
    _interposerModule = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!_interposerModule) { LogWarning("sl.interposer.dll LoadLibraryW failed"); }
}

void GDX12StreamlineSDK::LoadFunctions()
{
    _slInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(_interposerModule, "slInit"));
    _slShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(_interposerModule, "slShutdown"));
    _slUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(_interposerModule, "slUpgradeInterface"));
    _slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface*>(GetProcAddress(_interposerModule, "slGetNativeInterface"));
    _slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(_interposerModule, "slSetD3DDevice"));
    _slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(_interposerModule, "slIsFeatureSupported"));
    _slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(_interposerModule, "slGetFeatureFunction"));
    _slSetFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(GetProcAddress(_interposerModule, "slSetFeatureLoaded"));
    _slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(_interposerModule, "slSetTagForFrame"));
    _slSetConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(_interposerModule, "slSetConstants"));
    _slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(_interposerModule, "slGetNewFrameToken"));

    if (!_slInit || !_slShutdown || !_slUpgradeInterface || !_slGetNativeInterface || !_slSetD3DDevice
        || !_slIsFeatureSupported
        || !_slGetFeatureFunction || !_slSetFeatureLoaded || !_slSetTagForFrame
        || !_slSetConstants || !_slGetNewFrameToken)
    { LogFailure("Required Streamline exports are missing, Streamline is disabled.", sl::Result::eErrorMissingOrInvalidAPI); }
}

void GDX12StreamlineSDK::Log(const std::string& message) const
{
    std::string formattedMessage = std::string("Streamline INFO: ") + message + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

void GDX12StreamlineSDK::LogWarning(const std::string& message) const
{
    std::string formattedMessage = std::string("Streamline WARNING: ") + message + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

void GDX12StreamlineSDK::LogFailure(const char* operation, const sl::Result result) const
{
    std::string formattedMessage = std::string("Streamline ERROR: ") + operation + " failed: " + ResultToString(result) + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}
