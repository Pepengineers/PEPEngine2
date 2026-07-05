#include "Engine.RendererDX12/GDX12Streamline.h"

#include <filesystem>

#include "nvidia-sdk/sl_security.h"

namespace
{
    constexpr sl::Feature kRequestedFeatures[] =
    {
        sl::kFeatureNIS,
        sl::kFeatureReflex,
        sl::kFeatureDirectSR
    };

    constexpr const char* kLogPrefix = "Streamline: ";
}

GDX12Streamline& GDX12Streamline::Get()
{
    static GDX12Streamline instance;
    return instance;
}

bool GDX12Streamline::Initialize()
{
    if (_initialized)
    {
        return true;
    }

    if (_initializationAttempted)
    {
        return false;
    }

    _initializationAttempted = true;
    _runtimeDirectory = GetRuntimeDirectory();

    if (_runtimeDirectory.empty())
    {
        LogWarning("Failed to resolve executable directory, Streamline is disabled.");
        return false;
    }

    if (!LoadInterposer() || !LoadFunctions())
    {
        Shutdown();
        return false;
    }

    const wchar_t* pluginPaths[] = { _runtimeDirectory.c_str() };

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = static_cast<uint32_t>(std::size(pluginPaths));
    preferences.pathToLogsAndData = _runtimeDirectory.c_str();
    preferences.logMessageCallback = &GDX12Streamline::StreamlineLog;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking
        | sl::PreferenceFlags::eUseManualHooking
        | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    preferences.featuresToLoad = kRequestedFeatures;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(kRequestedFeatures));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "PEPEngine2";
    preferences.projectId = "8f52d4be-6cce-4e1a-b95f-31a43fc25369";
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

void GDX12Streamline::Shutdown()
{
    if (_initialized && _slShutdown)
    {
        const sl::Result shutdownResult = _slShutdown();
        if (shutdownResult != sl::Result::eOk)
        {
            LogFailure("slShutdown", shutdownResult);
        }
    }

    _initialized = false;
    _mainDeviceWasSet = false;

    _slInit = nullptr;
    _slShutdown = nullptr;
    _slUpgradeInterface = nullptr;
    _slGetNativeInterface = nullptr;
    _slSetD3DDevice = nullptr;

    if (_interposerModule)
    {
        FreeLibrary(_interposerModule);
        _interposerModule = nullptr;
    }
}

bool GDX12Streamline::IsEnabled() const
{
    return _initialized;
}

ComPtr<IDXGIFactory7> GDX12Streamline::CreateProxyFactory(const ComPtr<IDXGIFactory7>& nativeFactory) const
{
    return UpgradeInterface(nativeFactory, "slUpgradeInterface(IDXGIFactory7)");
}

ComPtr<ID3D12Device14> GDX12Streamline::CreateProxyDevice(const ComPtr<ID3D12Device14>& nativeDevice, bool setAsMainDevice) const
{
    if (!_initialized || !nativeDevice)
    {
        return nativeDevice;
    }

    if (setAsMainDevice && !_mainDeviceWasSet)
    {
        const sl::Result setDeviceResult = _slSetD3DDevice(nativeDevice.Get());
        if (setDeviceResult != sl::Result::eOk)
        {
            LogFailure("slSetD3DDevice", setDeviceResult);
            return nativeDevice;
        }

        const_cast<GDX12Streamline*>(this)->_mainDeviceWasSet = true;
    }

    return UpgradeInterface(nativeDevice, "slUpgradeInterface(ID3D12Device14)");
}

ComPtr<ID3D12CommandQueue> GDX12Streamline::GetNativeCommandQueue(const ComPtr<ID3D12CommandQueue>& commandQueue) const
{
    return UnwrapInterface(commandQueue);
}

ComPtr<IDXGISwapChain4> GDX12Streamline::GetNativeSwapChain(const ComPtr<IDXGISwapChain4>& swapChain) const
{
    return UnwrapInterface(swapChain);
}

ComPtr<ID3D12Resource> GDX12Streamline::GetNativeResource(const ComPtr<ID3D12Resource>& resource) const
{
    return UnwrapInterface(resource);
}

void GDX12Streamline::StreamlineLog(sl::LogType type, const char* message)
{
    const char* level = "INFO";
    if (type == sl::LogType::eWarn)
    {
        level = "WARN";
    }
    else if (type == sl::LogType::eError)
    {
        level = "ERROR";
    }

    std::string formattedMessage = std::string(kLogPrefix) + '[' + level + "] " + (message ? message : "<null>") + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

std::wstring GDX12Streamline::GetRuntimeDirectory()
{
    wchar_t executablePath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (pathLength == 0 || pathLength == MAX_PATH)
    {
        return {};
    }

    return std::filesystem::path(std::wstring(executablePath, executablePath + pathLength)).parent_path().wstring();
}

bool GDX12Streamline::LoadInterposer()
{
    const std::filesystem::path interposerPath = std::filesystem::path(_runtimeDirectory) / L"sl.interposer.dll";
    if (!std::filesystem::exists(interposerPath))
    {
        LogWarning("sl.interposer.dll was not found next to the executable, Streamline is disabled.");
        return false;
    }

    if (!sl::security::verifyEmbeddedSignature(interposerPath.c_str()))
    {
        LogWarning("sl.interposer.dll signature check failed, continuing because development Streamline DLLs can be unsigned.");
    }

    _interposerModule = LoadLibraryW(interposerPath.c_str());
    if (!_interposerModule)
    {
        LogWarning("LoadLibraryW failed for sl.interposer.dll, Streamline is disabled.");
        return false;
    }

    return true;
}

bool GDX12Streamline::LoadFunctions()
{
    _slInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(_interposerModule, "slInit"));
    _slShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(_interposerModule, "slShutdown"));
    _slUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(_interposerModule, "slUpgradeInterface"));
    _slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface*>(GetProcAddress(_interposerModule, "slGetNativeInterface"));
    _slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(_interposerModule, "slSetD3DDevice"));

    if (!_slInit || !_slShutdown || !_slUpgradeInterface || !_slGetNativeInterface || !_slSetD3DDevice)
    {
        LogWarning("Required Streamline exports are missing, Streamline is disabled.");
        return false;
    }

    return true;
}

void GDX12Streamline::Log(const std::string& message) const
{
    std::string formattedMessage = std::string(kLogPrefix) + message + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

void GDX12Streamline::LogWarning(const std::string& message) const
{
    std::string formattedMessage = std::string(kLogPrefix) + "WARN: " + message + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}

void GDX12Streamline::LogFailure(const char* operation, sl::Result result) const
{
    const char* resultName = sl::getResultAsStr(result);
    std::string formattedMessage = std::string(kLogPrefix) + operation + " failed: " + (resultName ? resultName : "Unknown") + '\n';
    OutputDebugStringA(formattedMessage.c_str());
}