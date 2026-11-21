#include "Engine.RendererDX12/RenderPasses/GDX12ShadowMapCullingPass.h"
#include <algorithm>

GDX12ShadowMapCullingPass::GDX12ShadowMapCullingPass()
{
    _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_INSTANCES |
        RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_CAMERAS;
}

void GDX12ShadowMapCullingPass::Describe(PassDescription& description) const
{
    auto& directional = description.AddOutputDrawStream(OutDirectionalDrawStream);
    directional.Stride = sizeof(GDX12ShadowDrawArgs);
    directional.Layout = ShadowDrawStreamLayout;
    directional.Usages = ResourceUsage::UnorderedAccess;
    directional.AllowShared = true;

    auto& pointSpotStatic = description.AddOutputDrawStream(OutPointSpotStaticDrawStream);
    pointSpotStatic.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotStatic.Layout = ShadowDrawStreamLayout;
    pointSpotStatic.Usages = ResourceUsage::UnorderedAccess;
    pointSpotStatic.AllowShared = true;

    auto& pointSpotDynamic = description.AddOutputDrawStream(OutPointSpotDynamicDrawStream);
    pointSpotDynamic.Stride = sizeof(GDX12ShadowDrawArgs);
    pointSpotDynamic.Layout = ShadowDrawStreamLayout;
    pointSpotDynamic.Usages = ResourceUsage::UnorderedAccess;
    pointSpotDynamic.AllowShared = true;
}

void GDX12ShadowMapCullingPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
    const auto& commands = context.GetDeviceResources()->IndirectCommandsCache;
    const auto& shadows = context.GetFrameData()->ShadowSystem;
    const auto count = commands->GetElementCount();
    const uint64_t directional = uint64_t(count) * shadows.DirectionalArraySize;
    const uint64_t pointSpot = uint64_t(count) * shadows.PointSpotArraySize;
    if ((std::max)(directional, pointSpot) > UINT_MAX)
        throw std::length_error("ERROR: Shadow DrawStream capacity exceeds its 32-bit command counter.");
    context.SetOutputBufferCapacity(OutDirectionalDrawStream, (std::max)(uint64_t(1), directional));
    context.SetOutputBufferCapacity(OutPointSpotStaticDrawStream, (std::max)(uint64_t(1), pointSpot));
    context.SetOutputBufferCapacity(OutPointSpotDynamicDrawStream, (std::max)(uint64_t(1), pointSpot));
}

void GDX12ShadowMapCullingPass::Initialize(PassInitContext& context)
{
    GDX12RootSignatureDesc description;
    description.Constants = {2};
    description.NumSingleSRVSlots = 4;
    description.NumSingleUAVSlots = 6;
    _root = std::make_unique<GDX12RootSignature>(context.GetDevice(), description);

    auto& compiler = GDX12ShaderCompiler::GetInstance();
    auto culling = compiler.CompileShader(context.GetDevice(), SHADERS_FOLDER "ShadowMapCullingPass.hlsl", nullptr, "CS", "cs");
    auto clear = compiler.CompileShader(context.GetDevice(), SHADERS_FOLDER "ShadowMapCullingPass.hlsl", nullptr, "ClearCounters", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.CS = {culling->GetBufferPointer(), culling->GetBufferSize()};
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_cullingPipeline)));
    pipeline.CS = {clear->GetBufferPointer(), clear->GetBufferSize()};
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_clearPipeline)));
}

void GDX12ShadowMapCullingPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto& directional = context.GetOutputDrawStream(OutDirectionalDrawStream);
    const auto& pointSpotStatic = context.GetOutputDrawStream(OutPointSpotStaticDrawStream);
    const auto& pointSpotDynamic = context.GetOutputDrawStream(OutPointSpotDynamicDrawStream);

    commands->BeginPixEvent("Shadow Map Culling", Colors::SteelBlue);
    commands->SetComputeRootSignature(_root.get());
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetComputeSRV(0, frame.InstanceCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, resources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, frame.MaterialCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(3, frame.ShadowViews->GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, directional.ArgumentUAV->GPUHandle);
    commands->SetComputeUAV(1, directional.CountUAV->GPUHandle);
    commands->SetComputeUAV(2, pointSpotStatic.ArgumentUAV->GPUHandle);
    commands->SetComputeUAV(3, pointSpotStatic.CountUAV->GPUHandle);
    commands->SetComputeUAV(4, pointSpotDynamic.ArgumentUAV->GPUHandle);
    commands->SetComputeUAV(5, pointSpotDynamic.CountUAV->GPUHandle);
    commands->SetPipelineState(_clearPipeline);
    commands->Dispatch(1, 1, 1);
    commands->ResourceBarrier({directional.Count->GetUAVBarrier(), pointSpotStatic.Count->GetUAVBarrier(), pointSpotDynamic.Count->GetUAVBarrier()});

    commands->SetPipelineState(_cullingPipeline);
    const uint64_t drawCount = resources->IndirectCommandsCache->GetElementCount();
    const uint64_t viewCount = context.GetFrameData()->ShadowSystem.Views.size();
    constexpr UINT maxGroups = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    constexpr UINT drawsPerBatch = maxGroups * 64;
    for (uint64_t viewOffset = 0; viewOffset < viewCount; viewOffset += maxGroups)
    {
        for (uint64_t drawOffset = 0; drawOffset < drawCount; drawOffset += drawsPerBatch)
        {
            if (viewOffset || drawOffset)
                commands->ResourceBarrier({directional.Arguments->GetUAVBarrier(), directional.Count->GetUAVBarrier(),
                    pointSpotStatic.Arguments->GetUAVBarrier(), pointSpotStatic.Count->GetUAVBarrier(),
                    pointSpotDynamic.Arguments->GetUAVBarrier(), pointSpotDynamic.Count->GetUAVBarrier()});
            const UINT offsets[] = {static_cast<UINT>(drawOffset), static_cast<UINT>(viewOffset)};
            commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, offsets, 0);
            commands->Dispatch(static_cast<UINT>(((std::min)(drawCount - drawOffset, uint64_t(drawsPerBatch)) + 63) / 64),
                static_cast<UINT>((std::min)(viewCount - viewOffset, uint64_t(maxGroups))), 1);
        }
    }
    commands->ResourceBarrier({directional.Arguments->GetUAVBarrier(), directional.Count->GetUAVBarrier(),
        pointSpotStatic.Arguments->GetUAVBarrier(), pointSpotStatic.Count->GetUAVBarrier(),
        pointSpotDynamic.Arguments->GetUAVBarrier(), pointSpotDynamic.Count->GetUAVBarrier()});
    commands->EndPixEvent();
}
