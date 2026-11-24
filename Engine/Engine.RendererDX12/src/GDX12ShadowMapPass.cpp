#include "Engine.RendererDX12/RenderPasses/GDX12ShadowMapPass.h"
#include <algorithm>

void GDX12ShadowMapPass::ValidateShadowMapResolutions(UINT directional, UINT pointSpot)
{
    if (directional < 3 || directional > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("ERROR: DirectionalLightShadowMapResolution must be between 3 and 16384.");
    if (!pointSpot || pointSpot > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
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

    auto& directional = description.AddInputDrawStream(InDirectionalDrawStream);
    directional.Stride = sizeof(GDX12ShadowDrawArgs);
    directional.Layout = ShadowDrawStreamLayout;
    directional.Usages = ResourceUsage::IndirectArguments;
    directional.AllowShared = true;

    auto& pointSpotStatic = description.AddInputDrawStream(InPointSpotStaticDrawStream);
    pointSpotStatic.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotStatic.Layout = ShadowDrawStreamLayout;
    pointSpotStatic.Usages = ResourceUsage::IndirectArguments;
    pointSpotStatic.AllowShared = true;

    auto& pointSpotDynamic = description.AddInputDrawStream(InPointSpotDynamicDrawStream);
    pointSpotDynamic.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotDynamic.Layout = ShadowDrawStreamLayout;
    pointSpotDynamic.Usages = ResourceUsage::IndirectArguments;
    pointSpotDynamic.AllowShared = true;

    auto& directionalMaps = description.AddOutputTexture(OutDirectionalMaps, ResourceType::Texture2DArray);
    directionalMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    directionalMaps.Usages = ResourceUsage::DepthWrite;
    directionalMaps.ClearColor = {1, 0, 0, 0};

    auto& pointSpotMaps = description.AddOutputTexture(OutPointSpotMaps, ResourceType::Texture2DArray);
    pointSpotMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    pointSpotMaps.Usages = ResourceUsage::DepthWrite | ResourceUsage::CopyDest;
    pointSpotMaps.ClearColor = {1, 0, 0, 0};
}

void GDX12ShadowMapPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
    const auto directional = context.GetParameter<UINT>("DirectionalLightShadowMapResolution");
    const auto pointSpot = context.GetParameter<UINT>("PointSpotLightShadowMapResolution");
    ValidateShadowMapResolutions(directional, pointSpot);
    context.RequireTextureSize(OutDirectionalMaps, {directional, directional});
    context.RequireTextureSize(OutPointSpotMaps, {pointSpot, pointSpot});
    const auto& shadows = context.GetFrameData()->ShadowSystem;
    context.SetOutputTextureArraySize(OutDirectionalMaps, (std::max)(1u, shadows.DirectionalArraySize));
    context.SetOutputTextureArraySize(OutPointSpotMaps, (std::max)(1u, shadows.PointSpotArraySize));
}

void GDX12ShadowMapPass::Initialize(PassInitContext& context)
{
    CreatePointSpotCache(context);
    _initializedSlots.assign(context.GetDeviceResources()->FrameConstants.size(), false);

    GDX12RootSignatureDesc root;
    root.Constants = {2};
    root.NumSingleSRVSlots = 4;
    root.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(context.GetDevice(), root);

    D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = 0;
    arguments[0].Constant.Num32BitValuesToSet = 2;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.ByteStride = sizeof(GDX12ShadowDrawArgs);
    signature.NumArgumentDescs = 2;
    signature.pArgumentDescs = arguments;
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateCommandSignature(&signature,
        _root->GetRootSignature().Get(), IID_PPV_ARGS(&_signature)));

    const bool layerFromVS = context.GetDevice()->GetDeviceFeatures().Features
        .VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation();
    const D3D_SHADER_MACRO defines[] = {{"SHADOW_LAYER_FROM_VS", layerFromVS ? "1" : "0"}, {nullptr, nullptr}};
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    auto vs = compiler.CompileShader(context.GetDevice(), SHADERS_FOLDER "ShadowMapPass.hlsl", defines, "VS", "vs");
    auto ps = compiler.CompileShader(context.GetDevice(), SHADERS_FOLDER "ShadowMapPass.hlsl", defines, "PS", "ps");
    ComPtr<ID3DBlob> gs;
    if (!layerFromVS)
        gs = compiler.CompileShader(context.GetDevice(), SHADERS_FOLDER "ShadowMapPass.hlsl", defines, "GS", "gs");

    const auto& layout = context.GetDeviceResources()->InputLayouts.at("Default");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    if (gs) pipeline.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pipeline.RasterizerState.FrontCounterClockwise = TRUE;
    pipeline.RasterizerState.DepthBias = 500;
    pipeline.RasterizerState.SlopeScaledDepthBias = 1.5f;
    pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.SampleDesc.Count = 1;
    pipeline.DSVFormat = context.GetOutputResourceDescription(OutDirectionalMaps).Format;
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_directionalPipeline)));
    pipeline.RasterizerState.DepthBias = 250;
    pipeline.DSVFormat = context.GetOutputResourceDescription(OutPointSpotMaps).Format;
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_pointSpotPipeline)));
}

