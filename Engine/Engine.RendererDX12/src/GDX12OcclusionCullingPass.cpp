#include "Engine.RendererDX12/RenderPasses/GDX12OcclusionCullingPass.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include <algorithm>
#include <cmath>

GDX12OcclusionCullingPass::GDX12OcclusionCullingPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_INSTANCES |
        RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_GEOMETRY;
}
GDX12OcclusionCullingPass::~GDX12OcclusionCullingPass() = default;

void GDX12OcclusionCullingPass::Describe(PassDescription& description) const
{
    auto& opaqueDrawStream = description.AddOutputDrawStream(OutOpaqueDrawStream);
    opaqueDrawStream.Usages = ResourceUsage::UnorderedAccess;
    opaqueDrawStream.AllowShared = true;
    auto& transparentDrawStream = description.AddOutputDrawStream(OutTransparentDrawStream);
    transparentDrawStream.Usages = ResourceUsage::UnorderedAccess;
    transparentDrawStream.AllowShared = true;
    description.AddParameter("MinOccluderSizePixels", 128.0f);
}

void GDX12OcclusionCullingPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
    const auto& sceneDrawCommands = context.GetDeviceResources()->IndirectCommandsCache;
    if (!sceneDrawCommands) { throw std::invalid_argument("ERROR: GPU culling has no scene indirect-command buffer. Initialize IndirectCommandsCache on the pass device "
        "before compiling the graph."); }
    const auto drawStreamCapacity = (std::max)(1u, sceneDrawCommands->GetElementCount());
    context.SetOutputBufferCapacity(OutOpaqueDrawStream, drawStreamCapacity);
    context.SetOutputBufferCapacity(OutTransparentDrawStream, drawStreamCapacity);
}

