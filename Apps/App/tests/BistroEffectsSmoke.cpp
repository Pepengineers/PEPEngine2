#include "App.Base/App.h"
#define WinMain BistroLoadingEntryPoint
#include "../src/App.cpp"
#undef WinMain
#include <d3d12sdklayers.h>
#include <iostream>

int main()
{
    std::cout << std::unitbuf;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ConsoleModule console;
        console.Initialize();
        BistroApp app(GetModuleHandleW(nullptr));
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
        const auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        const auto entityCount=world.GetECS().GetEntityCount();
        if(world.GetECS().GetPool<GrassRenderComponent>().Size()!=0||world.GetECS().GetPool<WindComponent>().Size()!=0)
            throw std::runtime_error("Bistro must not acquire grass/wind components");
        auto& transform = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        auto& camera = world.GetECS().Get<CameraComponent>(world.ActiveCamera);
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(render->GetCurrentPrimaryFrameConstants()->MainCB->GetResource().D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        const auto set = [&](const wchar_t* name, const char* value)
        {
            if (!console.FindConsoleVariable(name).SetFromString(value))
                throw std::runtime_error("Bistro effect CVar rejected a valid value");
        };
        set(L"Render.Upscaler.Enabled", "true");
        set(L"Render.Upscaler.Quality", "3");
        for (unsigned frame = 0; frame < 10; ++frame)
        {
            if (frame == 1 || frame == 8) set(L"Render.Upscaler.Enabled", "false");
            if (frame == 2)
            {
                set(L"Render.Upscaler.Quality", "0");
                set(L"Render.Upscaler.Enabled", "true");
            }
            if (frame >= 3 && frame <= 5)
                set(L"Render.Upscaler.Quality", std::to_string(frame-2).c_str());
            if (frame == 6) set(L"Render.Upscaler.Quality", "0");
            if (frame == 7) set(L"Render.Upscaler.Quality", "3");
            if (frame == 9) set(L"Render.Upscaler.Quality", "1");
            if (frame == 2 || frame == 7)
            {
                set(L"Render.SSR.Enabled", "true");
                set(L"Render.MotionBlur.Enabled", "true");
                set(L"Render.DOF.Enabled", "true");
                set(L"Render.DOF.FocusDistance", "1000");
            }
            if (frame == 4 || frame == 5)
            {
                const char* level = frame == 4 ? "1" : "3";
                set(L"Render.AO.Quality", level);
                set(L"Render.SSR.Quality", level);
                set(L"Render.MotionBlur.Quality", level);
                set(L"Render.DOF.Quality", level);
            }
            if (frame == 6)
            {
                set(L"Render.SSR.Enabled", "false");
                set(L"Render.MotionBlur.Enabled", "false");
                set(L"Render.DOF.Enabled", "false");
            }
            if (frame >= 3)
            {
                transform.Location.x += 1;
                transform.Rotation.y += 0.25f;
                transform.DirtyFlag = true;
            }
            if (frame == 9) render->SetActiveCamera(&camera);
            const auto* common = render->GetRenderPipelineCommonData();
            const auto previousHistory = common->HistoryRevision;
            app.BenchmarkFrame();
            const bool native = frame == 1 || frame == 2 || frame == 6 || frame == 8 || frame == 9;
            const bool modeChanged = frame == 1 || (frame >= 3 && frame <= 8);
            if (frame > 0 && common->HistoryRevision != previousHistory + (modeChanged ? 1 : 0))
                throw std::runtime_error("Upscaler transitions must reset history once; unchanged native mode must preserve it");
            if (render->PrimaryPipelineHasFlag(RENDER_PASS_FLAG_USE_JITTER) == native)
                throw std::runtime_error("Pipeline jitter flags must follow native/XeSS mode");
            if (native)
            {
                if (common->DownscaledWidth != common->WindowWidth || common->DownscaledHeight != common->WindowHeight ||
                    common->ActiveCameraJitterOffsetX != 0 || common->ActiveCameraJitterOffsetY != 0)
                    throw std::runtime_error("Native Bistro must use the full grid and clear camera jitter before GPU upload");
            }
            else if (common->DownscaledWidth >= common->WindowWidth || common->DownscaledHeight >= common->WindowHeight)
                throw std::runtime_error("XeSS must query and use its lower input dimensions");
            if(world.GetECS().GetEntityCount()!=entityCount||world.GetECS().GetPool<GrassRenderComponent>().Size()!=0||world.GetECS().GetPool<WindComponent>().Size()!=0)
                throw std::runtime_error("Rendering must not create grass/wind entities");
            std::cout << "Bistro HDR frame " << frame << " passed\n";
        }
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T bytes;
            ThrowIfFailed(info->GetMessage(i, nullptr, &bytes));
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        std::cout << "Bistro: ten native/XeSS HDR frames, camera motion, effects, quality switches and cut passed; no objects added.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
