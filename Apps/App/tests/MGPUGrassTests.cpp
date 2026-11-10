#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "GrassMGPURouteChecks.h"
#include "GrassShadowRetryChecks.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <string_view>

namespace
{
    class TestApp final : public App
    {
    public:
        TestApp() : App(GetModuleHandleW(nullptr)) {}
    };

    ComPtr<IDXGIAdapter4> SecondAdapter(LUID primary)
    {
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter4> adapter;
            const auto status = GDX12DeviceFactory::GetFactory()->EnumAdapterByGpuPreference(index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (status == DXGI_ERROR_NOT_FOUND) break;
            ThrowIfFailed(status);
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(adapter->GetDesc1(&desc));
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
                (desc.AdapterLuid.LowPart == primary.LowPart && desc.AdapterLuid.HighPart == primary.HighPart))
                continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr)))
                return adapter;
        }
        throw std::runtime_error("Grass MGPU validation requires a second compatible hardware adapter");
    }

    void CheckDebug(GDX12Device& device)
    {
        ThrowIfFailed(device.GetDevice()->GetDeviceRemovedReason());
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const bool gbv = argc == 2 && std::string_view(argv[1]) == "--gpu-validation";
        if (argc != 1 && !gbv) throw std::invalid_argument("Expected only --gpu-validation");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }

        ConsoleModule{}.Initialize();
        TestApp app;
        GDX12Device primary, secondary;
        ThrowIfFailed(primary.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(SecondAdapter(primary.GetAdapterLuid())));
        GDX12CommandQueue copyA(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12CommandQueue copyB(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12DeviceResources resources;
        resources.Initialize(&primary);
        resources.FrameConstants.front()->MaterialCache->Resize(1);
        RenderPipelineCommonData common;
        common.CameraView = Matrix::Identity;
        common.ResetHistory = false;
        RenderMGPURuntime runtime({&primary, &secondary, &copyA, &copyB});
#ifdef _DEBUG
        CheckNativeGrassShadowRetry(primary, secondary, runtime);
        std::cout << "G2 retry passed: native no-record refusal, same PointSource slot and masked depth.\n";
#endif
        CheckGrassMGPURoute(runtime, resources, secondary, common);
        runtime.Shutdown();
        GDX12ShaderCompiler::Shutdown();
        CheckDebug(primary);
        CheckDebug(secondary);
        std::cout << "Grass MGPU route passed: two adapters, seeded Latest and Exact, age, cadence and primary continuation.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << L'\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