void GDX12OcclusionCullingPass::Initialize(PassInitContext& context)
{
    _minOccluderSizePixels = context.GetParameter<float>("MinOccluderSizePixels");
    if (!std::isfinite(_minOccluderSizePixels) || _minOccluderSizePixels < 0)
        throw std::invalid_argument("ERROR: Minimum camera occluder size must be finite and non-negative.");
    auto* deviceResources = context.GetDeviceResources();
    // Shaders
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    auto frustumCullingShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "FrustumCullingPass.hlsl", nullptr, "CS", "cs");
    auto clearCandidateCountsShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "BufferClear.hlsl", nullptr, "CS", "cs");

    // Root Signatures
    GDX12RootSignatureDesc frustumCullingRootDescription;
    frustumCullingRootDescription.NumSingleCBVSlots = 1;
    frustumCullingRootDescription.NumSingleSRVSlots = 3;
    frustumCullingRootDescription.NumSingleUAVSlots = 4;
    _frustumCullingRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, frustumCullingRootDescription);

    GDX12RootSignatureDesc clearCandidateCountsRootDescription;
    clearCandidateCountsRootDescription.NumSingleUAVSlots = 2;
    _clearCandidateCountsRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, clearCandidateCountsRootDescription);

    // Pipeline State Objects
    D3D12_COMPUTE_PIPELINE_STATE_DESC frustumCullingPipelineDescription = {};
    frustumCullingPipelineDescription.pRootSignature = _frustumCullingRootSignature->GetRootSignature().Get();
    frustumCullingPipelineDescription.CS = { reinterpret_cast<BYTE*>(frustumCullingShader->GetBufferPointer()), frustumCullingShader->GetBufferSize() };

    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(
        &frustumCullingPipelineDescription, IID_PPV_ARGS(&_frustumCullingPipeline)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC clearCandidateCountsPipelineDescription = {};
    clearCandidateCountsPipelineDescription.pRootSignature = _clearCandidateCountsRootSignature->GetRootSignature().Get();
    clearCandidateCountsPipelineDescription.CS = { reinterpret_cast<BYTE*>(clearCandidateCountsShader->GetBufferPointer()), clearCandidateCountsShader->GetBufferSize() };

    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(
        &clearCandidateCountsPipelineDescription, IID_PPV_ARGS(&_clearCandidateCountsPipeline)));
    GDX12RootSignatureDesc occlusionCullingRootDescription;
    occlusionCullingRootDescription.Constants = {8};
    occlusionCullingRootDescription.NumSingleCBVSlots = 1;
    occlusionCullingRootDescription.NumSingleSRVSlots = 5;
    occlusionCullingRootDescription.NumSingleUAVSlots = 5;
    _occlusionCullingRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, occlusionCullingRootDescription);
    const auto occlusionShaderPath = SHADERS_FOLDER "OcclusionCullingPass.hlsl";
    auto clearDrawCountsShader = shaderCompiler.CompileShader(deviceResources->Device, occlusionShaderPath, nullptr, "ClearCS", "cs");
    auto selectOccludersShader = shaderCompiler.CompileShader(deviceResources->Device, occlusionShaderPath, nullptr, "SelectOccludersCS", "cs");
    auto reduceHiZDepthShader = shaderCompiler.CompileShader(deviceResources->Device, occlusionShaderPath, nullptr, "ReduceCS", "cs");
    auto occlusionCullingShader = shaderCompiler.CompileShader(deviceResources->Device, occlusionShaderPath, nullptr, "CullCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC occlusionPipelineDescription = {};
    occlusionPipelineDescription.pRootSignature = _occlusionCullingRootSignature->GetRootSignature().Get();
    occlusionPipelineDescription.CS = {clearDrawCountsShader->GetBufferPointer(), clearDrawCountsShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_clearDrawCountsPipeline)));
    occlusionPipelineDescription.CS = {selectOccludersShader->GetBufferPointer(), selectOccludersShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_selectOccludersPipeline)));
    occlusionPipelineDescription.CS = {reduceHiZDepthShader->GetBufferPointer(), reduceHiZDepthShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_reduceHiZDepthPipeline)));
    occlusionPipelineDescription.CS = {occlusionCullingShader->GetBufferPointer(), occlusionCullingShader->GetBufferSize()};
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(&occlusionPipelineDescription, IID_PPV_ARGS(&_occlusionCullingPipeline)));

    // Use the exact geometry VS and depth PS, including precise jittered SV_POSITION.
    auto occluderVertexShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "DeferredGeometryPass.hlsl", nullptr, "VS", "vs");
    auto occluderDepthPixelShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "DeferredGeometryPass.hlsl", nullptr, "DepthPS", "ps");
    GDX12RootSignatureDesc occluderDepthRootDescription;
    occluderDepthRootDescription.Constants = {1};
    occluderDepthRootDescription.NumSingleCBVSlots = 2;
    occluderDepthRootDescription.NumSingleSRVSlots = 3;
    occluderDepthRootDescription.StaticSamplers = GetStaticSamplers();
    occluderDepthRootDescription.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    _occluderDepthRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, occluderDepthRootDescription);
    D3D12_INDIRECT_ARGUMENT_DESC indirectArguments[2] = {};
    indirectArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    indirectArguments[0].Constant.RootParameterIndex = 0;
    indirectArguments[0].Constant.Num32BitValuesToSet = 1;
    indirectArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC drawCommandSignatureDescription = {};
    drawCommandSignatureDescription.ByteStride = sizeof(GDX12IndirectDrawArgs);
    drawCommandSignatureDescription.NumArgumentDescs = 2;
    drawCommandSignatureDescription.pArgumentDescs = indirectArguments;
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateCommandSignature(&drawCommandSignatureDescription, _occluderDepthRootSignature->GetRootSignature().Get(), IID_PPV_ARGS(&_occluderDrawCommandSignature)));
    const auto& vertexInputLayout = deviceResources->InputLayouts.at("Default");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC occluderDepthPipelineDescription = {};
    occluderDepthPipelineDescription.pRootSignature = _occluderDepthRootSignature->GetRootSignature().Get();
    occluderDepthPipelineDescription.InputLayout = {vertexInputLayout.data(), static_cast<UINT>(vertexInputLayout.size())};
    occluderDepthPipelineDescription.VS = {occluderVertexShader->GetBufferPointer(), occluderVertexShader->GetBufferSize()};
    occluderDepthPipelineDescription.PS = {occluderDepthPixelShader->GetBufferPointer(), occluderDepthPixelShader->GetBufferSize()};
    occluderDepthPipelineDescription.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    occluderDepthPipelineDescription.RasterizerState.FrontCounterClockwise = TRUE;
    occluderDepthPipelineDescription.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    occluderDepthPipelineDescription.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    occluderDepthPipelineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    occluderDepthPipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    occluderDepthPipelineDescription.SampleMask = UINT_MAX;
    occluderDepthPipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    occluderDepthPipelineDescription.SampleDesc.Count = 1;
    ThrowIfFailed(deviceResources->Device->GetDevice()->CreateGraphicsPipelineState(&occluderDepthPipelineDescription, IID_PPV_ARGS(&_occluderDepthPipeline)));
    CreateWorkingResources(context);
}

