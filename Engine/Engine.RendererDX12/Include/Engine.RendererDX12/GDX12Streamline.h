#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "nvidia-sdk/sl.h"
#include "nvidia-sdk/sl_helpers.h"

class GDX12Streamline
{
public:
    static GDX12Streamline& Get();

    bool Initialize();
    void Shutdown();

    bool IsEnabled() const;

    ComPtr<IDXGIFactory7> CreateProxyFactory(const ComPtr<IDXGIFactory7>& nativeFactory) const;
    ComPtr<ID3D12Device14> CreateProxyDevice(const ComPtr<ID3D12Device14>& nativeDevice, bool setAsMainDevice) const;
    ComPtr<ID3D12CommandQueue> GetNativeCommandQueue(const ComPtr<ID3D12CommandQueue>& commandQueue) const;
    ComPtr<IDXGISwapChain4> GetNativeSwapChain(const ComPtr<IDXGISwapChain4>& swapChain) const;
    ComPtr<ID3D12Resource> GetNativeResource(const ComPtr<ID3D12Resource>& resource) const;

private:
    GDX12Streamline() = default;

    static void StreamlineLog(sl::LogType type, const char* message);

    std::wstring GetRuntimeDirectory();
    bool LoadInterposer();
    bool LoadFunctions();

    void Log(const std::string& message) const;
    void LogWarning(const std::string& message) const;
    void LogFailure(const char* operation, sl::Result result) const;

    template <typename T>
    ComPtr<T> UpgradeInterface(const ComPtr<T>& baseInterface, const char* interfaceName) const
    {
        if (!_initialized || !_slUpgradeInterface || !baseInterface)
        {
            return baseInterface;
        }

        T* upgradedInterface = baseInterface.Get();
        const T* originalInterface = upgradedInterface;
        const sl::Result result = _slUpgradeInterface(reinterpret_cast<void**>(&upgradedInterface));
        if (result != sl::Result::eOk)
        {
            LogFailure(interfaceName, result);
            return baseInterface;
        }

        if (!upgradedInterface || upgradedInterface == originalInterface)
        {
            return baseInterface;
        }

        ComPtr<T> proxyInterface;
        proxyInterface.Attach(upgradedInterface);
        return proxyInterface;
    }

    template <typename T>
    ComPtr<T> UnwrapInterface(const ComPtr<T>& proxyInterface) const
    {
        if (!_initialized || !_slGetNativeInterface || !proxyInterface)
        {
            return proxyInterface;
        }

        T* nativeInterface = nullptr;
        const sl::Result result = _slGetNativeInterface(proxyInterface.Get(), reinterpret_cast<void**>(&nativeInterface));
        if (result != sl::Result::eOk || !nativeInterface)
        {
            return proxyInterface;
        }

        ComPtr<T> baseInterface;
        baseInterface.Attach(nativeInterface);
        return baseInterface;
    }

    HMODULE _interposerModule = nullptr;
    bool _initializationAttempted = false;
    bool _initialized = false;
    bool _mainDeviceWasSet = false;
    std::wstring _runtimeDirectory;

    PFun_slInit* _slInit = nullptr;
    PFun_slShutdown* _slShutdown = nullptr;
    PFun_slUpgradeInterface* _slUpgradeInterface = nullptr;
    PFun_slGetNativeInterface* _slGetNativeInterface = nullptr;
    PFun_slSetD3DDevice* _slSetD3DDevice = nullptr;
};