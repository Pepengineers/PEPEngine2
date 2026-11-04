#pragma once
#include "PlacementTiming.h"

static void BenchmarkGrassPlacement(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture* depth, const Matrix& vp, Vector3 eye)
{
    auto* queue = device.GetCommandQueue();
    const auto previousSlot = resources.CurrFrameConstantsIndex;
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        resources.FrameConstants[slot]->MaterialCache->CopyData(0, GDX12MaterialConstants());
        resources.FrameConstants[slot]->LocalLightCache->CopyData(0, GDX12LocalLightConstants());
        resources.PrepareLocalShadowData();
        auto* commands = queue->GetCommandList();
        resources.UploadLocalShadowData(commands);
        queue->ExecuteCommandList(commands);
    }
    queue->Flush();
    GDX12GrassRenderer renderer(&resources);
    auto controls = GetGrassWindControls();
    controls.GrassEnabled = true;
    controls.GrassQuality = 2;
    controls.WindEnabled = false;
    GDX12GrassInstance grass;
    grass.Id = 194;
    grass.MaxBlades = 8192;
    grass.Segments = 3;
    grass.WidthCm = 0.75f;
    grass.HeightCm = 20;
    grass.HeightVariation = 0;
    for (UINT y = 0; y < 128; ++y)
        for (UINT x = 0; x < 64; ++x)
            grass.RootsCm.emplace_back(-63.0f + x * 2, 0, -32.0f + y * 0.5f);
    for (const bool edited : {false, true})
        MeasurePlacement(device, edited ? "grass-edited-8192" : "grass-unchanged-8192",
            static_cast<UINT>(resources.FrameConstants.size()), [&](GDX12CommandList* commands, UINT slot, UINT iteration)
            {
                resources.CurrFrameConstantsIndex = slot;
                if (edited) grass.RootsCm[0].x += iteration % 2 ? 0.125f : -0.125f;
                renderer.BeginFrame(commands, common, {grass}, {}, controls);
                commands->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
                commands->ClearDepthStencilView(depth);
                commands->SetViewport(depth->GetViewport());
                commands->SetScissorRect(depth->GetScissorRect());
                commands->SetRenderTargets({}, depth);
                renderer.Draw(commands, GDX12GrassRenderer::Stage::Depth, vp, eye,
                    resources.FrameConstants[slot]->CameraCB->GetElementAddress(0));
            });
    resources.CurrFrameConstantsIndex = previousSlot;
}

