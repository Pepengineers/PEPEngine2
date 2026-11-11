#pragma once

// Exercise the opaque mesh material with independent cube slices and mip colors.
template<class Render, class CheckGeometry> static void TestReflectionPreview(GDX12Device& device, GDX12DeviceResources& resources,
    UINT width, UINT height, Render&& render, CheckGeometry&& checkGeometry)
{
    GDX12Texture cubes;
    GDX12TextureDesc desc;
    desc.Width = desc.Height = 2;
    desc.Format = desc.SRVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    desc.SRVDesc.TextureCubeArray.NumCubes = 2;
    desc.SRVDesc.TextureCubeArray.MipLevels = 2;
    desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.ExternalResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
        CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, 2, 2, 12, 2, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_RESOURCE_STATE_COMMON);
    cubes.Initialize(desc);
    const float colors[3][4] = {{4,0,0,1}, {0,4,0,1}, {0,0,8,1}};
    std::vector<std::unique_ptr<GDX12Descriptor>> faces;
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({cubes.GetResource()->GetRenderTargetBarrier()});
    for (UINT slice = 0; slice < 12; ++slice)
        for (UINT mip = 0; mip < 2; ++mip)
        {
            D3D12_RENDER_TARGET_VIEW_DESC view = {};
            view.Format = desc.Format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = 1;
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.MipSlice = mip;
            auto& face = faces.emplace_back(std::make_unique<GDX12Descriptor>());
            face->InitAsRTV(cubes.GetResource()->D3DResource.Get(), &view,
                resources.RTVHeap.get(), resources.RTVHeap->GetAvailableIndex());
            commands->GetCommandList()->ClearRenderTargetView(face->CPUHandle, colors[mip ? slice / 6 : 2], 0, nullptr);
        }
    commands->ResourceBarrier({cubes.GetResource()->GetSRVBarrier()});
    queue->ExecuteCommandList(commands); queue->Flush();
    const size_t center = (size_t(height / 2) * width + width / 2) * 3;
    for (UINT slot = 1; slot <= 2; ++slot)
    {
        const auto pixels = render(slot, cubes.GetSRV()->GPUHandle, true);
        for (UINT c = 0; c < 3; ++c)
            Require(std::abs(pixels[center + c] - colors[slot - 1][c]) < 0.001f,
                "Opaque preview material must sample its own cube slice and first populated mip without lighting");
    }
    for (UINT invalid : {33u, UINT_MAX})
    {
        const auto pixels = render(invalid, cubes.GetSRV()->GPUHandle, true);
        for (UINT c = 0; c < 3; ++c)
            Require(pixels[center + c] == 0, "Invalid cubemap slot must produce an opaque black placeholder");
    }
    const auto unpublished = render(1, cubes.GetSRV()->GPUHandle, false);
    for (UINT c = 0; c < 3; ++c)
        Require(unpublished[center + c] == 0, "Unpublished cubemap must produce an opaque black placeholder");
    checkGeometry(cubes.GetSRV()->GPUHandle);
    std::cout << "Opaque cubemap material: exact cube/mip, unlit radiance and unpublished material passed.\n";
}