void GDX12OcclusionCullingPass::CreateWorkingResources(PassInitContext& context)
{
    auto* deviceResources = context.GetDeviceResources();
    auto candidateStreamDescription = context.GetOutputResourceDescription(OutOpaqueDrawStream);
    candidateStreamDescription.Usages = ResourceUsage::UnorderedAccess | ResourceUsage::NonPixelShaderRead | ResourceUsage::IndirectArguments;
    if (!_opaqueCandidateDrawStream || _opaqueCandidateDrawStream->Description.Capacity != candidateStreamDescription.Capacity)
    {
        _opaqueCandidateDrawStream = CreateFrameGraphNative(deviceResources, candidateStreamDescription);
        _transparentCandidateDrawStream = CreateFrameGraphNative(deviceResources, candidateStreamDescription);
        _occluderDrawStream = CreateFrameGraphNative(deviceResources, candidateStreamDescription);
    }
    const auto* frameData = context.GetFrameData();
    const UINT renderWidth = frameData->DownscaledWidth, renderHeight = frameData->DownscaledHeight;
    if (_occluderDepthTexture && _occluderDepthTexture->GetWidth() == renderWidth && _occluderDepthTexture->GetHeight() == renderHeight) return;
    auto* shaderResourceHeap = deviceResources->SRV_UAV_Heap.get();
    GDX12TextureDesc occluderDepthDescription;
    occluderDepthDescription.Device = deviceResources->Device;
    occluderDepthDescription.Width = renderWidth;
    occluderDepthDescription.Height = renderHeight;
    occluderDepthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    occluderDepthDescription.ResourceFormat = DXGI_FORMAT_R32_TYPELESS;
    occluderDepthDescription.CreateDSV = true;
    occluderDepthDescription.ClearValue = {0, 0, 0, 0};
    occluderDepthDescription.DSVHeap = deviceResources->DSVHeap.get();
    occluderDepthDescription.DSVHeapIndex = occluderDepthDescription.DSVHeap->GetAvailableIndex();
    occluderDepthDescription.SRV_UAV_Heap = shaderResourceHeap;
    occluderDepthDescription.SRVHeapIndex = shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex);
    occluderDepthDescription.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    occluderDepthDescription.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    occluderDepthDescription.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    occluderDepthDescription.SRVDesc.Texture2D.MipLevels = 1;
    _occluderDepthTexture = std::make_unique<GDX12Texture>();
    _occluderDepthTexture->Initialize(occluderDepthDescription);

    UINT hiZWidth = 1, hiZHeight = 1, mipCount = 1;
    while (hiZWidth < (renderWidth + 1) / 2) hiZWidth *= 2;
    while (hiZHeight < (renderHeight + 1) / 2) hiZHeight *= 2;
    for (UINT largestMipDimension = (std::max)(hiZWidth, hiZHeight); largestMipDimension > 1; largestMipDimension /= 2) ++mipCount;
    GDX12TextureDesc hiZDescription;
    hiZDescription.Device = deviceResources->Device;
    hiZDescription.Width = hiZWidth;
    hiZDescription.Height = hiZHeight;
    hiZDescription.MipLevels = mipCount;
    hiZDescription.Format = DXGI_FORMAT_R32_FLOAT;
    hiZDescription.CreateUAV = true;
    hiZDescription.SRV_UAV_Heap = shaderResourceHeap;
    hiZDescription.SRVHeapIndex = shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex);
    hiZDescription.UAVHeapIndex = shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex);
    _hiZDepthTexture = std::make_unique<GDX12Texture>();
    _hiZDepthTexture->Initialize(hiZDescription);
    _hiZMipSRVs.clear();
    _hiZMipUAVs.clear();
    for (UINT mipIndex = 1; mipIndex < mipCount; ++mipIndex)
    {
        auto previousMipSRV = std::make_unique<GDX12Descriptor>();
        D3D12_SHADER_RESOURCE_VIEW_DESC previousMipSRVDescription = {};
        previousMipSRVDescription.Format = DXGI_FORMAT_R32_FLOAT;
        previousMipSRVDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        previousMipSRVDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        previousMipSRVDescription.Texture2D.MostDetailedMip = mipIndex - 1;
        previousMipSRVDescription.Texture2D.MipLevels = 1;
        previousMipSRV->InitAsSRV(_hiZDepthTexture->GetResource()->D3DResource.Get(), &previousMipSRVDescription, shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _hiZMipSRVs.push_back(std::move(previousMipSRV));
        auto currentMipUAV = std::make_unique<GDX12Descriptor>();
        D3D12_UNORDERED_ACCESS_VIEW_DESC currentMipUAVDescription = {};
        currentMipUAVDescription.Format = DXGI_FORMAT_R32_FLOAT;
        currentMipUAVDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        currentMipUAVDescription.Texture2D.MipSlice = mipIndex;
        currentMipUAV->InitAsUAV(_hiZDepthTexture->GetResource()->D3DResource.Get(), nullptr, &currentMipUAVDescription, shaderResourceHeap, shaderResourceHeap->GetAvailableIndex(RendererResources_StartIndex));
        _hiZMipUAVs.push_back(std::move(currentMipUAV));
    }
}

