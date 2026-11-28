#include "Engine.RendererDX12/RenderPasses/GDX12ShadowMapCullingPass.h"
#include "Engine.RendererDX12/ShadowMapSystem.h"
#include <algorithm>
#include <cmath>

GDX12ShadowMapCullingPass::GDX12ShadowMapCullingPass()
{
    _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_INSTANCES |
        RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_CAMERAS;
}

void GDX12ShadowMapCullingPass::Describe(PassDescription& description) const
{
    // Zero disables size culling. Other values require at least one projected AABB side
    // to reach this many shadow-map texels in the corresponding directional cascade.
    description.AddParameter("Cascade1MinCasterSizeTexels", 0.0f);
    description.AddParameter("Cascade2MinCasterSizeTexels", 4.0f);
    description.AddParameter("Cascade3MinCasterSizeTexels", 64.0f);
    description.AddParameter("Cascade4MinCasterSizeTexels", 128.0f);

    auto& directionalDrawStream = description.AddOutputDrawStream(OutDirectionalDrawStream);
    directionalDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    directionalDrawStream.Layout = ShadowDrawStreamLayout;
    directionalDrawStream.Usages = ResourceUsage::UnorderedAccess;
    directionalDrawStream.AllowShared = true;

    auto& pointSpotStaticDrawStream = description.AddOutputDrawStream(OutPointSpotStaticDrawStream);
    pointSpotStaticDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotStaticDrawStream.Layout = ShadowDrawStreamLayout;
    pointSpotStaticDrawStream.Usages = ResourceUsage::UnorderedAccess;
    pointSpotStaticDrawStream.AllowShared = true;

    auto& pointSpotDynamicDrawStream = description.AddOutputDrawStream(OutPointSpotDynamicDrawStream);
    pointSpotDynamicDrawStream.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotDynamicDrawStream.Layout = ShadowDrawStreamLayout;
    pointSpotDynamicDrawStream.Usages = ResourceUsage::UnorderedAccess;
    pointSpotDynamicDrawStream.AllowShared = true;
}

void GDX12ShadowMapCullingPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
    const auto& sceneDrawCommands = context.GetDeviceResources()->IndirectCommandsCache;
    const auto& shadowSystem = *context.GetFrameData()->ShadowSystem;
    const auto sceneDrawCount = sceneDrawCommands->GetElementCount();
    const uint64_t directionalDrawCapacity = uint64_t(sceneDrawCount) * shadowSystem.DirectionalArraySize;
    const uint64_t pointSpotDrawCapacity = uint64_t(sceneDrawCount) * shadowSystem.PointSpotArraySize;
    if ((std::max)(directionalDrawCapacity, pointSpotDrawCapacity) > UINT_MAX)
        throw std::length_error("ERROR: Shadow DrawStream capacity exceeds its 32-bit command counter.");
    context.SetOutputBufferCapacity(OutDirectionalDrawStream, (std::max)(uint64_t(1), directionalDrawCapacity));
    context.SetOutputBufferCapacity(OutPointSpotStaticDrawStream, (std::max)(uint64_t(1), pointSpotDrawCapacity));
    context.SetOutputBufferCapacity(OutPointSpotDynamicDrawStream, (std::max)(uint64_t(1), pointSpotDrawCapacity));
}

void GDX12ShadowMapCullingPass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    _cascadeMinCasterSizeTexels = {
        context.GetParameter<float>("Cascade1MinCasterSizeTexels"),
        context.GetParameter<float>("Cascade2MinCasterSizeTexels"),
        context.GetParameter<float>("Cascade3MinCasterSizeTexels"),
        context.GetParameter<float>("Cascade4MinCasterSizeTexels")
    };
    for (const float minimumCasterSizeTexels : {_cascadeMinCasterSizeTexels.x, _cascadeMinCasterSizeTexels.y, _cascadeMinCasterSizeTexels.z, _cascadeMinCasterSizeTexels.w})
    {
        if (!std::isfinite(minimumCasterSizeTexels) || minimumCasterSizeTexels < 0)
            throw std::invalid_argument("ERROR: Cascade minimum caster sizes must be finite, non-negative shadow-map texel counts.");
    }

    GDX12RootSignatureDesc shadowCullingRootDescription;
    shadowCullingRootDescription.Constants = {8};
    shadowCullingRootDescription.NumSingleSRVSlots = 5;
    shadowCullingRootDescription.NumSingleUAVSlots = 6;
    _shadowCullingRootSignature = std::make_unique<GDX12RootSignature>(device, shadowCullingRootDescription);

    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    auto shadowCullingShader = shaderCompiler.CompileShader(device, SHADERS_FOLDER "ShadowMapCullingPass.hlsl", nullptr, "CS", "cs");
    auto clearDrawCountsShader = shaderCompiler.CompileShader(device, SHADERS_FOLDER "ShadowMapCullingPass.hlsl", nullptr, "ClearCounters", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC shadowCullingPipelineDescription{};
    shadowCullingPipelineDescription.pRootSignature = _shadowCullingRootSignature->GetRootSignature().Get();
    shadowCullingPipelineDescription.CS = {shadowCullingShader->GetBufferPointer(), shadowCullingShader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&shadowCullingPipelineDescription, IID_PPV_ARGS(&_shadowCullingPipeline)));
    shadowCullingPipelineDescription.CS = {clearDrawCountsShader->GetBufferPointer(), clearDrawCountsShader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&shadowCullingPipelineDescription, IID_PPV_ARGS(&_clearDrawCountsPipeline)));
}

