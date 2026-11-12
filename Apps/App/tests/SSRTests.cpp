#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/GDX12SSRSettings.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Set(const wchar_t* name, const char* value)
{
    Require(ConsoleModule().FindConsoleVariable(name).SetFromString(value), "CVar assignment rejected");
}

static void SettingsTests()
{
    ConsoleModule console;
    const std::filesystem::path ini = "Build/Review/ssr-settings.ini";
    std::filesystem::create_directories(ini.parent_path());
    {
        std::ofstream file(ini);
        Require(bool(file), "Cannot write SSR INI fixture");
        file << "[ConsoleVariables]\nRender.SSR.Quality=3\nRender.SSR.SamplesPerQuad=1\n";
    }
    Require(console.LoadIni(ini).Error.empty() && GetSSRSettings().SamplesPerQuad == 1,
        "SSR INI override did not reach the frame snapshot");
    for (auto [name, bad] : { std::pair{ L"Render.SSR.Quality", "5" },
        { L"Render.SSR.SamplesPerQuad", "3" }, { L"Render.SSR.MaxIntersections", "0" },
        { L"Render.SSR.MinOccupancy", "33" }, { L"Render.SSR.Thickness", "0" },
        { L"Render.SSR.RoughnessCutoff", "1.01" }, { L"Render.SSR.TemporalStability", "nan" },
        { L"Render.SSR.VarianceThreshold", "inf" }, { L"Render.SSR.MostDetailedMip", "7" } })
        Require(!console.FindConsoleVariable(name).SetFromString(bad), "Invalid SSR setting accepted");
    Set(L"Render.SSR.SamplesPerQuad", "2");
    Set(L"Render.SSR.Quality", "1");
    Require(GetSSRSettings().SamplesPerQuad == 2, "Low erased manual ray override");
    Set(L"Render.SSR.Quality", "3");
    Require(GetSSRSettings().SamplesPerQuad == 2, "High erased manual ray override");
    Set(L"Render.SSR.SamplesPerQuad", "-1");
    Require(GetSSRSettings().SamplesPerQuad == 4, "Preset inheritance failed");
    Set(L"Render.SSR.Quality", "0");
    Require(!GetSSRSettings().Enabled, "Off still enabled");
    const int intersections[] = { 64, 128, 256, 512 };
    const int rays[] = { 1, 2, 4, 4 };
    for (int tier = 1; tier <= 4; ++tier)
    {
        Set(L"Render.SSR.Quality", std::to_string(tier).c_str());
        const auto effective = GetSSRSettings();
        Require(effective.Enabled && effective.MaxIntersections == intersections[tier - 1] &&
            effective.SamplesPerQuad == rays[tier - 1], "Wrong inherited SSR budget");
    }
    Set(L"Render.SSR.Quality", "2");
    Set(L"Render.SSR.Enabled", "false");
    Require(!GetSSRSettings().Enabled, "Enabled=false ignored");
    Set(L"Render.SSR.Enabled", "true");
    std::cout << "SSR CVar validation / override / Off tests passed\n";
}

// Isolated analytic fixture: a horizontal mirror and a red/green emissive wall, in centimetres.
// Its ray/plane intersections supply an oracle independent of screen-space ray marching.
class Fixture
{
public:
    GDX12Device& Device;
    GDX12DeviceResources Resources;
    RenderPipelineCommonData Common;
    GDX12Texture Color, Depth, Normal, F0, Motion, Environment, DFG, LocalEnvironment;
    GDX12SSSRPass SSR;
    UINT Width = 257, Height = 129;
    UINT Frame = 0;
    std::vector<UINT64> Fences;

    explicit Fixture(GDX12Device& device) : Device(device)
    {
        Resources.Initialize(&device);
        Fences.resize(Resources.FrameConstants.size());
        Common.ActiveCameraCBufferIndex = 0;
        for (auto& frame : Resources.FrameConstants) frame->CameraCB->Resize(1);
        Create(Color, DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height);
        Create(Depth, DXGI_FORMAT_R32_FLOAT, Width, Height);
        Create(Normal, DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height);
        Create(F0, DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height);
        Create(Motion, DXGI_FORMAT_R16G16_FLOAT, Width, Height);
        Create(Environment, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, true);
        Create(DFG, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1);
        Create(LocalEnvironment, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, true);
        const uint16_t localColor[] = { Half(1.5f), Half(2), Half(0.3f), Half(1) };
        for (UINT face = 0; face < 6; ++face) Upload(LocalEnvironment, localColor, 8, face);
        const uint16_t blue[] = { Half(0.05f), Half(0.1f), Half(0.2f), Half(1) };
        for (UINT face = 0; face < 6; ++face) Upload(Environment, blue, 8, face);
        const uint16_t dfg[] = { Half(1), 0, 0, 0 };
        Upload(DFG, dfg, 8);
        SSR.Setup(&Resources, &Common);
        SSR.SetInputs({ &Color, &Depth, &Normal, &F0, &Motion, &Environment, &DFG });
        SSR.Initialize();
        Fill();
    }

