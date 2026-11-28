#include "Engine.RendererDX12/RenderPasses/GDX12ShadowMapPass.h"
#include "Engine.RendererDX12/ShadowMapSystem.h"
#include <algorithm>
#include <cmath>

void GDX12ShadowMapPass::ValidateShadowMapResolutions(UINT directionalResolution, UINT pointSpotResolution)
{
    if (directionalResolution < 3 || directionalResolution > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("ERROR: DirectionalLightShadowMapResolution must be between 3 and 16384.");
    if (!pointSpotResolution || pointSpotResolution > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("ERROR: PointSpotLightShadowMapResolution must be between 1 and 16384.");
}

GDX12ShadowMapPass::GDX12ShadowMapPass()
{
    _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_INSTANCES |
        RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_LIGHTING;
}

void GDX12ShadowMapPass::Describe(PassDescription& description) const
{
    description.AddParameter("DirectionalLightShadowMapResolution", ShadowMapSystem::DefaultDirectionalLightShadowMapResolution);
    description.AddParameter("PointSpotLightShadowMapResolution", ShadowMapSystem::DefaultPointSpotLightShadowMapResolution);
    description.AddParameter("Cascade1MinOccluderSizeTexels", 256.0f);
    description.AddParameter("Cascade2MinOccluderSizeTexels", 256.0f);
    description.AddParameter("Cascade3MinOccluderSizeTexels", 256.0f);
    description.AddParameter("Cascade4MinOccluderSizeTexels", 256.0f);

    auto& directionalDrawStream = description.AddInputDrawStream(InDirectionalDrawStream);
    directionalDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    directionalDrawStream.Layout = ShadowDrawStreamLayout;
    directionalDrawStream.Usages = ResourceUsage::IndirectArguments | ResourceUsage::NonPixelShaderRead;
    directionalDrawStream.AllowShared = true;

    auto& pointSpotStaticDrawStream = description.AddInputDrawStream(InPointSpotStaticDrawStream);
    pointSpotStaticDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotStaticDrawStream.Layout = ShadowDrawStreamLayout;
    pointSpotStaticDrawStream.Usages = ResourceUsage::IndirectArguments;
    pointSpotStaticDrawStream.AllowShared = true;

    auto& pointSpotDynamicDrawStream = description.AddInputDrawStream(InPointSpotDynamicDrawStream);
    pointSpotDynamicDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotDynamicDrawStream.Layout = ShadowDrawStreamLayout;
    pointSpotDynamicDrawStream.Usages = ResourceUsage::IndirectArguments;
    pointSpotDynamicDrawStream.AllowShared = true;

    auto& directionalShadowMaps = description.AddOutputTexture(OutDirectionalMaps, ResourceType::Texture2DArray);
    directionalShadowMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    directionalShadowMaps.Usages = ResourceUsage::DepthWrite | ResourceUsage::NonPixelShaderRead;
    directionalShadowMaps.ClearColor = {1, 0, 0, 0};
    directionalShadowMaps.ReuseAcrossFrames = true;

    auto& pointSpotShadowMaps = description.AddOutputTexture(OutPointSpotMaps, ResourceType::Texture2DArray);
    pointSpotShadowMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    pointSpotShadowMaps.Usages = ResourceUsage::DepthWrite | ResourceUsage::CopyDest;
    pointSpotShadowMaps.ClearColor = {1, 0, 0, 0};
    pointSpotShadowMaps.ReuseAcrossFrames = true;
}

void GDX12ShadowMapPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
    const auto directionalResolution = context.GetParameter<UINT>("DirectionalLightShadowMapResolution");
    const auto pointSpotResolution = context.GetParameter<UINT>("PointSpotLightShadowMapResolution");
    ValidateShadowMapResolutions(directionalResolution, pointSpotResolution);
    context.RequireTextureSize(OutDirectionalMaps, {directionalResolution, directionalResolution});
    context.RequireTextureSize(OutPointSpotMaps, {pointSpotResolution, pointSpotResolution});
    const auto& shadowSystem = *context.GetFrameData()->ShadowSystem;
    context.SetOutputTextureArraySize(OutDirectionalMaps, (std::max)(1u, shadowSystem.DirectionalArraySize));
    context.SetOutputTextureArraySize(OutPointSpotMaps, (std::max)(1u, shadowSystem.PointSpotArraySize));
}

void GDX12ShadowMapPass::Initialize(PassInitContext& context)
{
    auto* deviceResources = context.GetDeviceResources();
    CreatePointSpotCache(context);
    _cascadeMinOccluderSizeTexels = {
        context.GetParameter<float>("Cascade1MinOccluderSizeTexels"),
        context.GetParameter<float>("Cascade2MinOccluderSizeTexels"),
        context.GetParameter<float>("Cascade3MinOccluderSizeTexels"),
        context.GetParameter<float>("Cascade4MinOccluderSizeTexels")};
    for (const float minimumOccluderSizeTexels : {_cascadeMinOccluderSizeTexels.x, _cascadeMinOccluderSizeTexels.y, _cascadeMinOccluderSizeTexels.z, _cascadeMinOccluderSizeTexels.w})
    {
        if (!std::isfinite(minimumOccluderSizeTexels) || minimumOccluderSizeTexels < 0)
            throw std::invalid_argument("ERROR: Minimum occluder sizes must be finite, non-negative shadow-map texel counts.");
    }

    GDX12RootSignatureDesc shadowRootDescription;
    shadowRootDescription.Constants = {2};
    shadowRootDescription.NumSingleSRVSlots = 4;
    shadowRootDescription.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    shadowRootDescription.StaticSamplers = GetStaticSamplers();
    _shadowRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, shadowRootDescription);

    D3D12_INDIRECT_ARGUMENT_DESC indirectArguments[2] = {};
    indirectArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    indirectArguments[0].Constant.RootParameterIndex = 0;
    indirectArguments[0].Constant.Num32BitValuesToSet = 2;
    indirectArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC drawCommandSignatureDescription = {};
    drawCommandSignatureDescription.ByteStride = sizeof(GDX12ShadowDrawArgs);
    drawCommandSignatureDescription.NumArgumentDescs = 2;
    drawCommandSignatureDescription.pArgumentDescs = indirectArguments;
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateCommandSignature(&drawCommandSignatureDescription,
        _shadowRootSignature->GetRootSignature().Get(), IID_PPV_ARGS(&_shadowDrawCommandSignature)));

    const bool supportsArrayIndexFromVertexShader = deviceResources->Device->GetDeviceFeatures().Features
        .VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation();
    const D3D_SHADER_MACRO shadowShaderDefines[] = {{"SHADOW_LAYER_FROM_VS", supportsArrayIndexFromVertexShader ? "1" : "0"}, {nullptr, nullptr}};
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    auto shadowVertexShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "ShadowMapPass.hlsl", shadowShaderDefines, "VS", "vs");
    auto shadowPixelShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "ShadowMapPass.hlsl", shadowShaderDefines, "PS", "ps");
    ComPtr<ID3DBlob> shadowGeometryShader;
    if (!supportsArrayIndexFromVertexShader)
        shadowGeometryShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "ShadowMapPass.hlsl", shadowShaderDefines, "GS", "gs");

    const auto& vertexInputLayout = deviceResources->InputLayouts.at("Default");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPipelineDescription = {};
    shadowPipelineDescription.pRootSignature = _shadowRootSignature->GetRootSignature().Get();
    shadowPipelineDescription.InputLayout = {vertexInputLayout.data(), static_cast<UINT>(vertexInputLayout.size())};
    shadowPipelineDescription.VS = {shadowVertexShader->GetBufferPointer(), shadowVertexShader->GetBufferSize()};
    shadowPipelineDescription.PS = {shadowPixelShader->GetBufferPointer(), shadowPixelShader->GetBufferSize()};
    if (shadowGeometryShader) shadowPipelineDescription.GS = {shadowGeometryShader->GetBufferPointer(), shadowGeometryShader->GetBufferSize()};
    shadowPipelineDescription.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    shadowPipelineDescription.RasterizerState.FrontCounterClockwise = TRUE;
    shadowPipelineDescription.RasterizerState.DepthBias = 500;
    shadowPipelineDescription.RasterizerState.SlopeScaledDepthBias = 1.5f;
    shadowPipelineDescription.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    shadowPipelineDescription.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    shadowPipelineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowPipelineDescription.SampleMask = UINT_MAX;
    shadowPipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowPipelineDescription.SampleDesc.Count = 1;
    shadowPipelineDescription.DSVFormat = context.GetOutputResourceDescription(OutDirectionalMaps).Format;
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateGraphicsPipelineState(&shadowPipelineDescription, IID_PPV_ARGS(&_directionalShadowPipeline)));
    shadowPipelineDescription.RasterizerState.DepthBias = 250;
    shadowPipelineDescription.DSVFormat = context.GetOutputResourceDescription(OutPointSpotMaps).Format;
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateGraphicsPipelineState(&shadowPipelineDescription, IID_PPV_ARGS(&_pointSpotShadowPipeline)));

    GDX12RootSignatureDesc directionalOcclusionRootDescription;
    directionalOcclusionRootDescription.Constants = {8};
    directionalOcclusionRootDescription.NumSingleSRVSlots = 6;
    directionalOcclusionRootDescription.NumSingleUAVSlots = 4;
    _directionalOcclusionRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, directionalOcclusionRootDescription);
    const auto directionalOcclusionShaderPath = SHADERS_FOLDER "ShadowOcclusionCullingPass.hlsl";
    auto clearDrawCountsShader = shaderCompiler.CompileShader(deviceResources->Device, directionalOcclusionShaderPath, nullptr, "ClearCountsCS", "cs");
    auto selectOccludersShader = shaderCompiler.CompileShader(deviceResources->Device, directionalOcclusionShaderPath, nullptr, "SelectOccludersCS", "cs");
    auto reduceHiZDepthShader = shaderCompiler.CompileShader(deviceResources->Device, directionalOcclusionShaderPath, nullptr, "ReduceDepthCS", "cs");
    auto occlusionCullingShader = shaderCompiler.CompileShader(deviceResources->Device, directionalOcclusionShaderPath, nullptr, "CullCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC occlusionPipelineDescription = {};
    occlusionPipelineDescription.pRootSignature = _directionalOcclusionRootSignature->GetRootSignature().Get();
    occlusionPipelineDescription.CS = {clearDrawCountsShader->GetBufferPointer(), clearDrawCountsShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_clearDirectionalDrawCountsPipeline)));
    occlusionPipelineDescription.CS = {selectOccludersShader->GetBufferPointer(), selectOccludersShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_selectDirectionalOccludersPipeline)));
    occlusionPipelineDescription.CS = {reduceHiZDepthShader->GetBufferPointer(), reduceHiZDepthShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_reduceDirectionalHiZDepthPipeline)));
    occlusionPipelineDescription.CS = {occlusionCullingShader->GetBufferPointer(), occlusionCullingShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_directionalOcclusionCullingPipeline)));
    CreateDirectionalOcclusionResources(context);
}