void GDX12ShadowMapCullingPass::Execute(PassExecutionContext& context)
{
    auto* commandList = context.GetCommandList();
    auto* deviceResources = context.GetDeviceResources();
    auto& frameConstants = *deviceResources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const UINT forcedShadowUpdates = _resetShadowCache ? SHADOW_UPDATE_STATIC | SHADOW_UPDATE_FINAL : 0;
    const auto& directionalDraws = context.GetOutputDrawStream(OutDirectionalDrawStream);
    const auto& pointSpotStaticDraws = context.GetOutputDrawStream(OutPointSpotStaticDrawStream);
    const auto& pointSpotDynamicDraws = context.GetOutputDrawStream(OutPointSpotDynamicDrawStream);

    commandList->BeginPixEvent("Shadow Map Culling", Colors::SteelBlue);
    commandList->SetComputeRootSignature(_shadowCullingRootSignature.get());
    commandList->SetDescriptorHeaps({deviceResources->SRV_UAV_Heap.get()});
    commandList->SetComputeSRV(0, frameConstants.InstanceCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(1, deviceResources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(2, frameConstants.MaterialCache->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(3, frameConstants.ShadowViews->GetSRV()->GPUHandle);
    commandList->SetComputeSRV(4, frameConstants.ShadowCacheUpdateFlags->GetSRV()->GPUHandle);
    commandList->SetComputeUAV(0, directionalDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(1, directionalDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(2, pointSpotStaticDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(3, pointSpotStaticDraws.CountUAV->GPUHandle);
    commandList->SetComputeUAV(4, pointSpotDynamicDraws.ArgumentUAV->GPUHandle);
    commandList->SetComputeUAV(5, pointSpotDynamicDraws.CountUAV->GPUHandle);
    commandList->SetPipelineState(_clearDrawCountsPipeline);
    commandList->Dispatch(1, 1, 1);
    commandList->ResourceBarrier({directionalDraws.Count->GetUAVBarrier(), pointSpotStaticDraws.Count->GetUAVBarrier(), pointSpotDynamicDraws.Count->GetUAVBarrier()});

    commandList->SetPipelineState(_shadowCullingPipeline);
    const uint64_t sceneDrawCount = deviceResources->IndirectCommandsCache->GetElementCount();
    const uint64_t shadowViewCount = context.GetFrameData()->ShadowSystem->Views.size();
    struct { UINT DrawOffset, ViewOffset, ForcedUpdates, ShadowMapResolution; XMFLOAT4 MinCasterSizeTexels; }
        shadowCullingConstants = {0, 0, forcedShadowUpdates, context.GetFrameData()->ShadowSystem->DirectionalLightShadowMapResolution, _cascadeMinCasterSizeTexels};
    constexpr UINT maxDispatchGroups = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    constexpr UINT maxDrawsPerDispatch = maxDispatchGroups * 64;
    for (uint64_t viewOffset = 0; viewOffset < shadowViewCount; viewOffset += maxDispatchGroups)
    {
        for (uint64_t drawOffset = 0; drawOffset < sceneDrawCount; drawOffset += maxDrawsPerDispatch)
        {
            if (viewOffset || drawOffset)
                commandList->ResourceBarrier({directionalDraws.Arguments->GetUAVBarrier(), directionalDraws.Count->GetUAVBarrier(),
                    pointSpotStaticDraws.Arguments->GetUAVBarrier(), pointSpotStaticDraws.Count->GetUAVBarrier(),
                    pointSpotDynamicDraws.Arguments->GetUAVBarrier(), pointSpotDynamicDraws.Count->GetUAVBarrier()});
            shadowCullingConstants.DrawOffset = static_cast<UINT>(drawOffset);
            shadowCullingConstants.ViewOffset = static_cast<UINT>(viewOffset);
            commandList->SetComputeRootConstants(0, shadowCullingConstants);
            commandList->Dispatch(static_cast<UINT>(((std::min)(sceneDrawCount - drawOffset, uint64_t(maxDrawsPerDispatch)) + 63) / 64),
                static_cast<UINT>((std::min)(shadowViewCount - viewOffset, uint64_t(maxDispatchGroups))), 1);
        }
    }
    commandList->ResourceBarrier({directionalDraws.Arguments->GetUAVBarrier(), directionalDraws.Count->GetUAVBarrier(),
        pointSpotStaticDraws.Arguments->GetUAVBarrier(), pointSpotStaticDraws.Count->GetUAVBarrier(),
        pointSpotDynamicDraws.Arguments->GetUAVBarrier(), pointSpotDynamicDraws.Count->GetUAVBarrier()});
    commandList->EndPixEvent();
    _resetShadowCache = false;
}

void GDX12ShadowMapCullingPass::OnResourcesRecreated(PassInitContext& context)
{
    _resetShadowCache = true;
}
