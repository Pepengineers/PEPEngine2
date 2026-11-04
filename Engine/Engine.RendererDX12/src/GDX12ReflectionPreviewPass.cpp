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
    root.NumSingleSRVSlots = 2;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    auto* color = _inputs[0]->GetTexture();
    if (!color->HasRTV() || !_inputs[1]->GetTexture()->HasDSV())
        throw std::invalid_argument("Reflection preview requires writable HDR and depth render targets");
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
    auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ReflectionPreview.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = _root->GetRootSignature().Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = color->GetFormat();
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
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
    commands->ResourceBarrier({ color->GetResource()->GetRenderTargetBarrier(),
        depth->GetResource()->GetDepthWriteBarrier(), frame.ReflectionProbes->GetResource().GetSRVBarrier() });
    commands->SetGraphicsRootSignature(_root.get());
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 1, &mask, 0);
    commands->SetGraphicsRootConstantBufferView(1,
        frame.CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    commands->SetGraphicsSRV(0, _resources->ReflectionRadianceSRV.GPUHandle);
    commands->SetGraphicsSRV(1, frame.ReflectionProbes->GetSRV()->GPUHandle);
    commands->SetPipelineState(_pipeline);
    commands->SetViewport(color->GetViewport());
    commands->SetScissorRect(color->GetScissorRect());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetRenderTargets({ color }, depth);
    commands->DrawInstanced(3, 1, 0, 0);
    commands->ResourceBarrier({ color->GetResource()->GetSRVBarrier(), depth->GetResource()->GetSRVBarrier() });
}