void GDX12ShadowMapPass::Execute(PassExecutionContext& context)
{
    auto* commandList = context.GetCommandList();
    auto* deviceResources = context.GetDeviceResources();
    const auto& frameConstants = deviceResources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto* directionalShadowMaps = context.GetOutputTexture(OutDirectionalMaps).GetTexture();
    auto* pointSpotShadowMaps = context.GetOutputTexture(OutPointSpotMaps).GetTexture();
    const auto* directionalDraws = context.GetInputDrawStream(InDirectionalDrawStream);
    const auto* pointSpotStaticDraws = context.GetInputDrawStream(InPointSpotStaticDrawStream);
    const auto* pointSpotDynamicDraws = context.GetInputDrawStream(InPointSpotDynamicDrawStream);

    commandList->BeginPixEvent("Shadow Maps", Colors::DarkSlateBlue);
    commandList->SetGraphicsRootSignature(_shadowRootSignature.get());
    commandList->SetDescriptorHeaps({deviceResources->SRV_UAV_Heap.get()});
    commandList->SetGraphicsSRV(0, frameConstants->MaterialCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(1, frameConstants->TransformCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(2, frameConstants->InstanceCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(3, frameConstants->ShadowViews->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(4, deviceResources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commandList->SetGeometryBuffer(deviceResources->GeometryBuffer.get());
    commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    commandList->SetPipelineState(_directionalShadowPipeline);
    commandList->SetViewport(directionalShadowMaps->GetViewport());
    commandList->SetScissorRect(directionalShadowMaps->GetScissorRect());
    commandList->SetRenderTargets({}, directionalShadowMaps);
    commandList->ClearDepthStencilView(directionalShadowMaps);
    RenderDirectionalShadows(context, directionalShadowMaps, *directionalDraws);

    commandList->SetPipelineState(_pointSpotShadowPipeline);
    commandList->SetViewport(pointSpotShadowMaps->GetViewport());
    commandList->SetScissorRect(pointSpotShadowMaps->GetScissorRect());
    auto* pointSpotStaticDepthCache = _pointSpotStaticDepthCache.get();
    const UINT forcedShadowUpdates = _resetShadowCache ? SHADOW_UPDATE_STATIC | SHADOW_UPDATE_FINAL : 0;
    const auto& shadowSystem = *context.GetFrameData()->ShadowSystem;
    const auto& shadowViews = shadowSystem.Views;
    if (_resetShadowCache)
    {
        commandList->ClearDepthStencilView(pointSpotStaticDepthCache);
        commandList->ClearDepthStencilView(pointSpotShadowMaps);
    }
    bool updateStaticShadows = _resetShadowCache, updateComposedShadows = _resetShadowCache;
    for (const auto& shadowView : shadowViews)
    {
        if (shadowView.Type == LightType::Directional) continue;
        const auto shadowUpdateFlags = shadowSystem.LightUpdateFlags[shadowView.LightIndex] | forcedShadowUpdates;
        updateStaticShadows |= (shadowUpdateFlags & SHADOW_UPDATE_STATIC) != 0;
        updateComposedShadows |= (shadowUpdateFlags & SHADOW_UPDATE_FINAL) != 0;
        if (!_resetShadowCache && (shadowUpdateFlags & SHADOW_UPDATE_STATIC))
            commandList->GetCommandList()->ClearDepthStencilView(pointSpotStaticDepthCache->GetDSV(shadowView.Layer)->CPUHandle,
                D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    }
    if (updateStaticShadows)
    {
        commandList->BeginPixEvent("Point/Spot Static Cache", Colors::SlateBlue);
        commandList->SetRenderTargets({}, pointSpotStaticDepthCache);
        commandList->ExecuteIndirect(_shadowDrawCommandSignature.Get(), pointSpotStaticDraws->Capacity,
            pointSpotStaticDraws->Arguments->D3DResource.Get(), 0, pointSpotStaticDraws->Count->D3DResource.Get(), 0);
        commandList->EndPixEvent();
    }
    if (updateComposedShadows)
    {
        commandList->BeginPixEvent("Point/Spot Compose Static&Dynamic Shadows", Colors::MediumPurple);
        commandList->ResourceBarrier({pointSpotStaticDepthCache->GetResource()->GetCopySourceBarrier(), pointSpotShadowMaps->GetResource()->GetCopyDestBarrier()});
        std::vector<UINT> updatedShadowArrayElements;
        for (const auto& shadowView : shadowViews)
        {
            if (shadowView.Type == LightType::Directional) continue;
            const auto shadowUpdateFlags = shadowSystem.LightUpdateFlags[shadowView.LightIndex] | forcedShadowUpdates;
            if (!(shadowUpdateFlags & SHADOW_UPDATE_FINAL)) continue;
            const CD3DX12_TEXTURE_COPY_LOCATION staticDepthSource(pointSpotStaticDepthCache->GetResource()->D3DResource.Get(), shadowView.Layer);
            const CD3DX12_TEXTURE_COPY_LOCATION composedDepthDestination(pointSpotShadowMaps->GetResource()->D3DResource.Get(), shadowView.Layer);
            commandList->GetCommandList()->CopyTextureRegion(&composedDepthDestination, 0, 0, 0, &staticDepthSource, nullptr);
            updatedShadowArrayElements.push_back(shadowView.Layer);
        }
        commandList->ResourceBarrier({pointSpotShadowMaps->GetResource()->GetDepthWriteBarrier()});
        commandList->SetRenderTargets({}, pointSpotShadowMaps);
        commandList->ExecuteIndirect(_shadowDrawCommandSignature.Get(), pointSpotDynamicDraws->Capacity,
            pointSpotDynamicDraws->Arguments->D3DResource.Get(), 0, pointSpotDynamicDraws->Count->D3DResource.Get(), 0);
        commandList->EndPixEvent();
        if (!_resetShadowCache) context.SetUpdatedTextureArrayElements(OutPointSpotMaps, updatedShadowArrayElements);
    }
    else { context.SetUpdatedTextureArrayElements(OutPointSpotMaps, {}); }
    _resetShadowCache = false;
    commandList->EndPixEvent();
}

void GDX12ShadowMapPass::CreatePointSpotCache(PassInitContext& context)
{
    const auto& pointSpotMapDescription = context.GetOutputResourceDescription(OutPointSpotMaps);
    const auto pointSpotMapSize = pointSpotMapDescription.GetSize();
    if (_pointSpotStaticDepthCache)
    {
        const auto& cachedDepthDescription = _pointSpotStaticDepthCache->GetDescription();
        if (cachedDepthDescription.Width == pointSpotMapSize.x && cachedDepthDescription.Height == pointSpotMapSize.y && cachedDepthDescription.ArraySize == pointSpotMapDescription.ArraySize) return;
    }
    auto* deviceResources = context.GetDeviceResources();
    GDX12TextureDesc staticDepthDescription;
    staticDepthDescription.Device = deviceResources->Device;
    staticDepthDescription.Width = pointSpotMapSize.x;
    staticDepthDescription.Height = pointSpotMapSize.y;
    staticDepthDescription.ArraySize = pointSpotMapDescription.ArraySize;
    staticDepthDescription.Format = pointSpotMapDescription.Format;
    staticDepthDescription.CreateSRV = false;
    staticDepthDescription.CreateDSV = true;
    staticDepthDescription.DSVHeap = deviceResources->DSVHeap.get();
    staticDepthDescription.ClearValue = {1, 0, 0, 0};
    _pointSpotStaticDepthCache = std::make_unique<GDX12Texture>();
    staticDepthDescription.DSVHeapIndex = deviceResources->DSVHeap->GetAvailableIndex();
    _pointSpotStaticDepthCache->Initialize(staticDepthDescription);
}

void GDX12ShadowMapPass::CreateDirectionalOcclusionResources(PassInitContext& context)
{
    auto* deviceResources = context.GetDeviceResources();
    auto* shaderResourceHeap = deviceResources->SRV_UAV_Heap.get();
    const auto directionalDrawCapacity = static_cast<UINT>(context.GetInputResourceDescription(InDirectionalDrawStream).Capacity);
    if (directionalDrawCapacity > UINT_MAX / sizeof(GDX12ShadowDrawArgs))
        throw std::length_error("ERROR: Directional occlusion command buffer exceeds the 32-bit upload-buffer allocation limit.");
    if (_directionalOccluderCommands && _directionalOccluderCommands->GetElementCount() != directionalDrawCapacity)
    {
        _directionalOccluderCommands.reset();
        _directionalVisibleCommands.reset();
        _directionalDrawCounts.reset();
    }
    if (!_directionalOccluderCommands)
    {
        _directionalOccluderCommands = std::make_unique<GDX12UploadBuffer<GDX12ShadowDrawArgs>>(deviceResources->Device, directionalDrawCapacity, EBufferType::Default);
        _directionalVisibleCommands = std::make_unique<GDX12UploadBuffer<GDX12ShadowDrawArgs>>(deviceResources->Device, directionalDrawCapacity, EBufferType::Default);
        _directionalDrawCounts = std::make_unique<GDX12UploadBuffer<UINT>>(deviceResources->Device, 2, EBufferType::Default);
        _directionalOccluderCommands->CreateUAV(shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _directionalVisibleCommands->CreateUAV(shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _directionalDrawCounts->CreateUAV(shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
    }

    const auto& directionalMapDescription = context.GetOutputResourceDescription(OutDirectionalMaps);
    UINT hiZSize = 1, mipCount = 1;
    while (hiZSize < (directionalMapDescription.Width + 1) / 2) { hiZSize *= 2; ++mipCount; }
    if (_directionalHiZDepthTexture && _directionalHiZDepthTexture->GetWidth() == hiZSize &&
        _directionalHiZDepthTexture->GetDescription().ArraySize == directionalMapDescription.ArraySize) return;
    GDX12TextureDesc hiZDescription;
    hiZDescription.Device = deviceResources->Device;
    hiZDescription.Width = hiZDescription.Height = hiZSize;
    hiZDescription.ArraySize = directionalMapDescription.ArraySize;
    hiZDescription.MipLevels = mipCount;
    hiZDescription.Format = DXGI_FORMAT_R32_FLOAT;
    hiZDescription.CreateUAV = true;
    hiZDescription.SRV_UAV_Heap = shaderResourceHeap;
    hiZDescription.SRVHeapIndex = shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex);
    hiZDescription.UAVHeapIndex = shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex);
    hiZDescription.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    hiZDescription.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hiZDescription.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    hiZDescription.SRVDesc.Texture2DArray.ArraySize = directionalMapDescription.ArraySize;
    hiZDescription.SRVDesc.Texture2DArray.MipLevels = mipCount;
    hiZDescription.UAVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    hiZDescription.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    hiZDescription.UAVDesc.Texture2DArray.ArraySize = directionalMapDescription.ArraySize;
    _directionalHiZDepthTexture = std::make_unique<GDX12Texture>();
    _directionalHiZDepthTexture->Initialize(hiZDescription);
    _directionalHiZMipSRVs.clear();
    _directionalHiZMipUAVs.clear();
    for (UINT mipIndex = 1; mipIndex < mipCount; ++mipIndex)
    {
        auto previousMipSRV = std::make_unique<GDX12Descriptor>();
        auto previousMipSRVDescription = hiZDescription.SRVDesc;
        previousMipSRVDescription.Texture2DArray.MostDetailedMip = mipIndex - 1;
        previousMipSRVDescription.Texture2DArray.MipLevels = 1;
        previousMipSRV->InitAsSRV(_directionalHiZDepthTexture->GetResource()->D3DResource.Get(), &previousMipSRVDescription, shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _directionalHiZMipSRVs.push_back(std::move(previousMipSRV));
        auto currentMipUAV = std::make_unique<GDX12Descriptor>();
        auto currentMipUAVDescription = hiZDescription.UAVDesc;
        currentMipUAVDescription.Texture2DArray.MipSlice = mipIndex;
        currentMipUAV->InitAsUAV(_directionalHiZDepthTexture->GetResource()->D3DResource.Get(), nullptr, &currentMipUAVDescription, shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _directionalHiZMipUAVs.push_back(std::move(currentMipUAV));
    }
}

void GDX12ShadowMapPass::RenderDirectionalShadows(PassExecutionContext& context, GDX12Texture* directionalShadowMaps, const DrawStreamBinding& directionalCandidateDraws)
{
    auto* commandList = context.GetCommandList();
    auto& frameConstants = *context.GetDeviceResources()->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto& drawCountBuffer = _directionalDrawCounts->GetResource();
    auto& occluderCommandBuffer = _directionalOccluderCommands->GetResource();
    auto& visibleCommandBuffer = _directionalVisibleCommands->GetResource();
    auto* hiZDepthResource = _directionalHiZDepthTexture->GetResource();
    struct { UINT DrawOffset, ShadowMapResolution, TargetSize, FirstReduction; XMFLOAT4 MinOccluderSizeTexels; }
        occlusionConstants = {0, directionalShadowMaps->GetWidth(), 0, 0, _cascadeMinOccluderSizeTexels};
    commandList->SetComputeRootSignature(_directionalOcclusionRootSignature.get());
    commandList->SetComputeSRV(0, directionalCandidateDraws.ArgumentSRV->GPUHandle);
    commandList->SetComputeSRV(1, directionalCandidateDraws.CountSRV->GPUHandle);
    commandList->SetComputeSRV(2, frameConstants.InstanceCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(3, frameConstants.MaterialCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(4, frameConstants.ShadowViews->GetSRV()->GPUHandle);
    commandList->SetComputeUAV(0, _directionalOccluderCommands->GetUAV()->GPUHandle);
    commandList->SetComputeUAV(1, _directionalDrawCounts->GetUAV()->GPUHandle);
    commandList->SetComputeUAV(2, _directionalVisibleCommands->GetUAV()->GPUHandle);

    commandList->BeginPixEvent("Directional Select Occluders", Colors::SlateBlue);
    commandList->SetPipelineState(_clearDirectionalDrawCountsPipeline);
    commandList->Dispatch(1, 1, 1);
    commandList->ResourceBarrier({drawCountBuffer.GetUAVBarrier()});
    commandList->SetPipelineState(_selectDirectionalOccludersPipeline);
    constexpr UINT maxDrawsPerDispatch = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * 64;
    for (uint64_t drawOffset = 0; drawOffset < directionalCandidateDraws.Capacity; drawOffset += maxDrawsPerDispatch)
    {
        occlusionConstants.DrawOffset = static_cast<UINT>(drawOffset);
        commandList->SetComputeRootConstants(0, occlusionConstants);
        commandList->Dispatch(static_cast<UINT>(((std::min)(uint64_t(directionalCandidateDraws.Capacity) - drawOffset, uint64_t(maxDrawsPerDispatch)) + 63) / 64), 1, 1);
        commandList->ResourceBarrier({drawCountBuffer.GetUAVBarrier(), occluderCommandBuffer.GetUAVBarrier()});
    }
    commandList->ResourceBarrier({occluderCommandBuffer.GetIndirectArgsBarrier(), drawCountBuffer.GetIndirectArgsBarrier()});
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Directional Shadow Occluders", Colors::DarkSlateBlue);
    commandList->SetPipelineState(_directionalShadowPipeline);
    commandList->ExecuteIndirect(_shadowDrawCommandSignature.Get(), directionalCandidateDraws.Capacity, occluderCommandBuffer.D3DResource.Get(), 0, drawCountBuffer.D3DResource.Get(), 0);
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Directional Shadow Hi-Z", Colors::DarkCyan);
    commandList->ResourceBarrier({directionalShadowMaps->GetResource()->GetNonPixelShaderResourceBarrier(), drawCountBuffer.GetUnorderedAccessBarrier()});
    commandList->SetPipelineState(_reduceDirectionalHiZDepthPipeline);
    UINT mipSize = _directionalHiZDepthTexture->GetWidth();
    const auto& hiZDescription = _directionalHiZDepthTexture->GetDescription();
    for (UINT mipIndex = 0; mipIndex < hiZDescription.MipLevels; ++mipIndex)
    {
        occlusionConstants.TargetSize = mipSize;
        occlusionConstants.FirstReduction = mipIndex == 0 ? 1u : 0u;
        commandList->SetComputeRootConstants(0, occlusionConstants);
        commandList->SetComputeSRV(5, mipIndex == 0 ? directionalShadowMaps->GetSRV()->GPUHandle : _directionalHiZMipSRVs[mipIndex - 1]->GPUHandle);
        commandList->SetComputeUAV(3, mipIndex == 0 ? _directionalHiZDepthTexture->GetUAV()->GPUHandle : _directionalHiZMipUAVs[mipIndex - 1]->GPUHandle);
        commandList->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, hiZDescription.ArraySize);
        for (UINT arrayElement = 0; arrayElement < hiZDescription.ArraySize; ++arrayElement)
        {
            commandList->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(hiZDepthResource->D3DResource.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12CalcSubresource(mipIndex, arrayElement, 0, hiZDescription.MipLevels, hiZDescription.ArraySize))});
        }
        mipSize = (std::max)(1u, mipSize / 2);
    }
    hiZDepthResource->SetCurrentState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Directional Shadow Occlusion Culling", Colors::SteelBlue);
    commandList->SetComputeSRV(5, _directionalHiZDepthTexture->GetSRV()->GPUHandle);
    commandList->SetPipelineState(_directionalOcclusionCullingPipeline);
    for (uint64_t drawOffset = 0; drawOffset < directionalCandidateDraws.Capacity; drawOffset += maxDrawsPerDispatch)
    {
        occlusionConstants.DrawOffset = static_cast<UINT>(drawOffset);
        commandList->SetComputeRootConstants(0, occlusionConstants);
        commandList->Dispatch(static_cast<UINT>(((std::min)(uint64_t(directionalCandidateDraws.Capacity) - drawOffset, uint64_t(maxDrawsPerDispatch)) + 63) / 64), 1, 1);
        commandList->ResourceBarrier({drawCountBuffer.GetUAVBarrier(), visibleCommandBuffer.GetUAVBarrier()});
    }
    commandList->ResourceBarrier({visibleCommandBuffer.GetIndirectArgsBarrier(), drawCountBuffer.GetIndirectArgsBarrier(), directionalShadowMaps->GetResource()->GetDepthWriteBarrier()});
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Directional Remaining Shadows", Colors::DarkSlateBlue);
    commandList->SetPipelineState(_directionalShadowPipeline);
    commandList->ExecuteIndirect(_shadowDrawCommandSignature.Get(), directionalCandidateDraws.Capacity, visibleCommandBuffer.D3DResource.Get(), 0, drawCountBuffer.D3DResource.Get(), sizeof(UINT));
    commandList->EndPixEvent();
}

void GDX12ShadowMapPass::OnResourcesRecreated(PassInitContext& context)
{
    CreatePointSpotCache(context);
    CreateDirectionalOcclusionResources(context);
    _resetShadowCache = true;
}

std::vector<ExternalResourceUse> GDX12ShadowMapPass::GetExternalResources(PassExecutionContext& context)
{
    auto externalResources = GDX12RenderPass::GetExternalResources(context);
    externalResources.push_back({_pointSpotStaticDepthCache->GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, {}});
    externalResources.push_back({_directionalHiZDepthTexture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    externalResources.push_back({&_directionalOccluderCommands->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    externalResources.push_back({&_directionalVisibleCommands->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    externalResources.push_back({&_directionalDrawCounts->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return externalResources;
}