    static uint16_t Half(float value) { return PackedVector::XMConvertFloatToHalf(value); }

    void Create(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, bool cube = false)
    {
        GDX12TextureDesc desc;
        desc.Width = width; desc.Height = height;
        desc.Format = desc.SRVDesc.Format = format;
        desc.SRV_UAV_Heap = Resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = cube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        if (cube)
        {
            auto resource = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 6, 1);
            desc.ExternalResource = Device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
                D3D12_RESOURCE_STATE_COMMON);
        }
        texture.Initialize(desc);
    }

    void Upload(GDX12Texture& texture, const void* data, UINT rowBytes, UINT subresource = 0)
    {
        auto desc = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        Device.GetDevice()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(Device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        void* mapped;
        ThrowIfFailed(upload->Map(0, nullptr, &mapped));
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
            memcpy(static_cast<BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                static_cast<const BYTE*>(data) + y * rowBytes, rowBytes);
        upload->Unmap(0, nullptr);
        auto* queue = Device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ texture.GetResource()->GetCopyDestBarrier() });
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        dst.pResource = texture.GetResource()->D3DResource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = subresource;
        commands->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        queue->ExecuteCommandList(commands);
        queue->Flush();
    }

    void Fill(float roughness = 0, bool wall = true, float cameraX = 0, float objectX = 0)
    {
        Common.WindowWidth = Common.DownscaledWidth = Width;
        Common.WindowHeight = Common.DownscaledHeight = Height;
        Common.CameraView = Matrix::CreateTranslation(-cameraX, 0, 0);
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, float(Width) / Height, 10, 2000);
        projection._33 = 10.0f / 1990;
        projection._43 = 20000.0f / 1990;
        projection._31 -= Common.ActiveCameraJitterOffsetX * 2 / Width;
        projection._32 += Common.ActiveCameraJitterOffsetY * 2 / Height;
        Common.CameraProjection = projection;
        const Matrix viewProjection = Common.CameraView * projection;
        GDX12CameraConstants camera;
        camera.View = Common.CameraView.Transpose();
        camera.ViewProj = viewProjection.Transpose();
        camera.InvViewProj = viewProjection.Invert().Transpose();
        camera.CameraLocation = { cameraX, 0, 0 };
        for (auto& frame : Resources.FrameConstants) frame->CameraCB->CopyData(0, camera);
        std::vector<uint16_t> color(Width * Height * 4), normals(color.size()), f0(color.size()), motion(Width * Height * 2);
        std::vector<float> depth(Width * Height);
        for (UINT y = 0; y < Height; ++y)
            for (UINT x = 0; x < Width; ++x)
            {
                const float rayX = ((x + 0.5f - Common.ActiveCameraJitterOffsetX) / Width * 2 - 1) * Width / Height;
                const float rayY = 1 - (y + 0.5f - Common.ActiveCameraJitterOffsetY) / Height * 2;
                const float floorT = rayY < 0 ? -100 / rayY : 1e9f;
                const bool floor = floorT < 500 || !wall;
                const float distance = floor ? floorT : 500;
                const size_t i = size_t(y) * Width + x;
                const bool valid = distance < 2000;
                if (valid) depth[i] = -projection._33 + projection._43 / distance;
                const float values[] = { floor ? 0.30f : (cameraX + rayX * distance > objectX ? 8.0f : 0.1f),
                    floor ? 0.35f : (cameraX + rayX * distance > objectX ? 0.1f : 8.0f), floor ? 0.45f : 0.1f, 1 };
                for (UINT c = 0; c < 4; ++c) color[i * 4 + c] = Half(values[c]);
                normals[i * 4 + 1] = floor ? Half(1) : 0;
                normals[i * 4 + 2] = floor ? 0 : Half(1);
                normals[i * 4 + 3] = Half(roughness);
                f0[i * 4] = f0[i * 4 + 1] = f0[i * 4 + 2] = Half(1);
                f0[i * 4 + 3] = floor ? 0 : Half(1);
                // Stationary world geometry under camera translation: prev-current UV.
                motion[i * 2] = valid ? Half(cameraX * projection._11 / distance) : 0;
            }
        Upload(Color, color.data(), Width * 8);
        Upload(Depth, depth.data(), Width * 4);
        Upload(Normal, normals.data(), Width * 8);
        Upload(F0, f0.data(), Width * 8);
        Upload(Motion, motion.data(), Width * 4);
    }

    double Draw(GDX12GPUTimer* timer = nullptr, bool wait = true)
    {
        Resources.CurrFrameConstantsIndex = Frame++ % static_cast<UINT>(Resources.FrameConstants.size());
        auto* queue = Device.GetCommandQueue();
        const auto slot = Resources.CurrFrameConstantsIndex;
        if (Fences[slot]) queue->CPUWaitForFenceValue(Fences[slot]);
        auto* commands = queue->GetCommandList();
        if (timer) timer->Start(commands);
        Resources.FrameConstants[slot]->UploadReflectionProbeData(commands);
        SSR.Execute(commands);
        if (timer) timer->Stop(commands);
        queue->ExecuteCommandList(commands);
        Fences[slot] = commands->FenceValue;
        if (timer) timer->OnCommandListExecuted(commands->FenceValue);
        if (wait) queue->Flush();
        return timer ? timer->GetTimeMS() : 0;
    }

    std::vector<float> Read(GDX12Texture& texture)
    {
        auto desc = texture.GetResource()->D3DResource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes;
        Device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(Device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        auto* queue = Device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = texture.GetResource()->D3DResource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        commands->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped;
        ThrowIfFailed(readback->Map(0, nullptr, &mapped));
        std::vector<float> result(Width * Height * 4);
        for (UINT y = 0; y < Height; ++y)
        {
            auto row = reinterpret_cast<const uint16_t*>(static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
            for (UINT x = 0; x < Width * 4; ++x)
            {
                const float value = texture.GetFormat() == DXGI_FORMAT_R8G8B8A8_UNORM ?
                    reinterpret_cast<const BYTE*>(row)[x] / 255.0f : PackedVector::XMConvertHalfToFloat(row[x]);
                Require(std::isfinite(value) && value >= 0, "Non-finite/negative HDR readback");
                result[(size_t(y) * Width * 4) + x] = value;
            }
        }
        readback->Unmap(0, nullptr);
        return result;
    }

    std::vector<float> Result() { return Read(*SSR.GetOutput(0)->GetTexture()); }

    void Resize(UINT width, UINT height)
    {
        Width = width; Height = height;
        Device.GetCommandQueue()->Flush();
        for (auto* texture : { &Color, &Depth, &Normal, &F0, &Motion }) texture->Resize(width, height);
        SSR.Resize();
        Fill();
    }
};

static void CheckHits(const std::vector<float>& pixels, UINT width, UINT height)
{
    const size_t red = (size_t(height * 4 / 5) * width + width * 2 / 3) * 4;
    const size_t green = (size_t(height * 4 / 5) * width + width / 3) * 4;
    std::cout << "Mirror red=" << pixels[red] << ',' << pixels[red + 1] << ',' << pixels[red + 2]
        << " green=" << pixels[green] << ',' << pixels[green + 1] << ',' << pixels[green + 2] << '\n';
    Require(pixels[red] > 5 && pixels[red + 1] < 1 && pixels[green + 1] > 5 && pixels[green] < 1,
        "SSSR did not reflect the distinct colored wall");
    Require(std::abs(pixels[red] - 8.25f) < 0.08f && std::abs(pixels[red + 2] - 0.35f) < 0.03f,
        "HDR/specular resolve lost energy or added global specular twice");
}

static void CheckDebug(ID3D12InfoQueue* info)
{
    bool failed = false;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        info->GetMessage(i, nullptr, &size);
        std::vector<BYTE> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        info->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            std::cerr << message->pDescription << '\n';
            failed = true;
        }
    }
    Require(!failed, "D3D12 debug layer reported an SSR warning/error");
}