void GDX12ShadowMapPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    const auto& frame = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto* directional = context.GetOutputTexture(OutDirectionalMaps).GetTexture();
    auto* pointSpot = context.GetOutputTexture(OutPointSpotMaps).GetTexture();
    const auto& directionalDraws = context.GetInputDrawStream(InDirectionalDrawStream);
    const auto& staticDraws = context.GetInputDrawStream(InPointSpotStaticDrawStream);
    const auto& dynamicDraws = context.GetInputDrawStream(InPointSpotDynamicDrawStream);

    commands->BeginPixEvent("Shadow Maps", Colors::DarkSlateBlue);
    commands->SetGraphicsRootSignature(_root.get());
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetGraphicsSRV(0, frame->MaterialCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(1, frame->TransformCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(2, frame->InstanceCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(3, frame->ShadowViews->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(4, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetGeometryBuffer(resources->GeometryBuffer.get());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    commands->SetPipelineState(_directionalPipeline);
    commands->SetViewport(directional->GetViewport());
    commands->SetScissorRect(directional->GetScissorRect());
    commands->SetRenderTargets({}, directional);
    commands->ClearDepthStencilView(directional);
    commands->ExecuteIndirect(_signature.Get(), directionalDraws.Capacity,
        directionalDraws.Arguments->D3DResource.Get(), 0, directionalDraws.Count->D3DResource.Get(), 0);

    commands->SetPipelineState(_pointSpotPipeline);
    commands->SetViewport(pointSpot->GetViewport());
    commands->SetScissorRect(pointSpot->GetScissorRect());
    auto* staticCache = _pointSpotStaticCache.get();
    const auto slot = context.GetFrameSlotResourcesIndex();
    const bool initializeOutput = !_initializedSlots[slot];
    const UINT forcedUpdates = _resetCache ? SHADOW_UPDATE_STATIC | SHADOW_UPDATE_FINAL :
        initializeOutput ? SHADOW_UPDATE_FINAL : 0;
    const auto& shadows = context.GetFrameData()->ShadowSystem;
    const auto& views = shadows.Views;
    if (_resetCache) commands->ClearDepthStencilView(staticCache);

    if (initializeOutput) commands->ClearDepthStencilView(pointSpot);
    bool updateStatic = _resetCache, updateFinal = initializeOutput || _resetCache;
    for (const auto& view : views)
    {
        if (view.Type == LightType::Directional) continue;
        const auto updates = shadows.LightUpdateFlags[view.LightIndex] | forcedUpdates;
        updateStatic |= (updates & SHADOW_UPDATE_STATIC) != 0;
        updateFinal |= (updates & SHADOW_UPDATE_FINAL) != 0;
        if (!_resetCache && (updates & SHADOW_UPDATE_STATIC))
            commands->GetCommandList()->ClearDepthStencilView(staticCache->GetDSV(view.Layer)->CPUHandle,
                D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    }
    if (updateStatic)
    {
        commands->BeginPixEvent("Point/Spot Static Cache", Colors::SlateBlue);
        commands->SetRenderTargets({}, staticCache);
        commands->ExecuteIndirect(_signature.Get(), staticDraws.Capacity,
            staticDraws.Arguments->D3DResource.Get(), 0, staticDraws.Count->D3DResource.Get(), 0);
        commands->EndPixEvent();
    }
    if (updateFinal)
    {
        commands->BeginPixEvent("Point/Spot Compose Static&Dynamic Shadows", Colors::MediumPurple);
        commands->ResourceBarrier({staticCache->GetResource()->GetCopySourceBarrier(), pointSpot->GetResource()->GetCopyDestBarrier()});
        for (const auto& view : views)
        {
            if (view.Type == LightType::Directional) continue;
            const auto updates = shadows.LightUpdateFlags[view.LightIndex] | forcedUpdates;
            if (!(updates & SHADOW_UPDATE_FINAL)) continue;
            const CD3DX12_TEXTURE_COPY_LOCATION source(staticCache->GetResource()->D3DResource.Get(), view.Layer);
            const CD3DX12_TEXTURE_COPY_LOCATION target(pointSpot->GetResource()->D3DResource.Get(), view.Layer);
            commands->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        commands->ResourceBarrier({pointSpot->GetResource()->GetDepthWriteBarrier()});
        commands->SetRenderTargets({}, pointSpot);
        commands->ExecuteIndirect(_signature.Get(), dynamicDraws.Capacity,
            dynamicDraws.Arguments->D3DResource.Get(), 0, dynamicDraws.Count->D3DResource.Get(), 0);
        commands->EndPixEvent();
    }
    _initializedSlots[slot] = true;
    _resetCache = false;
    commands->EndPixEvent();
}

void GDX12ShadowMapPass::CreatePointSpotCache(PassInitContext& context)
{
    const auto& output = context.GetOutputResourceDescription(OutPointSpotMaps);
    const auto size = output.GetSize();
    if (_pointSpotStaticCache)
    {
        const auto& cached = _pointSpotStaticCache->GetDescription();
        if (cached.Width == size.x && cached.Height == size.y && cached.ArraySize == output.ArraySize) return;
    }
    auto* resources = context.GetDeviceResources();
    GDX12TextureDesc description;
    description.Device = resources->Device;
    description.Width = size.x;
    description.Height = size.y;
    description.ArraySize = output.ArraySize;
    description.Format = output.Format;
    description.CreateSRV = false;
    description.CreateDSV = true;
    description.DSVHeap = resources->DSVHeap.get();
    description.ClearValue = {1, 0, 0, 0};
    _pointSpotStaticCache = std::make_unique<GDX12Texture>();
    description.DSVHeapIndex = resources->DSVHeap->GetAvailableIndex();
    _pointSpotStaticCache->Initialize(description);
}

void GDX12ShadowMapPass::OnResourcesRecreated(PassInitContext& context)
{
    CreatePointSpotCache(context);
    _initializedSlots.assign(context.GetDeviceResources()->FrameConstants.size(), false);
    _resetCache = true;
}

std::vector<ExternalResourceUse> GDX12ShadowMapPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_pointSpotStaticCache->GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, {}});
    return uses;
}