static void TestGrassStages(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture* depth, const Matrix& vp, Vector3 eye,
    GDX12GrassRenderer& renderer, UINT diffuseIndex)
{
    auto* queue = device.GetCommandQueue();
    auto stageCommon = common;
    stageCommon.ResetHistory = false;
    std::array<GDX12Texture, 5> surfaces;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (UINT i = 0; i < surfaces.size(); ++i)
        GDX12RenderHelpers::InitializeRenderTarget(surfaces[i], resources,
            depth->GetWidth(), depth->GetHeight(), formats[i], {});
    GDX12Texture cube, neutral2D, neutralArray, normalMap;
    const auto initializeLighting = [&](GDX12Texture& texture, bool isCube, bool isArray)
    {
        GDX12TextureDesc desc;
        desc.Width = desc.Height = 1;
        desc.Format = desc.SRVDesc.Format = desc.RTVDesc.Format = isCube ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (isCube)
        {
            const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, 1, 1, 6, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = desc.Format;
            desc.ExternalResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource, D3D12_RESOURCE_STATE_COMMON, &clear);
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.SRVDesc.TextureCube.MipLevels = 1;
        }
        else if (isArray)
        {
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.SRVDesc.Texture2DArray.ArraySize = desc.SRVDesc.Texture2DArray.MipLevels = 1;
        }
        else
        {
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.SRVDesc.Texture2D.MipLevels = 1;
        }
        desc.CreateRTV = true;
        desc.ClearValue = {};
        desc.RTVHeap = resources.RTVHeap.get();
        desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
        desc.RTVDesc.ViewDimension = isCube || isArray ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (isCube || isArray) desc.RTVDesc.Texture2DArray.ArraySize = isCube ? 6 : 1;
        texture.Initialize(desc);
    };
    initializeLighting(cube, true, false);
    initializeLighting(neutral2D, false, false);
    initializeLighting(neutralArray, false, true);
    GDX12TextureDesc normalDesc;
    normalDesc.Width = normalDesc.Height = 1;
    normalDesc.Format = normalDesc.SRVDesc.Format = normalDesc.RTVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    normalDesc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    normalDesc.SRVHeapIndex = normalDesc.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
    normalDesc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    normalDesc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    normalDesc.SRVDesc.Texture2D.MipLevels = 1;
    normalDesc.CreateRTV = true;
    normalDesc.RTVHeap = resources.RTVHeap.get();
    normalDesc.RTVHeapIndex = normalDesc.RTVHeap->GetAvailableIndex();
    normalDesc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    // The existing ribbon winding opposes its vertex normal; -Z gives this material camera-facing shading.
    normalDesc.ClearValue = {0.5f, 0.5f, 0, 1};
    normalMap.Initialize(normalDesc);
    auto* commands = queue->GetCommandList();
    for (auto* texture : {&cube, &neutral2D, &neutralArray, &normalMap})
    {
        commands->ResourceBarrier({texture->GetResource()->GetRenderTargetBarrier()});
        commands->ClearRenderTargetView(texture);
        commands->ResourceBarrier({texture->GetResource()->GetSRVBarrier()});
    }
    queue->ExecuteCommandList(commands);
    queue->Flush();
    const auto read = [&](GDX12Texture& texture, UINT channel)
    {
        const auto desc = texture.GetResource()->D3DResource->GetDesc();
        const UINT channels = desc.Format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes = 0;
        device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto buffer = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({texture.GetResource()->GetCopySourceBarrier()});
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.GetResource()->D3DResource.Get(), 0), destination(buffer.Get(), footprint);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
        ThrowIfFailed(buffer->Map(0, &range, &mapped));
        double maximum = 0, total = 0;
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
            for (UINT x = 0; x < footprint.Footprint.Width; ++x)
            {
                const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const BYTE*>(mapped) +
                    footprint.Offset + y * footprint.Footprint.RowPitch) + x * channels;
                const float value = PackedVector::XMConvertHalfToFloat(pixel[channel]);
                Require(std::isfinite(value), "Grass stage produced nonfinite output");
                maximum = (std::max)(maximum, static_cast<double>(std::abs(value)));
                total += std::abs(value);
            }
        const D3D12_RANGE noWrite = {};
        buffer->Unmap(0, &noWrite);
        return std::make_pair(maximum, total);
    };
    GDX12GrassInstance grass;
    grass.Id = 127;
    grass.RootsCm = {{0, 0, 0}};
    grass.WidthCm = 15;
    grass.HeightVariation = 0;
    auto controls = GetGrassWindControls();
    controls.GrassEnabled = true;
    controls.GrassQuality = 1;
    controls.WindEnabled = false;
    GDX12MaterialConstants material;
    material.DiffuseIndex = diffuseIndex;
    material.NormalIndex = normalMap.GetSRV()->HeapIndex - Texture2D_StartIndex;
    material.Flags = 1;
    material.Metallic = 0;
    material.EmissiveColor = {0.25f, 0.125f, 0.0625f};
    GDX12LocalLightConstants light;
    light.Position = eye;
    light.Range = 1000;
    light.Radiance = {100, 100, 100};
    GDX12MainConstants main;
    main.LightRadiance = {};
    main.LocalLightCount = 1;
    main.SunShadowFlags = 0;
    std::vector<GDX12MainConstants> savedMain(resources.FrameConstants.size());
    std::vector<GDX12ShadowConstants> savedShadow(resources.FrameConstants.size());
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        auto& frame = *resources.FrameConstants[slot];
        const auto save = [](auto& buffer, auto& value)
        {
            void* mapped = nullptr;
            const D3D12_RANGE range = {0, sizeof(value)};
            ThrowIfFailed(buffer->GetResource().D3DResource->Map(0, &range, &mapped));
            std::memcpy(&value, mapped, sizeof(value));
            const D3D12_RANGE noWrite = {};
            buffer->GetResource().D3DResource->Unmap(0, &noWrite);
        };
        save(frame.MainCB, savedMain[slot]);
        save(frame.ShadowCB, savedShadow[slot]);
        frame.MainCB->CopyData(0, main);
        frame.ShadowCB->CopyData(0, GDX12ShadowConstants());
        const auto publish = [&]
        {
            frame.MaterialCache->CopyData(0, material);
            frame.LocalLightCache->CopyData(0, light);
            resources.PrepareLocalShadowData();
            auto* commands = queue->GetCommandList();
            resources.UploadLocalShadowData(commands);
            queue->ExecuteCommandList(commands);
            queue->Flush();
            auto poisoned = material;
            poisoned.RenderLayer = 1;
            frame.MaterialCache->CopyData(0, poisoned);
            auto poisonedLight = light;
            poisonedLight.Radiance = light.Radiance.x ? XMFLOAT3() : XMFLOAT3(100, 100, 100);
            frame.LocalLightCache->CopyData(0, poisonedLight);
        };
        publish();
        commands = queue->GetCommandList();
        renderer.BeginFrame(commands, stageCommon, {grass}, {}, controls);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        GDX12GrassRenderer::CaptureLighting lighting;
        lighting.SunShadow = lighting.BRDF = &neutral2D;
        lighting.Irradiance = lighting.Prefiltered = &cube;
        lighting.LocalShadows = &neutralArray;
        lighting.ShadowConstants = frame.ShadowCB->GetElementAddress(0);
        const auto draw = [&](GDX12GrassRenderer::Stage stage)
        {
            auto* commands = queue->GetCommandList();
            commands->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
            if (stage != GDX12GrassRenderer::Stage::GBuffer) commands->ClearDepthStencilView(depth);
            for (auto& surface : surfaces)
            {
                commands->ResourceBarrier({surface.GetResource()->GetRenderTargetBarrier()});
                commands->ClearRenderTargetView(&surface);
            }
            commands->SetViewport(depth->GetViewport());
            commands->SetScissorRect(depth->GetScissorRect());
            if (stage == GDX12GrassRenderer::Stage::Capture) commands->SetRenderTargets({&surfaces[4]}, depth);
            else if (stage == GDX12GrassRenderer::Stage::PrepassNormals) commands->SetRenderTargets({&surfaces[2]}, depth);
            else commands->SetRenderTargets({&surfaces[0], &surfaces[1], &surfaces[2], &surfaces[3], &surfaces[4]}, depth);
            renderer.Draw(commands, stage, vp, eye, frame.CameraCB->GetElementAddress(0), &lighting);
            queue->ExecuteCommandList(commands);
            queue->Flush();
        };
        draw(GDX12GrassRenderer::Stage::Full);
        Require(read(surfaces[4], 0).first > 0.24, "Grass Full lost the DEFAULT material snapshot");
        draw(GDX12GrassRenderer::Stage::GBuffer);
        Require(read(surfaces[4], 0).first > 0.24, "Grass Equal GBuffer lost its depth/material snapshot");
        draw(GDX12GrassRenderer::Stage::PrepassNormals);
        Require(read(surfaces[2], 3).first > 0.49, "Grass prepass lost normal/roughness coverage");
        grass.World = Matrix::CreateTranslation(2, 0, 0);
        commands = queue->GetCommandList();
        renderer.BeginFrame(commands, stageCommon, {grass}, {}, controls);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        draw(GDX12GrassRenderer::Stage::Full);
        Require(read(surfaces[1], 0).second > 0.0001, "Grass motion lost the previous world transform");
        material.EmissiveColor.x = 0.75f;
        ++grass.MaterialRevision;
        commands = queue->GetCommandList();
        renderer.BeginFrame(commands, stageCommon, {grass}, {}, controls);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        publish();
        draw(GDX12GrassRenderer::Stage::Full);
        Require(read(surfaces[4], 0).first > 0.74, "Grass material edit did not reach DEFAULT");
        draw(GDX12GrassRenderer::Stage::Capture);
        const auto lit = read(surfaces[4], 0).second;
        light.Radiance = {};
        publish();
        draw(GDX12GrassRenderer::Stage::Capture);
        const auto dark = read(surfaces[4], 0).second;
        std::cout << "Grass capture slot=" << slot << " lit=" << lit << " dark=" << dark << '\n';
        Require(lit > dark + 0.0001, "Grass capture light edit did not reach DEFAULT");
        grass.World = Matrix::Identity;
        material.EmissiveColor.x = 0.25f;
        light.Radiance = {100, 100, 100};
    }
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.FrameConstants[slot]->MainCB->CopyData(0, savedMain[slot]);
        resources.FrameConstants[slot]->ShadowCB->CopyData(0, savedShadow[slot]);
        resources.FrameConstants[slot]->LocalLightCache->CopyData(0, GDX12LocalLightConstants());
    }
}

