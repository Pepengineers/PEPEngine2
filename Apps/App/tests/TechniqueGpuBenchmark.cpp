// Reuse the existing upload/texture fixture without adding production test hooks.
#define main FidelityFXGpuSmokeMain
#include "FidelityFXGpuSmoke.cpp"
#undef main
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>

namespace
{
    std::string AdapterName(IDXGIAdapter4* adapter)
    {
        DXGI_ADAPTER_DESC3 description{};
        ThrowIfFailed(adapter->GetDesc3(&description));
        const int length = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
        Require(length > 0, "Cannot encode adapter name");
        std::string name(static_cast<size_t>(length), '\0');
        Require(WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name.data(), length, nullptr, nullptr) > 0,
            "Cannot encode adapter name");
        name.pop_back();
        return name;
    }
}

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        Require(argc == 2, "Usage: TechniqueGpuBenchmark.exe <output.json>");
        const std::filesystem::path outputPath(argv[1]);
        if (outputPath.has_parent_path()) std::filesystem::create_directories(outputPath.parent_path());
        std::ofstream output(outputPath);
        Require(output.good(), "Cannot open benchmark artifact");
        output << std::setprecision(9);
        ConsoleModule console;
        const auto set = [&](const wchar_t* name, const char* value)
        { Require(console.FindConsoleVariable(name).SetFromString(value), "Invalid benchmark CVar"); };
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        auto adapter = GDX12DeviceFactory::GetMostPerformantAdapter();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<IDXGIAdapter3> memoryAdapter;
        ThrowIfFailed(adapter.As(&memoryAdapter));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        constexpr UINT width = 1920, height = 1080, warmup = 4, samples = 16;
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = width;
        common.WindowHeight = common.DownscaledHeight = height;
        common.ActiveCameraCBufferIndex = 0;
        constexpr float nearPlane = 0.1f, farPlane = 10000;
        common.ActiveCameraFOV = XMConvertToRadians(60.0f);
        common.ActiveCameraNearPlane = nearPlane;
        common.ActiveCameraFarPlane = farPlane;
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XMConvertToRadians(60.0f),
            static_cast<float>(width) / height, nearPlane, farPlane);
        projection._33 = nearPlane / (farPlane - nearPlane);
        projection._43 = farPlane * projection._33;
        common.CameraViewToClip = projection;
        common.ClipToCameraView = projection.Invert();
        common.ActiveCameraView = Matrix::Identity;
        common.ActiveCameraViewProjection = common.ActiveCameraPreviousViewProjection = projection;
        GDX12CameraConstants camera{};
        camera.View = Identity4x4();
        camera.ViewProj = camera.ViewProjNoJitter = camera.PrevViewProjNoJitter = projection.Transpose();
        camera.InvViewProj = projection.Invert().Transpose();
        camera.NearPlane = nearPlane; camera.FarPlane = farPlane;
        for (auto& frame : resources.FrameConstants)
        {
            frame->CameraCB->Resize(1);
            frame->CameraCB->CopyData(0, camera);
        }
        GDX12Texture color, depth, motion, normal, material;
        MakeInput(resources, color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, depth, width, height, DXGI_FORMAT_R32_FLOAT);
        MakeInput(resources, motion, width, height, DXGI_FORMAT_R16G16_FLOAT);
        MakeInput(resources, normal, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeInput(resources, material, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        std::vector<uint16_t> colors(width * height * 4), normals(colors.size()), materials(colors.size()), velocities(width * height * 2);
        std::vector<float> depths(width * height);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                const size_t pixel = static_cast<size_t>(y) * width + x, offset = pixel * 4;
                const float light = ((x / 8 + y / 8) & 1) ? 4.0f : 0.1f;
                for (UINT c = 0; c < 3; ++c)
                {
                    colors[offset + c] = PackedVector::XMConvertFloatToHalf(light);
                    materials[offset + c] = PackedVector::XMConvertFloatToHalf(-1);
                }
                colors[offset + 3] = PackedVector::XMConvertFloatToHalf(1);
                normals[offset] = PackedVector::XMConvertFloatToHalf(0.5f);
                normals[offset + 2] = PackedVector::XMConvertFloatToHalf(std::sqrt(0.75f));
                normals[offset + 3] = materials[offset + 3] = PackedVector::XMConvertFloatToHalf(0.3f);
                velocities[pixel * 2] = PackedVector::XMConvertFloatToHalf(128.0f / width);
                depths[pixel] = projection._43 / (x < width / 2 ? 1000.0f : 500.0f) - projection._33;
            }
        Upload(device, color, colors.data(), 8);
        Upload(device, depth, depths.data(), 4);
        Upload(device, motion, velocities.data(), 4);
        Upload(device, normal, normals.data(), 8);
        Upload(device, material, materials.data(), 8);
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common); environment.Initialize();
        environment.Submit({0,{{1,{}}},{}}); environment.PrepareFrame();
        auto* queue = device.GetCommandQueue();
        auto* list = queue->GetCommandList();
        environment.Execute(list); queue->ExecuteCommandList(list); queue->Flush();
        GDX12AmbientOcclusionPass ao;
        ao.Setup(&resources, nullptr, &common); ao.SetInputs({ &depth, &normal }); ao.Initialize();
        GDX12SssrPass ssr;
        ssr.Setup(&resources, nullptr, &common);
        ssr.SetInputs({ &color, &depth, &motion, &normal, &material, environment.GetOutput(1), environment.GetOutput(2) });
        ssr.Initialize();
        GDX12MotionBlurPass blur;
        blur.Setup(&resources, nullptr, &common); blur.SetInputs({ &color, &depth, &motion }); blur.Initialize();
        GDX12DepthOfFieldPass dof;
        dof.Setup(&resources, nullptr, &common); dof.SetInputs({ &color, &depth }); dof.Initialize();
        set(L"Render.AO.Enabled", "true"); set(L"Render.AO.Radius", "100");
        set(L"Render.AO.Intensity", "1"); set(L"Render.AO.Bias", "0.02");
        set(L"Render.AO.Samples", "-1"); set(L"Render.AO.DenoisePasses", "-1");
        set(L"Render.SSR.Enabled", "true"); set(L"Render.SSR.Thickness", "0.1");
        set(L"Render.SSR.RoughnessThreshold", "0.8"); set(L"Render.SSR.TemporalStability", "0.9");
        set(L"Render.SSR.MaxTraversalSteps", "-1"); set(L"Render.SSR.SamplesPerQuad", "-1");
        set(L"Render.MotionBlur.Enabled", "true"); set(L"Render.MotionBlur.ShutterAngle", "180");
        set(L"Render.MotionBlur.MaxRadius", "32"); set(L"Render.MotionBlur.Samples", "-1");
        set(L"Render.DOF.Enabled", "true"); set(L"Render.DOF.FocusDistance", "50");
        set(L"Render.DOF.Aperture", "0.5"); set(L"Render.DOF.MaxCoC", "-1");
        output << "{\n\"adapter\":" << std::quoted(AdapterName(adapter.Get()))
            << ",\n\"width\":" << width << ",\"height\":" << height
            << ",\"warmupFramesPerCase\":" << warmup << ",\"samplesPerCase\":" << samples
            << ",\n\"timing\":\"direct queue GPU timestamps around Execute only; PrepareFrame and allocation excluded; flushed each frame; D3D12 debug layer enabled\""
            << ",\n\"memoryScope\":\"DXGI process local CurrentUsage/Budget bytes after each case; inputs, IBL and all previously created live pass outputs remain allocated; not isolated per-pass allocation\""
            << ",\n\"aoImplementations\":{\"0\":\"SSAO\",\"1\":\"HBAO\",\"2\":\"XeGTAO\"},\"qualities\":{\"1\":\"Low\",\"2\":\"Mid\",\"3\":\"High\"}"
            << ",\n\"workload\":{\"color\":\"HDR checker 8px 0.1/4\",\"depth\":\"reverse-Z RH: left 1000cm/right 500cm\",\"fovDegrees\":60,\"nearCm\":0.1,\"farCm\":10000,\"normal\":[0.5,0,0.8660254],\"roughness\":0.3,\"materialRGB\":-1,\"motionFullFramePixels\":[64,0],\"cameraJitterPixels\":[0,0],\"aoRadiusCm\":100,\"aoIntensity\":1,\"aoBias\":0.02,\"ssrThickness\":0.1,\"ssrRoughnessThreshold\":0.8,\"ssrTemporalStability\":0.9,\"shutterDegrees\":180,\"motionMaxRadiusPx\":32,\"focusCm\":50,\"aperture\":0.5,\"presetOverrides\":-1},\n\"cases\":[\n";
        GDX12GPUTimer timer(&device);
        UINT frameIndex = 0;
        bool first = true;
        const auto measure = [&](GDX12RenderPass& pass, const char* name, int implementation, int quality)
        {
            std::vector<double> times;
            for (UINT frame = 0; frame < warmup + samples; ++frame)
            {
                resources.CurrFrameConstantsIndex = frameIndex++ % static_cast<UINT>(resources.FrameConstants.size());
                pass.PrepareFrame();
                auto* commands = queue->GetCommandList();
                timer.Start(commands); pass.Execute(commands); timer.Stop(commands);
                queue->ExecuteCommandList(commands);
                timer.OnCommandListExecuted(commands->FenceValue);
                queue->Flush();
                const double milliseconds = timer.GetTimeMS();
                Require(std::isfinite(milliseconds) && milliseconds >= 0, "GPU timestamp did not resolve");
                if (frame >= warmup) times.push_back(milliseconds);
            }
            Require(pass.GetOutput(0) && pass.GetOutput(0)->GetTexture()->GetWidth() == width,
                "Benchmark pass did not produce the expected output");
            if (name == std::string("SSSR")) Require(ssr.HasValidHistory(), "SSSR warmup did not establish temporal history");
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            ThrowIfFailed(memoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
            const double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
            std::sort(times.begin(), times.end());
            const double median = (times[times.size() / 2 - 1] + times[times.size() / 2]) * 0.5;
            if (!first) output << ",\n";
            first = false;
            output << "{\"effect\":" << std::quoted(name) << ",\"implementation\":" << implementation
                << ",\"quality\":" << quality << ",\"sampleCount\":" << times.size()
                << ",\"gpuMeanMs\":" << mean << ",\"gpuMedianMs\":" << median
                << ",\"gpuMinMs\":" << times.front() << ",\"gpuMaxMs\":" << times.back()
                << ",\"processLocalCurrentUsageBytes\":" << memory.CurrentUsage << ",\"processLocalBudgetBytes\":" << memory.Budget << "}";
            std::cout << name << " implementation=" << implementation << " quality=" << quality << " GPU median " << median << " ms\n";
        };
        for (int implementation = 0; implementation < 3; ++implementation)
        {
            set(L"Render.AO.Implementation", std::to_string(implementation).c_str());
            for (int quality = 1; quality <= 3; ++quality)
            {
                set(L"Render.AO.Quality", std::to_string(quality).c_str());
                measure(ao, "AO", implementation, quality);
            }
        }
        for (int quality = 1; quality <= 3; ++quality)
        {
            const auto value = std::to_string(quality);
            set(L"Render.SSR.Quality", value.c_str()); measure(ssr, "SSSR", 0, quality);
            set(L"Render.MotionBlur.Quality", value.c_str()); measure(blur, "HPG_MotionBlur", 0, quality);
            set(L"Render.DOF.Quality", value.c_str()); measure(dof, "FidelityFX_DoF", 0, quality);
        }
        output << "\n]}\n";
        output.flush(); Require(output.good(), "Cannot write benchmark artifact");
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T bytes; info->GetMessage(i, nullptr, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        queue->Flush();
        ao.ClearDenendencies(); ssr.ClearDenendencies(); blur.ClearDenendencies(); dof.ClearDenendencies(); environment.ClearDenendencies();
        GDX12ShaderCompiler::Shutdown();
        std::cout << "GPU benchmark artifact: " << outputPath.string() << '\n';
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
