#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionPreviewPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

GDX12ReflectionPreviewPass::GDX12ReflectionPreviewPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 2;
}

void GDX12ReflectionPreviewPass::Initialize()
{
    GDX12RootSignatureDesc root;
    root.Constants = { 1 };
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 3;
    root.NumSingleUAVSlots = 1;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    _pipeline = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
        SHADERS_FOLDER "ReflectionPreview.hlsl", "CS");
}

void GDX12ReflectionPreviewPass::Execute(GDX12CommandList* commands)
{
    UINT mask = 0;
    for (UINT i = 0; i < _resources->ReflectionProbes.size() && i < MaxReflectionProbes; ++i)
        if (_resources->ReflectionProbes[i].PreviewSphere) mask |= 1u << i;
    if (!mask) return;

    GDX12_SCOPED_EVENT(commands, "Reflection probe spheres", Colors::Cyan);
    auto* color = _inputs[0]->GetTexture();
    auto* depth = _inputs[1]->GetTexture();
    auto& frame = *_resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->ResourceBarrier({ color->GetResource()->GetUnorderedAccessBarrier(),
        depth->GetResource()->GetSRVBarrier(), frame.ReflectionProbes->GetResource().GetSRVBarrier() });
    commands->SetComputeRootSignature(_root.get());
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 1, &mask, 0);
    commands->SetComputeRootConstantBufferView(1,
        frame.CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, depth->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, _resources->ReflectionRadianceSRV.GPUHandle);
    commands->SetComputeSRV(2, frame.ReflectionProbes->GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, color->GetUAV()->GPUHandle);
    commands->SetPipelineState(_pipeline);
    commands->Dispatch((color->GetWidth() + 7) / 8, (color->GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::UAV(color->GetResource()->D3DResource.Get()),
        color->GetResource()->GetSRVBarrier() });
}