void GDX12OcclusionCullingPass::CullFrustum(PassExecutionContext& context, const DrawStreamBinding& opaqueCandidateDraws, const DrawStreamBinding& transparentCandidateDraws)
{
    auto* deviceResources = context.GetDeviceResources();
    auto* commandList = context.GetCommandList();
    const auto* frameData = context.GetFrameData();
    auto& frameConstants = deviceResources->FrameConstants[context.GetFrameSlotResourcesIndex()];

    commandList->BeginPixEvent("Frustum Culling", Colors::CornflowerBlue);

    commandList->SetComputeRootSignature(_clearCandidateCountsRootSignature.get());
    commandList->SetPipelineState(_clearCandidateCountsPipeline);
    commandList->SetDescriptorHeaps({ deviceResources->SRV_UAV_Heap.get() });
    commandList->SetComputeUAV(0, opaqueCandidateDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(1, transparentCandidateDraws.CountUAV->GPUHandle);
    commandList->Dispatch(1, 1, 1);
    commandList->ResourceBarrier({ opaqueCandidateDraws.Count->GetUAVBarrier(), transparentCandidateDraws.Count->GetUAVBarrier() });

    commandList->SetComputeRootSignature(_frustumCullingRootSignature.get());
    commandList->SetPipelineState(_frustumCullingPipeline);
    commandList->SetDescriptorHeaps({ deviceResources->SRV_UAV_Heap.get() });
    commandList->SetComputeRootConstantBufferView(0, frameConstants->CameraCB->GetElementAddress(frameData->ActiveCameraCBufferIndex));
    commandList->SetComputeSRV(0, frameConstants->InstanceCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(1, deviceResources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(2, frameConstants->MaterialCache->GetSRV()->GPUHandle);
    commandList->SetComputeUAV(0, opaqueCandidateDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(1, opaqueCandidateDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(2, transparentCandidateDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(3, transparentCandidateDraws.CountUAV->GPUHandle);
    const UINT sceneDrawCount = deviceResources->IndirectCommandsCache->GetElementCount();
    if (sceneDrawCount) { commandList->Dispatch((sceneDrawCount + 63) / 64, 1, 1); }

    commandList->ResourceBarrier({ opaqueCandidateDraws.Arguments->GetUAVBarrier(), opaqueCandidateDraws.Count->GetUAVBarrier(),
        transparentCandidateDraws.Arguments->GetUAVBarrier(), transparentCandidateDraws.Count->GetUAVBarrier() });
    commandList->EndPixEvent();
}

void GDX12OcclusionCullingPass::Execute(PassExecutionContext& context)
{
    auto* commandList = context.GetCommandList();
    auto* deviceResources = context.GetDeviceResources();
    auto& frameConstants = *deviceResources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto& visibleOpaqueDraws = context.GetOutputDrawStream(OutOpaqueDrawStream);
    const auto& visibleTransparentDraws = context.GetOutputDrawStream(OutTransparentDrawStream);
    auto opaqueCandidateDraws = _opaqueCandidateDrawStream->CreatePassBinding(ResourceUsage::UnorderedAccess | ResourceUsage::NonPixelShaderRead).Draws;
    auto transparentCandidateDraws = _transparentCandidateDrawStream->CreatePassBinding(ResourceUsage::UnorderedAccess | ResourceUsage::NonPixelShaderRead).Draws;
    auto occluderDraws = _occluderDrawStream->CreatePassBinding(ResourceUsage::UnorderedAccess).Draws;
    commandList->BeginPixEvent("Occlusion Culling", Colors::CornflowerBlue);
    CullFrustum(context, opaqueCandidateDraws, transparentCandidateDraws);
    commandList->ResourceBarrier({opaqueCandidateDraws.Arguments->GetNonPixelShaderResourceBarrier(), opaqueCandidateDraws.Count->GetNonPixelShaderResourceBarrier(),
        transparentCandidateDraws.Arguments->GetNonPixelShaderResourceBarrier(), transparentCandidateDraws.Count->GetNonPixelShaderResourceBarrier()});
    const UInt2 renderSize{_occluderDepthTexture->GetWidth(), _occluderDepthTexture->GetHeight()};
    struct { UInt2 RenderSize, TargetSize; UINT DrawOffset, FirstReduction; float MinOccluderSizePixels; UINT Padding; }
        occlusionConstants = {renderSize, {}, 0, 0, _minOccluderSizePixels, 0};
    commandList->SetComputeRootSignature(_occlusionCullingRootSignature.get());
    commandList->SetComputeRootConstantBufferView(1, frameConstants.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commandList->SetComputeSRV(0, opaqueCandidateDraws.ArgumentSRV->GPUHandle);
    commandList->SetComputeSRV(1, opaqueCandidateDraws.CountSRV->GPUHandle);
    commandList->SetComputeSRV(2, frameConstants.InstanceCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(3, frameConstants.MaterialCache->GetSRV()->GPUHandle);
    commandList->SetComputeUAV(0, occluderDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(1, occluderDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(3, visibleOpaqueDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(4, visibleTransparentDraws.CountUAV->GPUHandle);
    commandList->BeginPixEvent("Camera Select Occluders", Colors::SlateBlue);
    commandList->SetPipelineState(_clearDrawCountsPipeline);
    commandList->Dispatch(1, 1, 1);
    commandList->ResourceBarrier({occluderDraws.Count->GetUAVBarrier(), visibleOpaqueDraws.Count->GetUAVBarrier(), visibleTransparentDraws.Count->GetUAVBarrier()});
    const auto dispatchDrawStreamBatches = [&](const DrawStreamBinding& candidateDraws)
    {
        constexpr UINT maxDrawsPerDispatch = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * 64;
        for (uint64_t drawOffset = 0; drawOffset < candidateDraws.Capacity; drawOffset += maxDrawsPerDispatch)
        {
            occlusionConstants.DrawOffset = static_cast<UINT>(drawOffset);
            commandList->SetComputeRootConstants(0, occlusionConstants);
            commandList->Dispatch(static_cast<UINT>(((std::min)(uint64_t(candidateDraws.Capacity) - drawOffset, uint64_t(maxDrawsPerDispatch)) + 63) / 64), 1, 1);
        }
    };
    commandList->SetPipelineState(_selectOccludersPipeline);
    dispatchDrawStreamBatches(opaqueCandidateDraws);
    commandList->ResourceBarrier({occluderDraws.Arguments->GetIndirectArgsBarrier(), occluderDraws.Count->GetIndirectArgsBarrier()});
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Camera Occluder Depth", Colors::SteelBlue);
    commandList->SetGraphicsRootSignature(_occluderDepthRootSignature.get());
    commandList->SetPipelineState(_occluderDepthPipeline);
    commandList->SetGraphicsRootConstantBufferView(1, frameConstants.MainCB->GetElementAddress(0));
    commandList->SetGraphicsRootConstantBufferView(2, frameConstants.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commandList->SetGraphicsSRV(0, frameConstants.MaterialCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(1, frameConstants.TransformCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(2, frameConstants.InstanceCache->GetSRV()->GPUHandle);
    commandList->SetGraphicsSRV(3, deviceResources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commandList->SetGeometryBuffer(deviceResources->GeometryBuffer.get());
    commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetViewport(_occluderDepthTexture->GetViewport());
    commandList->SetScissorRect(_occluderDepthTexture->GetScissorRect());
    commandList->SetRenderTargets({}, _occluderDepthTexture.get());
    commandList->ClearDepthStencilView(_occluderDepthTexture.get());
    commandList->ExecuteIndirect(_occluderDrawCommandSignature.Get(), occluderDraws.Capacity, occluderDraws.Arguments->D3DResource.Get(), 0, occluderDraws.Count->D3DResource.Get(), 0);
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Camera Hi-Z", Colors::DarkCyan);
    commandList->ResourceBarrier({_occluderDepthTexture->GetResource()->GetNonPixelShaderResourceBarrier()});
    commandList->SetComputeRootSignature(_occlusionCullingRootSignature.get());
    commandList->SetPipelineState(_reduceHiZDepthPipeline);
    UINT mipWidth = _hiZDepthTexture->GetWidth(), mipHeight = _hiZDepthTexture->GetHeight();
    const UINT mipCount = _hiZDepthTexture->GetDescription().MipLevels;
    for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
    {
        occlusionConstants.TargetSize = {mipWidth, mipHeight};
        occlusionConstants.FirstReduction = mipIndex == 0 ? 1 : 0;
        commandList->SetComputeRootConstants(0, occlusionConstants);
        commandList->SetComputeSRV(4, mipIndex == 0 ? _occluderDepthTexture->GetSRV()->GPUHandle : _hiZMipSRVs[mipIndex - 1]->GPUHandle);
        commandList->SetComputeUAV(2, mipIndex == 0 ? _hiZDepthTexture->GetUAV()->GPUHandle : _hiZMipUAVs[mipIndex - 1]->GPUHandle);
        commandList->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
        commandList->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(_hiZDepthTexture->GetResource()->D3DResource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mipIndex)});
        mipWidth = (std::max)(1u, mipWidth / 2);
        mipHeight = (std::max)(1u, mipHeight / 2);
    }
    _hiZDepthTexture->GetResource()->SetCurrentState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->EndPixEvent();

    commandList->BeginPixEvent("Camera Occlusion Test", Colors::CornflowerBlue);
    commandList->SetComputeSRV(4, _hiZDepthTexture->GetSRV()->GPUHandle);
    commandList->SetComputeUAV(0, visibleOpaqueDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(1, visibleOpaqueDraws.CountUAV->GPUHandle);
    commandList->SetPipelineState(_occlusionCullingPipeline);
    dispatchDrawStreamBatches(opaqueCandidateDraws);
    commandList->SetComputeSRV(0, transparentCandidateDraws.ArgumentSRV->GPUHandle);
    commandList->SetComputeSRV(1, transparentCandidateDraws.CountSRV->GPUHandle);
    commandList->SetComputeUAV(0, visibleTransparentDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(1, visibleTransparentDraws.CountUAV->GPUHandle);
    dispatchDrawStreamBatches(transparentCandidateDraws);
    commandList->ResourceBarrier({visibleOpaqueDraws.Arguments->GetUAVBarrier(), visibleOpaqueDraws.Count->GetUAVBarrier(), visibleTransparentDraws.Arguments->GetUAVBarrier(), visibleTransparentDraws.Count->GetUAVBarrier()});
    commandList->EndPixEvent();
    commandList->EndPixEvent();
}

void GDX12OcclusionCullingPass::OnResourcesRecreated(PassInitContext& context)
{ CreateWorkingResources(context); }

std::vector<ExternalResourceUse> GDX12OcclusionCullingPass::GetExternalResources(PassExecutionContext& context)
{
    auto externalResources = GDX12RenderPass::GetExternalResources(context);
    for (auto* drawStreamResource : {_opaqueCandidateDrawStream.get(), _transparentCandidateDrawStream.get(), _occluderDrawStream.get()})
        for (auto& drawStreamBuffer : drawStreamResource->Buffers) externalResources.push_back({&drawStreamBuffer.Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    externalResources.push_back({_occluderDepthTexture->GetResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, {}});
    externalResources.push_back({_hiZDepthTexture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return externalResources;
}