static void TestGrassPlacement(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture* depth, const Matrix& vp, Vector3 eye, GDX12GrassRenderer& originalRenderer)
{
    auto* queue = device.GetCommandQueue();
    const UINT previousSlot = resources.CurrFrameConstantsIndex;
    GDX12GrassRenderer renderer(&resources);
    auto controls = GetGrassWindControls();
    controls.GrassEnabled = true;
    controls.GrassQuality = 1;
    controls.WindEnabled = false;
    GDX12GrassInstance grass;
    grass.Id = 97;
    grass.WidthCm = 15;
    grass.HeightVariation = 0;
    grass.RootsCm = {{0, 0, 0}};

    GDX12Descriptor blackTexture;
    D3D12_SHADER_RESOURCE_VIEW_DESC blackView = {};
    blackView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    blackView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    blackView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    blackView.Texture2D.MipLevels = 1;
    blackTexture.InitAsSRV(nullptr, &blackView, resources.SRV_UAV_Heap.get(),
        resources.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength));
    GDX12MaterialConstants opaque;
    opaque.DiffuseIndex = blackTexture.HeapIndex - Texture2D_StartIndex;
    auto masked = opaque;
    masked.RenderLayer = 1;
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        auto& frame = *resources.FrameConstants[slot];
        frame.MaterialCache->CopyData(0, opaque);
        resources.PrepareLocalShadowData();
        auto* commands = queue->GetCommandList();
        resources.UploadLocalShadowData(commands);
        queue->ExecuteCommandList(commands);
        queue->Flush();
        // The queued copy has completed; only an incorrect UPLOAD binding observes this poison.
        frame.MaterialCache->CopyData(0, masked);
    }

    const auto desc = depth->GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    std::vector<GDX12ResourceHandle> readbacks;
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
        readbacks.push_back(device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST));
    const auto batch = [&](const std::vector<GDX12GrassInstance>& owners, bool expected,
        GDX12GrassRenderer::Stage stage = GDX12GrassRenderer::Stage::Depth)
    {
        const bool shadow = stage == GDX12GrassRenderer::Stage::Shadow;
        for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
        {
            resources.CurrFrameConstantsIndex = slot;
            auto* commands = queue->GetCommandList();
            renderer.BeginFrame(commands, common, owners, {}, controls);
            commands->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
            commands->GetCommandList()->ClearDepthStencilView(depth->GetDSV()->CPUHandle,
                D3D12_CLEAR_FLAG_DEPTH, shadow ? 1.0f : 0.0f, 0, 0, nullptr);
            commands->SetViewport(depth->GetViewport());
            commands->SetScissorRect(depth->GetScissorRect());
            commands->SetRenderTargets({}, depth);
            renderer.Draw(commands, stage, vp, eye,
                resources.FrameConstants[slot]->CameraCB->GetElementAddress(0));
            commands->ResourceBarrier({depth->GetResource()->GetCopySourceBarrier()});
            const CD3DX12_TEXTURE_COPY_LOCATION source(depth->GetResource()->D3DResource.Get(), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(readbacks[slot].Get(), footprint);
            commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            queue->ExecuteCommandList(commands);
        }
        queue->Flush();
        for (auto& readback : readbacks)
        {
            void* mapped = nullptr;
            const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
            ThrowIfFailed(readback->Map(0, &range, &mapped));
            bool coverage = false;
            for (UINT y = 0; y < footprint.Footprint.Height; ++y)
                for (UINT x = 0; x < footprint.Footprint.Width; ++x)
                {
                    const auto* pixel = reinterpret_cast<const float*>(static_cast<const BYTE*>(mapped) +
                        footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(float));
                    Require(std::isfinite(*pixel), "Grass readback contains invalid depth");
                    coverage |= shadow ? *pixel < 0.999f : *pixel > 0;
                }
            const D3D12_RANGE noWrite = {};
            readback->Unmap(0, &noWrite);
            Require(coverage == expected, "Grass roots/material snapshot lost an edit, growth, or owner lifetime");
        }
    };
    batch({grass}, true);
    for (auto& root : grass.RootsCm) root.x = 100000;
    batch({grass}, false);
    grass.RootsCm.resize(9, {100000, 0, 0});
    grass.RootsCm.back() = {0, 0, 0};
    batch({grass}, true);
    batch({grass}, true, GDX12GrassRenderer::Stage::Shadow);
    batch({}, false);
    Require(!renderer.HasCasters(), "Deleted grass owner retained casters");
    grass.RootsCm = {{0, 0, 0}};
    batch({grass}, true);
    controls.GrassEnabled = false;
    batch({grass}, false);
    controls.GrassEnabled = true;
    batch({grass}, true);
    grass.RootsCm.clear();
    grass.ExtentsCm = Vector2(1, 1);
    grass.DensityPerSquareMetre = 1;
    batch({grass}, true);
    grass.DensityPerSquareMetre = 0;
    batch({grass}, false);
    TestGrassStages(device, resources, common, depth, vp, eye, renderer, opaque.DiffuseIndex);
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        resources.FrameConstants[slot]->MaterialCache->CopyData(0, GDX12MaterialConstants());
        resources.PrepareLocalShadowData();
        auto* commands = queue->GetCommandList();
        resources.UploadLocalShadowData(commands);
        queue->ExecuteCommandList(commands);
    }
    queue->Flush();
    resources.CurrFrameConstantsIndex = previousSlot;
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier()});
    commands->ClearDepthStencilView(depth);
    commands->SetViewport(depth->GetViewport());
    commands->SetScissorRect(depth->GetScissorRect());
    commands->SetRenderTargets({}, depth);
    originalRenderer.Draw(commands, GDX12GrassRenderer::Stage::Depth, vp, eye,
        resources.FrameConstants[previousSlot]->CameraCB->GetElementAddress(0));
    queue->ExecuteCommandList(commands);
    queue->Flush();
    std::cout << "Grass queued frame slots: roots edits/growth, DEFAULT material/light edits, all draw stages, motion, deletion/reuse and disable passed.\n";
}
