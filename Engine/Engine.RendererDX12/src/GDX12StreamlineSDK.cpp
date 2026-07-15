#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

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
    LoadFunctions();

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

    sl::Feature FeaturestoLoad[] = { sl::kFeatureDLSS, sl::kFeatureNIS };
    preferences.featuresToLoad = FeaturestoLoad;
    preferences.numFeaturesToLoad = _countof(FeaturestoLoad);
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0.0";
    preferences.projectId = "c9202a3c-8f89-4cf2-952d-65427c52041d";
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

    _initialized = false;
    _mainDeviceWasSet = false;

    _slInit = nullptr;
    _slShutdown = nullptr;
    _slUpgradeInterface = nullptr;
    _slGetNativeInterface = nullptr;
    _slSetD3DDevice = nullptr;
    _slIsFeatureSupported = nullptr;
    _slGetFeatureFunction = nullptr;
    _slEvaluateFeature = nullptr;
    _slFreeResources = nullptr;
    _slSetTagForFrame = nullptr;
    _slSetConstants = nullptr;
    _slGetNewFrameToken = nullptr;
    _slDLSSGetOptimalSettings = nullptr;
    _slDLSSSetOptions = nullptr;
    _slNISSetOptions = nullptr;

    if (_interposerModule)
    {
        FreeLibrary(_interposerModule);
        _interposerModule = nullptr;
    }
}

bool GDX12StreamlineSDK::IsEnabled() const
{
    return _initialized;
}

std::string GDX12StreamlineSDK::ResultToString(sl::Result result)
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

ComPtr<ID3D12Device14> GDX12StreamlineSDK::CreateProxyDevice(const ComPtr<ID3D12Device14>& nativeDevice, bool setAsMainDevice) const
{
    if (!_initialized || !nativeDevice)
    {
        return nativeDevice;
    }

    const ComPtr<ID3D12Device14> resolvedNativeDevice = UnwrapInterface(nativeDevice);
    const bool inputAlreadyProxy = resolvedNativeDevice && resolvedNativeDevice.Get() != nativeDevice.Get();
    const ComPtr<ID3D12Device14>& deviceToRegister = inputAlreadyProxy ? resolvedNativeDevice : nativeDevice;

    if (setAsMainDevice && !_mainDeviceWasSet)
    {
        const sl::Result setDeviceResult = _slSetD3DDevice(deviceToRegister.Get());
        if (setDeviceResult != sl::Result::eOk)
        {
            LogFailure("slSetD3DDevice", setDeviceResult);
            return nativeDevice;
        }

        const_cast<GDX12StreamlineSDK*>(this)->_mainDeviceWasSet = true;
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

sl::Result GDX12StreamlineSDK::IsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo) const
{
    return _slIsFeatureSupported(feature, adapterInfo);
}

sl::Result GDX12StreamlineSDK::GetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) const
{
    return _slGetNewFrameToken(token, frameIndex);
}

sl::Result GDX12StreamlineSDK::SetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer) const
{
    return _slSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer);
}

sl::Result GDX12StreamlineSDK::EvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer) const
{
    return _slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
}

sl::Result GDX12StreamlineSDK::FreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) const
{
    return _slFreeResources(feature, viewport);
}

sl::Result GDX12StreamlineSDK::SetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) const
{
    return _slSetConstants(values, frame, viewport);
}

sl::Result GDX12StreamlineSDK::DLSSGetOptimalSettings(const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings) const
{
    const sl::Result loadResult = LoadDLSSFunctions();
    if (loadResult != sl::Result::eOk || !_slDLSSGetOptimalSettings)
    {
        return loadResult;
    }

    return _slDLSSGetOptimalSettings(options, settings);
}

sl::Result GDX12StreamlineSDK::DLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) const
{
    const sl::Result loadResult = LoadDLSSFunctions();
    if (loadResult != sl::Result::eOk || !_slDLSSSetOptions)
    {
        return loadResult;
    }

    return _slDLSSSetOptions(viewport, options);
}

sl::Result GDX12StreamlineSDK::NISSetOptions(const sl::ViewportHandle& viewport, const sl::NISOptions& options) const
{
    const sl::Result loadResult = LoadNISFunctions();
    if (loadResult != sl::Result::eOk || !_slNISSetOptions)
    {
        return loadResult;
    }

    return _slNISSetOptions(viewport, options);
}

void GDX12StreamlineSDK::StreamlineLog(sl::LogType type, const char* message)
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
    const std::filesystem::path interposerPath = std::filesystem::path(_runtimeDirectory) / L"sl.interposer.dll";
    if (!std::filesystem::exists(interposerPath)) { LogWarning("sl.interposer.dll was not found"); }
    if (!sl::security::verifyEmbeddedSignature(interposerPath.c_str())) 
    { LogWarning("sl.interposer.dll signature check failed"); }
    _interposerModule = LoadLibraryW(interposerPath.c_str());
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
    _slEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(_interposerModule, "slEvaluateFeature"));
    _slFreeResources = reinterpret_cast<PFun_slFreeResources*>(GetProcAddress(_interposerModule, "slFreeResources"));
    _slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(_interposerModule, "slSetTagForFrame"));
    _slSetConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(_interposerModule, "slSetConstants"));
    _slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(_interposerModule, "slGetNewFrameToken"));

    if (!_slInit || !_slShutdown || !_slUpgradeInterface || !_slGetNativeInterface || !_slSetD3DDevice
        || !_slIsFeatureSupported
        || !_slGetFeatureFunction || !_slEvaluateFeature || !_slFreeResources || !_slSetTagForFrame
        || !_slSetConstants || !_slGetNewFrameToken)
    { LogWarning("Required Streamline exports are missing, Streamline is disabled."); }
}

sl::Result GDX12StreamlineSDK::LoadDLSSFunctions() const
{
    sl::Result result = LoadFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", _slDLSSGetOptimalSettings);
    if (result != sl::Result::eOk) { return result; }

    result = LoadFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", _slDLSSSetOptions);
    return result;
}

sl::Result GDX12StreamlineSDK::LoadNISFunctions() const
{
    return LoadFeatureFunction(sl::kFeatureNIS, "slNISSetOptions", _slNISSetOptions);
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

void GDX12StreamlineSDK::LogFailure(const char* operation, sl::Result result) const
{
    std::string formattedMessage = std::string("Streamline ERROR: ") + operation + " failed: " + ResultToString(result) + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}