static void CheckToneMap(Fixture& fixture)
{
    GDX12Texture display;
    GDX12TextureDesc desc;
    desc.Width = fixture.Width; desc.Height = fixture.Height;
    desc.Format = desc.RTVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.CreateSRV = false;
    desc.CreateRTV = true;
    desc.RTVHeap = fixture.Resources.RTVHeap.get();
    desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
    desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    display.Initialize(desc);
    GDX12OutputToScreenPass toneMap;
    toneMap.Setup(&fixture.Resources, &fixture.Common);
    toneMap.SetInputs({ fixture.SSR.GetOutput(0), &display });
    toneMap.Initialize();
    auto* queue = fixture.Device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    toneMap.Execute(commands);
    queue->ExecuteCommandList(commands); queue->Flush();
    const auto hdr = fixture.Result(), ldr = fixture.Read(display);
    const size_t red = (size_t(fixture.Height * 4 / 5) * fixture.Width + fixture.Width * 2 / 3) * 4;
    for (UINT c = 0; c < 3; ++c)
    {
        const float mapped = hdr[red + c] / (1 + hdr[red + c]);
        const float srgb = mapped <= 0.0031308f ? mapped * 12.92f : 1.055f * std::pow(mapped, 1 / 2.4f) - 0.055f;
        Require(std::abs(ldr[red + c] - srgb) < 1.5f / 255, "Final HDR tone map/sRGB encoding is wrong");
    }
}

