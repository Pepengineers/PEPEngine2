#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void CheckReadback(GDX12Device& device, GDX12Texture& texture, UINT subresource, bool lut, bool depth = false)
{
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetResource()->GetCopySourceBarrier() });
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = texture.GetResource()->D3DResource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    const auto* data = static_cast<const unsigned char*>(mapped);
    bool foundCaster = false;
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < footprint.Footprint.Width; ++x)
        {
            const auto* address = data + footprint.Offset + y * footprint.Footprint.RowPitch + x * (depth ? 4 : 8);
            if (depth)
            {
                const float value = *reinterpret_cast<const float*>(address);
                Require(std::isfinite(value) && value >= 0.0f && value <= 1.0f, "Invalid shadow atlas depth");
                foundCaster |= value < 0.999f;
                continue;
            }
            const auto* pixel = reinterpret_cast<const uint16_t*>(address);
            float values[4];
            for (UINT channel = 0; channel < 4; ++channel)
            {
                values[channel] = PackedVector::XMConvertHalfToFloat(pixel[channel]);
                Require(std::isfinite(values[channel]) && values[channel] >= 0.0f, "IBL readback contains invalid values");
            }
            if (!lut) Require(values[0] + values[1] + values[2] > 0.01f, "Environment map was not populated");
            else if (x == footprint.Footprint.Width - 1)
                Require(values[0] + values[1] <= 1.02f, "BRDF LUT exceeds unit reflectance at normal incidence");
        }
    D3D12_RANGE noWrite = { 0, 0 };
    readback->Unmap(0, &noWrite);
    Require(!depth || foundCaster, "Shadow ExecuteIndirect did not render the test caster");
}

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        BenchmarkEngine engine;
        auto& locator = BenchmarkEngine::GetLocator();
        locator.RegisterModule(std::make_shared<RenderModule>(nullptr, nullptr));
        const std::weak_ptr<RenderModule> original = locator.GetModule<RenderModule>();
        locator.RegisterModule(std::make_shared<RenderModule>(nullptr, nullptr));
        Require(original.expired(), "Replacing an uninitialized renderer must release it safely");
        locator.UnregisterModule<RenderModule>();

        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        // Existing empty scene caches create null/zero-element descriptors before scene loading.
        info->ClearStoredMessages();
        RenderPipelineCommonData common;
        common.WindowWidth = common.DownscaledWidth = 64;
        common.WindowHeight = common.DownscaledHeight = 64;
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common);
        environment.Initialize();
        const auto sourceCube = environment.GetOutput(3)->GetTexture()->GetResource()->D3DResource->GetDesc();
        Require(sourceCube.Width == 1024 && sourceCube.Height == 1024 && sourceCube.DepthOrArraySize == 6 &&
            sourceCube.MipLevels == 11, "Scene cubemap dimensions/subresources were not preserved");
        Require(environment.GetOutput(3)->GetTexture()->GetFormat() == DXGI_FORMAT_BC1_UNORM_SRGB,
            "Scene cubemap must be sampled in linear light through an sRGB view");
        GDX12CascadedShadowPass shadows;
        shadows.Setup(&resources, nullptr, &common);
        shadows.Initialize();
        GDX12OpaquePass opaque;
        opaque.Setup(&resources, nullptr, &common);
        opaque.SetInputs({ shadows.GetOutput(0), environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2) });
        opaque.Initialize();
        GDX12WBOITTransparencyPass transparency;
        transparency.Setup(&resources, nullptr, &common);
        transparency.SetInputs({ opaque.GetOutput(2), shadows.GetOutput(0), environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2) });
        transparency.Initialize();
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        environment.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        CheckReadback(device, *environment.GetOutput(0)->GetTexture(), 0, false);
        CheckReadback(device, *environment.GetOutput(1)->GetTexture(), 0, false);
        CheckReadback(device, *environment.GetOutput(1)->GetTexture(), 7, false);
        CheckReadback(device, *environment.GetOutput(2)->GetTexture(), 0, true);

        SubMesh triangle;
        triangle.Vertices.resize(3);
        triangle.Vertices[0].Position = Vector3(-100, 0, -100);
        triangle.Vertices[1].Position = Vector3(100, 0, -100);
        triangle.Vertices[2].Position = Vector3(0, 0, 100);
        triangle.Indices = { 0, 2, 1 };
        BoundingBox bounds(XMFLOAT3(0, 0, 0), XMFLOAT3(100, 0.1f, 100));
        Mesh mesh({ std::move(triangle) }, bounds);
        resources.GeometryBuffer->AddMesh(&mesh, MeshHandle(0));
        auto& frame = resources.FrameConstants[0];
        frame->TransformCache->Resize(1);
        frame->TransformCache->CopyData(0, GDX12TransformConstants{});
        frame->MaterialCache->Resize(1);
        frame->MaterialCache->CopyData(0, GDX12MaterialConstants{});
        frame->InstanceCache->Resize(1);
        GDX12InstanceData instance = {};
        instance.BoundingBoxCenter = bounds.Center;
        instance.BoundingBoxExtents = bounds.Extents;
        frame->InstanceCache->CopyData(0, instance);
        resources.IndirectCommandsCache->Resize(1);
        GDX12IndirectDrawArgs draw = {};
        draw.IndexCountPerInstance = 3;
        draw.InstanceCount = 1;
        resources.IndirectCommandsCache->CopyData(0, draw);
        const Matrix view = Matrix::CreateLookAt(Vector3(0, 300, -400), Vector3::Zero, Vector3::UnitY);
        frame->ShadowCB->CopyData(0, BuildShadowCascades(view.Invert(), XMConvertToRadians(60.0f), 1.0f,
            0.1f, 10000.0f, Vector3(0.35f, -1.0f, 0.25f)));
        const UINT textureSlot = resources.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
        D3D12_SHADER_RESOURCE_VIEW_DESC testTexture = {};
        testTexture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        testTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        testTexture.Texture2D.MipLevels = 1;
        testTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device.GetDevice()->CreateShaderResourceView(environment.GetOutput(2)->GetTexture()->GetResource()->D3DResource.Get(),
            &testTexture, resources.SRV_UAV_Heap->GetCPUHandle(textureSlot));
        commands = queue->GetCommandList();
        shadows.Execute(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        CheckReadback(device, *shadows.GetOutput(0)->GetTexture(), 0, false, true);
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "DX12 lighting smoke: CSM/PBR PSOs, IBL dispatch/readback, and debug validation passed.\n";
        GDX12ShaderCompiler::Shutdown();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
