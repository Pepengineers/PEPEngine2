#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionPreviewPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

// Included after Require/ReadTexture in ReflectionProbeTests.cpp.
static void TestReflectionPreview(GDX12Device& device)
{
    // A small independent heap keeps this fixture from modifying the real probe publication.
    GDX12DeviceResources resources;
    resources.Device = &device;
    resources.SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(&device,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    resources.RTVHeap = std::make_unique<GDX12DescriptorHeap>(&device,
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 32, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    resources.FrameConstants.push_back(std::make_unique<GDX12FrameConstants>(&device));
    auto& frame = *resources.FrameConstants[0];
    frame.ReflectionProbes->CreateSRV(resources.SRV_UAV_Heap.get(), 11);
    frame.CameraCB->Resize(1);
    RenderPipelineCommonData common;
    common.ActiveCameraCBufferIndex = 0;
    const Matrix view = Matrix::CreateLookAt(Vector3::Zero, Vector3::UnitZ, Vector3::UnitY);
    Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, 65.0f / 37, 1, 10000);
    projection._33 = 1.0f / 9999;
    projection._43 = 10000.0f / 9999;
    GDX12CameraConstants camera = {};
    XMStoreFloat4x4(&camera.ViewProj, (view * projection).Transpose());
    XMStoreFloat4x4(&camera.InvViewProj, (view * projection).Invert().Transpose());
    frame.CameraCB->CopyData(0, camera);

    GDX12Texture color, depth, cubes;
    GDX12RenderHelpers::InitializeComputeTexture(color, resources, 65, 37, DXGI_FORMAT_R16G16B16A16_FLOAT);
    GDX12RenderHelpers::InitializeComputeTexture(depth, resources, 65, 37, DXGI_FORMAT_R32_FLOAT);
    GDX12DescriptorHeap cpuUAVs(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    D3D12_UNORDERED_ACCESS_VIEW_DESC clearView = {};
    clearView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    clearView.Format = color.GetFormat();
    device.GetDevice()->CreateUnorderedAccessView(color.GetResource()->D3DResource.Get(), nullptr,
        &clearView, cpuUAVs.GetCPUHandle(0));
    clearView.Format = depth.GetFormat();
    device.GetDevice()->CreateUnorderedAccessView(depth.GetResource()->D3DResource.Get(), nullptr,
        &clearView, cpuUAVs.GetCPUHandle(1));
    GDX12TextureDesc cubeDesc;
    cubeDesc.Width = cubeDesc.Height = 2;
    cubeDesc.Format = cubeDesc.SRVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cubeDesc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cubeDesc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    cubeDesc.SRVDesc.TextureCubeArray.NumCubes = 2;
    cubeDesc.SRVDesc.TextureCubeArray.MipLevels = 2;
    cubeDesc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    cubeDesc.SRVHeapIndex = 12;
    cubeDesc.ExternalResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
        CD3DX12_RESOURCE_DESC::Tex2D(cubeDesc.Format, 2, 2, 12, 2, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_RESOURCE_STATE_COMMON);
    cubes.Initialize(cubeDesc);
    resources.ReflectionRadianceSRV.InitAsSRV(cubes.GetResource()->D3DResource.Get(),
        &cubeDesc.SRVDesc, resources.SRV_UAV_Heap.get(), 13);
    std::vector<std::unique_ptr<GDX12Descriptor>> faces;
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ cubes.GetResource()->GetRenderTargetBarrier() });
    const float colors[3][4] = {{4,0,0,1}, {0,4,0,1}, {0,0,8,1}};
    for (UINT slice = 0; slice < 12; ++slice)
        for (UINT mip = 0; mip < 2; ++mip)
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = cubeDesc.Format;
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.ArraySize = 1;
            desc.Texture2DArray.FirstArraySlice = slice;
            desc.Texture2DArray.MipSlice = mip;
            auto& face = faces.emplace_back(std::make_unique<GDX12Descriptor>());
            face->InitAsRTV(cubes.GetResource()->D3DResource.Get(), &desc,
                resources.RTVHeap.get(), resources.RTVHeap->GetAvailableIndex());
            commands->GetCommandList()->ClearRenderTargetView(face->CPUHandle, colors[mip ? slice / 6 : 2], 0, nullptr);
        }
    commands->ResourceBarrier({ cubes.GetResource()->GetSRVBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    resources.ReflectionProbes.resize(2);
    GDX12ReflectionProbeData published;
    published.Position = {0,0,100};
    published.BoxMin = {-20,-20,80};
    published.BoxMax = {20,20,120};
    published.Intensity = 1;
    published.MinMip = published.MaxMip = 1;
    for (UINT i = 0; i < 2; ++i) frame.ReflectionProbesUpload->CopyData(i, published);
    GDX12ReflectionPreviewPass preview;
    preview.Setup(&resources, nullptr, &common);
    preview.SetInputs({ &color, &depth });
    preview.Initialize();
    const float background[4] = {0.125f,0.25f,0.5f,1};
    const auto render = [&](UINT selected, float sceneDepth)
    {
        for (UINT i = 0; i < 2; ++i) resources.ReflectionProbes[i].PreviewSphere = selected == i;
        auto* list = queue->GetCommandList();
        list->SetDescriptorHeaps({ resources.SRV_UAV_Heap.get() });
        list->ResourceBarrier({ color.GetResource()->GetUnorderedAccessBarrier(), depth.GetResource()->GetUnorderedAccessBarrier() });
        const float depthClear[4] = {sceneDepth,0,0,0};
        for (auto [texture, clear] : {std::pair{&color, background}, std::pair{&depth, depthClear}})
            list->ClearUnorderedAccessViewFloat(texture->GetUAV()->GPUHandle, cpuUAVs.GetCPUHandle(texture == &color ? 0 : 1),
                texture->GetResource()->D3DResource.Get(), clear);
        list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::UAV(color.GetResource()->D3DResource.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(depth.GetResource()->D3DResource.Get()) });
        frame.UploadReflectionProbeData(list);
        preview.Execute(list);
        queue->ExecuteCommandList(list);
        queue->Flush();
        return ReadTexture(device, color, 0);
    };
    const auto identity = [&](const std::vector<float>& pixels)
    {
        for (size_t i = 0; i < pixels.size(); ++i)
            Require(std::abs(pixels[i] - background[i % 3]) < 0.001f, "Probe preview changed a hidden/disabled pixel");
    };
    for (UINT index = 0; index < 2; ++index)
    {
        const auto pixels = render(index, 0);
        const size_t center = (18 * 65 + 32) * 3;
        for (UINT c = 0; c < 3; ++c)
        {
            Require(std::abs(pixels[center + c] - colors[index][c]) < 0.001f,
                "Preview did not sample its exact cube slice/first populated mip");
            Require(std::abs(pixels[c] - background[c]) < 0.001f, "Sphere preview painted outside its silhouette");
        }
    }
    identity(render(32, 0));
    identity(render(0, 0.1f)); // Opaque depth is in front of the complete sphere.
    published.Intensity = 0;
    frame.ReflectionProbesUpload->CopyData(0, published);
    identity(render(0, 0));
    std::cout << "Reflection preview exact cube/mip, silhouette, occlusion, disabled and unpublished checks passed.\n";
}