int main(int argc, char** argv)
{
    try
    {
        SettingsTests();
        if (argc > 1 && std::string(argv[1]) == "--cpu-only") return 0;
        if (argc > 1 && std::string(argv[1]) == "--five-frames") Set(L"Render.NumFrames", "5");
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
        Require(mutex != nullptr, "GPU validation mutex creation failed");
        std::cout << "Waiting for feature GPU validation mutex..." << std::endl;
        DWORD wait = WaitForSingleObject(mutex, INFINITE);
        Require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "GPU validation mutex wait failed");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (argc > 1 && std::string(argv[1]) == "--gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        {
            Fixture fixture(device);
            info->ClearStoredMessages(); // Existing empty scene caches emit zero-element descriptor warnings.
            fixture.Draw();
            CheckDebug(info.Get());
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            CheckToneMap(fixture);
            for (int i = 0; i < 12; ++i) fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            Set(L"Render.SSR.Quality", "0"); fixture.Draw();
            Require(fixture.Result() == fixture.Read(fixture.Color), "Off is not an exact neutral copy");
            Set(L"Render.SSR.Quality", "4"); fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            Set(L"Render.SSR.Quality", "1"); fixture.Draw();
            fixture.Result();
            Set(L"Render.SSR.Quality", "3"); fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            ++fixture.Common.HistoryRevision; fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            fixture.Fill(1); fixture.Draw();
            Require(fixture.Result() == fixture.Read(fixture.Color), "Roughness cutoff changed excluded pixels");
            fixture.Fill(0, false); ++fixture.Common.HistoryRevision; fixture.Draw();
            const auto fallback = fixture.Result(), source = fixture.Read(fixture.Color);
            float maximumError = 0;
            size_t worst = 0;
            for (size_t i = 0; i < fallback.size(); ++i)
            {
                const float error = std::abs(fallback[i] - source[i]);
                if (error > maximumError) { maximumError = error; worst = i; }
            }
            std::cout << "Miss max_error=" << maximumError << " x=" << (worst / 4) % fixture.Width
                << " y=" << (worst / 4) / fixture.Width << " source=" << source[worst] << " output=" << fallback[worst] << '\n';
            // Denoiser neighborhoods straddle the sky at silhouettes; use interior receivers for the constant-environment oracle.
            for (UINT y = fixture.Height * 3 / 4; y < fixture.Height * 7 / 8; ++y)
                for (UINT x = fixture.Width / 4; x < fixture.Width * 3 / 4; ++x)
                    for (UINT c = 0; c < 3; ++c)
                    {
                        const size_t i = (size_t(y) * fixture.Width + x) * 4 + c;
                        Require(std::abs(fallback[i] - source[i]) < 0.015f, "Miss failed to preserve global IBL fallback");
                    }
            auto* localCommands = device.GetCommandQueue()->GetCommandList();
            localCommands->ResourceBarrier({ fixture.LocalEnvironment.GetResource()->GetSRVBarrier() });
            device.GetCommandQueue()->ExecuteCommandList(localCommands);
            device.GetCommandQueue()->Flush();
            D3D12_SHADER_RESOURCE_VIEW_DESC localView = {};
            localView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            localView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            localView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            localView.TextureCubeArray.NumCubes = localView.TextureCubeArray.MipLevels = 1;
            fixture.Resources.ReflectionRadianceSRV.InitAsSRV(fixture.LocalEnvironment.GetResource()->D3DResource.Get(),
                &localView, fixture.Resources.SRV_UAV_Heap.get(), fixture.Resources.ReflectionRadianceSRV.HeapIndex);
            for (auto& frame : fixture.Resources.FrameConstants)
            {
                GDX12ReflectionProbeData probe;
                probe.Intensity = 1;
                probe.BoxMin = { -5000, -5000, -5000 };
                probe.BoxMax = { 5000, 5000, 5000 };
                probe.BlendDistance = 1;
                frame->ReflectionProbesUpload->CopyData(0, probe);
            }
            ++fixture.Common.HistoryRevision;
            fixture.Draw();
            const auto localFallback = fixture.Result();
            const size_t localCenter = (size_t(fixture.Height * 4 / 5) * fixture.Width + fixture.Width / 2) * 4;
            std::cout << "Local miss center source=" << source[localCenter] << "," << source[localCenter+1] << "," << source[localCenter+2]
                << " output=" << localFallback[localCenter] << "," << localFallback[localCenter+1] << "," << localFallback[localCenter+2] << std::endl;
            float relativeMissError = 0;
            constexpr float localRadiance[] = { 1.5f, 2.0f, 0.3f };
            for (UINT y = fixture.Height * 3 / 4; y < fixture.Height * 7 / 8; ++y)
                for (UINT x = fixture.Width / 4; x < fixture.Width * 3 / 4; ++x)
                    for (UINT c = 0; c < 3; ++c)
                    {
                        const size_t i = (size_t(y) * fixture.Width + x) * 4 + c;
                        relativeMissError = (std::max)(relativeMissError,
                            std::abs(localFallback[i] - source[i]) / (std::max)(1.0f, localRadiance[c]));
                    }
            // FP16 denoiser filtering is approximate; a wrong global fallback differs by 0.1..1.9 radiance.
            std::cout << "Local miss normalized max error=" << relativeMissError << std::endl;
            Require(relativeMissError < 0.02f, "SSSR miss and subtraction did not use the same local probe fallback");
            for (auto& frame : fixture.Resources.FrameConstants)
                frame->ReflectionProbesUpload->CopyData(0, GDX12ReflectionProbeData{});
            std::cout << "Local/global SSSR miss fallback preserves one specular contribution.\n";
            fixture.Resize(319, 181); fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            for (int i = 0; i < 8; ++i)
            {
                fixture.Common.ActiveCameraJitterOffsetX = i % 2 ? 0.25f : -0.25f;
                fixture.Common.ActiveCameraJitterOffsetY = i % 3 * 0.25f - 0.25f;
                fixture.Fill(); fixture.Draw();
            }
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            fixture.Fill(0, true, 10); fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            fixture.Fill(0, true, 10, 30); fixture.Draw();
            CheckHits(fixture.Result(), fixture.Width, fixture.Height);
            fixture.Resize(1280, 720);
            fixture.Fill(0.25f);
            GDX12GPUTimer timer(&device);
            auto adapter = GDX12DeviceFactory::GetMostPerformantAdapter();
            for (int quality = 1; quality <= 3; ++quality)
            {
                Set(L"Render.SSR.Quality", std::to_string(quality).c_str());
                for (int i = 0; i < 16; ++i) fixture.Draw();
                std::vector<double> times;
                for (int i = 0; i < 96; ++i) times.push_back(fixture.Draw(&timer));
                std::sort(times.begin(), times.end());
                DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
                ThrowIfFailed(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
                std::cout << "1280x720 preset=" << quality << " GPU median_ms=" << times[times.size() / 2]
                    << " p95_ms=" << times[times.size() * 95 / 100]
                    << " process_local_resident_MiB=" << memory.CurrentUsage / 1048576.0 << '\n';
                fixture.Result();
            }
            for (int frame = 0; frame < 15; ++frame) fixture.Draw(nullptr, false);
            device.GetCommandQueue()->Flush();
            fixture.Result();
            ++fixture.Resources.ReflectionEnvironmentRevision;
            fixture.Draw();
            fixture.Result();
            std::cout << "Queued frame slots and environment revision reset passed.\n";
            fixture.SSR.ClearDenendencies();
        }
        device.GetCommandQueue()->Flush();
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<BYTE> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                std::cerr << message->pDescription << '\n';
                Require(false, "D3D12 debug layer reported an SSR warning/error");
            }
        }
        ReleaseMutex(mutex); CloseHandle(mutex);
        std::cout << "SSR GPU hits/HDR/fallback/roughness/history/Off/resize/jitter/motion/frame slots passed\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
