#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include <string>

#ifdef free
#pragma push_macro("free")
#undef free
#define PEP_RESTORE_FREE_MACRO_STREAMLINE 1
#endif
#include "nvidia-sdk/sl.h"
#include "nvidia-sdk/sl_consts.h"
#include "nvidia-sdk/sl_dlss.h"
#include "nvidia-sdk/sl_helpers.h"
#ifdef PEP_RESTORE_FREE_MACRO_STREAMLINE
#pragma pop_macro("free")
#undef PEP_RESTORE_FREE_MACRO_STREAMLINE
#endif

class GDX12StreamlineSDK
{
public:
    static GDX12StreamlineSDK& Get();

    bool Initialize();
    void Shutdown();

    bool IsEnabled() const;

    static std::string ResultToString(sl::Result result);

    ComPtr<IDXGIFactory7> CreateProxyFactory(const ComPtr<IDXGIFactory7>& nativeFactory) const;
    ComPtr<ID3D12Device14> CreateProxyDevice(const ComPtr<ID3D12Device14>& nativeDevice, bool setAsMainDevice) const;
    ComPtr<IDXGIFactory7> GetNativeFactory(const ComPtr<IDXGIFactory7>& factory) const;
    ComPtr<ID3D12Device14> GetNativeDevice(const ComPtr<ID3D12Device14>& device) const;
    ComPtr<ID3D12CommandQueue> GetNativeCommandQueue(const ComPtr<ID3D12CommandQueue>& commandQueue) const;
    ComPtr<IDXGISwapChain4> GetNativeSwapChain(const ComPtr<IDXGISwapChain4>& swapChain) const;
    ComPtr<ID3D12Resource> GetNativeResource(const ComPtr<ID3D12Resource>& resource) const;
    sl::Result IsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo) const;
    sl::Result GetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex = nullptr) const;
    sl::Result SetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer) const;
    sl::Result EvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer) const;
    sl::Result FreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) const;
    sl::Result SetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) const;
    sl::Result DLSSGetOptimalSettings(const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings) const;
    sl::Result DLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) const;

private:
    GDX12StreamlineSDK() = default;

    static void StreamlineLog(sl::LogType type, const char* message);

    std::wstring GetRuntimeDirectory() const;
    void LoadInterposer();
    void LoadFunctions();
    sl::Result LoadDLSSFunctions() const;

    void Log(const std::string& message) const;
    void LogWarning(const std::string& message) const;
    void LogFailure(const char* operation, sl::Result result) const;

    template <typename T>
    sl::Result LoadFeatureFunction(sl::Feature feature, const char* functionName, T*& function) const
    {
        if (function) { return sl::Result::eOk; }
        if (!_initialized || !_slGetFeatureFunction) { return sl::Result::eErrorInvalidState; }

        void* rawFunction = nullptr;
        const sl::Result result = _slGetFeatureFunction(feature, functionName, rawFunction);
        if (result == sl::Result::eOk) { function = reinterpret_cast<T*>(rawFunction); }

        return result;
    }

    template <typename T>
    ComPtr<T> UpgradeInterface(const ComPtr<T>& baseInterface, const char* interfaceName) const
    {
        if (!_initialized || !_slUpgradeInterface || !baseInterface) { return baseInterface; }

        T* upgradedInterface = baseInterface.Get();
        const T* originalInterface = upgradedInterface;
        const sl::Result result = _slUpgradeInterface(reinterpret_cast<void**>(&upgradedInterface));
        if (result != sl::Result::eOk)
        {
            LogFailure(interfaceName, result);
            return baseInterface;
        }

        if (!upgradedInterface || upgradedInterface == originalInterface) { return baseInterface; }

        ComPtr<T> proxyInterface;
        proxyInterface.Attach(upgradedInterface);
        return proxyInterface;
    }

    template <typename T>
    ComPtr<T> UnwrapInterface(const ComPtr<T>& proxyInterface) const
    {
        if (!_initialized || !_slGetNativeInterface || !proxyInterface) { return proxyInterface; }

        T* nativeInterface = nullptr;
        const sl::Result result = _slGetNativeInterface(proxyInterface.Get(), reinterpret_cast<void**>(&nativeInterface));
        if (result != sl::Result::eOk || !nativeInterface) { return proxyInterface; }

        ComPtr<T> baseInterface;
        baseInterface.Attach(nativeInterface);
        return baseInterface;
    }

    HMODULE _interposerModule = nullptr;
    bool _initialized = false;
    bool _mainDeviceWasSet = false;
    std::wstring _runtimeDirectory;

    PFun_slInit* _slInit = nullptr;
    PFun_slShutdown* _slShutdown = nullptr;
    PFun_slUpgradeInterface* _slUpgradeInterface = nullptr;
    PFun_slGetNativeInterface* _slGetNativeInterface = nullptr;
    PFun_slSetD3DDevice* _slSetD3DDevice = nullptr;
    PFun_slIsFeatureSupported* _slIsFeatureSupported = nullptr;
    PFun_slGetFeatureFunction* _slGetFeatureFunction = nullptr;
    PFun_slEvaluateFeature* _slEvaluateFeature = nullptr;
    PFun_slFreeResources* _slFreeResources = nullptr;
    PFun_slSetTagForFrame* _slSetTagForFrame = nullptr;
    PFun_slSetConstants* _slSetConstants = nullptr;
    PFun_slGetNewFrameToken* _slGetNewFrameToken = nullptr;
    mutable PFun_slDLSSGetOptimalSettings* _slDLSSGetOptimalSettings = nullptr;
    mutable PFun_slDLSSSetOptions* _slDLSSSetOptions = nullptr;
};
